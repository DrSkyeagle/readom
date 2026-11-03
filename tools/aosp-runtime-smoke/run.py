#!/usr/bin/env python3
"""Run actual ARM ART/core libraries with the built image in isolated QEMU.

Uses a synthetic init rc and one VExpress block-device label. Does not boot
Android framework services or exercise PW3 hardware. Writes only RAM in guest.
"""
import argparse
import gzip
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import re
import shutil
import stat
import subprocess
import sys


def sha(path):
    with path.open('rb') as stream:
        digest = hashlib.sha256()
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()



def sd_overlay(image, output):
    """A disk QEMU's SD model will take, without touching the image.

    The vexpress machine's card refuses a size that is not a power of two
    ("Invalid SD card size: 576 MiB"), which is what this project's system
    image was until 2026-11-02; it is 512 MiB now, which is a power of two, so
    the rounding below is a no-op for it. The overlay is kept either way: a
    qcow2 overlay with the image as its backing file reads as the image, pads
    the rest with zeros, and takes every write itself, so the file the build
    made is still read-only and still exactly what the guest mounts -- and the
    size is read from the file, so neither this nor the next change to it needs
    anything here.
    """
    size = image.stat().st_size
    rounded = 1
    while rounded < size:
        rounded *= 2
    overlay = output / 'sd-overlay.qcow2'
    subprocess.run(['qemu-img', 'create', '-q', '-f', 'qcow2', '-b', str(image.resolve()),
                    '-F', 'raw', str(overlay), str(rounded)], check=True)
    return overlay

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--system-image', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--timeout', type=int, default=120)
    parser.add_argument('--check-strings', action='store_true',
                        help='Also exercise Bionic string copies at alignment and page boundaries')
    args = parser.parse_args()
    assert platform.node() == 'lima-pw3' and os.geteuid() != 0
    assert args.system_image.is_file() and not args.system_image.is_symlink()
    assert 30 <= args.timeout <= 600
    args.output.mkdir(exist_ok=False)
    args.output = args.output.resolve()
    here = Path(__file__).resolve().parent
    shutil.copyfile(Path(__file__), args.output / 'harness.py')
    spec = importlib.util.spec_from_file_location('pw3_pack', here.parent / 'preinit-pack/pack.py')
    pack = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = pack
    spec.loader.exec_module(pack)
    image_info = pack.ext4_info(args.system_image)
    export = args.system_image.resolve().parent
    image_record = json.loads((export / 'build-result.json').read_text())
    assert image_record['build_completed'] and not image_record['private_firmware_injected']
    assert image_record['files'][args.system_image.name]['sha256'] == image_info['sha256']
    original_ramdisk = export / 'ramdisk.img'
    assert sha(original_ramdisk) == image_record['files']['ramdisk.img']['sha256']
    product = Path('/home/builder/android-src/out/target/product/pw3')
    kernel_root = Path('/mnt/build-share/aosp-runtime-kernel')
    record = json.loads((kernel_root / 'build-result.json').read_text())
    for name, digest in record['sha256'].items():
        assert sha(kernel_root / name) == digest, name
    java = Path('/usr/lib/jvm/java-8-openjdk-arm64/bin')
    dex = Path('/home/builder/android-src/prebuilts/sdk/tools/lib/dx.jar')
    source = args.output / 'ArtSmoke.java'
    shutil.copyfile(here / source.name, source)
    subprocess.run([str(java / 'javac'), '-encoding', 'UTF-8', '-source', '7', '-target', '7',
                    '-d', str(args.output), str(source)], check=True)
    jar = args.output / 'art-smoke.jar'
    subprocess.run([str(java / 'java'), '-jar', str(dex), '--dex', '--output=' + str(jar),
                    *(p.name for p in sorted(args.output.glob('*.class')))],
                   cwd=args.output, check=True)
    string_test = None
    if args.check_strings:
        string_source = args.output / 'string-smoke.c'
        string_binary = args.output / 'string-smoke'
        shutil.copyfile(here / string_source.name, string_source)
        aosp = Path('/home/builder/android-src')
        compiler = aosp / 'prebuilts/gcc/linux-x86/arm/arm-linux-androideabi-4.9/bin/arm-linux-androideabi-gcc'
        command = ['python3', str(compiler),
                   '--sysroot=' + str(aosp / 'prebuilts/ndk/r10/platforms/android-21/arch-arm'),
                   '-std=gnu11', '-fPIE', '-pie', '-fno-builtin', '-march=armv7-a',
                   '-mthumb', '-O1', '-Wall', '-Wextra', '-Werror',
                   str(string_source), '-o', str(string_binary)]
        subprocess.run(command, check=True)
        string_test = {'command': command, 'source_sha256': sha(string_source),
                       'binary_sha256': sha(string_binary), 'compiler_wrapper_sha256': sha(compiler),
                       'compiler_sha256': sha(compiler.with_name('real-arm-linux-androideabi-gcc'))}
    entries = pack.read_ramdisk(original_ramdisk)
    original = pack.verify_ramdisk(entries)
    assert 'adb_keys' not in original and not any(e.name.startswith('firmware/') for e in entries)
    context = original['nonplat_file_contexts']
    context_before = hashlib.sha256(context.data).hexdigest()
    context.data += b'\n/dev/block/mmcblk0 u:object_r:system_block_device:s0\n'
    script = b'''#!/system/bin/sh
export ANDROID_DATA=/data/local/tmp/art-data
sleep 1
/system/bin/logcat -b all -v brief &
logger=$!
trap 'kill "$logger" 2>/dev/null; wait "$logger" 2>/dev/null' EXIT
/system/bin/log -p i -t PW3_LOGGER_READY ready || exit 93
echo ART_SELINUX_ENFORCING=$(cat /sys/fs/selinux/enforce)
case "$(ls -Z /sys/devices/system/cpu/online)" in
    *u:object_r:sysfs_devices_system_cpu:s0*) echo ART_CPU_GENFS_LABEL=1 ;;
    *) echo ART_CPU_GENFS_LABEL=0; exit 91 ;;
esac
cat /sys/devices/system/cpu/online >/dev/null || exit 92
if grep -q ' /system ext4 ro,' /proc/mounts; then
    echo ART_SYSTEM_READONLY=1
else
    echo ART_SYSTEM_READONLY=0
    exit 90
fi
mkdir -p "$ANDROID_DATA/dalvik-cache"
/system/bin/dalvikvm -Xms8m -Xmx32m -cp /data/nativetest/art-smoke.jar ArtSmoke /data/local/tmp
status=$?
sleep 1
echo ART_SMOKE_EXIT=$status
exit $status
'''
    rc = b'''import /init.environ.rc

on early-init
    mount cgroup none /acct cpuacct
    mkdir /acct/uid
    start ueventd

service ueventd /sbin/ueventd
    class core
    critical
    seclabel u:r:ueventd:s0

on init
    symlink /system/etc /etc
    symlink /system/vendor /vendor
    symlink /proc/self/fd /dev/fd
    write /proc/sys/kernel/randomize_va_space 2
    trigger fs
    trigger post-fs-data
    trigger boot

on fs
    wait /dev/block/mmcblk0 10
    mount ext4 /dev/block/mmcblk0 /system ro nodev noload
    mount tmpfs tmpfs /data nosuid nodev mode=0771,uid=1000,gid=1000,size=128m
    restorecon /data

on post-fs-data
    load_system_props
    mkdir /data/local 0751 root root
    mkdir /data/local/tmp 0771 shell shell
    mkdir /data/nativetest 0755 root root
    copy /art-smoke.jar /data/nativetest/art-smoke.jar
    copy /art-smoke.sh /data/nativetest/art-smoke.sh
    chmod 0644 /data/nativetest/art-smoke.jar
    chmod 0644 /data/nativetest/art-smoke.sh
    restorecon_recursive /data
    restorecon_recursive /sys

on boot
    # The actual product init.pw3-main.rc applies this standard Android setting.
    write /proc/sys/vm/overcommit_memory 1
    write /dev/kmsg PW3_AOSP_ART_SMOKE_START
    start logd
    setprop pw3.qemu.art_started 1
    start art-smoke

service art-smoke /system/bin/sh /data/nativetest/art-smoke.sh
    user shell
    group shell log readproc
    seclabel u:r:shell:s0
    console
    disabled
    oneshot

on property:init.svc.art-smoke=stopped && property:pw3.qemu.art_started=1
    setprop sys.powerctl shutdown
'''
    inspection = json.loads((export / 'filesystem-inspection.json').read_text())
    assert inspection['inspection_completed'] and inspection['image']['sha256'] == image_info['sha256']
    inspected = {entry['path']: entry for entry in inspection['entries']}
    logd_rc = (product / 'system/etc/init/logd.rc').read_bytes()
    assert hashlib.sha256(logd_rc).hexdigest() == inspected['/system/etc/init/logd.rc']['sha256']
    rc += b'\n' + logd_rc
    if args.check_strings:
        script = script.replace(b'/system/bin/dalvikvm -Xms8m',
                                b'/data/nativetest/string-smoke || exit $?\n/system/bin/dalvikvm -Xms8m')
        rc = rc.replace(b'    copy /art-smoke.jar',
                        b'    copy /string-smoke /data/nativetest/string-smoke\n'
                        b'    chmod 0755 /data/nativetest/string-smoke\n    copy /art-smoke.jar')
        entries.append(pack.Entry('string-smoke', stat.S_IFREG | 0o755,
                                  data=string_binary.read_bytes()))
    for name, data, mode in [('art-smoke.jar', jar.read_bytes(), 0o644),
                             ('art-smoke.sh', script, 0o644), ('init.qemu-art.rc', rc, 0o644)]:
        assert name not in original
        entries.append(pack.Entry(name, stat.S_IFREG | mode, data=data))
    ramdisk = args.output / 'initramfs.cpio.gz'
    ramdisk.write_bytes(gzip.compress(pack.cpio_bytes(entries), mtime=0))
    inputs = {'schema': 1, 'system_image': image_info, 'kernel': record,
              'ramdisk_original_sha256': sha(original_ramdisk),
              'image_export_receipt_sha256': sha(export / 'build-result.json'),
              'ramdisk_test_sha256': sha(ramdisk), 'java_source_sha256': sha(source),
              'dx_sha256': sha(dex), 'dex_jar_sha256': sha(jar),
              'harness_sha256': sha(args.output / 'harness.py'),
              'logd_rc_sha256': hashlib.sha256(logd_rc).hexdigest(),
              'policy_sha256': hashlib.sha256(original['sepolicy'].data).hexdigest(),
              'nonplat_file_contexts_original_sha256': context_before,
              'fixture_changes': ['Synthetic init rc', 'VExpress mmcblk0 system-block label',
                                  'Test Java DEX and shell runner'],
              'physical_device_access': False, 'android_framework_tested': False}
    inputs['optional_string_test'] = string_test
    inputs['drive_scope'] = 'QEMU SD takes a qcow2 overlay padded to a power of two; the image is its read-only backing file'
    (args.output / 'inputs.json').write_text(json.dumps(inputs, indent=2) + '\n')
    command = ['timeout', str(args.timeout) + 's', 'qemu-system-arm', '-machine', 'vexpress-a9',
               '-cpu', 'cortex-a9', '-m', '512', '-smp', '1', '-nodefaults', '-display', 'none',
               '-monitor', 'none', '-serial', 'stdio', '-nic', 'none', '-no-reboot',
               '-kernel', str(kernel_root / 'zImage'), '-initrd', str(ramdisk),
               '-drive', 'file=' + str(sd_overlay(args.system_image, args.output)) + ',if=sd,format=qcow2',
               '-append', 'console=ttyAMA0 rdinit=/init androidboot.hardware=pw3 '
                          'androidboot.init_rc=/init.qemu-art.rc panic=1 loglevel=7']
    (args.output / 'qemu-command.json').write_text(json.dumps(command, indent=2) + '\n')
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            env={**os.environ, 'QEMU_AUDIO_DRV': 'none'})
    log = result.stdout.decode(errors='replace')
    (args.output / 'console.log').write_bytes(result.stdout)
    java_checks_passed = 'PW3_AOSP_ART_SMOKE_PASS checks=12' in log and 'ART_SMOKE_EXIT=0' in log
    child_crash = 'Fatal signal' in log or 'Runtime aborting' in log or 'FATAL EXCEPTION' in log or re.search(r'(?:^|[\r\n])F/[^\r\n]*\(\s*\d+\):', log) is not None
    image_fallback = 'Attempting to fall back to imageless running' in log
    logs_captured = re.search(r'I/PW3_LOGGER_READY\s*\(\s*\d+\): ready', log) is not None
    passed = result.returncode == 0 and all(marker in log for marker in
        ('PW3_AOSP_ART_SMOKE_PASS checks=12', 'ART_SELINUX_ENFORCING=1',
         'ART_CPU_GENFS_LABEL=1', 'ART_SYSTEM_READONLY=1', 'ART_SMOKE_EXIT=0'))
    passed = passed and not any(marker in log for marker in ('Kernel panic', 'Oops:', 'BUG:'))
    passed = passed and not child_crash
    passed = passed and not image_fallback
    passed = passed and logs_captured
    strings_passed = 'PW3_STRING_SMOKE_PASS cases=29376' in log
    passed = passed and (not args.check_strings or strings_passed)
    unchanged = sha(args.system_image) == image_info['sha256']
    passed = passed and unchanged
    summary = {'schema': 1, 'passed': passed, 'qemu_exit': result.returncode,
               'java_checks_passed': java_checks_passed, 'child_crash_detected': child_crash,
               'android_logs_captured': logs_captured,
               'boot_image_fallback_detected': image_fallback,
               'system_image_unchanged': unchanged, 'system_image_sha256': image_info['sha256'],
               'console_sha256': sha(args.output / 'console.log'),
               'avc_log_lines': sum(bool(re.search(r'avc:\s+denied', line)) for line in log.splitlines()),
               'scope': 'Actual ARM ART/core libraries with test DEX, test rc, RAM data and read-only system image',
               'android_framework_tested': False, 'pw3_hardware_tested': False}
    summary['string_contract_cases_passed'] = 29376 if args.check_strings and strings_passed else None
    (args.output / 'result.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(log[-18000:])
    print(json.dumps(summary, indent=2))
    if not passed:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
