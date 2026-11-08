#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build the Android entry for the Kindle library, in the build VM: one shell
script (launcher/launcher-template.sh) carrying a static helper, which carries
the handoff module, the kernel and the public initramfs. Nothing in it belongs
to a device; the helper adds the device's part on the device (devicedata/).

Usage: build_launcher.py --module DIR --kernel ZIMAGE --public-initramfs FILE --cmdline TEXTFILE
                         --preinit-conf FILE [--data PATH:BYTES] --name FILE.sh --title TITLE --output DIR
                         [--profile PROFILE] [--emulator]
                         [--installer DIR --screens DIR --system-image FILE --release-url URL
                          --ca-bundle FILE --zstd-archive FILE]
  --module         output of build_module.py for the same kernel, initramfs and command line
  --preinit-conf   pw3-preinit.conf without the device's fat_* and block= lines
  --data           data image (relative to the user FAT) and its size, for pw3-testdata.conf
  --profile        the model's boot profile (boot/profiles/*.json): which stock firmware the
                   helper checks for, and which model the installer's screens name. Default:
                   the PW3's, which is the only one this project has run on a device
  --emulator       build the helper for the emulator's synthesised probe report (PW3_EMULATOR);
                   the module must be a build_module.py --emulator build too, and this is a
                   test entry only (a release never defines it)
  --installer      installer/build_payload.py output for the same system image: the entry
                   installs it first (installer/install/), from documents/ or downloaded
                   from --release-url/<payload name>; the system image must be
                   android/system.img and the data image android/data.img
  --screens        installer/screens/render_screens.py output for the same payload
  --system-image   the export's system.raw.img, the image that payload unpacks to: the
                   build that made it is read out of it (build_incremental in install.h)
  --ca-bundle      the CA bundle of source-lock.json "installer_ca_bundle", for the download
  --zstd-archive   the zstd release of source-lock.json "installer_zstd"
