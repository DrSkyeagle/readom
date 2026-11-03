#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Every class a package's own XML names has to be in that package.

The defect this is for took the launcher down at every start of the first
clean install, and nothing before the device could have seen it. Setting
LEGACY_ICON_TREATMENT to false made wrapToAdaptiveIconDrawable() return before
it touched FixedScaleDrawable, so that method stopped being a reference the
shrinker could see, so the class was dropped -- while
res/drawable-v26/adaptive_icon_drawable_wrapper.xml went on naming it. The
build succeeded. Every test passed. The APK was missing one class that only an
XML file mentions, and the first icon the cache missed threw
ClassNotFoundException out of the loader thread.

A shrinker cannot see through a string in a resource, so this is a property of
the built package rather than of the source, and it is checked here the only
way it can be: open what the build made, read the class names out of its
compiled XML, and look for each one in its code.

Where a class name can appear is a short list, and this walks the compiled XML
to find exactly those places rather than sifting the string pool for names of
the right shape. The difference is false positives: an intent action, a
permission, a provider's authorities and a meta-data key are written exactly
like a class name (com.android.systemui.action.CLEAR_TUNER,
com.android.webview.WebViewLibrary), and sifting cannot tell them from the real
thing. The places that do name a class:

  * a tag with a dot in it -- <com.android.launcher3.graphics.FixedScaleDrawable/>
    is the whole of the defect above, and is how every custom view and drawable
    is written;
  * android:name on a component (activity, service, receiver, provider,
    application, instrumentation) and on <fragment>, and targetActivity on an
    <activity-alias>, which is where the alias's class actually is;
  * a bare class= attribute, which <fragment> and <view> both accept.

