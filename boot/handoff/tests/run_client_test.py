#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""The launcher and its helper on ARM, under the stock shell, on a FAT disk.

What a tap on the Kindle runs is a shell script read by the stock BusyBox, which
unpacks a static helper out of itself and runs it. This boots that pair in QEMU
(vexpress-a9) with the device's own shell and libc as the fixture and a
disposable FAT image as the user store, and checks what the helper leaves
behind: the kernel it refuses to run under is refused by name
(`check=pm_test_sysfs_available`), no module is loaded, both workspaces under
/tmp are gone, no result file is written, the launcher records the helper's exit
code, and every file it did write is listed in its own SHA256SUMS.

It needs the virtual profile of the launcher (boot/profiles/qemu-vexpress.json,
CHECK_PROFILE=2 in launcher/helper.c): the stock profile refuses to start on
anything but the Kindle's own kernel and firmware, which is the point of it.

The fixture is the device owner's: their shell, their libc. Nothing here is
publishable, which is what `private` in the result says.

Given neither --launcher nor --module it builds both into its output directory,
from the loader-stage kernel and the RAM-only observer, so that running it is
one command and the launcher under test is always the current one. That is how
it came to be dark for weeks: it took two directories somebody had to make
first, and the builder that made them was retired without anyone noticing.

Usage: run_client_test.py --output DIR [--launcher DIR --module DIR]
  --launcher  build_launcher.py output for the qemu profile (its module is --module)
  --module    build_module.py output for boot/profiles/qemu-vexpress.json
