#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""The handoff module end to end in QEMU (vexpress-a9), in the build VM: a first
Linux loads the module, is refused what breaks the build's pins or rules, then
jumps to a second Linux whose initramfs is the public archive with a device
archive appended, as on the Kindle.

The second Linux is the RAM-only observer (build-share/minimal-linux-observer-
qemu-v2). Its 4 MiB FAT image /pw3/report.fat is moved from the public archive
into the device archive: the observer reports only if the kernel unpacked the
appended archive. No device, disk or network is involved.

Usage: run_qemu.py --output DIR
"""
import argparse, gzip, hashlib, importlib.util, json, os, platform, re, stat, struct, subprocess, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
BOOT = HERE.parent.parent
SHARE = Path('/mnt/build-share')
MUSL = '/home/builder/toolchains/musl-arm/bin/musl-gcc'
BASE = 0x60000000
CMDLINE = 'console=ttyAMA0 rdinit=/init panic=1 loglevel=7'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def tag(kind, payload):
    payload += bytes(-len(payload) % 4)
    return struct.pack('<II', 2 + len(payload) // 4, kind) + payload


def atags(cmdline, initrd_bytes, extra=b''):
    return (tag(0x54410001, struct.pack('<III', 1, 4096, 0)) + tag(0x54410002, struct.pack('<II', 0x20000000, BASE)) + extra +
            tag(0x54410009, cmdline.encode() + b'\0') + tag(0x54420005, struct.pack('<II', BASE + (64 << 20), initrd_bytes)) + bytes(8))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--output', type=Path, required=True)
    a = ap.parse_args()
    if platform.node() != (os.environ.get('READOM_BUILD_HOST') or 'lima-pw3') or os.geteuid() == 0:
        sys.exit('Run as the builder in the build VM.')
    if subprocess.run(['pgrep', '-x', 'qemu-system-arm'], stdout=subprocess.DEVNULL).returncode == 0:
        sys.exit('Another qemu-system-arm is running.')
    out = a.output
    out.mkdir(parents=True, exist_ok=False)
    spec = importlib.util.spec_from_file_location('pack', BOOT.parent / 'tools/preinit-pack/pack.py')
    pack = importlib.util.module_from_spec(spec); sys.modules['pack'] = pack; spec.loader.exec_module(pack)

    # Payloads: the build's kernel and public initramfs; the device's archive and ATAGs.
    kernel = (SHARE / 'loader-stage-kernel/zImage').read_bytes()
    observer = pack.read_ramdisk(SHARE / 'minimal-linux-observer-qemu-v2/initramfs.cpio.gz')
    moved = [e for e in observer if e.name == 'pw3/report.fat']
    assert len(moved) == 1 and len(moved[0].data) == 4 << 20
    public = gzip.compress(pack.cpio_bytes([e for e in observer if e.name != 'pw3/report.fat']), mtime=0)
    device = pack.cpio_bytes(moved)
    assert device[:6] == b'070701' and b'TRAILER!!!' in device[-640:]
    slot = (len(public) + 3) & ~3
    good = atags(CMDLINE, slot + len(device))
    files = {
        'target-zImage': kernel, 'target-public-initramfs': public, 'target-device-archive': device, 'target-atags': good,
        # A tag the build does not allow (SERIAL16), another command line, and ATAGs for the public part alone.
        'target-atags-extra-tag': atags(CMDLINE, slot + len(device), tag(0x5441000a, b'0123456789abcdef')),
        'target-atags-other-cmdline': atags(CMDLINE + ' init=/bin/sh', slot + len(device)),
        'target-atags-public-only': atags(CMDLINE, len(public)),
    }
    (out / 'cmdline.txt').write_text(CMDLINE + '\n')
    (out / 'public-initramfs.cpio.gz').write_bytes(public)

    # The module, pinned to this kernel, public initramfs and command line.
    subprocess.run([sys.executable, str(BOOT / 'build_module.py'), str(BOOT / 'profiles/qemu-vexpress.json'),
                    '--kernel', str(SHARE / 'loader-stage-kernel/zImage'), '--public-initramfs', str(out / 'public-initramfs.cpio.gz'),
                    '--cmdline', str(out / 'cmdline.txt'), '--output', str(out / 'module')], check=True)

    # The first Linux's PID 1: the module tests, then the jump.
    flags = [MUSL, '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror', '-I', str(HERE.parent)]
    for name, entry in [('test-engine-guard.c', 'pws_engine_guard_test'), ('test-identity-attrs.c', 'pws_identity_attrs_test'),
                        ('test-allocation-ranges.c', 'pws_allocation_ranges_test')]:
        subprocess.run(flags + ['-Dmain=' + entry, '-c', str(HERE / name), '-o', str(out / (name + '.o'))], check=True)
    subprocess.run([MUSL, '-std=gnu99', '-static', '-O2', '-Wall', '-Wextra', '-Werror', '-I', str(HERE.parent),
                    str(HERE / 'test-init.c'), str(HERE.parent / 'sha256.c'), str(HERE.parent / 'pw3_plan.c')] +
                   [str(out / (n + '.o')) for n in ['test-engine-guard.c', 'test-identity-attrs.c', 'test-allocation-ranges.c']] +
                   ['-o', str(out / 'init')], check=True)
    entries = [pack.Entry('.', stat.S_IFDIR | 0o755), pack.Entry('dev', stat.S_IFDIR | 0o755),
               pack.Entry('dev/console', stat.S_IFCHR | 0o600, rdevmajor=5, rdevminor=1),
               pack.Entry('proc', stat.S_IFDIR | 0o555), pack.Entry('sys', stat.S_IFDIR | 0o555),
               pack.Entry('init', stat.S_IFREG | 0o755, data=(out / 'init').read_bytes())]
    for name in ['pw3_first_kernel.ko', 'pw3_qemu_pm.ko']:
        entries.append(pack.Entry(name, stat.S_IFREG | 0o600, data=(out / 'module' / name).read_bytes()))
    for name, data in files.items():
        entries.append(pack.Entry(name, stat.S_IFREG | 0o600, data=data))
    first = out / 'first.cpio.gz'
    first.write_bytes(gzip.compress(pack.cpio_bytes(entries), mtime=0))

    command = ['timeout', '-k', '3s', '120s', 'qemu-system-arm', '-machine', 'vexpress-a9', '-cpu', 'cortex-a9', '-m', '512',
               '-smp', '1', '-nodefaults', '-display', 'none', '-monitor', 'none', '-serial', 'stdio', '-nic', 'none',
               '-no-reboot', '-kernel', str(SHARE / 'loader-stage-kernel/zImage'), '-initrd', str(first),
               '-append', 'console=ttyAMA0 rdinit=/init panic=1 loglevel=7 pw3_virtual_stage=first']
    with (out / 'console.log').open('wb') as log:
        done = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, env={**os.environ, 'QEMU_AUDIO_DRV': 'none'})
    text = (out / 'console.log').read_text(errors='replace')
    first_ok = re.search(r'PW3_FIRST_KERNEL_READY checks=(\d+) native_core_roundtrips=4 rejection_checks=8', text)
    second_ok = re.search(r'PW3_MINIMAL_BOOT_RAM_REPORT_CREATED checks=(\d+)', text)
    boots = text.count('Linux version 3.0.35-pw3-core-smoke ')
    passed = (done.returncode == 0 and bool(first_ok and second_ok) and boots == 2 and
              'PW3_ENGINE_GUARD_PASS checks=73 observed_fixture=1' in text and 'PW3_IDENTITY_ATTRS_PASS checks=14' in text and
              'PW3_ALLOCATION_RANGES_PASS checks=' in text and 'Initramfs unpacking failed' not in text and
              not re.search(r'FAIL|Kernel panic|Oops:|PW3_HANDOFF_RETURN', text))
    result = {'schema': 1, 'passed': passed, 'qemu_exit': done.returncode, 'kernel_boots': boots,
              'first_kernel_checks': int(first_ok[1]) if first_ok else None,
              'second_kernel_checks': int(second_ok[1]) if second_ok else None,
              'public_initramfs_bytes': len(public), 'device_archive_bytes': len(device), 'console_sha256': sha(text.encode())}
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    print(text[-3000:])
    sys.exit(0 if passed else 1)


if __name__ == '__main__':
    main()
