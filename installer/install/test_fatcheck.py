#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Tests for the installer's cross-link check (fatcheck.c), on a real FAT.

What this file exists for is the three seams nothing else can reach. The shared
algorithm has its own test against a FAT built byte by byte in memory
(device/amazon/pw3/preinit/tests/fat_crosslink_test.c); what is not tested there
is the part that only a mounted filesystem and a running kernel can answer:

  which volume   the images are reached through a proxy on the device -- /mnt/us
                 is fsp over the vfat at /mnt/base-us -- so the filesystem that
                 holds the bytes is not the one the paths are on. Finding it is
                 this code's own work, and getting it wrong is invisible: the
                 answer is then always "could not look", which is a line in a
                 log and an install that goes on unchecked.
  the mapping    FIBMAP's answers on vfat, in the units and the frame this
                 assumes they are in. Reasoning about that is not knowing it.
  the frame      a boot sector read at offset 0 of the device the vfat is
                 mounted from, against extents FIBMAP counted from the volume's
                 first sector. On the device those agree because the mount is a
                 loop device at an offset; here they agree because the mount is
                 a loop device at no offset. Either way it is the same claim,
                 and it is checked by building a volume and asking.

So this makes a FAT, mounts it, puts images on it, mounts it a second time
elsewhere to stand in for the proxy's own path, and cross-links it on purpose.
It also puts a folder 40 levels deep on a clean volume, because a walk that
cannot finish is not a cross-link and must not refuse an install. Mounting needs
root, which the test does not have: the privileged steps go through `sudo -n`,
and run-tests.py lists this test as wanting that rather than running it without.

