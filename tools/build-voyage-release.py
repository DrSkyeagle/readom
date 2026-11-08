#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""The Voyage bootstrap images: the released v0.1.0 system image, re-identified, plus an empty data image.

This is the no-AOSP half of the port's release (readom-port/bootstrap-build-report.md):
take `release/system.img` (the shipped PW3 image), replace its kernel modules with the
Voyage kernel's, rewrite the six identity properties in `/build.prop`, and make an empty
`data` image -- then pack both the way a release is packed (zstd -19, window <= 8 MiB) and
write the manifest fields schema 4 needs. It does NOT build the entry (`Readom.sh`), render
screens, or install anything; those are later phases.

Why the released image can be reused at all: a Voyage system image differs from the PW3
one in exactly six identity properties and the kernel modules (docs/design/
release-pipeline-report.md section 5.3). Everything else -- density, rc files, SELinux,
fonts, applications -- is inherited on purpose, and the kernel binds modules by vermagic
only (CONFIG_MODVERSIONS is off in both kernels). So the bootstrap is: same filesystem,
nine PW3 modules out, the Voyage kernel's modules in, `build.prop` rewritten.

The image is edited with debugfs, as `builder`, not by mounting: debugfs needs no root and
edits the file in place. The two things it would be easy to lose are set explicitly for
every file it writes -- the mode (build.prop is 0600, modules 0644) and the SELinux xattr
(`u:object_r:system_file:s0` / `u:object_r:vendor_file:s0`, with the trailing NUL the
kernel expects), because a file that loses either is a system Android may refuse to read.

What is deliberately left alone: product.board / board.platform / hardware.* stay `pw3`
(the hardware implementation names are the same code -- device/amazon/voyage/voyage.mk
says why), and the ART boot image is not touched, so `boot_image_sha256` stays the released
value.

Usage: build-voyage-release.py --system release/system.img --modules out-artifacts/modules.tar.gz \\
        --data-template release/pw3_data_template.bin --output readom-port/build-voyage [options]

