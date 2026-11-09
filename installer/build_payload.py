#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""The release files the device-side installer fetches (README.md, "发行载荷"), in the build VM:

  readom-system-VERSION.img.zst   the system image
  readom-data-VERSION.img.zst     an empty data image (about 60 KB for 1.75 GiB: carried
                                   inside the entry, README.md "发行载荷")
  manifest.json                    size and SHA256 of each file, packed and unpacked; the
                                   build time (the installer refuses a clock before it: TLS
                                   cannot be checked), the data image's format, and the
                                   system's boot image, which says whether this upgrade
                                   makes Android compile the installed applications again

Both are zstd at level 19 with its default window (8 MiB, the most pw3-unpack accepts),
so the Kindle needs one decoder and 8 MiB of memory for it.

The data image is made here, from an empty directory, by the tool and with the arguments
the build uses for userdata.img. Run this on the tree that produced the export.

Usage: build_payload.py --export DIR --data-format N --version VERSION --output DIR [--model MODEL]
  --export       export-aosp-images.py output (system.raw.img)
  --data-format  what Android keeps in /data and how big it is: a release keeps an existing
                 data image only of its own format (1: Android 8.1, 1.75 GiB)
  --model        the device directory under device/amazon/ whose BoardConfig.mk says how big
                 the data image is, and the AOSP product directory that was built from it.
                 Default: pw3, the only model this project has run on a device