A name there may be relative (".Foo", "Foo", "Settings$FooActivity"), and is
resolved against the package the way the framework resolves it.
"""
import argparse
import re
import subprocess
import sys
import zipfile
from pathlib import Path

RES_STRING_POOL = 0x0001
RES_XML_START_ELEMENT = 0x0102
ANDROID_NS = 'http://schemas.android.com/apk/res/android'

# The tags whose android:name is a class. <activity-alias> is not one of them:
# its android:name is the alias, and the class is in targetActivity.
COMPONENT_TAGS = {'activity', 'service', 'receiver', 'provider', 'application',
                  'instrumentation', 'fragment'}

# A fully qualified name whose last segment is a type: two or more lowercase
# package segments and a final segment that starts with a capital. Inner
# classes are written with a dollar, which is why it is in the last segment.
CLASS_NAME = re.compile(r'^(?:[a-z][a-z0-9_]*\.){2,}[A-Z][A-Za-z0-9_$]*$')

# Names that are missing from the package that mentions them, that this project
# did not write and does not reach. Each one is an entry AOSP left behind when
# the class went away; the screen behind it is not in any menu, and nothing
# sends the explicit intent that would land on it. They are listed so that a
# name this project drops is a failure rather than one more line in a list.
KNOWN_DANGLING = {
    ('Settings.apk', 'com.android.settings.LocalePickerFragment'):
        'res/layout/locale_picker.xml is AOSP 8.1 dead weight: the class went '
        'when the locale picker moved to LocalePicker/LocaleListEditor, the '
        'layout stayed, and nothing inflates it (no R.layout.locale_picker in '
        'the sources).',
    ('Settings.apk', 'com.android.settings.Display'):
        'an <activity> AOSP 8.1 still declares with no Display.java anywhere in '
        'its sources. Its only way in is the DEVELOPMENT_PREFERENCE category, '
        'which the old Dev Tools application enumerated and nothing on this '
        'device does.',
    ('Settings.apk', 'com.android.settings.CryptKeeper$FadeToBlack'):
        'the same: declared, and not in the sources (CryptKeeper.java is there '
        'and has no such inner class). CryptKeeper itself only runs on an '
        'encrypted boot, and this device keeps its data in a plain ext4 image.',
    ('PackageInstaller.apk', 'com.android.packageinstaller.permission.ui.wear.WarningConfirmationActivity'):
        'declared by AOSP 8.1 and not in its sources (the wear directory has '
        'two fragments and no such activity). It belongs to the Wear permission '
        'flow, which a Kindle never enters.',
}


def length8(chunk, at):
    """A UTF-8 pool length: one byte, or two when the first has the high bit set."""
    if at >= len(chunk):
        raise ValueError('string pool: a length runs past the chunk')
    size = chunk[at]
    if size & 0x80:
        if at + 1 >= len(chunk):
            raise ValueError('string pool: a two-byte length runs past the chunk')
        return ((size & 0x7f) << 8) | chunk[at + 1], at + 2
    return size, at + 1


def strings_of_string_pool(chunk):
    """The strings of a RES_STRING_POOL chunk, in pool order.

    Pool order is what the references in the rest of the file index into, so
    the list is returned as it is stored.

    A UTF-8 string carries *two* lengths, the character count and then the byte
    count, each of them one byte or two. Reading the first as always one byte
    works until a string is 128 characters or longer, and then every byte after
    it is off by one -- which this test would have reported as the package
    naming nothing at all. Anything that does not decode raises instead of
    coming back empty, for the same reason.
    """
    if len(chunk) < 28:
        raise ValueError('string pool: chunk too short')
    count = int.from_bytes(chunk[8:12], 'little')
    flags = int.from_bytes(chunk[16:20], 'little')
    start = int.from_bytes(chunk[20:24], 'little')
    utf8 = bool(flags & (1 << 8))
    out = []
    for i in range(count):
        at = 28 + 4 * i
        if at + 4 > len(chunk):
            raise ValueError('string pool: the offset table runs past the chunk')
        at = start + int.from_bytes(chunk[at:at + 4], 'little')
        if at >= len(chunk):
            raise ValueError('string pool: a string begins past the chunk')
        try:
            if utf8:
                _, at = length8(chunk, at)          # characters; the bytes follow
                size, at = length8(chunk, at)
                if at + size > len(chunk):
                    raise ValueError('string pool: a string runs past the chunk')
                out.append(chunk[at:at + size].decode('utf-8'))
            else:
                size = int.from_bytes(chunk[at:at + 2], 'little')
                at += 2
                if size & 0x8000:
                    size = ((size & 0x7fff) << 16) | int.from_bytes(chunk[at:at + 2], 'little')
                    at += 2
                if at + 2 * size > len(chunk):
                    raise ValueError('string pool: a string runs past the chunk')
                out.append(chunk[at:at + 2 * size].decode('utf-16-le'))
        except (IndexError, UnicodeDecodeError) as problem:
            raise ValueError('string pool: %s' % problem)
    return out


def start_element(chunk, header, pool):
    """One RES_XML_START_ELEMENT as (tag, {(namespace, attribute): value}).

    The layout is ResXMLTree_node followed by ResXMLTree_attrExt, whose
    attributeStart is counted from the start of the attrExt rather than from
    the start of the chunk -- hence header + attr_start.
    """
    def text(index):
        return pool[index] if 0 <= index < len(pool) else None

    if header + 14 > len(chunk):
        raise ValueError('a start tag has no attribute header')
    name = int.from_bytes(chunk[header + 4:header + 8], 'little')
    attr_start = int.from_bytes(chunk[header + 8:header + 10], 'little')
    attr_size = int.from_bytes(chunk[header + 10:header + 12], 'little')
    count = int.from_bytes(chunk[header + 12:header + 14], 'little')
    # An attribute is a ResXMLTree_attribute: four words and a Res_value. A
    # larger one is a newer format with more after them and is read as far as
    # it is known; a smaller one is not this format at all.
    if count and attr_size < 20:
        raise ValueError('attributes are %d bytes, which is not this format' % attr_size)
    attrs = {}
    for i in range(count):
        at = header + attr_start + i * attr_size
        if at + 20 > len(chunk):
            raise ValueError('an attribute runs past the tag')
        namespace = text(int.from_bytes(chunk[at:at + 4], 'little'))
        attribute = text(int.from_bytes(chunk[at + 4:at + 8], 'little'))
        raw = int.from_bytes(chunk[at + 8:at + 12], 'little')
        kind = chunk[at + 15]
        data = int.from_bytes(chunk[at + 16:at + 20], 'little')
        if raw != 0xffffffff:
            value = text(raw)
        elif kind == 0x03:                      # TYPE_STRING
            value = text(data)
        else:
            value = None
        attrs[(namespace, attribute)] = value
    return text(name), attrs


def elements_of_binary_xml(blob):
    """Every start tag of a compiled XML resource, with its attributes."""
    if len(blob) < 8 or blob[:2] != b'\x03\x00':
        return []
    at = int.from_bytes(blob[2:4], 'little')
    total = min(int.from_bytes(blob[4:8], 'little'), len(blob))
    pool, out = [], []
    while at + 8 <= total:
        kind = int.from_bytes(blob[at:at + 2], 'little')
        header = int.from_bytes(blob[at + 2:at + 4], 'little')
        size = int.from_bytes(blob[at + 4:at + 8], 'little')
        if size < 8 or at + size > total or header < 8:
            break
        if kind == RES_STRING_POOL:
            pool = strings_of_string_pool(blob[at:at + size])
        elif kind == RES_XML_START_ELEMENT:
            out.append(start_element(blob[at:at + size], header, pool))
        at += size
    return out


def resolved(name, package):
    """A class name as the framework reads it: relative names are the package's."""
    if not name:
        return None
    if name.startswith('.'):
        return package + name
    if '.' not in name:
        return package + '.' + name
    return name