Run as builder in the build VM (hostname lima-pw3), like every other build script in this
tree. Requires debugfs/e2fsck/dumpe2fs (e2fsprogs), zstd and mke2fs.
"""
import argparse
import datetime
import hashlib
import json
import os
import platform
import re
import shlex
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

# The released v0.1.0 set these default expectations are taken from
# (readom-port/release-pipeline-report.md, appendix; installer/build_payload.py builds the
# same manifest fields for the PW3 release).
RELEASE_SYSTEM_BYTES = 536870912
RELEASE_SYSTEM_SHA256 = '968ca8e2b358107a17757770f80cc4291809d9bda0657753b6b5c3e5de74371a'
RELEASE_DATA_BYTES = 1879048192
RELEASE_DATA_SHA256 = '414954ac6c405972fb6f6549cbeac94f596ad3785e2b62a26d3f47e1bbef065e'
RELEASE_DATA_TEMPLATE_SHA256 = 'ed2c9c44393c9ac7768854567e6729c5d3d46939f9460e2c1008bc9c98fb2b2c'
# The export's hash of /system/framework/arm/boot*: reused, not recomputed. This image does
# not touch the ART boot image, so the value from the release receipt still describes it.
RELEASE_BOOT_IMAGE_SHA256 = '1cdfe692c99f8ce09b21668a71f2d97e9d70404829015b5e8710494b3775afbb'

DATA_BLOCKS = 458752                  # 458752 * 4096 = 1,879,048,192, BOARD_USERDATAIMAGE_PARTITION_SIZE
DATA_BLOCK_SIZE = 4096
VERMAGIC = '3.0.35-lab126-voyage-android81'
MODULES_DIR = '/vendor/lib/modules'
MODULE_SELINUX = 'u:object_r:vendor_file:s0'
BUILD_PROP_SELINUX = 'u:object_r:system_file:s0'
ZSTD_LEVEL = 19
ZSTD_WINDOW_MAX = 8 << 20             # what pw3-unpack accepts (installer/unpack)

# build.prop properties that make this image a Voyage one, and the values they get. The
# rest of build.prop is kept byte for byte; in particular ro.product.board, ro.board.platform
# and ro.hardware.* stay pw3, which is the port's design (voyage.mk).
IDENTITY = {
    'ro.build.display.id': 'Readom Voyage v0.1.0',
    'ro.build.flavor': 'voyage-user',
    'ro.product.model': 'Kindle Voyage',
    'ro.product.brand': 'Readom',
    'ro.product.name': 'voyage',
    'ro.product.device': 'voyage',
    'ro.build.product': 'voyage',
    'ro.vendor.product.model': 'Kindle Voyage',
    'ro.vendor.product.brand': 'Readom',
    'ro.vendor.product.name': 'voyage',
    'ro.vendor.product.device': 'voyage',
}
PLATFORM_VERSION = '8.1.0'
BUILD_ID = 'OC'


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def fingerprint_id(incremental):
    """The fingerprint's build-version segment, as AOSP's buildinfo.sh derives it.

    `eng.builde.20261104.042050` is the same instant as `builde11040420` (the released
    image's fingerprint has exactly this pair, diagnostics.md line 98): user, then month
    and day, then hour and minute.
    """
    match = re.fullmatch(r'(?:eng\.)?([A-Za-z0-9_]+)\.(\d{8})\.(\d{6})', incremental)
    if not match:
        return re.sub(r'[^A-Za-z0-9]', '', incremental)
    user, date, clock = match.group(1), match.group(2), match.group(3)
    return user + date[4:8] + clock[:4]


def zstd_window(path):
    listing = subprocess.run(['zstd', '-lv', str(path)], text=True,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if listing.returncode:
        sys.exit('zstd -lv %s failed:\n%s' % (path, listing.stdout))
    match = re.search(r'Window Size: .*\((\d+) B\)', listing.stdout)
    if not match:
        sys.exit('no window size in zstd -lv output for %s' % path)
    return int(match.group(1))


def stream_sha256(argv):
    """SHA-256 and byte count of a child's stdout (zstd -dc), streamed."""
    process = subprocess.Popen([str(a) for a in argv], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    digest = hashlib.sha256()
    total = 0
    assert process.stdout is not None
    for block in iter(lambda: process.stdout.read(1 << 20), b''):
        digest.update(block)
        total += len(block)
    error = process.stderr.read() if process.stderr else b''
    if process.wait():
        sys.exit('command failed: %s\n%s' % (' '.join(str(a) for a in argv),
                                             error.decode(errors='replace')))
    return digest.hexdigest(), total


class Builder:
    def __init__(self):
        self.transcript = []

    def run(self, argv, cwd=None, quiet=False):
        """Run a command; raise on failure. stdout/stderr are captured separately.

        debugfs prints its version banner on stderr and command output on stdout, so the
        two cannot be merged when a command's stdout is the wanted data.
        """
        line = ' '.join(shlex.quote(str(a)) for a in argv)
        result = subprocess.run([str(a) for a in argv], cwd=str(cwd) if cwd else None,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        out = result.stdout.decode(errors='replace')
        err = result.stderr.decode(errors='replace')
        self.transcript.append('$ %s\n%s%s' % (line, out, err))
        if not quiet:
            print('+ ' + line)
            for text in (out, err):
                if text.strip():
                    print(text.rstrip('\n'))
        if result.returncode:
            sys.exit('command failed with exit code %d:\n  %s\n%s%s'
                     % (result.returncode, line, out, err))
        return out

    def debugfs(self, image, request, writable=False):
        argv = ['debugfs'] + (['-w'] if writable else []) + ['-R', request, image]
        return self.run(argv, quiet=True)

    def debugfs_script(self, image, commands, path):
        path.write_text('\n'.join(commands) + '\n')
        self.transcript.append('$ debugfs -w -f %s %s\n%s' % (path, image, '\n'.join(commands)))
        result = subprocess.run(['debugfs', '-w', '-f', str(path), str(image)],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        out = (result.stdout + result.stderr).decode(errors='replace')
        self.transcript.append('$ debugfs -w -f %s %s\n%s' % (path, image, out))
        if result.returncode:
            sys.exit('debugfs failed with exit code %d on %s:\n%s' % (result.returncode, image, out))
        # debugfs continues past most per-command errors and still exits 0; the caller
        # verifies the result by reading the image back, which is the only proof that counts.
        return out

    def read_dir(self, image, path):
        out = self.debugfs(image, 'ls -p %s' % path)
        entries = []
        for line in out.splitlines():
            if not line.startswith('/'):
                continue
            parts = line.split('/')
            if len(parts) < 7:
                continue
            name = parts[5]
            if name in ('.', '..'):
                continue
            entries.append({'name': name, 'mode': parts[2], 'uid': parts[3], 'gid': parts[4],
                            'bytes': int(parts[6]) if parts[6].isdigit() else None})
        return entries

    def cat(self, image, path):
        return self.debugfs(image, 'cat %s' % path).encode()

    def dump(self, image, path, destination):
        self.debugfs(image, 'dump %s %s' % (path, destination))
        if not destination.is_file():
            sys.exit('debugfs dump of %s produced no file' % path)
        return destination

    def write_script(self):
        return self.transcript


def data_image_facts(path):
    """Read the ext4 superblock the way preinit/StorageFormat.h judges it."""
    with open(path, 'rb') as stream:
        head = stream.read(2048)
    if len(head) < 2048:
        sys.exit('%s is shorter than a superblock' % path)
    superblock = head[1024:2048]

    def u16(offset):
        return struct.unpack_from('<H', superblock, offset)[0]

    def u32(offset):
        return struct.unpack_from('<I', superblock, offset)[0]

    raw_uuid = superblock[0x68:0x78]
    facts = {
        'magic': u16(0x38),
        'block_log': u32(0x18),
        'inodes': u32(0x00),
        'free_inodes': u32(0x10),
        'blocks': u32(0x04),
        'state': u16(0x3a),
        'last_orphan': u32(0xe8),
        'incompat': u32(0x60),
        'ro_compat': u32(0x64),
        'label': superblock[0x78:0x88].split(b'\0')[0].decode(errors='replace'),
        'uuid': '%s-%s-%s-%s-%s' % (raw_uuid[0:4].hex(), raw_uuid[4:6].hex(),
                                    raw_uuid[6:8].hex(), raw_uuid[8:10].hex(),
                                    raw_uuid[10:16].hex()),
    }
    facts['block_size'] = 1024 << facts['block_log']
    facts['blocks'] = facts['blocks'] | (u32(0x150) << 32) if (facts['incompat'] & 0x80) else facts['blocks']
    facts['file_size'] = facts['blocks'] * facts['block_size']
    facts['used_inodes'] = facts['inodes'] - facts['free_inodes']
    problems = []
    if facts['magic'] != 0xef53:
        problems.append('not ext4 (magic 0x%x)' % facts['magic'])
    if facts['state'] != 1:
        problems.append('state is %d, not 1 (clean)' % facts['state'])
    if facts['last_orphan'] != 0:
        problems.append('orphan list is not empty (%d)' % facts['last_orphan'])
    if facts['incompat'] & ~0x2d6:
        problems.append('incompat 0x%x has bits outside 0x2d6' % facts['incompat'])
    if facts['ro_compat'] & ~0x7f:
        problems.append('ro_compat 0x%x has bits outside 0x7f' % facts['ro_compat'])
    if facts['file_size'] != path.stat().st_size:
        problems.append('blocks*block_size is %d, the file is %d'
                        % (facts['file_size'], path.stat().st_size))
    if facts['used_inodes'] != 11:
        problems.append('%d inodes are in use, an empty image uses 11' % facts['used_inodes'])
    facts['problems'] = problems
    return facts


def module_vermagic(path):
    data = path.read_bytes()
    index = data.find(b'vermagic=')
    if index < 0:
        sys.exit('%s carries no vermagic' % path)
    end = data.find(b'\0', index)
    if end < 0:
        end = len(data)
    return data[index + len(b'vermagic='):end].decode(errors='replace')


def collect_modules(source):
    """Every .ko under `source` (a directory or the kernel's modules.tar.gz), by basename."""
    temporary = None
    root = source
    if source.is_file():
        temporary = tempfile.TemporaryDirectory(prefix='voyage-modules-')
        with tarfile.open(source) as archive:
            archive.extractall(temporary.name)
        root = Path(temporary.name)
    elif not source.is_dir():
        sys.exit('%s is neither a directory nor a tarball' % source)
    found = {}
    for path in sorted(root.rglob('*.ko')):
        if path.name in found:
            sys.exit('two modules are both named %s: %s and %s' % (path.name, found[path.name][0], path))
        found[path.name] = (path, None)
    modules = []
    for name in sorted(found):
        path = found[name][0]
        modules.append({'name': name, 'source': path, 'bytes': path.stat().st_size,
                        'sha256': sha256_file(path), 'vermagic': module_vermagic(path)})
    return modules, temporary


def module_vermagic_problems(modules, expected):
    return ['%s: vermagic %r does not contain %r' % (m['name'], m['vermagic'], expected)
            for m in modules if expected not in m['vermagic']]


def build_prop_new(original, built_utc):
    """The identity properties replaced; every other line byte for byte as it was."""
    text = original.decode()
    lines = text.splitlines(keepends=True)
    incremental = IDENTITY['ro.build.version.incremental']
    fingerprint = IDENTITY['ro.build.fingerprint']
    stamp = '{flavor} {platform} {build_id} {incremental} dev-keys'.format(
        flavor=IDENTITY['ro.build.flavor'], platform=PLATFORM_VERSION,
        build_id=BUILD_ID, incremental=incremental)
    values = dict(IDENTITY)
    values.update({
        'ro.build.version.incremental': incremental,
        'ro.build.date': built_utc.strftime('%a %b %e %H:%M:%S UTC %Y'),
        'ro.build.date.utc': str(int(built_utc.timestamp())),
        'ro.build.description': stamp,
        'ro.build.fingerprint': fingerprint,
    })
    wanted = set(values)
    seen = {}
    out = []
    for line in lines:
        stripped = line.rstrip('\n')
        match = re.match(r'^([A-Za-z0-9_.]+)=', stripped)
        if match and match.group(1) in wanted:
            key = match.group(1)
            if key in seen:
                sys.exit('build.prop assigns %s more than once; refusing to guess which one is real' % key)
            seen[key] = stripped
            out.append('%s=%s\n' % (key, values[key]))
        else:
            out.append(line if line.endswith('\n') else line + '\n')
    missing = wanted - set(seen)
    if missing:
        sys.exit('build.prop has no %s; the released image is not the expected one'
                 % ', '.join(sorted(missing)))
    new = ''.join(out)
    if '\r' in new or any(ord(c) > 127 for c in new):
        sys.exit('the new build.prop is not plain ASCII with LF line endings')
    return new.encode()


def ensure_tool(name):
    found = shutil.which(name)
    if not found:
        sys.exit('%s is not installed' % name)
    return found


def main():
    argument = argparse.ArgumentParser(description=__doc__,
                                       formatter_class=argparse.RawDescriptionHelpFormatter)
    argument.add_argument('--system', type=Path, required=True,
                          help='the released system.img (512 MiB, sha256 968ca8e2...)')
    argument.add_argument('--modules', type=Path, required=True,
                          help='the Voyage kernel modules: modules.tar.gz or an extracted tree')
    argument.add_argument('--data-template', type=Path, required=True,
                          help='the released empty data image, zstd (pw3_data_template.bin)')
    argument.add_argument('--output', type=Path, required=True)
    argument.add_argument('--version', default='voyage-v0.1.0',
                          help='release version for the manifest and file names (default: %(default)s)')
    argument.add_argument('--incremental', default=None,
                          help='ro.build.version.incremental; default eng.builde.<UTC date>.<UTC time>')
    argument.add_argument('--built-utc', default=None,
                          help='build time as YYYY-MM-DDTHH:MM:SSZ; default: now')
    argument.add_argument('--display-id', default=None,
                          help='ro.build.display.id; default: "Readom Voyage <version>"')
    argument.add_argument('--expect-system-sha256', default=RELEASE_SYSTEM_SHA256)
    argument.add_argument('--expect-system-bytes', type=int, default=RELEASE_SYSTEM_BYTES)
    argument.add_argument('--expect-data-sha256', default=RELEASE_DATA_SHA256)
    argument.add_argument('--expect-data-template-sha256', default=RELEASE_DATA_TEMPLATE_SHA256)
    argument.add_argument('--expect-modules', type=int, default=32,
                          help='number of Voyage kernel modules (default: %(default)s)')
    argument.add_argument('--vermagic', default=VERMAGIC)
    argument.add_argument('--boot-image-sha256', default=RELEASE_BOOT_IMAGE_SHA256,
                          help='manifest boot_image_sha256; default: the released receipt value')
    argument.add_argument('--data-format', type=int, default=1)
    argument.add_argument('--no-mke2fs-check', action='store_true',
                          help='skip making a second data image with mke2fs to prove the recipe')
    a = argument.parse_args()

    if platform.node() != 'lima-pw3' or os.geteuid() == 0:
        sys.exit('Run as builder in the build VM (hostname lima-pw3).')
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]{0,47}', a.version):
        sys.exit('version: ASCII letters, digits, ".", "_", "-" (at most 48)')
    if a.data_format < 1:
        sys.exit('data format: 1 or more')
    # debugfs's command language would need quoting for these, and a module written to the
    # wrong name is not a mistake the read-back would catch (the name would be the quoted
    # one). Refuse rather than guess.
    for label, path in (('--system', a.system), ('--modules', a.modules),
                        ('--data-template', a.data_template), ('--output', a.output)):
        if any(c.isspace() for c in str(path)):
            sys.exit('%s path %r contains whitespace; debugfs commands cannot carry it'
                     % (label, str(path)))
    for tool in ('debugfs', 'e2fsck', 'dumpe2fs', 'zstd', 'mke2fs'):
        ensure_tool(tool)
    if not a.system.is_file():
        sys.exit('%s is not there' % a.system)
    if not a.data_template.is_file():
        sys.exit('%s is not there' % a.data_template)

    if a.built_utc:
        try:
            built_utc = datetime.datetime.strptime(a.built_utc, '%Y-%m-%dT%H:%M:%SZ').replace(
                tzinfo=datetime.timezone.utc)
        except ValueError:
            sys.exit('--built-utc is YYYY-MM-DDTHH:MM:SSZ')
    else:
        built_utc = datetime.datetime.now(datetime.timezone.utc).replace(microsecond=0)
    built_utc_text = built_utc.strftime('%Y-%m-%dT%H:%M:%SZ')
    incremental = a.incremental or ('eng.builde.%s.%s'
                                    % (built_utc.strftime('%Y%m%d'), built_utc.strftime('%H%M%S')))
    if not re.fullmatch(r'[A-Za-z0-9_.-]{1,60}', incremental):
        sys.exit('--incremental: ASCII letters, digits, ".", "_", "-" (at most 60)')
    display_id = a.display_id or ('Readom Voyage %s' % re.sub(r'^voyage-', '', a.version))
    IDENTITY['ro.build.version.incremental'] = incremental
    IDENTITY['ro.build.display.id'] = display_id
    IDENTITY['ro.build.fingerprint'] = 'Readom/%s/%s:%s/%s/%s:user/dev-keys' % (
        'voyage', 'voyage', PLATFORM_VERSION, BUILD_ID, fingerprint_id(incremental))

    needed = 3 << 30
    free = shutil.disk_usage(a.output.parent if a.output.parent.exists() else a.output).free
    if free < needed:
        sys.exit('%s has %d bytes free; this build writes about %d'
                 % (a.output.parent, free, needed))
    a.output.mkdir(parents=True, exist_ok=True)
    for stale in ('system.img', 'system.img.zst', 'data.img', 'data.img.zst',
                  'manifest.draft.json', 'build-voyage-facts.json', 'build.log',
                  'modules.sha256', 'build.prop'):
        path = a.output / stale
        if path.exists():
            path.unlink()

    builder = Builder()
    facts = {
        'schema': 1,
        'generated_utc': datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'),
        'version': a.version,
        'incremental': incremental,
        'display_id': display_id,
        'boot_image_sha256': a.boot_image_sha256,
        'data_format': a.data_format,
        'source': {},
        'modules': [],
        'system': {},
        'data': {},
        'packed': {},
        'manifest': {},
    }

    try:
        # 1. The released system image: copy, then check the copy is the expected one.
        source_sha = sha256_file(a.system)
        if source_sha != a.expect_system_sha256:
            sys.exit('%s is sha256 %s, expected %s' % (a.system, source_sha, a.expect_system_sha256))
        if a.system.stat().st_size != a.expect_system_bytes:
            sys.exit('%s is %d bytes, expected %d'
                     % (a.system, a.system.stat().st_size, a.expect_system_bytes))
        facts['source']['system'] = {'name': a.system.name, 'bytes': a.system.stat().st_size,
                                     'sha256': source_sha}
        target = a.output / 'system.img'
        print('+ copy %s -> %s' % (a.system, target))
        shutil.copyfile(a.system, target)
        if sha256_file(target) != source_sha:
            sys.exit('the copy of %s does not hash to the source' % a.system)
        source_check = builder.run(['e2fsck', '-fn', target], quiet=True)
        if 'Pass 5' not in source_check:
            sys.exit('e2fsck -fn on the copied source did not complete:\n%s' % source_check)
        facts['system']['source_e2fsck'] = source_check
        free_before = int(re.search(r'Free blocks:\s+(\d+)',
                                    builder.run(['dumpe2fs', '-h', target], quiet=True)).group(1))
        facts['system']['free_blocks_before'] = free_before

        # 2. The Voyage kernel modules, each checked against the Voyage kernel's vermagic.
        modules_sha = sha256_file(a.modules) if a.modules.is_file() else None
        modules, temporary = collect_modules(a.modules)
        try:
            if len(modules) != a.expect_modules:
                sys.exit('%s has %d modules, expected %d'
                         % (a.modules, len(modules), a.expect_modules))
            problems = module_vermagic_problems(modules, a.vermagic)
            if problems:
                sys.exit('the modules do not belong to this kernel:\n  %s' % '\n  '.join(problems))
            (a.output / 'modules.sha256').write_text(
                ''.join('%s  %s\n' % (m['sha256'], m['name']) for m in modules))
            facts['source']['modules'] = {'name': a.modules.name,
                                          'bytes': a.modules.stat().st_size if a.modules.is_file() else None,
                                          'sha256': modules_sha, 'count': len(modules)}
            facts['modules'] = [{'name': m['name'], 'bytes': m['bytes'], 'sha256': m['sha256'],
                                 'vermagic': m['vermagic']} for m in modules]
            print('modules: %d, all with vermagic containing %s' % (len(modules), a.vermagic))

            # 3. build.prop: the released text with the six identity properties replaced.
            original_prop = builder.cat(target, '/build.prop')
            facts['system']['build_prop_source_sha256'] = sha256_bytes(original_prop)
            new_prop = build_prop_new(original_prop, built_utc)
            (a.output / 'build.prop').write_bytes(new_prop)
            facts['system']['build_prop'] = {'sha256': sha256_bytes(new_prop), 'bytes': len(new_prop),
                                             'values': dict(sorted(IDENTITY.items()))}

            # 4. Replace /vendor/lib/modules and /build.prop through debugfs.
            existing = builder.read_dir(target, MODULES_DIR)
            for entry in existing:
                if not entry['name'].endswith('.ko'):
                    sys.exit('%s/%s is not a module; this script only knows how to replace modules'
                             % (MODULES_DIR, entry['name']))
            commands = ['rm %s/%s' % (MODULES_DIR, e['name']) for e in existing]
            for module in modules:
                remote = '%s/%s' % (MODULES_DIR, module['name'])
                commands.append('write %s %s' % (module['source'], remote))
                commands.append('sif %s mode 0100644' % remote)
                commands.append('sif %s uid 0' % remote)
                commands.append('sif %s gid 0' % remote)
                commands.append('ea_set %s security.selinux "%s\\000"' % (remote, MODULE_SELINUX))
            commands.append('rm /build.prop')
            commands.append('write %s /build.prop' % (a.output / 'build.prop'))
            commands.append('sif /build.prop mode 0100600')
            commands.append('sif /build.prop uid 0')
            commands.append('sif /build.prop gid 0')
            commands.append('ea_set /build.prop security.selinux "%s\\000"' % BUILD_PROP_SELINUX)
            builder.debugfs_script(target, commands, a.output / 'debugfs-commands.txt')

            # 5. Read the image back: names, sizes, hashes, mode and xattr of every file.
            after = builder.read_dir(target, MODULES_DIR)
            got = {e['name']: e for e in after}
            if set(got) != ({m['name'] for m in modules}):
                sys.exit('%s holds %s, expected %s'
                         % (MODULES_DIR, sorted(got), sorted(m['name'] for m in modules)))
            with tempfile.TemporaryDirectory(prefix='voyage-check-') as temporary_check:
                for module in modules:
                    entry = got[module['name']]
                    if entry['bytes'] != module['bytes']:
                        sys.exit('%s/%s is %s bytes in the image, %d on disk'
                                 % (MODULES_DIR, module['name'], entry['bytes'], module['bytes']))
                    dumped = builder.dump(target, '%s/%s' % (MODULES_DIR, module['name']),
                                          Path(temporary_check) / module['name'])
                    if sha256_file(dumped) != module['sha256']:
                        sys.exit('%s/%s in the image is not the module from the kernel build'
                                 % (MODULES_DIR, module['name']))
            if builder.cat(target, '/build.prop') != new_prop:
                sys.exit('/build.prop in the image is not the rewritten one')
            xattrs = builder.debugfs(target, 'ea_list /build.prop')
            if BUILD_PROP_SELINUX + '\\000' not in xattrs:
                sys.exit('/build.prop lost its SELinux label:\n%s' % xattrs)
            one_module = '%s/%s' % (MODULES_DIR, modules[0]['name'])
            xattrs = builder.debugfs(target, 'ea_list %s' % one_module)
            if MODULE_SELINUX + '\\000' not in xattrs:
                sys.exit('%s lost its SELinux label:\n%s' % (one_module, xattrs))
            facts['system']['module_dir'] = {'path': MODULES_DIR, 'count': len(after),
                                             'entries': [{'name': e['name'], 'bytes': e['bytes']}
                                                         for e in after]}

            # 6. e2fsck has to pass and the filesystem has to be clean afterwards.
            check = builder.run(['e2fsck', '-fn', target], quiet=True)
            if 'Pass 5' not in check:
                sys.exit('e2fsck -fn on the built image did not complete:\n%s' % check)
            if 'FILE SYSTEM WAS MODIFIED' in check:
                sys.exit('e2fsck -fn says the filesystem has errors:\n%s' % check)
            facts['system']['e2fsck'] = check
            facts['system']['free_blocks_after'] = int(re.search(
                r'Free blocks:\s+(\d+)', builder.run(['dumpe2fs', '-h', target], quiet=True)).group(1))
            target_sha = sha256_file(target)
            facts['system'].update({'name': 'system.img', 'bytes': target.stat().st_size,
                                    'sha256': target_sha})
        finally:
            if temporary is not None:
                temporary.cleanup()

        # 7. Pack the system image the way a release is packed.
        builder.run(['zstd', '-q', '-19', '-T0', '-f', '-o', a.output / 'system.img.zst', target])
        window = zstd_window(a.output / 'system.img.zst')
        if window > ZSTD_WINDOW_MAX:
            sys.exit('system.img.zst needs a %d byte window; pw3-unpack accepts at most %d'
                     % (window, ZSTD_WINDOW_MAX))
        unpacked_sha, unpacked_bytes = stream_sha256(['zstd', '-dc', a.output / 'system.img.zst'])
        if (unpacked_sha, unpacked_bytes) != (target_sha, target.stat().st_size):
            sys.exit('system.img.zst does not decompress to system.img')
        facts['packed']['system'] = {'name': 'system.img.zst',
                                     'bytes': (a.output / 'system.img.zst').stat().st_size,
                                     'sha256': sha256_file(a.output / 'system.img.zst'),
                                     'window': window,
                                     'unpacked_bytes': unpacked_bytes,
                                     'unpacked_sha256': unpacked_sha}

        # 8. The data image: the release's own empty one, out of its template.
        template_sha = sha256_file(a.data_template)
        if template_sha != a.expect_data_template_sha256:
            sys.exit('%s is sha256 %s, expected %s'
                     % (a.data_template, template_sha, a.expect_data_template_sha256))
        data = a.output / 'data.img'
        builder.run(['zstd', '-d', '-q', '-f', '-o', data, a.data_template])
        data_sha = sha256_file(data)
        if data_sha != a.expect_data_sha256:
            sys.exit('the data template unpacks to sha256 %s, expected %s'
                     % (data_sha, a.expect_data_sha256))
        data_facts = data_image_facts(data)
        if data_facts['problems']:
            sys.exit('the unpacked data image is not acceptable:\n  %s'
                     % '\n  '.join(data_facts['problems']))
        facts['source']['data_template'] = {'name': a.data_template.name,
                                            'bytes': a.data_template.stat().st_size,
                                            'sha256': template_sha}
        facts['data'] = {'name': 'data.img', 'bytes': data.stat().st_size, 'sha256': data_sha,
                         'from': 'template', 'superblock': data_facts}

        # 9. The recipe from the task, run as the build machine would make one from nothing:
        #    mke2fs -q -t ext4 -O ^metadata_csum -L data -b 4096 <file> 458752. (The task
        #    writes "-l 458752"; mke2fs's -l takes a bad-blocks list, the block count is the
        #    positional argument -- make_ext4fs's -l is the size, which is probably where
        #    that came from.)
        if not a.no_mke2fs_check:
            with tempfile.TemporaryDirectory(prefix='voyage-mke2fs-') as temporary_data:
                made = Path(temporary_data) / 'data.img'
                builder.run(['mke2fs', '-q', '-t', 'ext4', '-O', '^metadata_csum', '-L', 'data',
                             '-b', str(DATA_BLOCK_SIZE), made, str(DATA_BLOCKS)])
                made_facts = data_image_facts(made)
                if made_facts['problems']:
                    sys.exit('the mke2fs data image is not acceptable:\n  %s'
                             % '\n  '.join(made_facts['problems']))
                facts['data']['mke2fs'] = {
                    'command': 'mke2fs -q -t ext4 -O ^metadata_csum -L data -b %d <file> %d'
                               % (DATA_BLOCK_SIZE, DATA_BLOCKS),
                    'bytes': made.stat().st_size, 'sha256': sha256_file(made),
                    'superblock': made_facts}
                print('mke2fs data image: %d bytes, superblock checks passed'
                      % made.stat().st_size)

        builder.run(['zstd', '-q', '-19', '-T0', '-f', '-o', a.output / 'data.img.zst', data])
        window = zstd_window(a.output / 'data.img.zst')
        if window > ZSTD_WINDOW_MAX:
            sys.exit('data.img.zst needs a %d byte window; pw3-unpack accepts at most %d'
                     % (window, ZSTD_WINDOW_MAX))
        unpacked_sha, unpacked_bytes = stream_sha256(['zstd', '-dc', a.output / 'data.img.zst'])
        if (unpacked_sha, unpacked_bytes) != (data_sha, data.stat().st_size):
            sys.exit('data.img.zst does not decompress to data.img')
        facts['packed']['data'] = {'name': 'data.img.zst',
                                   'bytes': (a.output / 'data.img.zst').stat().st_size,
                                   'sha256': sha256_file(a.output / 'data.img.zst'),
                                   'window': window,
                                   'unpacked_bytes': unpacked_bytes,
                                   'unpacked_sha256': unpacked_sha}

        # 10. The manifest, in the shape installer/build_payload.py writes (schema 4). The
        #     `name` fields are the release artifact names; the files beside this manifest
        #     are the local build outputs (system.img.zst, data.img.zst).
        def entry(packed):
            return {'name': 'readom-%s-%s.img.zst' % (packed['name'].split('.')[0], a.version),
                    'bytes': packed['bytes'], 'sha256': packed['sha256'],
                    'unpacked_bytes': packed['unpacked_bytes'],
                    'unpacked_sha256': packed['unpacked_sha256'], 'format': 'zstd'}

        manifest = {
            'schema': 4, 'version': a.version, 'built_utc': built_utc_text,
            'built': int(built_utc.timestamp()), 'data_format': a.data_format,
            'boot_image_sha256': a.boot_image_sha256,
            'files': {'system': entry(facts['packed']['system']),
                      'data': entry(facts['packed']['data'])},
        }
        (a.output / 'manifest.draft.json').write_text(json.dumps(manifest, indent=2) + '\n')
        facts['manifest'] = manifest

        # 11. Facts for the report.
        facts['commands'] = {
            'build_prop_identity_fields': ','.join(sorted(IDENTITY)),
            'data_template_decompress': 'zstd -d -q -f -o data.img %s' % a.data_template,
            'module_replacement': 'debugfs -w -f debugfs-commands.txt system.img',
        }
        (a.output / 'build-voyage-facts.json').write_text(json.dumps(facts, indent=2) + '\n')
        print('system.img     %d B  sha256 %s' % (target.stat().st_size, target_sha))
        print('system.img.zst %d B  sha256 %s' % ((a.output / 'system.img.zst').stat().st_size,
                                                  facts['packed']['system']['sha256']))
        print('data.img       %d B  sha256 %s' % (data.stat().st_size, data_sha))
        print('data.img.zst   %d B  sha256 %s' % ((a.output / 'data.img.zst').stat().st_size,
                                                  facts['packed']['data']['sha256']))
        print('Wrote %s' % (a.output / 'build-voyage-facts.json'))
    finally:
        (a.output / 'build.log').write_text('\n'.join(builder.transcript))


if __name__ == '__main__':
    main()