"""
import argparse
import gzip
import hashlib
import importlib.util
import json
import os
import platform
import stat
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
KERNEL = Path('/mnt/build-share/loader-stage-kernel')
FIXTURE = Path('/home/builder/early-observe/private-fixture/root')
MUSL = '/home/builder/toolchains/musl-arm/bin/musl-gcc'
# The stock system's own shell and the libraries it needs, as the test's
# /bin and /lib: the launcher is a script for this shell and no other.
STOCK = ['bin/busybox', 'bin/sh', 'lib/ld-linux.so.3', 'lib/ld-2.20.so',
         'lib/libc.so.6', 'lib/libc-2.20.so', 'lib/libm.so.6', 'lib/libm-2.20.so']
# What this test is of, so that a change to any of it is a change to the
# recorded inputs rather than a silent one.
SOURCES = ['boot/launcher/helper.c', 'boot/launcher/launcher-template.sh',
           'boot/handoff/tests/client-test-init.c', 'boot/handoff/tests/run_client_test.py']
OBSERVER = Path('/mnt/build-share/minimal-linux-observer-qemu-v2/initramfs.cpio.gz')
CMDLINE = 'console=ttyAMA0 rdinit=/init panic=1 loglevel=6'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def build_inputs(out):
    """The qemu-profile module and launcher, for a run that was given neither.

    The module pins a kernel, a public initramfs and a command line, and this
    test never gets as far as loading any of them -- the helper is refused by
    the kernel it is running under long before that. So the initramfs is the
    observer's archive as it stands and the configuration is the smallest one
    build_launcher.py accepts: fixtures, in the places where a release build
    puts the image it is really installing.
    """
    public = out / 'public-initramfs.cpio.gz'
    public.write_bytes(OBSERVER.read_bytes())
    cmdline = out / 'cmdline.txt'
    cmdline.write_text(CMDLINE + '\n')
    conf = out / 'pw3-preinit.conf'
    conf.write_text(''.join('%s=%s\n' % pair for pair in [
        ('schema', 1), ('system_path', 'android/system.img'), ('system_size', 4096),
        ('system_sha256', sha(b'')), ('init_sha256', sha(b'')),
        ('cmdline_sha256', sha(CMDLINE.encode())), ('kernel_release', 'virtual-profile-test'),
        ('adb_key_sha256', 'none')]))
    module, launcher = out / 'module', out / 'launcher'
    subprocess.run([sys.executable, str(ROOT / 'boot/build_module.py'),
                    str(ROOT / 'boot/profiles/qemu-vexpress.json'),
                    '--kernel', str(KERNEL / 'zImage'), '--public-initramfs', str(public),
                    '--cmdline', str(cmdline), '--output', str(module)],
                   check=True, stdout=subprocess.DEVNULL)
    subprocess.run([sys.executable, str(ROOT / 'boot/build_launcher.py'),
                    '--module', str(module), '--kernel', str(KERNEL / 'zImage'),
                    '--public-initramfs', str(public), '--cmdline', str(cmdline),
                    '--preinit-conf', str(conf), '--name', 'PW3-First-Kernel.sh',
                    '--title', 'First Kernel', '--output', str(launcher)],
                   check=True, stdout=subprocess.DEVNULL)
    return launcher, module


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--launcher', type=Path)
    ap.add_argument('--module', type=Path)
    ap.add_argument('--output', type=Path, required=True)
    a = ap.parse_args()
    if platform.node() != 'lima-pw3' or os.geteuid() == 0:
        sys.exit('Run as the builder in the build VM.')
    if subprocess.run(['pgrep', '-x', 'qemu-system-arm'], stdout=subprocess.DEVNULL).returncode == 0:
        sys.exit('Another qemu-system-arm is running.')
    if (a.launcher is None) != (a.module is None):
        sys.exit('--launcher and --module come together, or neither does')

    os.umask(0o077)
    out = a.output
    out.mkdir(mode=0o700, parents=True, exist_ok=False)
    if a.launcher is None:
        a.launcher, a.module = build_inputs(out)

    # The launcher, its module and the kernel all have to be the virtual ones,
    # and each has to be the file its own receipt describes.
    module_receipt = (a.module / 'build-result.json').read_bytes()
    module = json.loads(module_receipt)
    if module['module_profile'] != 2:
        sys.exit('--module is not the qemu profile: %r' % module['module_profile'])
    built = json.loads((a.launcher / 'build-result.json').read_text())
    if built['module_receipt_sha256'] != sha(module_receipt):
        sys.exit('--launcher was built from another module')
    if built['installer'] is not None:
        sys.exit('--launcher is a release entry; this test is of the virtual profile')
    launcher = a.launcher / built['launcher']
    helper = a.launcher / 'helper'
    if sha(launcher.read_bytes()) != built['launcher_sha256'] or sha(helper.read_bytes()) != built['helper_sha256']:
        sys.exit('the launcher directory does not hold the files its receipt describes')
    kernel = json.loads((KERNEL / 'build-result.json').read_text())
    for name, value in kernel['sha256'].items():
        if sha((KERNEL / name).read_bytes()) != value:
            sys.exit('the loader-stage kernel export does not match its receipt: ' + name)

    command = [MUSL, '-std=gnu11', '-static', '-O2', '-Wall', '-Wextra', '-Werror',
               str(HERE / 'client-test-init.c'), '-o', str(out / 'init')]
    with (out / 'compile.log').open('w') as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)

    # The user store: a FAT16 image, attached read-only to the emulator
    # (snapshot=on), so what the helper writes is thrown away with the run and
    # this file can be compared afterwards.
    disk = out / 'usb.fat'
    with disk.open('wb') as f:
        f.truncate(32 * 1024 * 1024)
    subprocess.run(['/usr/sbin/mkfs.vfat', '-F', '16', '-n', 'DRVCHECK', str(disk)],
                   check=True, capture_output=True)

    spec = importlib.util.spec_from_file_location('pack', ROOT / 'tools/preinit-pack/pack.py')
    pack = importlib.util.module_from_spec(spec)
    sys.modules['pack'] = pack
    spec.loader.exec_module(pack)
    entries = [pack.Entry('.', stat.S_IFDIR | 0o755), pack.Entry('dev', stat.S_IFDIR | 0o755),
               pack.Entry('dev/console', stat.S_IFCHR | 0o600, rdevmajor=5, rdevminor=1)]
    for name, path in [('init', out / 'init'), ('helper', helper), ('launcher.sh', launcher)]:
        entries.append(pack.Entry(name, stat.S_IFREG | 0o700, data=path.read_bytes()))
    stock = {}
    for name in ['bin', 'lib']:
        entries.append(pack.Entry(name, stat.S_IFDIR | 0o755))
    for name in STOCK:
        path = FIXTURE / name
        if path.is_symlink():
            entries.append(pack.Entry(name, stat.S_IFLNK | 0o777, data=os.readlink(path).encode()))
            stock[name] = {'symlink': os.readlink(path)}
        else:
            entries.append(pack.Entry(name, stat.S_IFREG | 0o755, data=path.read_bytes()))
            stock[name] = {'sha256': sha(path.read_bytes())}
    ramdisk = out / 'initramfs.cpio.gz'
    ramdisk.write_bytes(gzip.compress(pack.cpio_bytes(entries), mtime=0))

    inputs = {'schema': 2, 'private': True, 'publication_allowed': False,
              'launcher': built['launcher'], 'launcher_sha256': built['launcher_sha256'],
              'helper_sha256': built['helper_sha256'],
              'module_receipt_sha256': built['module_receipt_sha256'],
              'kernel_sha256': sha((KERNEL / 'zImage').read_bytes()),
              'ramdisk_sha256': sha(ramdisk.read_bytes()), 'disk_sha256': sha(disk.read_bytes()),
              'stock_shell_fixture': stock,
              'source_sha256': {name: sha((ROOT / name).read_bytes()) for name in SOURCES},
              'physical_device_access': False}
    (out / 'inputs.json').write_text(json.dumps(inputs, indent=2) + '\n')

    qemu = ['timeout', '-k', '3s', '180s', 'qemu-system-arm', '-machine', 'vexpress-a9',
            '-cpu', 'cortex-a9', '-m', '512', '-smp', '1', '-nodefaults', '-display', 'none',
            '-monitor', 'none', '-serial', 'stdio', '-nic', 'none', '-no-reboot',
            '-kernel', str(KERNEL / 'zImage'), '-initrd', str(ramdisk),
            '-append', 'console=ttyAMA0 rdinit=/init panic=1 loglevel=6',
            '-drive', 'if=sd,format=raw,snapshot=on,file=' + str(disk)]
    (out / 'qemu-command.json').write_text(json.dumps(qemu, indent=2) + '\n')
    with (out / 'console.log').open('wb') as log:
        done = subprocess.run(qemu, stdout=log, stderr=subprocess.STDOUT,
                              env={**os.environ, 'QEMU_AUDIO_DRV': 'none'})
    text = (out / 'console.log').read_text(errors='replace')
    passed_line = 'PW3_FIRST_KERNEL_CLIENT_TEST_PASS checks='
    found = [line for line in text.splitlines() if line.startswith(passed_line)]
    unchanged = sha(disk.read_bytes()) == inputs['disk_sha256']
    passed = (done.returncode == 0 and len(found) == 1 and unchanged and
              not any(mark in text for mark in
                      ['PW3_FIRST_KERNEL_CLIENT_TEST_FAIL', 'Kernel panic', 'Oops:', 'BUG:']))
    result = {'schema': 2, 'passed': passed, 'qemu_exit': done.returncode,
              'checks': int(found[0][len(passed_line):]) if found else None,
              'source_disk_unchanged': unchanged, 'console_sha256': sha(text.encode()),
              'scope': 'Actual ARM refusal of the kernel it may not run under, the stock '
                       'shell unpacking and running the helper, its logging, its SHA256SUMS '
                       'and the cleanup of both workspaces, on a disposable FAT image. No '
                       'handoff is executed and no device is touched.',
              'physical_device_tested': False, 'network': False}
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    print(text[-5000:])
    raise SystemExit(0 if passed else 1)


if __name__ == '__main__':
    main()