def classes_named_by(blob, package):
    """The classes a compiled XML resource names, absolute."""
    out = []
    for tag, attrs in elements_of_binary_xml(blob):
        if not tag:
            continue
        if '.' in tag:
            out.append(resolved(tag, package))
        if tag in COMPONENT_TAGS:
            out.append(resolved(attrs.get((ANDROID_NS, 'name')), package))
        if tag == 'activity-alias':
            out.append(resolved(attrs.get((ANDROID_NS, 'targetActivity')), package))
        out.append(resolved(attrs.get((None, 'class')), package))
    return [name for name in out if name]


def a_pool(texts, utf8):
    """A RES_STRING_POOL chunk holding these strings, as aapt writes one."""
    data, offsets = bytearray(), []
    for text in texts:
        offsets.append(len(data))
        if utf8:
            encoded = text.encode('utf-8')
            for number in (len(text), len(encoded)):   # characters, then bytes
                data += (bytes(((number >> 8) | 0x80, number & 0xff)) if number > 0x7f
                         else bytes((number,)))
            data += encoded + b'\0'
        else:
            number = len(text)
            data += ((((number >> 16) | 0x8000).to_bytes(2, 'little')
                      + (number & 0xffff).to_bytes(2, 'little')) if number > 0x7fff
                     else number.to_bytes(2, 'little'))
            data += text.encode('utf-16-le') + b'\0\0'
    start = 28 + 4 * len(texts)
    chunk = bytearray()
    chunk += RES_STRING_POOL.to_bytes(2, 'little') + (28).to_bytes(2, 'little')
    chunk += (start + len(data)).to_bytes(4, 'little')
    chunk += len(texts).to_bytes(4, 'little') + (0).to_bytes(4, 'little')
    chunk += ((1 << 8) if utf8 else 0).to_bytes(4, 'little')
    chunk += start.to_bytes(4, 'little') + (0).to_bytes(4, 'little')
    for offset in offsets:
        chunk += offset.to_bytes(4, 'little')
    return bytes(chunk + data)


def pool_reader_works():
    """The string pool reader, against strings whose lengths are the awkward ones.

    A UTF-8 string carries a character count and a byte count, each one byte or
    two, and the two differ for anything that is not ASCII. Read one of them
    wrongly and every string after it comes back as rubbish or as nothing --
    and nothing is what this test would then have reported as 'no classes
    named', a pass over nothing. So the reader is given strings it knows the
    answer for before it is given a package.
    """
    texts = ['a', 'x' * 127, 'y' * 128, 'z' * 255, 'w' * 256,
             '正在启动' * 40,              # 160 characters, 480 bytes
             'com.android.launcher3.graphics.FixedScaleDrawable']
    for utf8 in (True, False):
        back = strings_of_string_pool(a_pool(texts, utf8))
        if back != texts:
            raise SystemExit('the string pool reader is wrong for %s: %s'
                             % ('UTF-8' if utf8 else 'UTF-16',
                                [(len(a), len(b)) for a, b in zip(texts, back) if a != b]))


def package_of(aapt, path):
    """The package name, asked of the package rather than guessed.

    A package that cannot be read is a failure rather than one quietly left
    out: a test that skips everything it cannot open ends up reporting a pass
    over nothing, which is the one result this must never give.
    """
    out = subprocess.run([str(aapt), 'dump', 'badging', str(path)],
                         capture_output=True, text=True)
    found = re.search(r"^package: name='([^']+)'", out.stdout, re.M)
    if not found:
        raise ValueError('%s says no package name: %s'
                         % (aapt, (out.stderr or out.stdout).strip().splitlines()[:1]))
    return found.group(1)


