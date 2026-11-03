#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""What a built image points at, and whether the image has it.

Two things an image can say about itself and be wrong about, both of which
cost a boot-time error and a reader's afternoon rather than a crash:

  - an init .rc file names a program that is not in the image. A module that
    stops being installed -- dropped by the build variant, by a package list,
    or by a patch -- leaves its .rc behind when something else installs that,
    and init fails to start the service at every boot. Found twice on this
    device: AOSP's flash_recovery, whose install-recovery.sh is generated only
    for a product that builds a recovery image (this one sets TARGET_NO_RECOVERY),
    and perfprofd, whose .rc names /system/xbin and whose binary is in /system/bin.
  - an executable or library asks for a shared library the image does not have.
    The loader then fails at exec or at the first dlopen, and says only which
    library, not what took it away.

Both are read out of the finished tree, so they see what the device will get,
including anything a variant or a package list dropped.

Usage: check-image-references.py [PRODUCT_DIR] [--readelf PATH]
  PRODUCT_DIR   out/target/product/<device> (default: this project's build VM path)
"""
import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

VM_PRODUCT = Path('/home/builder/android-src/out/target/product/pw3')
VM_READELF = Path('/home/builder/android-src/prebuilts/gcc/linux-x86/arm/'
                  'arm-linux-androideabi-4.9/bin/arm-linux-androideabi-readelf')
# "service <name> <path>", and the two forms of exec init accepts.
SERVICE = re.compile(r'^\s*service\s+\S+\s+(/\S+)')
EXEC_SECLABEL = re.compile(r'^\s*exec(?:_background)?\s+(?:\S+\s+)*?--\s+(/\S+)')
EXEC_PLAIN = re.compile(r'^\s*exec(?:_background)?\s+(/\S+)')
NEEDED = re.compile(r'\(NEEDED\).*\[(.+?)\]')
# Where the dynamic loader looks on this device, in its order.
LIBRARY_DIRS = ('system/lib', 'system/vendor/lib', 'vendor/lib')


def image_files(product):
    """Every path the device will have, as the device will spell it."""
    paths = set()
    for root, prefix in ((product / 'root', ''), (product / 'system', '/system')):
        if not root.is_dir():
            continue
        for path in root.rglob('*'):
            if path.is_file() or path.is_symlink():
                paths.add(prefix + '/' + str(path.relative_to(root)))
    return paths


def rc_files(product):
    for root, where in ((product / 'root', ''), (product / 'system/etc/init', 'system/etc/init/')):
        if root.is_dir():
            for path in sorted(root.rglob('*.rc')):
                yield where + path.name if where else str(path.relative_to(root)), path


def programs(product, files):
    """Programs named by .rc files that the image does not have.

    No .rc file at all is a failure, not a clean result: this device's image
    carries dozens of them, so an empty walk means the directories moved or the
    staged tree is not what it looks like, and "0 programs named, 0 missing"
    reads exactly like a pass (Codex 2026-10-31).
    """
    missing, checked = [], 0
    for where, path in rc_files(product):
        for line in path.read_text(errors='replace').splitlines():
            for pattern in (SERVICE, EXEC_SECLABEL, EXEC_PLAIN):
                found = pattern.match(line)
                if not found:
                    continue
                checked += 1
                if found.group(1) not in files:
                    missing.append('%s: %s' % (where, line.strip()))
                break
    if not checked:
        missing.append('no .rc file in this tree names a program: the check did not run')
    return checked, missing


def libraries(product, readelf):
    """Shared libraries asked for that are not in the loader's path."""
    have = set()
    for where in LIBRARY_DIRS:
        directory = product / where
        if directory.is_dir():
            have |= {p.name for p in directory.iterdir()}
    missing, checked, dynamic = {}, 0, 0
    unreadable = []
    system = product / 'system'
    for path in sorted(system.rglob('*')):
        if not path.is_file() or path.is_symlink():
            continue
        with path.open('rb') as f:
            if f.read(4) != b'\x7fELF':
                continue
        out = subprocess.run([str(readelf), '-d', str(path)], capture_output=True, text=True)
        # readelf's exit code, because an empty stdout and a clean one are the
        # same answer here otherwise: a corrupt ELF, or a readelf that cannot
        # read this object, used to pass as "this file asks for nothing"
        # (Codex 2026-10-31).
        if out.returncode:
            unreadable.append('%s: readelf -d exited %d: %s'
                              % (path.relative_to(system), out.returncode,
                                 (out.stderr.strip().splitlines() or ['no output'])[0]))
            continue
        names = NEEDED.findall(out.stdout)
        if not names:
            continue
        dynamic += 1
        for name in names:
            checked += 1
            if name not in have:
                missing.setdefault(name, []).append(str(path.relative_to(system)))
    for line in unreadable:
        missing.setdefault('(an object whose dynamic section could not be read)', []).append(line)
    return dynamic, checked, len(have), missing


# Modules this device asks for that are deliberately not in a release. Each
# one has to be named here, because the alternative -- letting
# PRODUCT_PACKAGES_DEBUG mean "we meant it" -- is what hid the bug this check
# was written for: pw3-boot-report sat in that variable from the first import,
# so the one channel a release's reader can send anything through was in every
# build that already had adb and root and in none of the builds it was for.
# Empty since 2026-10-29: the one entry was pw3-reset-observer, and it went
# with the rest of the debug chain. A module put in PRODUCT_PACKAGES_DEBUG
# from now on has to be justified here, which is the whole point of the check.
DEBUG_ONLY = {}
PACKAGES = re.compile(r'^\s*PRODUCT_PACKAGES(_DEBUG)?\s*\+?=\s*(.*)$')


def wanted_modules(port):
    """(release, debug) module names the device's makefiles ask for.

    Line continuations are followed; a name with a `$` in it is a variable this
    does not expand and is left out rather than guessed at.
    """
    release, debug = set(), set()
    for makefile in sorted((port / 'device/amazon/pw3').rglob('*.mk')):
        lines = makefile.read_text().splitlines()
        i = 0
        while i < len(lines):
            found = PACKAGES.match(lines[i])
            if not found:
                i += 1
                continue
            value = found.group(2)
            while value.endswith('\\') and i + 1 < len(lines):
                i += 1
                value = value[:-1] + ' ' + lines[i].strip()
            names = {n for n in value.split() if n and '$' not in n}
            (debug if found.group(1) else release).update(names)
            i += 1
    if not release:
        # The device's makefiles always ask for modules. An empty set means the
        # makefiles were not found -- a wrong --port, a moved directory -- and
        # the two callers would each print a confident "0 ... 0" and exit 0
        # (Codex 2026-10-31).
        sys.exit('no PRODUCT_PACKAGES line under %s/device/amazon/pw3: the check did not run'
                 % port)
    return release, debug


def modules(product, port):
    """Modules asked for that the image does not have, and debug ones not accounted for.

    The two earlier checks read what the image points at. This reads what the
    image was asked to contain, which is the other way a module goes missing:
    nothing dangles when a module and its .rc leave together, and the image is
    simply quieter than it should be.
    """
    release, debug = wanted_modules(port)
    missing, unexplained = [], []
    # Said first, and without the image: this is the half that would have
    # caught pw3-boot-report on the day the line was written rather than on a
    # device four months later. Nobody could have written a reason for it.
    for name in sorted(debug - release):
        if name not in DEBUG_ONLY:
            unexplained.append(
                '%s is in PRODUCT_PACKAGES_DEBUG, so a release does not get it. If that is '
                'meant, say why in DEBUG_ONLY in this file; if it is not, the module belongs '
                'in PRODUCT_PACKAGES.' % name)
    for name in sorted(debug & release):
        unexplained.append('%s is in both PRODUCT_PACKAGES and PRODUCT_PACKAGES_DEBUG, so what '
                           'a release gets depends on which makefile is read last' % name)
    if product is None:
        return len(release), missing, unexplained
    info_path = product / 'module-info.json'
    if not info_path.is_file():
        return len(release), [], unexplained + ['no module-info.json in %s' % product]
    info = json.loads(info_path.read_text())
    # module-info.json writes every path from the source root
    # ("out/target/product/pw3/system/bin/..."); the product directory given
    # here is that same directory by whatever route, so the tail after it is
    # what to join on. Anchoring on the directory's own name keeps this right
    # when the tree is reached through a different mount.
    tail = re.compile(r'^.*?/product/%s/' % re.escape(product.name))
    for name in sorted(release):
        entry = info.get(name)
        if entry is None:
            missing.append('%s: no such module in module-info.json' % name)
            continue
        installed = [product / tail.sub('', p) for p in entry.get('installed', [])]
        if not installed:
            missing.append('%s: the build installs no file for it' % name)
        elif not any(p.exists() for p in installed):
            missing.append('%s: installs %s, and none of them is in the image'
                           % (name, ', '.join(str(p.name) for p in installed)))
    return len(release), missing, unexplained


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('product', nargs='?', type=Path, default=VM_PRODUCT)
    ap.add_argument('--readelf', type=Path, default=VM_READELF)
    ap.add_argument('--port', type=Path, default=Path(__file__).resolve().parents[1],
                    help='this repository, whose device makefiles say what the image should hold')
    ap.add_argument('--packages-only', action='store_true',
                    help='only the half that reads the makefiles, which needs no built image')
    a = ap.parse_args()
    if a.packages_only:
        asked, _, unexplained = modules(None, a.port)
        print('%d modules asked for by this device, %d debug-only and not accounted for'
              % (asked, len(unexplained)))
        for line in unexplained:
            print('   ', line)
        return 1 if unexplained else 0
    if not (a.product / 'system').is_dir():
        sys.exit('no staged system in %s; build the image first' % a.product)
    if not a.readelf.exists():
        sys.exit('no readelf at %s' % a.readelf)

    files = image_files(a.product)
    checked, missing_programs = programs(a.product, files)
    print('%d programs named in .rc files, %d missing' % (checked, len(missing_programs)))
    for line in missing_programs:
        print('   ', line)
    dynamic, entries, have, missing_libraries = libraries(a.product, a.readelf)
    print('%d dynamic files asking for %d libraries, %d in the loader\'s path, %d missing'
          % (dynamic, entries, have, len(missing_libraries)))
    for name, users in sorted(missing_libraries.items()):
        rest = ' and %d more' % (len(users) - 4) if len(users) > 4 else ''
        print('    %s <- %s%s' % (name, ', '.join(users[:4]), rest))
    asked, missing_modules, unexplained = modules(a.product, a.port)
    # Which build this staged tree is, because the answer above means different
    # things for each: PRODUCT_PACKAGES_DEBUG is added to a userdebug or eng
    # image and not to a release, so a clean result here on a userdebug tree
    # says nothing about what a release carries. That is not hypothetical --
    # the tree this was written against was userdebug and held the very file
    # the V78 release did not.
    variant = 'unknown'
    build_prop = a.product / 'system/build.prop'
    if build_prop.is_file():
        found = re.search(r'^ro\.build\.type=(\S+)', build_prop.read_text(errors='replace'), re.M)
        variant = found.group(1) if found else 'unknown'
    print('%d modules asked for by this device, %d not in the image, %d debug-only and '
          'not accounted for (this tree is %s%s)'
          % (asked, len(missing_modules), len(unexplained), variant,
             '' if variant == 'user' else
             '; a release is the one to read for what a release carries'))
    for line in missing_modules + unexplained:
        print('   ', line)
    return 1 if missing_programs or missing_libraries or missing_modules or unexplained else 0


if __name__ == '__main__':
    raise SystemExit(main())
