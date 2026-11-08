#!/usr/bin/env python3
"""Export completed AOSP image files and receipts inside the isolated build VM.

The product is the one tools/build-aosp-system.sh was given in
PW3_AOSP_PRODUCT (default pw3): it names the output directory, and its device
directories are the ones whose copies in the AOSP tree are checked against this
repository. A second model inherits pw3's device tree and adds its own beside it,
so both are hashed; the kernel component is the one that model's kernel build left
under /mnt/build-share/android-core-kernel (with the model suffix for a second
model), because the modules in the image must be that kernel's.
"""
import argparse
import fcntl
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET


def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--product', default='pw3',
                        help='the AOSP product to export (PW3_AOSP_PRODUCT): lower-case letters, '
                             'digits and "-"; default pw3, the one this project ships')
    args = parser.parse_args()
    if platform.node() != 'lima-pw3' or os.geteuid() == 0:
        raise RuntimeError('Run as builder inside the project VM')
    if not re.fullmatch(r'[a-z0-9][a-z0-9-]*', args.product):
        raise RuntimeError('--product: the device directory name (lower-case letters, digits, "-")')
    output = args.output.resolve()
    if output.exists():
        raise RuntimeError('Refusing to overwrite an export')
    source = Path('/home/builder/android-src')
    product = source / 'out/target/product' / args.product
    # The kernel build leaves its artifacts under the same suffix rule as its
    # model (tools/build-kernel.sh ARTIFACT_ROOT): pw3 keeps the plain name, and
    # any other model gets its own directory.
    model_suffix = '' if args.product == 'pw3' else '-' + args.product
    share = Path('/mnt/build-share')
    port = Path('/mnt/port')
    with Path('/home/builder/pw3-build/aosp-build.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_SH | fcntl.LOCK_NB)
        receipt = share / 'logs/aosp-system-job.json'
        build_log = share / 'logs/aosp-system-build.log'
        job = json.loads(receipt.read_text())
        if job['state'] != 'completed' or job['exit_code'] != 0:
            raise RuntimeError('The most recent full image build did not complete')
        # When the images were built and what the build said are taken from these
        # two files, which only the job wrapper writes (run-aosp-build-job.sh). A
        # build started any other way leaves the previous job's receipt in place,
        # and this export would then certify another build's time and log: the
        # time reaches the release manifest, and through it the installer's "the
        # device's clock cannot be earlier than the build" check.
        #
        # The receipt names the images it is the receipt of, by their contents. A
        # time would not do: output trees are swapped by renaming and restored
        # from copies, and both keep an image's timestamp, so a receipt newer than
        # the images is no evidence that it is theirs. The log is covered by the
        # same binding, because the wrapper writes both in the one run.
        images = job.get('images') or {}
        for name in ('system.img', 'ramdisk.img'):
            if images.get(name) != sha(product / name):
                raise RuntimeError(
                    'logs/aosp-system-job.json is not the receipt of this ' + name +
                    '; start the build with tools/run-aosp-build-job.sh')
        converter = Path('/home/builder/pw3-preinit-check/format-tools/simg2img')
        if sha(converter) != '2fe2432c68f0f864b4a952211ac46b19254a54e30a0e70406af157196a21a460':
            raise RuntimeError('Unverified sparse-image converter')
        spec = importlib.util.spec_from_file_location('pw3_pack', port / 'tools/preinit-pack/pack.py')
        pack = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = pack
        spec.loader.exec_module(pack)
        entries = pack.read_ramdisk(product / 'ramdisk.img')
        ramdisk = pack.verify_ramdisk(entries)
        if 'adb_keys' in ramdisk or any(e.name.startswith('firmware/') for e in entries):
            raise RuntimeError('Public build contains a private ramdisk asset')
        fonts_xml = product / 'system/etc/fonts.xml'
        fonts = {item.text.strip() for item in ET.parse(fonts_xml).iter('font')
                 if item.text and item.text.strip()}
        missing_fonts = [name for name in sorted(fonts)
                         if Path(name).name != name or not (product / 'system/fonts' / name).is_file()]
        if not fonts or missing_fonts:
            raise RuntimeError('Font configuration has missing files: ' + ', '.join(missing_fonts))
        keyboard_files = ['usr/keychars/Generic.kcm', 'usr/keychars/Virtual.kcm',
                          'usr/keylayout/Generic.kl']
        for name in keyboard_files:
            if not (product / 'system' / name).is_file():
                raise RuntimeError('Missing platform keyboard file: ' + name)
        component = source / 'vendor/amazon/pw3-hardware'
        manifest = json.loads((component / 'component-manifest.json').read_text())
        kernel_receipt = share / ('android-core-kernel' + model_suffix) / 'build-result.json'
        if not kernel_receipt.is_file():
            raise RuntimeError('Missing kernel receipt: ' + str(kernel_receipt) +
                               ' (build the kernel for --product ' + args.product + ' first)')
        kernel_hash = sha(kernel_receipt)
        if manifest['build_receipt_sha256'] != kernel_hash:
            raise RuntimeError('The image component uses a different kernel build')
        spec = importlib.util.spec_from_file_location(
            'pw3_series', port / 'tools/verify-tree-matches-series.py')
        series_check = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = series_check
        spec.loader.exec_module(series_check)
        patch_sources = dict(line.split() for line in (port / 'patches/projects.txt').read_text().splitlines()
                             if line.strip() and not line.startswith('#'))
        patches = {}
        for relative, series in patch_sources.items():
            files = sorted((port / 'patches' / series).glob('*.patch'))
            expected = [{'name': p.name, 'sha256': sha(p)} for p in files]
            recorded = json.loads((source / relative / '.pw3-applied-patches.json').read_text())
            if not expected or recorded != expected:
                raise RuntimeError('Patch ledger mismatch: ' + relative)
            # The ledger says which patches were applied, not that the tree holds
            # nothing else. A hunk dropped from a patch stays in the tree and is
            # recorded again, so the image would have code the published sources do
            # not make. Whoever rebuilds them has to get this image.
            problems = series_check.tree_matches_series(source / relative, files)
            if problems:
                raise RuntimeError('The tree is not its pinned revision plus the series: '
                                   + relative + '\n  ' + '\n  '.join(problems))
            patches[relative] = recorded
        device_files = {}
        # pw3 is always read: a second model inherits its product and the build
        # copies both device trees (tools/prepare-aosp-device.sh). The default
        # model keeps the keys it has always had; with a second one the model is
        # part of the key, so the two trees cannot hash into one set.
        models = ['pw3'] if args.product == 'pw3' else ['pw3', args.product]
        for model in models:
            root = port / 'device/amazon' / model
            for path in sorted(root.rglob('*')):
                if path.is_file():
                    relative = path.relative_to(root)
                    digest = sha(path)
                    copied = source / 'device/amazon' / model / relative
                    if not copied.is_file() or sha(copied) != digest:
                        raise RuntimeError('Device sources changed since synchronization: '
                                           + str(Path('device/amazon') / model / relative))
                    key = str(relative) if args.product == 'pw3' else 'device/amazon/%s/%s' % (model, relative)
                    device_files[key] = digest
        output.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='.pw3-image-export-', dir=output.parent) as temp:
            stage = Path(temp) / 'images'
            stage.mkdir()
            records = {}
            for name in ('system.img', 'ramdisk.img'):
                original = product / name
                digest = sha(original)
                shutil.copyfile(original, stage / name)
                if sha(stage / name) != digest:
                    raise RuntimeError('Image copy mismatch: ' + name)
                records[name] = {'bytes': original.stat().st_size, 'sha256': digest}
            subprocess.run([str(converter), str(stage / 'system.img'),
                            str(stage / 'system.raw.img')], check=True)
            raw = stage / 'system.raw.img'
            pack.ext4_info(raw)
            records[raw.name] = {'bytes': raw.stat().st_size, 'sha256': sha(raw)}
            raw.chmod(0o444)
            for name in ('component-manifest.json', 'build-verification.json'):
                shutil.copyfile(component / name, stage / name)
            # Whether the applications already in /data have to be compiled again
            # is ART's decision, and ART makes it from the boot image: an odex
            # records the image it was compiled against. So an upgrade makes the
            # first boot recompile the applications in /data only when these files
            # change, and the installer shows the screen that says so only then
            # (installer/README.md, "只在换了启动镜像时提示重编译").
            # The names go into the hash with the contents, so that one more file
            # or one fewer counts as a change.
            #
            # Read out of the image being exported, not out of the tree it was
            # staged from: the two can differ -- a staged file replaced, an older
            # image restored -- and then the hash would describe a boot image the
            # device is not getting, which is the one mistake this field must not
            # make.
            unpacked = Path(temp) / 'boot'
            unpacked.mkdir()
            subprocess.run(['debugfs', '-R', 'rdump /framework/arm ' + str(unpacked), str(raw)],
                           check=True, capture_output=True)
            boot_files = sorted((unpacked / 'arm').glob('boot*'))
            if not boot_files:
                raise RuntimeError('No boot image in the system image at /framework/arm')
            boot_image = hashlib.sha256()
            for path in boot_files:
                boot_image.update(('%s %s\n' % (path.name, sha(path))).encode())
            result = {
                'schema': 1, 'built_utc': job['recorded_utc'], 'build_completed': True,
                'build_log_sha256': sha(share / 'logs/aosp-system-build.log'),
                'kernel_receipt_sha256': kernel_hash, 'files': records,
                'boot_image': {'files': len(boot_files), 'sha256': boot_image.hexdigest()},
                # From the job receipt, because the files cannot say it: whether
                # the staged tree was cleared before this build, and which variant
                # it is. A release is a user build with the tree cleared
                # (docs/design/release-build.md).
                'variant': job.get('variant'), 'installclean': job.get('installclean'),
                # The value the image was stamped with: ro.build.date.utc and
                # the fingerprint built from it. Passing it back as
                # PW3_BUILD_DATETIME rebuilds an image holding the same files,
                # not the same bytes -- the ext4 container is not reproducible
                # either way, because the plain systemimage path hands
                # make_ext4fs no timestamp and it writes the time of the run.
                # The per-file sha256 manifest is what an image is checked
                # against.
                'build_datetime': job.get('build_datetime'),
                # What the image calls itself in Settings > About tablet > Build
                # number (ro.build.display.id). It is the only release name any
                # file in the image carries, and the one a reader can read out.
                'build_version': job.get('build_version'),
                'scope': 'AOSP %s-%s files; not installable firmware or a device boot'
                         % (args.product, job.get('variant') or 'userdebug'),
                'private_firmware_injected': False, 'ramdisk_structure_verified': True,
                'aosp_patches': patches,
                'device_source_sha256': device_files,
                'font_configuration_files_verified': len(fonts),
                'keyboard_files_verified': keyboard_files,
            }
            (stage / 'build-result.json').write_text(json.dumps(result, indent=2) + '\n')
            stage.rename(output)
        print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