def code_of(archive, path):
    """Every byte of compiled code that belongs to this package.

    On a user build the APK carries no dex: dexpreopt strips it and the code
    ships beside the package as oat/<arch>/<name>.vdex and .odex. Both forms
    are read, because a package can be either.

    Only the package's own oat counts, which is why the glob carries its name.
    A neighbour's does not: framework-res.apk is a resources-only package that
    sits in /system/framework, next to the boot classpath's oat, and reading
    boot-framework.vdex as if it were framework-res's code would both hide
    framework-res's own misses and answer for classes that belong to
    framework.jar.

    A dex keeps every type it defines or uses as a `Lpackage/Type;` string, so
    membership is a substring test rather than a parse. A class that is only
    *used* would also match -- and a class used but not defined is a different
    defect, one the verifier catches at load rather than this one.
    """
    blob = b''
    for name in archive.namelist():
        if name.startswith('classes') and name.endswith('.dex'):
            blob += archive.read(name)
    for extra in (sorted(path.parent.glob('oat/*/%s.vdex' % path.stem)) +
                  sorted(path.parent.glob('oat/*/%s.odex' % path.stem))):
        blob += extra.read_bytes()
    return blob


def check(aapt, path):
    """The class names this package's XML names and its code does not have."""
    missing = []
    package = package_of(aapt, path)
    with zipfile.ZipFile(path) as archive:
        code = code_of(archive, path)
        if not code:
            # A package with no code of its own -- framework-res and the other
            # resource-only ones. The classes their layouts name belong to the
            # boot classpath, which is not shrunk and is not theirs to keep.
            return package, None, []
        seen = set()
        for name in archive.namelist():
            if not (name == 'AndroidManifest.xml' or
                    (name.startswith('res/') and name.endswith('.xml'))):
                continue
            for text in classes_named_by(archive.read(name), package):
                if text in seen or not CLASS_NAME.match(text):
                    continue
                # A name outside this package is somebody else's to keep: an
                # app layout may use a framework view, and framework.jar is
                # not shrunk.
                if not text.startswith(package + '.'):
                    continue
                seen.add(text)
                descriptor = ('L' + text.replace('.', '/') + ';').encode()
                if descriptor not in code:
                    missing.append((name, text))
        return package, len(seen), missing


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--product', type=Path, required=True,
                        help='out/target/product/pw3')
    parser.add_argument('--aapt', type=Path, required=True,
                        help="the build's own aapt, for the package name")
    args = parser.parse_args()
    pool_reader_works()
    apks = sorted((args.product / 'system').rglob('*.apk'))
    if not apks:
        raise SystemExit('no packages under %s/system' % args.product)
    failures, checked, names, allowed, resources = [], 0, 0, 0, []
    for apk in apks:
        try:
            package, count, missing = check(args.aapt, apk)
        except (zipfile.BadZipFile, KeyError, ValueError) as problem:
            failures.append('%s could not be read: %s' % (apk.name, problem))
            continue
        if count is None:
            resources.append(apk.name)
            continue
        checked += 1
        names += count
        for where, text in missing:
            if (apk.name, text) in KNOWN_DANGLING:
                allowed += 1
                continue
            failures.append('%s: %s names %s, and the code does not have it. A class only '
                            'an XML file mentions has no reference a shrinker can see; keep '
                            'it in proguard.flags, or stop naming it.'
                            % (apk.name, where, text))
    # Nothing read is not a pass. Every way this test can come to look at
    # nothing -- a tool that cannot run, packages that all turn out to carry no
    # code, a parser that quietly returns an empty list -- ends here instead of
    # in a line that says everything is present.
    if not checked or not names:
        failures.append('%d packages with code and %d class names: this checked nothing, '
                        'which is not a pass. %d packages were read as resource-only.'
                        % (checked, names, len(resources)))
    if failures:
        print('\n'.join(failures))
        return 1
    print('%d packages, %d class names in their own XML, all present in their code '
          '(%d known AOSP leftovers skipped, %d resource-only packages: %s)'
          % (checked, names, allowed, len(resources), ', '.join(resources)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