"""
import argparse, datetime, hashlib, json, os, platform, re, struct, subprocess, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
PORT = HERE.parent
# The build tree (tools/build-aosp-system.sh): the host tool that makes every ext4 image
# here, and the labels it gives them. The x86 host binaries run in the arm VM under its
# interpreter, as they do in the build. Both paths carry the product's name, which is
# also the name of its directory under device/amazon/.
AOSP_OUT = Path('/home/builder/android-src/out')
MAKE_EXT4FS = AOSP_OUT / 'host/linux-x86/bin/make_ext4fs'
# What an ext4 holds when nothing has been put in it: inodes 1 to 10 are ext4's own and
# 11 is lost+found. One more means a file, and a file here reaches every device.
EMPTY_INODES = 11
WINDOW = 8 << 20


def board_configs(model):
    """The BoardConfig.mk files that decide this model's image sizes, in order.

    A model's own BoardConfig.mk inherits the pw3 one (`include device/amazon/
    pw3/BoardConfig.mk`, which is what device/amazon/voyage does), and the sizes
    are stated in the file that owns them. The include is followed rather than
    the number copied into a second file, so there is one place a size can be
    changed. Includes written with a make variable (`$(PW3_USB_COMPONENT_ROOT)/
    ...`) are skipped: they cannot be resolved here, and none of them states a
    size.
    """
    root = PORT / 'device/amazon' / model / 'BoardConfig.mk'
    seen, found, todo = set(), [], [root]
    while todo:
        path = todo.pop(0)
        if path in seen or not path.is_file():
            continue
        seen.add(path)
        found.append(path)
        for line in path.read_text().splitlines():
            included = re.match(r'\s*include\s+([A-Za-z0-9_./-]+\.mk)\s*$', line)
            if included:
                todo.append(PORT / included.group(1))
    return found


def product(model):
    """The AOSP product directory a build of that device directory writes."""
    return AOSP_OUT / 'target/product' / model


def sha_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(8 << 20), b''):
            h.update(block)
    return h.hexdigest()


def compress(raw, out):
    subprocess.run(['zstd', '-q', '-19', '-T0', '-o', str(out), str(raw)], check=True)
    listing = subprocess.check_output(['zstd', '-lv', str(out)], text=True)
    window = int(re.search(r'Window Size: .*\((\d+) B\)', listing).group(1))
    if window > WINDOW:
        sys.exit('%s needs a %d byte window; pw3-unpack accepts at most %d' % (out.name, window, WINDOW))


def data_bytes(model):
    """The size of the data image, from the configuration the system was built with."""
    for config in board_configs(model):
        found = re.search(r'^BOARD_USERDATAIMAGE_PARTITION_SIZE\s*:=\s*(\d+)\s*$', config.read_text(), re.M)
        if found:
            return int(found.group(1))
    sys.exit('no BOARD_USERDATAIMAGE_PARTITION_SIZE for %s (looked in %s)'
             % (model, ', '.join(str(p) for p in board_configs(model))))


def empty_data_image(out, size, model):
    """An empty data image, made the way the build makes userdata.img.

    AOSP's own userdata.img is not an empty data image: `make userdataimage` packs whatever
    is in $OUT/data, which is where the build installs its test modules. A release built
    from it would hand every device 4.2 MB of AOSP native tests, and this project's
    PW3FormatTest.apk in /data/app, where the package manager would find it. The source
    directory here is empty instead, so the image holds nothing but lost+found.

    Without -s make_ext4fs writes the image itself rather than Android's sparse form, which
    is byte for byte what unpacking the sparse form gives and needs no second tool. The
    arguments are otherwise the build's (mkuserimg.sh): fixed timestamps, the labels of this
    build, the label and size of the data partition.
    """
    built = product(model)
    contexts = built / 'obj/ETC/file_contexts.bin_intermediates/file_contexts.bin'
    for tool in (MAKE_EXT4FS, contexts):
        if not tool.exists():
            sys.exit('%s is not there; build the system first (tools/build-aosp-system.sh)' % tool)
    with tempfile.TemporaryDirectory() as empty:
        subprocess.run([str(MAKE_EXT4FS), '-T', '-1', '-S', str(contexts), '-L', 'data',
                        '-l', str(size), '-a', 'data', str(out), empty, str(built / 'system')],
                       check=True, stdout=subprocess.DEVNULL)
    with open(out, 'rb') as f:
        f.seek(1024)
        superblock = f.read(1024)
    if superblock[56:58] != b'\x53\xef':
        sys.exit('%s is not an ext4 image' % out)
    inodes, blocks = struct.unpack_from('<II', superblock, 0)
    free_inodes, = struct.unpack_from('<I', superblock, 16)
    held = blocks * (1024 << struct.unpack_from('<I', superblock, 24)[0])
    if held != size or out.stat().st_size != size:
        sys.exit('the data image is %d bytes of filesystem in a %d byte file, not %d of each'
                 % (held, out.stat().st_size, size))
    if inodes - free_inodes != EMPTY_INODES:
        sys.exit('the data image is not empty: %d inodes are in use, and an empty one uses %d'
                 % (inodes - free_inodes, EMPTY_INODES))


def entry(packed, raw, raw_sha):
    return {'name': packed.name, 'bytes': packed.stat().st_size, 'sha256': sha_file(packed),
            'unpacked_bytes': raw.stat().st_size, 'unpacked_sha256': raw_sha, 'format': 'zstd'}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--export', type=Path, required=True)
    ap.add_argument('--data-format', type=int, required=True)
    ap.add_argument('--version', required=True)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--model', default='pw3',
                    help='device/amazon/<model>: whose BoardConfig.mk states the data size, and '
                         'whose AOSP product directory the build wrote. Default: pw3')
    a = ap.parse_args()
    if platform.node() != (os.environ.get('READOM_BUILD_HOST') or 'lima-pw3') or os.geteuid() == 0:
        sys.exit('Run as the builder in the build VM.')
    if not re.fullmatch(r'[a-z0-9][a-z0-9-]*', a.model):
        sys.exit('--model is the device directory name: lower-case letters, digits, "-"')
    export = json.loads((a.export / 'build-result.json').read_text())
    system = a.export / 'system.raw.img'
    if sha_file(system) != export['files']['system.raw.img']['sha256']:
        sys.exit('system.raw.img is not the exported one')
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]{0,47}', a.version) or a.data_format < 1:
        sys.exit('version: ASCII letters, digits, ".", "_", "-" (at most 48); data format: 1 or more')
    built = datetime.datetime.strptime(export['built_utc'], '%Y-%m-%dT%H:%M:%SZ').replace(tzinfo=datetime.timezone.utc)
    a.output.mkdir(parents=True, exist_ok=False)
    packed_system = a.output / ('readom-system-%s.img.zst' % a.version)
    packed_data = a.output / ('readom-data-%s.img.zst' % a.version)
    raw_data = a.output / ('readom-data-%s.img' % a.version)
    empty_data_image(raw_data, data_bytes(a.model), a.model)
    compress(system, packed_system)
    compress(raw_data, packed_data)
    manifest = {
        'schema': 4, 'version': a.version, 'built_utc': export['built_utc'], 'built': int(built.timestamp()),
        'data_format': a.data_format,
        # The export's hash of /system/framework/arm/boot*: the installer compares it
        # with the one it recorded for the system now installed, and only a difference
        # means Android will compile the applications in /data again.
        'boot_image_sha256': export['boot_image']['sha256'],
        'files': {
            'system': entry(packed_system, system, export['files']['system.raw.img']['sha256']),
            'data': entry(packed_data, raw_data, sha_file(raw_data)),
        },
    }
    # The release is the two packed files and the manifest; the 1.75 GiB it was made from is
    # not one of them, and pw3-unpack makes it again whenever a check wants it.
    raw_data.unlink()
    (a.output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps(manifest, indent=1))


if __name__ == '__main__':
    main()