Usage: test_fatcheck.py
"""
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
PREINIT = ROOT / 'device/amazon/pw3/preinit'

# enum pw3_fat_cross, for the one value a test asserts on (FatCrossLink.h).
CROSS_SHARED = 6
# The images, at a length that is a whole number of the volume's clusters and
# small enough to write in a moment. The release's own lengths are 512 MiB and
# 1.75 GiB; what this exercises is the per-cluster loop, and 1024 clusters go
# round it 1024 times just as 65,536 do.
IMAGE_BYTES = 8 * 1024 * 1024
# A FAT32 needs 65,525 clusters at the least, so 8 KiB clusters -- the stock
# volume's -- need half a gigabyte before mkfs.vfat will make one. The file is
# sparse and the tables are a few hundred KB, so this costs nothing.
FAT32_BYTES = 600 * 1024 * 1024
FAT16_BYTES = 64 * 1024 * 1024


class Tests:
    def __init__(self):
        self.passed = self.failed = 0

    def check(self, name, condition, detail=''):
        if condition:
            self.passed += 1
            print('ok   ' + name)
        else:
            self.failed += 1
            print('FAIL ' + name + (': ' + detail if detail else ''))

    def answer(self, name, got, want, note=()):
        """One run of the driver: the answer, and the fields the test names."""
        parts = got.split('\t')
        fields = dict(part.split('=', 1) for part in parts[1:] if '=' in part)
        ok = parts[0] == want and all(fields.get(k, '') == v for k, v in note)
        self.check(name, ok, got.strip())
        return fields


def sudo(*command, check=True):
    done = subprocess.run(['sudo', '-n', *command], capture_output=True, text=True)
    # The message, not just the status: a mount refuses for a reason, and the
    # reason is in its stderr. A CalledProcessError hides it behind the command.
    if check and done.returncode:
        sys.exit('%s: %s' % (' '.join(command[:2]), done.stderr.strip() or done.returncode))
    return done


def build(tmp):
    binary = tmp / 'test_fatcheck'
    subprocess.run(['gcc', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I', str(HERE), '-I', str(PREINIT),
                    str(HERE / 'fatcheck.c'), str(HERE / 'test_fatcheck.c'), '-o', str(binary)],
                   check=True)
    return binary


class Volume:
    """A FAT of its own on a loop device, mounted, unmounted and gone again."""

    def __init__(self, tmp, name, size, mkfs_flags):
        self.image = tmp / (name + '.img')
        self.mount = tmp / name
        self.mount.mkdir()
        with open(self.image, 'wb') as f:
            f.truncate(size)
        subprocess.run(['/usr/sbin/mkfs.vfat', *mkfs_flags, str(self.image)],
                       check=True, capture_output=True)
        self.loop = sudo('/usr/sbin/losetup', '--find', '--show', str(self.image)).stdout.strip()
        self.mounted = False
        self.remount()

    def remount(self):
        sudo('mount', '-t', 'vfat', '-o', 'rw,uid=%d,gid=%d' % (os.getuid(), os.getgid()),
             self.loop, str(self.mount))
        self.mounted = True

    def unmount(self):
        if self.mounted:
            sudo('umount', str(self.mount))
            self.mounted = False

    def close(self):
        self.unmount()
        if self.loop:
            sudo('/usr/sbin/losetup', '-d', self.loop, check=False)
            self.loop = ''


def boot_geometry(image):
    """The few BPB fields a directory walk needs, from the volume's own boot sector.

    Enough for these fixtures and no more: the walk below reads one cluster of a
    directory and never follows its chain, which holds because each directory
    here has a handful of entries. It is not a general FAT reader and nothing
    else should be built on it -- the reader under test is fatcheck.c, and the
    directory walk under test is the C one in FatCrossLink.h.
    """
    boot = image.read_bytes()[:512]
    sector, per_cluster, reserved, fats = struct.unpack_from('<HBHB', boot, 11)
    small_root, small_total, _media, small_fat = struct.unpack_from('<HHBH', boot, 17)
    big_fat, = struct.unpack_from('<I', boot, 36)
    root_cluster, = struct.unpack_from('<I', boot, 44)
    fat_sectors = small_fat or big_fat
    root_sectors = (small_root * 32 + sector - 1) // sector
    data_start = reserved + fats * fat_sectors + root_sectors
    # FAT16 keeps the root directory in its own sectors before the data area;
    # FAT32 keeps it in a cluster chain like any other directory.
    root_at = (reserved + fats * fat_sectors) * sector if small_root else \
        (data_start + (root_cluster - 2) * per_cluster) * sector
    return {'sector': sector, 'cluster_bytes': per_cluster * sector, 'data_start': data_start,
            'root_at': root_at, 'root_bytes': (small_root * 32) or per_cluster * sector,
            'per_cluster': per_cluster}


def entries(data):
    """The 8.3 directory entries of one directory, by name, skipping long-name parts."""
    out = {}
    for at in range(0, len(data) - 31, 32):
        row = data[at:at + 32]
        if row[0] in (0x00, 0xe5) or row[11] == 0x0f:
            continue
        out[row[:11].decode('latin-1')] = {'at': at, 'attr': row[11],
                                           'cluster': struct.unpack_from('<H', row, 26)[0] |
                                           struct.unpack_from('<H', row, 20)[0] << 16}
    return out


def cross_link(image, directory, from_name, to_name):
    """Point one directory entry at another file's first cluster.

    This is the fault that needs no table entry at all and so is invisible to a
    table walk: two names, one chain. It is also the one a repair tool produces
    by guessing, which is the way a reader would arrive at it.
    """
    g = boot_geometry(image)
    raw = bytearray(image.read_bytes())
    root = entries(raw[g['root_at']:g['root_at'] + g['root_bytes']])
    sub = root[directory]
    assert sub['attr'] & 0x10, 'not a directory: ' + directory
    sub_at = (g['data_start'] + (sub['cluster'] - 2) * g['per_cluster']) * g['sector']
    inside = entries(raw[sub_at:sub_at + g['cluster_bytes']])
    target = inside[to_name]['cluster']
    at = g['root_at'] + root[from_name]['at']
    struct.pack_into('<H', raw, at + 26, target & 0xffff)
    struct.pack_into('<H', raw, at + 20, target >> 16)
    image.write_bytes(bytes(raw))
    return target


def main():
    if sys.platform != 'linux':
        sys.exit('this test mounts a filesystem, so it runs on Linux')
    if subprocess.run(['sudo', '-n', 'true'], capture_output=True).returncode:
        sys.exit('this test mounts a filesystem, so it needs sudo without a password')
    t = Tests()
    with tempfile.TemporaryDirectory() as raw:
        tmp = Path(raw)
        # The mount points have to be reachable by root's umount and by the
        # second mount, which a private temporary directory's 0700 parents allow.
        os.chmod(raw, 0o755)
        driver = build(tmp)
        fat = fat16 = None
        view = tmp / 'view'
        view.mkdir()
        bound = False
        try:
            fat = Volume(tmp, 'fat32', FAT32_BYTES, ['-F', '32', '-s', '16', '-n', 'PW3TEST'])
            store = fat.mount
            (store / 'android').mkdir()
            # 8.3 names, so that the directory walk this test does itself needs
            # no long-name reassembly. What is being tested is below the names.
            for name in ('SYS.IMG', 'DAT.IMG'):
                with open(store / 'android' / name, 'wb') as f:
                    f.write(b'\xa5' * IMAGE_BYTES)
            with open(store / 'BOOK.TXT', 'wb') as f:
                f.write(b'a book of one cluster\n' * 100)
            system, data = store / 'android/SYS.IMG', store / 'android/DAT.IMG'

            def run(user_dir, system_path, data_path, system_bytes=IMAGE_BYTES,
                    data_bytes=IMAGE_BYTES):
                # As root, which is what the installer is on the stock system:
                # the volume is a block device, and reading one is root's.
                out = sudo(str(driver), str(user_dir),
                           str(system_path) if system_path else '-', str(system_bytes),
                           str(data_path) if data_path else '-', str(data_bytes))
                return out.stdout.strip()

            t.answer('a clean FAT, the store being the volume itself', run(store, system, data),
                     'OK', [('mount', str(store)), ('volume', fat.loop)])
            t.answer('the data image alone finds the volume', run(store, None, data),
                     'OK', [('mount', str(store))])

            # The device's shape: the store the installer is given is not the
            # path of the filesystem that holds the bytes. A bind mount is not
            # fsp -- it is the same filesystem, where fsp is another one on top
            # -- and it is the whole of what this code can tell apart, because
            # it never looks at the store: the store is a prefix to take off a
            # path, and the premise that the proxy shows this volume's tree is
            # stated in fatcheck.h, not checked here. What a second mount does
            # exercise is the dedup: two lines of /proc/mounts naming one
            # filesystem are one candidate, and counting lines instead would
            # make this configuration an ambiguity and the answer "could not
            # look".
            sudo('mount', '--bind', str(store), str(view))
            bound = True
            t.answer('a store that reaches the volume by another mount point',
                     run(view, view / 'android/SYS.IMG', view / 'android/DAT.IMG'),
                     'OK', [('volume', fat.loop)])
            sudo('umount', str(view))
            bound = False

            t.answer('a path no mounted vfat holds',
                     run(tmp, tmp / 'elsewhere/SYS.IMG', None),
                     'UNREADABLE', [('where', 'no mounted vfat holds this image')])
            t.answer('a length the volume does not agree with',
                     run(store, system, None, system_bytes=IMAGE_BYTES + 512),
                     'UNREADABLE', [('where', 'no mounted vfat holds this image')])
            t.answer('an image that is not under the store',
                     run(store / 'android', store / 'BOOK.TXT', None),
                     'UNREADABLE', [('where', 'the image is not under the user store')])

            # The fault itself: a third name pointing at the system image's
            # chain, which is what an unplugged write or a guessing repair tool
            # leaves behind.
            fat.unmount()
            first = cross_link(fat.image, 'ANDROID    ', 'BOOK    TXT', 'SYS     IMG')
            t.check('the cross-link was written', first >= 2, 'first cluster %d' % first)
            fat.remount()
            t.answer('a book sharing the system image\'s first cluster', run(store, system, data),
                     'SHARED', [('detail', str(CROSS_SHARED))])
            t.answer('the same volume, asked about the other image only',
                     run(store, None, data), 'OK')
            # A refusal has to come from sharing and from nothing else. The
            # shared check walks the whole volume and gives up past 32 levels of
            # directory, which is a reader's own deep folder and not a
            # cross-link; answering SHARED there would refuse every install on
            # that Kindle. The walk stops at the first thing it cannot do, so
            # this is put on a volume with no cross-link of its own.
            fat.unmount()
            deep = Volume(tmp, 'deep', FAT32_BYTES, ['-F', '32', '-s', '16', '-n', 'PW3DEEP'])
            (deep.mount / 'android').mkdir()
            with open(deep.mount / 'android/SYS.IMG', 'wb') as f:
                f.write(b'\xa5' * IMAGE_BYTES)
            folder = deep.mount
            for level in range(40):
                folder = folder / ('L%02d' % level)
                folder.mkdir()
            (folder / 'BOOK.TXT').write_bytes(b'a book 40 folders down\n')
            t.answer('a reader\'s folder 40 levels deep, on a volume with no cross-link',
                     run(deep.mount, deep.mount / 'android/SYS.IMG', None),
                     'UNREADABLE', [('where', 'a directory of this FAT loops or nests too deep')])
            deep.close()
            fat.remount()
            # An image that is not the length this release writes is written
            # over all the same -- the unpack opens it as it is and writes from
            # the first byte -- so it has to be checked at the length it has.
            # Asking only about files already at the release's length would skip
            # exactly the one an interrupted install left behind.
            #
            # The odd byte is the point: an interrupted write leaves whatever
            # length it got to, and a length that is not whole sectors used to be
            # refused before a single FIBMAP ran, which skipped the check on the
            # very file it was added for (Codex 2026-10-31). The sector holding
            # that last byte is allocated like any other, so the map rounds up.
            ragged = IMAGE_BYTES // 2 + 1
            short = store / 'android/DAT.IMG'
            with open(short, 'r+b') as f:
                f.truncate(ragged)
            t.answer('an image left at a length that is not whole sectors: still checked',
                     run(store, None, short, data_bytes=ragged), 'OK')
            fat.unmount()
            book = cross_link(fat.image, 'ANDROID    ', 'BOOK    TXT', 'DAT     IMG')
            fat.remount()
            t.check('the short image\'s chain was cross-linked', book >= 2, 'first cluster %d' % book)
            t.answer('a book sharing a cluster of the ragged image',
                     run(store, None, short, data_bytes=ragged),
                     'SHARED', [('detail', str(CROSS_SHARED))])
            # The other end of the same boundary: shorter than one sector. An
            # unpack killed in its first moments leaves this, and the one
            # cluster it holds is written over like any other.
            #
            # The cross-link is moved off this image first, and has to be: the
            # case above pointed the book at the data image's FIRST cluster,
            # which a truncation to one byte keeps. Without this line the answer
            # would be SHARED -- correctly -- and the length boundary would go
            # unasked.
            fat.unmount()
            cross_link(fat.image, 'ANDROID    ', 'BOOK    TXT', 'SYS     IMG')
            fat.remount()
            with open(short, 'r+b') as f:
                f.truncate(1)
            t.answer('an image of one byte: one cluster, still checked',
                     run(store, None, short, data_bytes=1), 'OK')

            # The other FAT width the shared check reads. The stock volume is
            # FAT32; this is the branch of pw3_fat_geometry nothing else here
            # walks, and a volume whose root directory is not a cluster chain.
            # It is made while the first one is still mounted, holding the same
            # path at the same length, which is the one real ambiguity: two
            # filesystems that both answer to the name, and nothing to choose
            # between them.
            fat16 = Volume(tmp, 'fat16', FAT16_BYTES, ['-F', '16', '-s', '4', '-n', 'PW3TEST16'])
            (fat16.mount / 'android').mkdir()
            with open(fat16.mount / 'android/SYS.IMG', 'wb') as f:
                f.write(b'\x5a' * IMAGE_BYTES)
            with open(fat16.mount / 'BOOK.TXT', 'wb') as f:
                f.write(b'a book of one cluster\n' * 100)
            t.answer('two volumes that both hold the image', run(store, system, None),
                     'UNREADABLE', [('where', 'more than one mounted vfat holds this image')])
            fat.close()
            fat = None

            store = fat16.mount
            t.answer('a clean FAT16 volume', run(store, store / 'android/SYS.IMG', None), 'OK',
                     [('mount', str(store)), ('volume', fat16.loop)])
            fat16.unmount()
            cross_link(fat16.image, 'ANDROID    ', 'BOOK    TXT', 'SYS     IMG')
            fat16.remount()
            t.answer('a cross-linked FAT16 volume', run(store, store / 'android/SYS.IMG', None),
                     'SHARED', [('detail', str(CROSS_SHARED))])
        finally:
            if bound:
                sudo('umount', str(view), check=False)
            for volume in (fat, fat16):
                if volume:
                    volume.close()
    print('%d passed, %d failed' % (t.passed, t.failed))
    return 1 if t.failed else 0


if __name__ == '__main__':
    sys.exit(main())
