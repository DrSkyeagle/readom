#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""The rules `tools/build-voyage-release.py` applies to the artifacts it writes.

The script rebuilds the released v0.1.0 system image as a Voyage one: the
modules are replaced, `build.prop` gets the six identity properties, the data
image is an empty ext4 with the release's own superblock, and both images are
packed with zstd inside a window `pw3-unpack` accepts. None of that has to be
run here -- it needs the 512 MiB release image -- but the parts that decide
whether a finished image is acceptable are pure: the superblock judge, the
module vermagic check, the zstd window check and the `build.prop` rewrite. A
wrong mask or a missed identity property is a device that boots as the wrong
model, or one that does not boot at all, and the build would have said it was
fine.

The board-config include chain and `BOARD_USERDATAIMAGE_PARTITION_SIZE` that
`installer/build_payload.py` derives live in the installer, not here, and are
covered where that tool is (its own tests). What this file exercises is what
this script actually does with them: the data image size it writes must be
that partition size, and the superblock must pass the same masks.

The temporary ext4 images are a few hundred KiB; nothing here reads the release
image, starts QEMU or touches the network.
"""
import datetime
import importlib.util
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BUILDER = ROOT / 'tools/build-voyage-release.py'

checks = 0
failures = []
skipped = []


def check(ok, what):
    global checks
    checks += 1
    if not ok:
        failures.append(what)


def skip(what):
    skipped.append(what)


def load():
    spec = importlib.util.spec_from_file_location('pw3_build_voyage_release', BUILDER)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def synth_data_image(path, *, blocks=16, block_log=2, inodes=11, free_inodes=0,
                     state=1, incompat=0x2c2, ro_compat=0x6b, last_orphan=0,
                     label=b'data', short_by=0, magic=0xef53):
    """An ext4 superblock at the offsets preinit/StorageFormat.h reads.

    Small enough to write by hand: the image is `blocks * block_size` bytes and
    the judge only reads the first 2 KiB, so this is the same check a 1.75 GiB
    release data image gets, made in a few KiB.
    """
    block_size = 1024 << block_log
    superblock = bytearray(1024)
    struct.pack_into('<I', superblock, 0x00, inodes)
    struct.pack_into('<I', superblock, 0x04, blocks)
    struct.pack_into('<I', superblock, 0x10, free_inodes)
    struct.pack_into('<I', superblock, 0x18, block_log)
    struct.pack_into('<H', superblock, 0x38, magic)
    struct.pack_into('<H', superblock, 0x3a, state)
    struct.pack_into('<I', superblock, 0x60, incompat)
    struct.pack_into('<I', superblock, 0x64, ro_compat)
    struct.pack_into('<I', superblock, 0xe8, last_orphan)
    superblock[0x68:0x78] = bytes(range(16))
    # Slice assignment with a shorter sequence would shrink the bytearray, which
    # would make the file a few bytes short and test the wrong thing.
    superblock[0x78:0x78 + len(label)] = label
    data = bytearray(blocks * block_size - short_by)
    data[1024:2048] = superblock
    path.write_bytes(data)
    return path


def superblock_cases(module, work):
    cases = [
        ('good', {}, []),
        ('state not clean', {'state': 0}, ['state']),
        ('orphan list not empty', {'last_orphan': 5}, ['orphan']),
        ('incompat outside the mask', {'incompat': 0x400}, ['incompat']),
        ('ro_compat outside the mask', {'ro_compat': 0x80}, ['ro_compat']),
        ('a used inode count that is not an empty image', {'inodes': 12}, ['inodes']),
        ('a file shorter than its block count', {'short_by': 4096}, ['blocks*block_size']),
        ('not ext4', {'magic': 0}, ['not ext4']),
    ]
    for index, (name, changes, wanted) in enumerate(cases):
        path = work / ('superblock-%d.img' % index)
        synth_data_image(path, **changes)
        facts = module.data_image_facts(path)
        if name == 'good':
            check(facts['problems'] == [], 'a valid synthetic data image was refused: %r'
                  % facts['problems'])
            check(facts['used_inodes'] == 11 and facts['file_size'] == path.stat().st_size,
                  'the good data image was read wrong: %r' % facts)
            check(facts['uuid'] == '00010203-0405-0607-0809-0a0b0c0d0e0f',
                  'the data image UUID was not read as the bytes say: %r' % facts['uuid'])
        else:
            check(any(wanted[0] in problem for problem in facts['problems']),
                  'a data image with %s was accepted: %r' % (name, facts['problems']))


def mke2fs_case(module, work):
    if not shutil.which('mke2fs'):
        skip('the mke2fs data-image recipe needs mke2fs (e2fsprogs)')
        return
    image = work / 'mke2fs.img'
    # 256 blocks of 4 KiB: the same recipe the build uses, small enough for a
    # test. The release image is 458752 blocks and the judge does not read more.
    result = subprocess.run(['mke2fs', '-q', '-t', 'ext4', '-O', '^metadata_csum', '-L', 'data',
                             '-b', '4096', str(image), '256'],
                            capture_output=True, text=True)
    if result.returncode != 0:
        skip('mke2fs could not make the test image: %s' % result.stderr.strip()[-200:])
        return
    facts = module.data_image_facts(image)
    check(facts['problems'] == [], 'the mke2fs recipe the build checks makes an image it '
          'refuses: %r' % facts['problems'])
    check(facts['used_inodes'] == 11 and facts['state'] == 1,
          'the mke2fs image is not the empty, clean one the build expects: %r' % facts)


def zstd_cases(module, work):
    if not shutil.which('zstd'):
        skip('the zstd window check needs zstd')
        return
    small = work / 'small.bin'
    small.write_bytes(b'readom voyage synthetic test data\n' * 1024)
    small_zst = work / 'small.zst'
    subprocess.run(['zstd', '-q', '-19', '-f', '-o', str(small_zst), str(small)], check=True)
    window = module.zstd_window(small_zst)
    check(0 < window <= module.ZSTD_WINDOW_MAX,
          'a zstd file the build would accept has window %d, max %d'
          % (window, module.ZSTD_WINDOW_MAX))
    # The refusal the build applies: a window wider than pw3-unpack's buffer.
    big = work / 'big.bin'
    big.write_bytes(bytes(16 << 20))
    big_zst = work / 'big.zst'
    result = subprocess.run(['zstd', '-q', '-19', '--long=27', '-f', '-o', str(big_zst), str(big)],
                            capture_output=True, text=True)
    if result.returncode == 0:
        check(module.zstd_window(big_zst) > module.ZSTD_WINDOW_MAX,
              'zstd --long=27 on 16 MiB did not produce the wide window the build refuses')


def module_cases(module, work):
    check(module.VERMAGIC == '3.0.35-lab126-voyage-android81',
          'the script checks modules against %r' % module.VERMAGIC)
    good = work / 'good.ko'
    good.write_bytes(b'\x7fELF\x01\x01\x01\0' + b'\0' * 16 +
                     b'vermagic=' + module.VERMAGIC.encode() + b' preempt mod_unload ARMv7 p2v8\0')
    check(module.module_vermagic(good) == module.VERMAGIC + ' preempt mod_unload ARMv7 p2v8',
          'module_vermagic() did not read the module it was given')
    other = work / 'other.ko'
    other.write_bytes(b'vermagic=3.0.35-lab126-pw3-android81 preempt mod_unload ARMv7 p2v8\0')
    modules, temporary = module.collect_modules(write_module_dir(work / 'mods', module, 2))
    check(temporary is None, 'collect_modules() made a tarball extraction for a directory')
    check([m['name'] for m in modules] == ['a.ko', 'b.ko'],
          'collect_modules() did not name the directory modules a.ko and b.ko: %r' % modules)
    problems = module.module_vermagic_problems(modules, module.VERMAGIC)
    check(problems == [], 'the good modules were refused: %r' % problems)
    check(module.module_vermagic_problems(
        modules + [{'name': 'other.ko', 'vermagic': module.module_vermagic(other)}],
        module.VERMAGIC) != [], 'a module from another kernel was accepted')
    # Two modules with one basename: the image has one /vendor/lib/modules
    # directory, so there is no name one of them could keep.
    duplicate = work / 'duplicate'
    (duplicate / 'one').mkdir(parents=True)
    (duplicate / 'two').mkdir()
    for where in (duplicate / 'one', duplicate / 'two'):
        (where / 'same.ko').write_bytes(b'vermagic=' + module.VERMAGIC.encode() + b'\0')
    try:
        module.collect_modules(duplicate)
    except SystemExit:
        check(True, 'two modules with one basename are refused')
    else:
        check(False, 'two modules with one basename were accepted')
    # And the tarball form the kernel build produces.
    tar_path = work / 'modules.tar.gz'
    with tarfile.open(tar_path, 'w:gz') as archive:
        archive.add(work / 'mods', arcname='lib/modules/test')
    modules, temporary = module.collect_modules(tar_path)
    try:
        check([m['name'] for m in modules] == ['a.ko', 'b.ko'],
              'collect_modules() did not read the tarball: %r' % modules)
        check(temporary is not None, 'collect_modules() did not report its tarball extraction')
    finally:
        if temporary is not None:
            temporary.cleanup()


def write_module_dir(path, module, count):
    path.mkdir()
    for index in range(count):
        (path / ('%s.ko' % 'ab'[index])).write_bytes(
            b'vermagic=' + module.VERMAGIC.encode() + b' preempt\0')
    return path


def build_prop_cases(module, work):
    saved = dict(module.IDENTITY)
    try:
        module.IDENTITY.update({
            'ro.build.version.incremental': 'eng.builde.20261104.042050',
            'ro.build.display.id': 'Readom Voyage v0.1.0',
            'ro.build.flavor': 'voyage-user',
            'ro.build.fingerprint': 'Readom/voyage/voyage:8.1.0/OC/builde11040420:user/dev-keys',
        })
        keys = sorted(set(module.IDENTITY) | {
            'ro.build.version.incremental', 'ro.build.date', 'ro.build.date.utc',
            'ro.build.description', 'ro.build.fingerprint'})
        original = ''.join('%s=old-%d\n' % (key, index) for index, key in enumerate(keys))
        original += 'ro.product.board=pw3\n'
        built = datetime.datetime(2026, 11, 4, 4, 20, 50, tzinfo=datetime.timezone.utc)
        new = module.build_prop_new(original.encode(), built).decode()
        check('ro.product.device=voyage' in new and 'ro.product.name=voyage' in new,
              'the rewritten build.prop does not identify as Voyage: %s' % new[:400])
        check('ro.build.display.id=Readom Voyage v0.1.0' in new,
              'the rewritten build.prop keeps the old display id')
        check('ro.build.version.incremental=eng.builde.20261104.042050' in new,
              'the rewritten build.prop keeps the old incremental')
        check('ro.product.board=pw3\n' in new,
              'the rewrite changed a property outside the identity set')
        check('ro.build.date.utc=%d' % int(built.timestamp()) in new,
              'the rewritten build.prop has the wrong build date')
        check('ro.build.description=voyage-user 8.1.0 OC eng.builde.20261104.042050 dev-keys' in new,
              'the rewritten build.prop has the wrong description')
        check('ro.build.fingerprint=Readom/voyage/voyage:8.1.0/OC/builde11040420:user/dev-keys'
              in new, 'the rewritten build.prop has the wrong fingerprint')

        def refused(name, text):
            try:
                module.build_prop_new(text.encode(), built)
            except SystemExit:
                check(True, 'build_prop_new() refuses a build.prop with %s' % name)
            else:
                check(False, 'build_prop_new() accepted a build.prop with %s' % name)

        refused('a duplicated identity property',
                original + 'ro.product.model=Kindle Paperwhite 3\n')
        refused('a missing identity property',
                ''.join('%s=old-%d\n' % (key, index) for index, key in enumerate(keys[1:])))
        refused('CRLF line endings', original.replace('\n', '\r\n'))
        refused('a non-ASCII value', original + 'ro.build.note=caf\u00e9\n')
    finally:
        module.IDENTITY.clear()
        module.IDENTITY.update(saved)


def constant_cases(module):
    check(module.DATA_BLOCKS * module.DATA_BLOCK_SIZE == module.RELEASE_DATA_BYTES,
          'the data image recipe does not add up to the released data size')
    check(module.RELEASE_DATA_BYTES == 1879048192,
          'the data image size changed from the released 1,879,048,192')
    check(module.RELEASE_SYSTEM_BYTES == 536870912,
          'the system image size changed from the released 536,870,912')
    check(module.ZSTD_WINDOW_MAX == 8 << 20,
          'the zstd window limit is not the 8 MiB pw3-unpack accepts')
    check(module.MODULES_DIR == '/vendor/lib/modules',
          'the module directory changed from /vendor/lib/modules')
    for key, value in (('ro.product.name', 'voyage'), ('ro.product.device', 'voyage'),
                       ('ro.build.flavor', 'voyage-user'), ('ro.product.model', 'Kindle Voyage')):
        check(module.IDENTITY.get(key) == value,
              'IDENTITY[%r] is %r, not %r' % (key, module.IDENTITY.get(key), value))
    check(module.fingerprint_id('eng.builde.20261104.042050') == 'builde11040420',
          'the fingerprint build segment is not the released pair')
    check(module.fingerprint_id('a.b c') == 'abc',
          'the fingerprint fallback does not strip what is not a name')


def main():
    check(BUILDER.is_file(), '%s is missing' % BUILDER.name)
    if not BUILDER.is_file():
        return report()
    module = load()
    constant_cases(module)
    with tempfile.TemporaryDirectory(prefix='pw3-voyage-release-test-') as temporary:
        work = Path(temporary)
        superblock_cases(module, work)
        mke2fs_case(module, work)
        zstd_cases(module, work)
        module_cases(module, work)
        build_prop_cases(module, work)
    return report()


def report():
    print('%d checks, %d failed%s'
          % (checks, len(failures),
             '' if not skipped else ' (%d skipped: %s)' % (len(skipped), '; '.join(skipped))))
    for failure in failures:
        print('  FAIL %s' % failure)
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
