#!/usr/bin/env python3
"""Check actual Zygote preloading and command-loop readiness in isolated QEMU.

Uses a synthetic init rc and one VExpress block-device label. Does not start SystemServer, fork an application, or exercise PW3 hardware. Writes only RAM in guest.
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
    assert image_record['files'][args.system_image.name]['sha256'] == image_info['sha256']
    original_ramdisk = export / 'ramdisk.img'
    assert sha(original_ramdisk) == image_record['files']['ramdisk.img']['sha256']
    product = Path('/home/builder/android-src/out/target/product/pw3')
    kernel_root = Path('/mnt/build-share/aosp-runtime-kernel')
    record = json.loads((kernel_root / 'build-result.json').read_text())
    for name, digest in record['sha256'].items():
        assert sha(kernel_root / name) == digest, name
    entries = pack.read_ramdisk(original_ramdisk)
    original = pack.verify_ramdisk(entries)
    assert 'adb_keys' not in original and not any(e.name.startswith('firmware/') for e in entries)
    context = original['nonplat_file_contexts']
    context_before = hashlib.sha256(context.data).hexdigest()
    context.data += b'\n/dev/block/mmcblk0 u:object_r:system_block_device:s0\n'
    script = b'''#!/system/bin/sh
sleep 1
/system/bin/logcat -b all -v brief &
logger=$!
trap 'kill "$logger" 2>/dev/null; wait "$logger" 2>/dev/null' EXIT
/system/bin/log -p i -t PW3_LOGGER_READY ready || exit 93
echo SERVICE_SELINUX_ENFORCING=$(cat /sys/fs/selinux/enforce)
case "$(ls -Z /sys/devices/system/cpu/online)" in
    *u:object_r:sysfs_devices_system_cpu:s0*) echo SERVICE_CPU_GENFS_LABEL=1 ;;
    *) exit 91 ;;
esac
grep -q ' /system ext4 ro,' /proc/mounts || exit 90
echo SERVICE_SYSTEM_READONLY=1
for attempt in $(seq 1 75); do
    if logcat -b main -d -s Zygote:I '*:S' | grep -q 'Accepting command socket connections'; then
        test "$(getprop init.svc.zygote)" = running || exit 95
        sleep 2
        test "$(getprop init.svc.zygote)" = running || exit 98
        echo PW3_ZYGOTE_PRELOAD_PASS
        echo SERVICE_SMOKE_EXIT=0
        exit 0
    fi
    test "$(getprop init.svc.zygote)" != restarting || exit 97
    sleep 1
done
echo PW3_ZYGOTE_PRELOAD_TIMEOUT
exit 96
'''
    rc = b'''import /init.environ.rc

on early-init
    mount cgroup none /acct cpuacct
    mkdir /dev/cpuset
    mount cpuset none /dev/cpuset
    mkdir /dev/cpuset/foreground
    copy /dev/cpuset/cpus /dev/cpuset/foreground/cpus
    copy /dev/cpuset/mems /dev/cpuset/foreground/mems
    mkdir /dev/cpuset/system-background
    copy /dev/cpuset/cpus /dev/cpuset/system-background/cpus
    copy /dev/cpuset/mems /dev/cpuset/system-background/mems
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
    setrlimit 13 40 40
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
    mkdir /data/dalvik-cache 0771 root root
    mkdir /data/dalvik-cache/arm 0771 root root
    mkdir /data/misc 01771 system misc
    mkdir /data/misc/keystore 0700 keystore keystore
    mkdir /data/misc/gatekeeper 0700 system system
    mkdir /data/local 0751 root root
    mkdir /data/local/tmp 0771 shell shell
    mkdir /data/nativetest 0755 root root
    copy /service-smoke.sh /data/nativetest/service-smoke.sh
    chmod 0644 /data/nativetest/service-smoke.sh
    restorecon_recursive /data
    restorecon_recursive /sys

on boot
    # The actual product init.pw3-main.rc applies this standard Android setting.
    write /proc/sys/vm/overcommit_memory 1
    write /dev/kmsg PW3_AOSP_SERVICE_SMOKE_START
    start logd
    start servicemanager
    start hwservicemanager
    start keymaster-3-0
    start gatekeeperd
    start keystore
    start zygote
    setprop pw3.qemu.art_started 1
    start service-smoke

service service-smoke /system/bin/sh /data/nativetest/service-smoke.sh
    user shell
    group shell log readproc
    seclabel u:r:shell:s0
    console
    disabled
    oneshot

on property:init.svc.service-smoke=stopped && property:pw3.qemu.art_started=1
    setprop sys.powerctl shutdown
'''
    inspection = json.loads((export / 'filesystem-inspection.json').read_text())
    assert inspection['inspection_completed'] and inspection['image']['sha256'] == image_info['sha256']
    inspected = {entry['path']: entry for entry in inspection['entries']}
    service_rcs = {}
    for name in ['logd', 'servicemanager', 'hwservicemanager', 'gatekeeperd', 'keystore']:
        content = (product / ('system/etc/init/' + name + '.rc')).read_bytes()
        service_rcs[name] = hashlib.sha256(content).hexdigest()
        assert service_rcs[name] == inspected['/system/etc/init/' + name + '.rc']['sha256']
        rc += b'\n' + content
    name = 'keymaster'
    content = (product / 'system/vendor/etc/init/android.hardware.keymaster@3.0-service.rc').read_bytes()
    service_rcs[name] = hashlib.sha256(content).hexdigest()
    assert service_rcs[name] == inspected['/system/vendor/etc/init/android.hardware.keymaster@3.0-service.rc']['sha256']
    rc += b'\n' + content
    zygote_rc = original['init.zygote32.rc'].data
    assert zygote_rc.count(b' --start-system-server') == 1
    rc += b'\n' + zygote_rc.replace(b' --start-system-server', b'')
    service_rcs['zygote_original'] = hashlib.sha256(zygote_rc).hexdigest()
    for name, data, mode in [('service-smoke.sh', script, 0o644), ('init.qemu-art.rc', rc, 0o644)]:
        assert name not in original
        entries.append(pack.Entry(name, stat.S_IFREG | mode, data=data))
    ramdisk = args.output / 'initramfs.cpio.gz'
    ramdisk.write_bytes(gzip.compress(pack.cpio_bytes(entries), mtime=0))
    inputs = {'schema': 1, 'system_image': image_info, 'kernel': record,
              'ramdisk_original_sha256': sha(original_ramdisk),
              'ramdisk_test_sha256': sha(ramdisk),
              'harness_sha256': sha(args.output / 'harness.py'),
              'service_rc_sha256': service_rcs,
              'policy_sha256': hashlib.sha256(original['sepolicy'].data).hexdigest(),
              'nonplat_file_contexts_original_sha256': context_before,
              'fixture_changes': ['Synthetic init rc', 'VExpress mmcblk0 system-block label',
                                  'Shell readiness observer', 'Zygote without --start-system-server; no Java test client executed'],
              'physical_device_access': False, 'android_framework_tested': False, 'zygote_preload_tested': True}
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
    zygote_ready = 'PW3_ZYGOTE_PRELOAD_PASS' in log and 'SERVICE_SMOKE_EXIT=0' in log
    child_crash = 'Fatal signal' in log or 'Runtime aborting' in log or 'FATAL EXCEPTION' in log or re.search(r'(?:^|[\r\n])F/[^\r\n]*\(\s*\d+\):', log) is not None
    image_fallback = 'Attempting to fall back to imageless running' in log
    logs_captured = re.search(r'I/PW3_LOGGER_READY\s*\(\s*\d+\): ready', log) is not None
    # android.graphics.Typeface is in preloaded-classes, so its static
    # initializer parses /system/etc/fonts.xml here and loads every family.
    # Every way that can go wrong is swallowed: a family it cannot build is
    # logged and skipped ("Unable to load Family", then "if (fontFamily ==
    # null) continue"), and a configuration it cannot read or parse at all
    # leaves init() through a catch that only logs (Typeface.java:901-913 and
    # 984-990). So a font the image no longer installs, an unreadable one, a
    # variation axis the parser rejects, or a malformed file costs text that
    # silently falls back, with nothing else to say so. This is the one place
    # that code runs outside the device.
    font_errors = [line for line in log.splitlines()
                   if re.search(r'(Unable to load Family|Error creating font'
                                r'|Error mapping font file'
                                r'|(?:Error opening|Error reading'
                                r'|XML parse exception for) [^ ]*fonts\.xml)', line)]
    passed = result.returncode == 0 and all(marker in log for marker in
        ('PW3_ZYGOTE_PRELOAD_PASS', 'SERVICE_SELINUX_ENFORCING=1',
         'SERVICE_CPU_GENFS_LABEL=1', 'SERVICE_SYSTEM_READONLY=1', 'SERVICE_SMOKE_EXIT=0'))
    passed = passed and not any(marker in log for marker in ('Kernel panic', 'Oops:', 'BUG:'))
    passed = passed and not child_crash
    passed = passed and not image_fallback
    passed = passed and logs_captured
    passed = passed and not font_errors
    unchanged = sha(args.system_image) == image_info['sha256']
    passed = passed and unchanged
    summary = {'schema': 1, 'passed': passed, 'qemu_exit': result.returncode,
               'zygote_ready': zygote_ready, 'child_crash_detected': child_crash,
               'android_logs_captured': logs_captured,
               'font_family_errors': font_errors,
               'boot_image_fallback_detected': image_fallback,
               'system_image_unchanged': unchanged, 'system_image_sha256': image_info['sha256'],
               'console_sha256': sha(args.output / 'console.log'),
               'avc_log_lines': sum(bool(re.search(r'avc:\s+denied', line)) for line in log.splitlines()),
               'scope': 'Actual Zygote preload and command-socket readiness, without SystemServer or app fork',
               'android_framework_tested': False, 'zygote_preload_tested': True, 'pw3_hardware_tested': False}
    (args.output / 'result.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(log[-18000:])
    print(json.dumps(summary, indent=2))
    if not passed:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