"""
import argparse, base64, hashlib, json, os, platform, re, shutil, subprocess, sys, tarfile
import urllib.parse
from pathlib import Path

HERE = Path(__file__).resolve().parent
PORT = HERE.parent
# The profile every build used before --profile existed: one model, one firmware,
# and the one this project has run on a device (docs/design/porting-to-other-models.md).
PROFILE = HERE / 'profiles/pw3-5.16.2.1.1.json'
# The four stock files the helper checks by content on every model: the boot the
# stock system came up with and the display every screen goes through. The paths
# are the same everywhere; the sizes and hashes are what the profile's firmware
# ships, and boot/devicedata/devicedata.c carries the same four entries. The
# script's fallback line checks the display's hash before it draws through it,
# which is why this build needs that one hash even though the helper does the
# same check itself: it runs when the helper could not.
STOCK_FILES = ('/sbin/init', '/etc/upstart/init.sh', '/sbin/init.exe', '/usr/sbin/eips')
MUSL = '/home/builder/toolchains/musl-arm/bin/musl-gcc'
READELF = '/home/builder/toolchains/aosp-arm-gcc49/bin/arm-linux-androideabi-readelf'
INITRD_START = 0x84000000   # PHYS_OFFSET + 64 MiB: the module's initramfs destination
ZSTD_FLAGS = ['-DZSTD_LEGACY_SUPPORT=0', '-DZSTD_TRACE=0', '-DZSTD_DISABLE_ASM', '-DDEBUGLEVEL=0']


def sha(data):
    return hashlib.sha256(data).hexdigest()


def c_bytes(data):
    return '{' + ','.join(str(b) for b in data) + '}'


def c_string(text):
    out = ''
    for c in text:
        if c == '\n':
            out += '\\n'
        elif 32 <= ord(c) < 127 and c not in '"\\':
            out += c
        else:
            raise SystemExit('unsupported character in a built-in string: %r' % c)
    return '"' + out + '"'


def c_sha(hex_digest):
    return c_bytes(bytes.fromhex(hex_digest))


RELEASE_URL_SHAPE = 'https://github.com/OWNER/REPO/releases/download/TAG'


# The installer's own ceiling, read out of its header rather than written twice:
# two copies of a number like this drift, and the one that drifts is the one that
# is not compiled.
def max_origins():
    header = (HERE.parent / 'installer/install/install.h').read_text()
    found = re.search(r'^#define INST_MAX_ORIGINS\s+(\d+)', header, re.M)
    if not found:
        sys.exit('installer/install/install.h has no INST_MAX_ORIGINS')
    return int(found.group(1))


def release_asset_url(directory, name):
    """The URL of one release asset, from the directory it sits in and its name.

    Checked by the one shape such a URL has, rather than by the one mistake that
    was made. V65 shipped with the name given twice -- .../v65/NAME/NAME --
    because the full file URL was passed instead of the directory. Nothing
    noticed: the install being tested had the payload on the Kindle drive
    already, so the URL was never fetched, and it would have 404'd for the one
    person who needs it, the one who only downloads.

    The guard written then compared the last path segment to the file name and
    counted the name's occurrences, and both read the URL as undecoded text:
    `.../download/v67/android%2Dsystem%2Dv67.img.zst` does not match the name as
    written, so the name was appended to it a second time, and the tag *page*
    (`.../releases/tag/v67`) passed as well. Both 404, for the whole of this
    URL's audience.

    A wrong *version* in the URL needs no check of its own: the payload is
    admitted by its sha256 (install.h), so the wrong asset is refused rather than
    installed. That the asset is actually there is checked after the upload, by
    fetching it (docs/design/release-build.md); nothing at build time can.

    Two kinds of address are allowed, and the strictness differs because what is
    known about them differs (the owner's decision, 2026-10-30). github.com has
    a shape, and inside that shape are two traps that cost a release each -- the
    asset's own URL, and the tag page -- so an address there is held to the
    shape. Anywhere else there is no shape to hold it to: the entry carries one
    address and the route behind it is the project's own to arrange
    (docs/design/release-build.md, "下载线路"), so what is checked is what is
    true of any such address -- https, no query or fragment, nothing empty or
    `.`/`..` in the path, and the asset named exactly once once the name is
    appended. That last one is the V65 guard, and it is the one that catches an
    asset URL passed where a directory belongs, whatever the host.
    """
    # The asset's name is joined on below, so it has to be a name and not a
    # path: '../outside.img.zst' and 'a/b.img.zst' both left the directory they
    # were given. build_payload.py makes this name from a fixed prefix and a
    # constrained version, so the release pipeline cannot produce either -- but
    # this function must not be the reason that stays true (a Codex review,
    # 2026-10-30).
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]*', name):
        sys.exit('the asset name must be a plain file name: ' + name)
    raw = directory.rstrip('/')
    # Printable ASCII only, before anything is parsed: a trailing newline is
    # dropped by urlsplit and would otherwise pass unnoticed, and nothing in a
    # release address needs a character outside this range.
    if any(not 0x21 <= ord(c) <= 0x7e for c in raw):
        sys.exit('the release URL has a space or a character outside printable ASCII: ' + repr(directory))
    split = urllib.parse.urlsplit(raw)
    # The authority is settled before the host is used for anything, because
    # the classification below turns on it. urlsplit leaves the case, a
    # trailing dot and any percent-encoding exactly as given, so 'GITHUB.COM'
    # and 'github.com.' and '%67ithub.com' are all three not equal to
    # 'github.com' -- and each one let a GitHub *tag page* through the shape
    # check while rejecting the correct shape. Rather than teach the comparison
    # every spelling, the spellings are refused and one is kept: lower case, no
    # userinfo, no port, no percent-encoding, no empty label, no trailing dot,
    # at least two labels (which also turns away localhost and [::1]).
    # The lengths are part of the name being a name: a label is at most 63
    # bytes and the whole of it at most 253 in this form, so a longer one is
    # not a host that can be looked up and an address built on it would fail on
    # every device that tried it (RFC 1035 2.3.4; a Codex review, 2026-10-30,
    # which got an accept out of a 64-byte label).
    host = split.netloc.lower()
    if (split.scheme != 'https' or split.query or split.fragment
            or '@' in split.netloc or ':' in split.netloc or '%' in split.netloc
            or len(host) > 253 or any(len(label) > 63 for label in host.split('.'))
            or not re.fullmatch(r'[a-z0-9]([a-z0-9-]*[a-z0-9])?(\.[a-z0-9]([a-z0-9-]*[a-z0-9])?)+', host)):
        sys.exit('--release-url must be https://HOST/PATH with a plain lower-case host name, '
                 'no user, no port and no percent-encoding. Given: %s' % directory)
    segments = split.path.split('/')
    # No percent-encoding in the path either, for the same reason and one more:
    # a single decode is not enough to know what a server will do with it.
    # '%2e%2e' decodes to '..' and is caught, '%252e%252e' decodes to '%2e%2e'
    # and is not, and chasing that is modelling somebody else's decoder. A
    # release path is a version and a folder name; it has no need of the
    # mechanism, so it does not get it.
    if (len(segments) < 2 or segments[0] != '' or not all(segments[1:])
            or any('%' in s or s in ('.', '..') for s in segments[1:])):
        sys.exit('--release-url must be the directory the asset sits in, with plain path segments '
                 'and no percent-encoding. Given: %s' % directory)
    # The shape cuts both ways. On github.com the address has to have it, which
    # is what rejects the asset's own URL and the tag page. Off github.com,
    # having it is itself the fault: nobody else serves /OWNER/REPO/releases/
    # download/TAG, so an address with that path and another host is a typo in
    # the host -- githubb.com, which this check is here for.
    shaped = len(segments) == 6 and segments[3] == 'releases' and segments[4] == 'download'
    if shaped != (host == 'github.com'):
        shape = RELEASE_URL_SHAPE if shaped or host == 'github.com' else 'https://HOST/PATH'
        sys.exit('--release-url must be %s: the directory the asset sits in, not the asset '
                 'itself and not the tag page. Given: %s' % (shape, directory))
    # The directory must be the directory, not the asset: given the asset's own
    # URL, this would otherwise build .../NAME/NAME. Asked segment by segment,
    # because counting '/' + name inside the path also counts the names that
    # merely start with it -- a folder called NAME-backup, or a GitHub tag
    # called NAME-rc, each a legitimate segment that was being refused (a Codex
    # review, 2026-10-30). The path carries no percent-encoding by the check
    # above, so a segment is what it reads as.
    if name in segments[1:]:
        sys.exit('%s is already a path segment of --release-url, which must be the directory the '
                 'asset sits in: %s' % (name, directory))
    url = 'https://' + host + split.path + '/' + name
    if not re.fullmatch(r'https://[A-Za-z0-9._~/%-]+', url):
        sys.exit('the release URL must be https:// with plain characters: ' + url)
    return url


def stock_identity(profile):
    """The -D that tells the helper which stock firmware it is for.

    `boot/devicedata/devicedata.c` holds one `struct dd_profile` per supported
    model and firmware -- the stock command line, the user store's mount, the two
    Wi-Fi file paths, and the kernel release and version -- and the helper asks
    the running system against it twice: once before anything is downloaded or
    written, and again at the handover. Which struct it reads is a build-time
    choice (`DD_PROFILE`, defaulting in devicedata.h to the PW3's), because one
    helper binary serves one model on one firmware.

    The name comes from the profile JSON, so that the same file that says what
    the E101 screen says also says which facts the device is checked against.
    A profile without the key builds exactly what it built before the key
    existed, which is what keeps the PW3's entry unchanged.
    """
    name = profile.get('device_data_profile')
    if name is None:
        return []
    if not isinstance(name, str) or not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', name):
        sys.exit('device_data_profile is not a C identifier: %r' % (name,))
    header = (HERE / 'devicedata/devicedata.h').read_text()
    if not re.search(r'^extern const struct dd_profile %s;' % re.escape(name), header, re.M):
        sys.exit('device_data_profile names %s, which boot/devicedata/devicedata.h does not '
                 'declare: the profile and the struct have to be written together' % name)
    return ['-DDD_PROFILE=' + name]


def stock_file_hash(profile, path):
    """The sha256 of one stock file, from the profile this build was given.

    The launcher template's fallback line checks /usr/sbin/eips before it draws
    through it, at the one moment the helper could not show a reason itself, so
    the hash in the script has to be this firmware's. It is the same number the
    helper's identity table carries (boot/devicedata/devicedata.c) and the same
    one the profile JSON carries; the test in tools/tests/ holds the last two
    together, and this reads the JSON because the script is written here.

    A virtual profile (module_profile 2) is a QEMU fixture that never reaches a
    Kindle: it has no stock firmware, so its script keeps the PW3's hash, which
    is what every launcher carried before --profile existed. The fallback line
    cannot draw on such a machine in any case.
    """
    files = profile.get('stock_files')
    if files is None:
        files = json.loads(PROFILE.read_text()).get('stock_files')
    if not isinstance(files, list) or not files:
        sys.exit('%s carries no stock_files: the display hash the launcher checks cannot '
                 'be written, and a build made without it would draw screens through a '
                 'program it has not identified' % profile.get('description', 'the profile'))
    by_path = {}
    for entry in files:
        if not isinstance(entry, dict) or not isinstance(entry.get('path'), str):
            sys.exit('stock_files entries must be objects with a path: %r' % (entry,))
        by_path[entry['path']] = entry
    missing = [name for name in STOCK_FILES if name not in by_path]
    if missing:
        sys.exit('the profile does not carry %s, which the helper checks before it runs '
                 'anything: a build for it cannot start a device' % ', '.join(missing))
    value = by_path[path].get('sha256')
    if not isinstance(value, str) or not re.fullmatch(r'[0-9a-f]{64}', value):
        sys.exit('stock_files[%r].sha256 is not a sha256: %r' % (path, value))
    return value


def build_incremental(image, expect_sha256):
    """ro.build.version.incremental of that system image, read out of the image.

    Out of the image and not out of a receipt beside it: the installer compares
    this against the line every report carries, and what has to match is the
    system the entry actually installs. The image is the payload's own, by sha256,
    before a byte of it is believed.

    The value has to survive the sampler, which writes properties through
    Clean(value, N) -- printable ASCII, cut at N (BootReport.cpp). A value that
    would come back cut or rewritten could never be matched, so the build stops
    here instead of shipping an entry whose compiling notice can never clear.
    """
    got = hashlib.sha256()
    with open(image, 'rb') as f:
        for block in iter(lambda: f.read(1 << 22), b''):
            got.update(block)
    if got.hexdigest() != expect_sha256:
        sys.exit('--system-image is not the image the payload unpacks to: ' + str(image))
    out = subprocess.run(['debugfs', '-R', 'cat build.prop', str(image)],
                         stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    if out.returncode:
        sys.exit('cannot read build.prop out of ' + str(image))
    # Parsed the way the device parses it, not the way the file looks: init's
    # property loader strips leading whitespace from the key, and a `ro.` property
    # keeps the value of its *first* assignment (property_service.cpp). A file
    # with " ro.build…=OLD" above "ro.build…=NEW" therefore runs as OLD while a
    # plain ^-anchored search here reads NEW -- and an entry built on NEW could
    # never match a report from that system. Both forms are collected, the first
    # wins, and anything that disagrees stops the build rather than shipping a
    # notice that can never clear.
    # Every ASCII space init skips, not the two that look like whitespace in an
    # editor: load_properties() walks the line with isspace(), so a vertical tab
    # in front of a key is skipped there and would be a line this search did not
    # see -- and the first assignment of a ro. property is the one that stands.
    found = re.findall(r'^[ \t\v\f\r]*ro\.build\.version\.incremental[ \t\v\f\r]*=(.*)$', out.stdout, re.M)
    if not found:
        sys.exit('build.prop carries no ro.build.version.incremental: ' + str(image))
    values = {v.strip() for v in found}
    if len(values) != 1:
        sys.exit('build.prop assigns ro.build.version.incremental more than once, with different values: %s' % sorted(values))
    value = found[0].strip()
    sampler = (PORT / 'device/amazon/pw3/initcheck/BootReport.cpp').read_text()
    limit = int(re.search(r'Clean\(android::base::GetProperty\(name, "unset"\), (\d+)\)', sampler).group(1))
    if not re.fullmatch(r'[\x21-\x7e]{1,%d}' % limit, value):
        sys.exit('ro.build.version.incremental is not a value the sampler reports unchanged: ' + repr(value))
    return value


def installer_inputs(a, conf, data_path, data_size):
    """The release the entry installs: checked against the entry, as C and blobs."""
    if not (a.screens and a.release_url and a.ca_bundle and a.zstd_archive and a.system_image):
        sys.exit('--installer needs --screens, --system-image, --release-url, --ca-bundle and --zstd-archive')
    lock = json.loads((PORT / 'source-lock.json').read_text())
    manifest = json.loads((a.installer / 'manifest.json').read_text())
    if manifest['schema'] != 4:
        sys.exit('the payload manifest is not schema 4: ' + str(a.installer))
    system, data = manifest['files']['system'], manifest['files']['data']
    values = dict(line.split('=', 1) for line in conf.splitlines())
    if (values['system_path'] != 'android/system.img' or int(values['system_size']) != system['unpacked_bytes'] or
            values['system_sha256'] != system['unpacked_sha256']):
        sys.exit('the payload is not this entry\'s system image at android/system.img')
    if data_path != 'android/data.img' or data_size != data['unpacked_bytes']:
        sys.exit('the payload\'s data image is not this entry\'s android/data.img of %d bytes' % data_size)
    template = (a.installer / data['name']).read_bytes()
    if len(template) != data['bytes'] or sha(template) != data['sha256']:
        sys.exit('the data template is not the manifest\'s')
    ca = a.ca_bundle.read_bytes()
    if sha(ca) != lock['installer_ca_bundle']['sha256']:
        sys.exit('the CA bundle is not the one in source-lock.json')
    if sha(a.zstd_archive.read_bytes()) != lock['installer_zstd']['archive_sha256']:
        sys.exit('the zstd archive is not the one in source-lock.json')
    screens = json.loads((a.screens / 'build-result.json').read_text())
    screens_bin = (a.screens / 'screens.bin').read_bytes()
    if screens['payload_mb'] != round(system['bytes'] / 1e6) or sha(screens_bin) != screens['screens_bin']['sha256']:
        sys.exit('the screens were rendered for another payload')
    # E101 names the model and firmware this build is for, and a screen that
    # named another one would send someone looking for the wrong thing. The
    # names come from the profile both sides read (boot/profiles/).
    profile = json.loads(a.profile.read_text())
    if screens.get('supported') != [profile['supported_zh'], profile['supported_en']]:
        sys.exit('the screens were rendered for another model: ' + str(screens.get('supported')))
    # --release-url is the directory the asset sits in; the payload's own name is
    # appended here. V65 shipped with the name given twice -- .../v65/NAME/NAME --
    # because the full file URL was passed instead. Nothing noticed: the install
    # being tested had the payload on the Kindle drive already, so the URL was
    # never fetched, and it would have 404'd for the one person who needs it, the
    # one who only downloads. A wrong *version* in the URL needs no check of its
    # own -- the payload is admitted by its sha256 (install.h), so the wrong asset
    # is refused rather than installed.
    # One per origin, in the order the installer will try them. The validator is
    # per URL, so a second origin is held to exactly the same shape as the first;
    # duplicates are refused because an origin listed twice costs an attempt and
    # buys nothing.
    urls = [release_asset_url(u, system['name']) for u in a.release_url]
    if len(set(urls)) != len(urls):
        sys.exit('--release-url is given twice with the same address')
    if len(urls) > max_origins():
        sys.exit('--release-url is given %d times; the installer carries at most %d'
                 % (len(urls), max_origins()))
    incremental = build_incremental(a.system_image, system['unpacked_sha256'])
    release = ['#include "install.h"', '#include "payload.h"', 'static const struct inst_release pw3_release = {',
               '    .version = %s, .built = %dLL, .data_format = %du,' % (c_string(manifest['version']), manifest['built'],
                                                                      manifest['data_format']),
               '    .payload_name = %s,' % c_string(system['name']),
               '    .payload_urls = {%s}, .payload_origins = %d,'
               % (', '.join(c_string(u) for u in urls), len(urls)),
               '    .payload = {%dull, %s},' % (system['bytes'], c_sha(system['sha256'])),
               '    .system = {%dull, %s},' % (system['unpacked_bytes'], c_sha(system['unpacked_sha256'])),
               '    .template_data = pw3_data_template, .template_file = {%dull, %s},' % (data['bytes'], c_sha(data['sha256'])),
               '    .data = {%dull, %s},' % (data['unpacked_bytes'], c_sha(data['unpacked_sha256'])),
               '    .boot_image = %s,' % c_sha(manifest['boot_image_sha256']),
               '    .build_incremental = %s,' % c_string(incremental),
               '    .ca_bundle = pw3_ca_bundle, .ca_bundle_bytes = %du,' % len(ca), '};']
    receipt = {'version': manifest['version'], 'built_utc': manifest['built_utc'], 'data_format': manifest['data_format'],
               'payload_urls': urls, 'payload': {k: system[k] for k in ('name', 'bytes', 'sha256')},
               'data_template_sha256': data['sha256'], 'boot_image_sha256': manifest['boot_image_sha256'],
               'build_incremental': incremental,
               'screens_sha256': screens['screens_bin']['sha256'],
               'ca_bundle': lock['installer_ca_bundle']['version'],
               'zstd': lock['installer_zstd']['version']}
    return {'pw3_data_template': template, 'pw3_ca_bundle': ca, 'pw3_screens': screens_bin}, '\n'.join(release) + '\n', receipt


def zstd_objects(archive, out):
    """zstd's decompression library, built for the device as in installer/build_unpack.py."""
    lock = json.loads((PORT / 'source-lock.json').read_text())['installer_zstd']
    with tarfile.open(archive) as tar:
        tar.extractall(out / 'zstd', filter='data')
    lib = out / 'zstd' / ('zstd-' + lock['version']) / 'lib'
    objects = []
    for c in sorted(list((lib / 'common').glob('*.c')) + list((lib / 'decompress').glob('*.c'))):
        o = out / 'zstd' / (c.parent.name + '-' + c.stem + '.o')
        subprocess.run([MUSL, '-std=gnu11', '-O2', '-c', *ZSTD_FLAGS, '-I', str(lib), '-I', str(lib / 'common'), str(c),
                        '-o', str(o)], check=True)
        objects.append(str(o))
    return lib, objects


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ['module', 'kernel', 'public-initramfs', 'cmdline', 'preinit-conf', 'output']:
        ap.add_argument('--' + name, type=Path, required=True)
    ap.add_argument('--data')
    ap.add_argument('--profile', type=Path, default=PROFILE,
                    help='the model\'s boot profile (boot/profiles/*.json); default: the PW3\'s')
    ap.add_argument('--emulator', action='store_true',
                    help='build the helper for the emulator (PW3_EMULATOR): its probe checks the '
                         'module\'s synthesised report instead of hardware the emulator does not '
                         'have. The module must have been built with build_module.py --emulator; '
                         'test entries only')
    for name in ['installer', 'screens', 'system-image', 'ca-bundle', 'zstd-archive']:
        ap.add_argument('--' + name, type=Path)
    ap.add_argument('--release-url', action='append', default=[],
                    help='the directory the payload sits in; repeat it for a second origin')
    ap.add_argument('--name', required=True)
    ap.add_argument('--title', required=True)
    a = ap.parse_args()
    if platform.node() != 'lima-pw3' or os.geteuid() == 0:
        sys.exit('Run as the builder in the build VM.')
    if not re.fullmatch(r'[A-Za-z0-9._-]+\.sh', a.name) or not re.fullmatch(r'[A-Za-z0-9 ._-]+', a.title):
        sys.exit('name: ASCII file name ending in .sh; title: ASCII letters, digits, spaces')
    profile = json.loads(a.profile.read_text())
    receipt = json.loads((a.module / 'build-result.json').read_text())
    module = (a.module / 'pw3_first_kernel.ko').read_bytes()
    kernel, public = a.kernel.read_bytes(), a.public_initramfs.read_bytes()
    cmdline = a.cmdline.read_text().rstrip('\n')
    conf = a.preinit_conf.read_text()
    # Profile 1 is the Kindle's stock kernel; profile 2 is the QEMU one, which
    # launcher/helper.c has its own branches for (CHECK_PROFILE == 2: the kernel
    # it refuses to run under, and the /test-fault-stage checkpoint the client
    # test drives). Those branches had no builder: this refused anything but
    # profile 1, so boot/handoff/tests/run_client_test.py -- the only test that
    # runs this helper and the launcher script on ARM, under the stock shell,
    # against a FAT disk -- could not be given anything to run.
    if receipt['module_profile'] not in (1, 2):
        sys.exit('unknown module profile: %r' % receipt['module_profile'])
    # The module and the helper have to agree about the probe: one built for the
    # emulator hands over synthesised hardware values and the other, built for a
    # device, would judge them against hardware it never sampled (or the reverse,
    # which fails the emulator's probe). Two half-finished test entries are worse
    # than one that refuses to build.
    if bool(receipt.get('emulator', False)) != a.emulator:
        sys.exit('the module was built %s --emulator and this launcher %s: the probe '
                 'report and the checks on it have to be the same kind of build'
                 % ('with' if receipt.get('emulator') else 'without',
                    'with' if a.emulator else 'without'))
    virtual = receipt['module_profile'] == 2
    if virtual and a.installer:
        sys.exit('a release entry installs a system image, which is the stock profile')
    if a.emulator and a.installer:
        sys.exit('--emulator is for a test entry, not a release: a release never defines '
                 'PW3_EMULATOR, and an installer-shaped emulator build would have no '
                 'device to install to')
    if (sha(module) != receipt['modules_sha256']['pw3_first_kernel'] or sha(kernel) != receipt['kernel']['sha256'] or
            sha(public) != receipt['public_initramfs']['sha256'] or cmdline != receipt['cmdline']):
        sys.exit('the module was built for another kernel, initramfs or command line')
    if re.search(r'^(fat_offset|fat_sizelimit|block)=', conf, re.M) or not conf.endswith('\n'):
        sys.exit('the public pw3-preinit.conf must not carry device lines')
    data_path, data_size = None, 0
    if a.data:
        data_path, size = a.data.rsplit(':', 1)
        data_size = int(size)
        # The same rules the entry applies when it reads pw3-testdata.conf on the
        # device (preinit/TestFileFormat.h: pw3_test_data_path and pw3_test_size).
        # Looser ones here build an entry that rejects its own images at boot and
        # then waits for ever, which only the device would tell us.
        if not re.fullmatch(r'[a-z0-9][a-z0-9-]{0,47}/data\.img', data_path):
            sys.exit('invalid --data: the path must be <directory>/data.img')
        if not 512 << 20 <= data_size <= 3072 << 20 or data_size % 4096:
            sys.exit('invalid --data: the size must be 512 MiB to 3 GiB and a whole number of 4 KiB blocks')
    installer_blobs, release_h, installer_receipt = installer_inputs(a, conf, data_path, data_size) if a.installer else ({}, None, None)
    os.umask(0o022)
    out = a.output
    out.mkdir(parents=True, exist_ok=False)
    blobs = {'module_payload': module, 'kernel_payload': kernel, 'public_initramfs_payload': public, **installer_blobs}
    header = ['#include <stdint.h>', '#define MODULE_BYTES %du' % len(module), '#define KERNEL_BYTES %du' % len(kernel),
              '#define PUBLIC_INITRAMFS_BYTES %du' % len(public), '#define INITRD_START %#xu' % INITRD_START]
    asm = '.section .rodata.pw3_inputs,"a",%progbits\n'
    for name, data in blobs.items():
        (out / (name + '.bin')).write_bytes(data)
        header.append('extern const unsigned char %s[%d];' % (name, len(data)))
        asm += '.balign 4\n.global %s\n.type %s, %%object\n%s:\n.incbin "%s"\n.size %s, .-%s\n' % (
            name, name, name, out / (name + '.bin'), name, name)
    asm += '.section .note.GNU-stack,"",%progbits\n'
    header += ['static const unsigned char module_sha256[32]=%s;' % c_bytes(bytes.fromhex(sha(module))),
               'static const unsigned char kernel_sha256[32]=%s;' % c_bytes(bytes.fromhex(sha(kernel))),
               'static const unsigned char public_initramfs_sha256[32]=%s;' % c_bytes(bytes.fromhex(sha(public))),
               '#define BUILD_CMDLINE %s' % c_string(cmdline), '#define BUILD_PREINIT_CONF %s' % c_string(conf),
               '#define BUILD_DATA_PATH %s' % (c_string(data_path) if data_path else 'NULL'),
               '#define BUILD_DATA_SIZE %du' % data_size]
    (out / 'payload.h').write_text('#pragma once\n' + '\n'.join(header) + '\n')
    (out / 'payload.S').write_text(asm)
    helper = out / 'helper'
    # The virtual profile says what it is doing in its own words ("FIRST KERNEL
    # CHECK", "BOOT ATTEMPT") rather than Android's, so it is the one build that
    # does not ask for PW3_ANDROID_FIRST_BOOT.
    command = [MUSL, '-std=gnu11', '-static', '-O2', '-Wall', '-Wextra', '-Werror',
               '-DCHECK_PROFILE=%d' % receipt['module_profile']] + \
              ([] if virtual else ['-DPW3_ANDROID_FIRST_BOOT']) + \
              stock_identity(profile) + (['-DPW3_EMULATOR'] if a.emulator else []) + [
               '-I', str(out), '-I', str(HERE / 'launcher'), '-I', str(HERE / 'handoff'), '-I', str(HERE / 'devicedata'),
               '-I', str(PORT / 'device/amazon/pw3/report'),
               str(HERE / 'launcher/helper.c'), str(HERE / 'launcher/userlog.c'),
               str(HERE / 'handoff/sha256.c'), str(HERE / 'devicedata/devicedata.c'),
               str(out / 'payload.S'), '-o', str(helper)]
    if release_h:
        (out / 'release.h').write_text(release_h)
        lib, objects = zstd_objects(a.zstd_archive, out)
        command[-2:-2] = [str(PORT / 'installer/install/install.c'), str(PORT / 'installer/install/kindle.c'),
                          str(PORT / 'installer/install/screen.c'), str(PORT / 'installer/install/fatcheck.c'),
                          str(PORT / 'installer/unpack/unpack.c'), *objects]
        # preinit's headers for the cross-link check the installer shares with it
        # (installer/install/fatcheck.h says what is shared and what is not).
        command[1:1] = ['-DPW3_INSTALLER', '-I', str(PORT / 'installer/install'), '-I', str(PORT / 'installer/unpack'),
                        '-I', str(PORT / 'device/amazon/pw3/preinit'),
                        '-I', str(a.screens), '-I', str(lib)]
    # Each input is compiled to a named object and the objects are linked,
    # rather than handing the compiler every source at once. The one reason is
    # that the published launcher has to be checkable against the published
    # sources: compiling and linking in one step lets the compiler name its
    # intermediate objects ccXXXXXX.o, and one of those random names lands in
    # the symbol table as a STT_FILE entry (one of 226). Two builds of the same
    # sources in the same directory differed by exactly those six bytes, so
    # nobody could rebuild the Readom.sh they downloaded and compare it. The
    # flags are the ones above, unchanged and the same for every input, which
    # is what the compiler was already doing with them; only the intermediate
    # names change, and the symbols are kept rather than stripped because an
    # address out of a device's report is read against them.
    # Split by extension, not by position: the command is assembled above with
    # two insertions, so a rule like "the flags come first" would be a guess
    # about that assembly. command[1:-2] drops the compiler and the trailing
    # -o: the remainder is every flag and every input.
    middle = command[1:-2]
    if command[-2] != '-o' or command[-1] != str(helper):
        sys.exit('the helper command no longer ends in -o <helper>')
    inputs = [arg for arg in middle if arg.endswith(('.c', '.S'))]
    prebuilt = [arg for arg in middle if arg.endswith('.o')]
    flags = [arg for arg in middle if not arg.endswith(('.c', '.S', '.o'))]
    if len(inputs) + len(prebuilt) + len(flags) != len(middle) or not inputs:
        sys.exit('the helper command has an argument this does not classify')
    objdir = out / 'obj'
    objdir.mkdir(exist_ok=True)
    objects = []
    for source in inputs:
        obj = objdir / (Path(source).name + '.o')
        subprocess.run([MUSL, *flags, '-c', source, '-o', str(obj)], check=True)
        objects.append(str(obj))
    subprocess.run([MUSL, *flags, *objects, *prebuilt, '-o', str(helper)], check=True)
    random_names = [line.split()[-1] for line in
                    subprocess.check_output([READELF, '-s', str(helper)], text=True).splitlines()
                    if ' FILE ' in line]
    stray = [n for n in random_names if re.fullmatch(r'cc[A-Za-z0-9]{6}\.o', n)]
    if stray:
        sys.exit('the helper still carries a compiler temporary name, so it '
                 'cannot be rebuilt and compared: ' + ', '.join(stray))
    elf = subprocess.check_output([READELF, '-h', '-lW', str(helper)], text=True)
    if 'INTERP' in elf or 'ARM' not in elf:
        sys.exit('the helper is not a static ARM executable')
    template = (HERE / 'launcher/launcher-template.sh').read_text()
    # The display's hash for the script's fallback line, from the profile's own
    # identity table: the helper checks the same bytes before it draws anything,
    # and this line runs when the helper could not -- so it has to be this
    # firmware's program, not a number copied into the template.
    eips_sha256 = stock_file_hash(profile, '/usr/sbin/eips')
    if template.count('@EIPS_HASH@') != 1:
        sys.exit('launcher-template.sh no longer carries @EIPS_HASH@ exactly once, so '
                 'the fallback line would check the display against a stale hash')
    encoded = base64.b64encode(helper.read_bytes()).decode()
    encoded = '\n'.join(encoded[i:i + 76] for i in range(0, len(encoded), 76))
    # The same two strings the helper was compiled with (PW3_OUTPUT_PREFIX in
    # launcher/helper.c). A release writes one folder per tap inside the folder
    # the installer owns; the virtual-profile build, which only
    # boot/handoff/tests/run_client_test.py makes, writes to the top of the user
    # store, where nothing collects it -- that build never reaches a device.
    prefix = '/mnt/us/android/logs/' if release_h else '/mnt/us/pw3-first-kernel-'
    # The last thing that can be said when the helper stopped before it could
    # show a screen of its own: `eips` in text mode, so ASCII and one line. A
    # release reaches it when the store has no room for this run's folder, which
    # is also the only thing the reader can do something about; the other line is
    # for whoever is holding the cable in the virtual-profile test.
    stopped = ('ANDROID DID NOT START. FREE SOME SPACE ON THE KINDLE DRIVE AND TAP AGAIN.'
               if release_h else 'BOOT TEST STOPPED. RECONNECT USB.')
    # These two are the only substitutions keyed on the template's own wording
    # rather than on an @PLACEHOLDER@, so a reworded template makes
    # str.replace() a silent no-op instead of an error. The name is the one that
    # matters: the entry finds itself by the path in SELF, so an entry shipped
    # under a different file name fails at `[ -f "$SELF" ]` on every tap, and
    # the trap's only line is the out-of-space message -- the reader would be
    # told to free space for a reason that has nothing to do with space.
    # `sh -n` reads it as valid, and the placeholder checks below never see it.
    title_from, self_from = '# Name: PW3 First Kernel', 'SELF=/mnt/us/documents/PW3-First-Kernel.sh'
    for literal in (title_from, self_from):
        if template.count(literal) != 1:
            sys.exit('launcher-template.sh no longer carries this line exactly once, so '
                     'substituting it would do nothing: ' + literal)
    script = (template.replace(title_from, '# Name: ' + a.title)
              .replace(self_from, 'SELF=/mnt/us/documents/' + a.name)
              .replace('@STOPPED_MESSAGE@', stopped)
              .replace('@LOGPREFIX@', prefix).replace('@LOGPARENT@', prefix.rsplit('/', 1)[0])
              .replace('@EIPS_HASH@', eips_sha256)
              .replace('@HELPER_HASH@', sha(helper.read_bytes())).replace('@BASE64@', encoded))
    head = script.split('\n__PW3_FIRST_KERNEL_HELPER_BASE64__\n', 1)[0]
    if '@LOG' in head:
        sys.exit('the launcher script still has a log placeholder in it')
    if '@STOPPED_MESSAGE@' in head:
        sys.exit('the launcher script still has the stopped-message placeholder in it')
    if '@EIPS_HASH@' in head:
        sys.exit('the launcher script still has the display-hash placeholder in it')
    if ('\nSELF=/mnt/us/documents/' + a.name + '\n') not in head:
        sys.exit('the launcher script does not point at the name it is written under: ' + a.name)
    assert base64.b64decode(script.split('\n__PW3_FIRST_KERNEL_HELPER_BASE64__\n', 1)[1]) == helper.read_bytes()
    launcher = out / a.name
    launcher.write_text(script)
    launcher.chmod(0o755)
    subprocess.run(['/bin/sh', '-n', str(launcher)], check=True)
    for name in blobs:
        (out / (name + '.bin')).unlink()
    if release_h:
        shutil.rmtree(out / 'zstd')
    result = {'schema': 1, 'launcher': a.name, 'launcher_bytes': launcher.stat().st_size, 'launcher_sha256': sha(launcher.read_bytes()),
              'helper_sha256': sha(helper.read_bytes()), 'module_receipt_sha256': sha((a.module / 'build-result.json').read_bytes()),
              'kernel_sha256': sha(kernel), 'public_initramfs_sha256': sha(public), 'cmdline': cmdline,
              'profile': a.profile.name, 'device_data_profile': profile.get('device_data_profile'),
              'emulator': a.emulator,
              'data_path': data_path, 'data_size': data_size, 'installer': installer_receipt, 'contains_device_data': False}
    (out / 'build-result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=1))


if __name__ == '__main__':
    main()
