#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build the Android entry for the Kindle library from public build outputs, in the
build VM: the boot programs, the public initramfs, the handoff module
(build_module.py) and the launcher (build_launcher.py).

What only a device has (EPDC waveform, Wi-Fi calibration, ATAGs, the place of
its user store on the eMMC, its data image's UUID) is not an input: the
launcher makes it on the device at each start (devicedata/). The result can be
given to anyone with the same model and stock firmware.

There is one kind of entry: the one that installs a release. The test entry
and its scaffolding -- an init rc of its own, the test helper that held a
logcat capture open for the life of the device and sampled it on a timer, and
the switches for diagnostics, the framework start gate and staying awake --
went on 2026-10-29. A device nobody is watching should not carry what only a
watched one can use.

Usage: build_entry.py --export DIR --kernel DIR --system-path PATH --data PATH:BYTES
                      --installer DIR --screens DIR --release-url URL [--release-url URL] --ca-bundle FILE
                      --zstd-archive FILE
                      --name FILE.sh --title TITLE --output DIR [--profile PROFILE]
  --export       export-aosp-images.py output (ramdisk.img, system.raw.img)
  --kernel       build-kernel.sh android-core output (zImage, modules.tar.gz, kernel.release);
                 every kernel module in the system image must be this kernel's, byte for byte
  --system-path  the system image on the user store, e.g. pw3-android/system.img
  --data         the data image on the user store and its size
  --profile      the model's boot profile (boot/profiles/*.json): the stock command line the
                 entry appends to, and the model the E101 screen names. Default: the PW3's,
                 the only one this project has run on a device
  --installer …  the system image the entry installs first (build_launcher.py); all
                 five are required, because an entry that installs nothing is not
                 one this project makes. build_launcher.py can still build the
                 launcher without them, which is what one host test does
                 (boot/handoff/tests/run_client_test.py); that is not an entry
"""
import argparse, gzip, hashlib, importlib.util, json, os, platform, re, shutil, stat, subprocess, sys, tarfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
PORT = HERE.parent
# The profile every entry used before --profile existed.
PROFILE = HERE / 'profiles/pw3-5.16.2.1.1.json'
KERNEL_SOURCE = Path('/home/builder/pw3-build/linux-android-core')   # tools/build-kernel.sh android-core
TOOLS = '/home/builder/toolchains/aosp-arm-gcc49/bin/arm-linux-androideabi-'
MUSL = '/home/builder/toolchains/musl-arm/bin/musl-gcc'
# After the stock command line (profile "cmdline_base"): the storage entry,
# Android's init with this device's rc, the system and data images as files on
# the user store. Nothing follows them: the test switches went with the test
# entry on 2026-10-29, which is why the entry contract is V10 and the key count
# PID 1 checks is three.
# loglevel=1 (after the stock "quiet", so it wins): only emergencies go to the
# serial console. The stock line keeps console=ttymxc0,115200, which nothing
# reads on a Kindle in use, and this 3.0 kernel writes the console with
# interrupts off: each error-level line held the CPU about 12 ms (Test V48, 20
# lines of 120 characters: 0.27 s against 0.03 s below the console level).
# Init's errors, the boot report and failing rc writes all came through there;
# the kernel log itself keeps every line.
# panic=5: without it, a panic spends two minutes destroying the evidence of
# what caused it.
#
# panic_timeout is 0 unless set, and 3.0's panic() then spins in mdelay() with
# interrupts enabled rather than stopping. The V81 capture of 2026-10-23 shows
# both halves of what that costs: `PC is at __delay+0x0/0xc` with
# `LR is at panic+0x194/0x1a0` is the spin, and `imx2_wdt_isr` in a stack above
# `show_regs` is the i.MX2 watchdog reaching the end of its own count and
# resetting the board -- which is the restart the reader saw about two minutes
# after the sleep screen. So a panic here does not hang forever; it leaves a
# dead screen for two minutes.
#
# The worse half is what happens during those two minutes. With interrupts on,
# the timer that caused the fault keeps firing, so the oops repeats, and the
# kernel ring is 32 KB: that capture holds three dumps and the text of the
# first oops is already gone. Rebooting on the first one keeps the only dump
# that names the cause.
#
# The evidence survives the restart: log_buf is written back to physical memory
# as each line is stored (patches/android-core, kernel/printk.c), the next
# kernel saves that memory as /proc/printkbuf, and the launcher files it at the
# next start -- which is how this panic was read at all. And a restart cannot
# loop, because it boots the stock Kindle: Android only starts when the reader
# taps the library entry.
# androidboot.hardware names the init rc files, not the product: init looks up
# /init.${ro.hardware}.rc, and the files this product installs are the pw3 ones
# (device/amazon/pw3/initialization.mk, inherited by device/amazon/voyage). A
# second model therefore keeps this word even when its product name differs --
# changing it here without renaming those files leaves a system that boots and
# finds no init rc at all.
CMDLINE_ANDROID = ['loglevel=1', 'panic=5', 'rdinit=/pw3-testfile-init', 'androidboot.hardware=pw3',
                   'androidboot.init_rc=/init.pw3-main.rc']
# Never in a public initramfs: they are made on the device (devicedata/) or are a person's.
PRIVATE = ('adb_keys', 'firmware', 'pw3-preinit.conf', 'pw3-testdata.conf')



def sha(data):
    return hashlib.sha256(data).hexdigest()


def run(command, **kwargs):
    subprocess.run(command, check=True, **kwargs)


def static_arm(path):
    elf = subprocess.check_output([TOOLS + 'readelf', '-h', '-lW', str(path)], text=True)
    if 'INTERP' in elf or 'Machine:                           ARM' not in elf:
        sys.exit('not a static ARM executable: ' + str(path))


def boot_programs(work):
    """The restore program (PID 1 after the jump) and the state tool, against this kernel's UAPI headers."""
    headers = work / 'uapi'
    (work / 'headers-build').mkdir()
    run(['make', '-s', '-C', str(KERNEL_SOURCE), 'O=' + str(work / 'headers-build'), 'ARCH=arm', 'headers_install',
         'INSTALL_HDR_PATH=' + str(headers)], stdout=subprocess.DEVNULL)
    flags = [MUSL, '-std=gnu11', '-static', '-Os', '-Wall', '-Wextra', '-Werror', '-isystem', str(headers / 'include'),
             '-I', str(HERE / 'handoff-init')]
    out = {}
    for name, source in [('pw3-handoff-init', HERE / 'handoff-init/pw3-handoff-init.c')]:
        run(flags + [str(source), '-o', str(work / (name + '.unstripped'))])
        run([TOOLS + 'strip', '-o', str(work / name), str(work / (name + '.unstripped'))])
        static_arm(work / name)
        out[name] = (work / name).read_bytes()
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ['export', 'kernel', 'output']:
        ap.add_argument('--' + name, type=Path, required=True)
    for name in ['system-path', 'data', 'name', 'title']:
        ap.add_argument('--' + name, required=True)
    for name in ['installer', 'screens', 'ca-bundle', 'zstd-archive']:
        ap.add_argument('--' + name, type=Path, required=True)
    ap.add_argument('--release-url', action='append', default=[], required=True,
                    help='the directory the payload sits in; repeat it for a second origin, in the order the installer tries them')
    ap.add_argument('--profile', type=Path, default=PROFILE,
                    help='the model\'s boot profile (boot/profiles/*.json); default: the PW3\'s')
    a = ap.parse_args()
    if platform.node() != 'lima-pw3' or os.geteuid() == 0:
        sys.exit('Run as the builder in the build VM (tools/vm.sh shell).')
    # One directory of the user store holds both images, and the entry reads its
    # name from the configuration under the rule of preinit/TestFileFormat.h. The
    # device refuses to start when the two paths are not that one directory
    # ("system and data test directories differ"), so refuse it here instead.
    if not re.fullmatch(r'[a-z0-9][a-z0-9-]{0,47}/system\.img', a.system_path):
        sys.exit('invalid --system-path')
    if a.system_path.rsplit('/', 1)[0] != a.data.rsplit('/', 1)[0]:
        sys.exit('--system-path and --data must be in one directory of the user store')
    profile = json.loads(a.profile.read_text())
    # A profile with a TODO in it is one whose facts nobody has read off the
    # device yet. It would build: the command line would be a sentence, the
    # kernel version a sentence, and the entry would refuse every Kindle it is
    # copied to with an E101 that names the right model. The build is the cheap
    # place to find that out, and this is the only place that can tell.
    unfinished = sorted(set(re.findall(r'TODO\([a-z-]+\)', json.dumps(profile))))
    if unfinished:
        sys.exit('%s still has %s in it: those facts are read off the device, and an entry '
                 'built without them cannot start the device it names'
                 % (a.profile.name, ', '.join(unfinished)))
    spec = importlib.util.spec_from_file_location('pack', PORT / 'tools/preinit-pack/pack.py')
    pack = importlib.util.module_from_spec(spec); sys.modules['pack'] = pack; spec.loader.exec_module(pack)

    export = json.loads((a.export / 'build-result.json').read_text())
    if not export['build_completed'] or export['private_firmware_injected'] is not False:
        sys.exit('not a complete public export: ' + str(a.export))
    system = export['files']['system.raw.img']
    kernel = (a.kernel / 'zImage').read_bytes()
    release = (a.kernel / 'kernel.release').read_text().strip()
    with tarfile.open(a.kernel / 'modules.tar.gz') as archive:
        modules = {Path(m.name).name: archive.extractfile(m).read() for m in archive.getmembers() if m.name.endswith('.ko')}
    epdc = modules['mxc_epdc_fb.ko']
    if b'vermagic=%s ' % release.encode() not in epdc:
        sys.exit('the EPDC module is not of this kernel')

    out = a.output
    out.mkdir(parents=True, exist_ok=False)
    work = Path('/home/builder/pw3-entry') / out.name
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    # What ties the system to the kernel is its modules (/vendor/lib/modules): the
    # same files, or the kernel is not the system's.
    run(['debugfs', '-R', 'rdump /vendor/lib/modules %s' % work, str(a.export / 'system.raw.img')], stderr=subprocess.DEVNULL)
    system_modules = sorted((work / 'modules').glob('*.ko'))
    if not system_modules or any(modules.get(m.name) != m.read_bytes() for m in system_modules):
        sys.exit('the system image carries kernel modules other than --kernel\'s')
    programs = boot_programs(work)

    # The public initramfs: this build's ramdisk with the boot programs, the
    # EPDC module, and the storage entry's file-based mount table.
    entries = pack.read_ramdisk(a.export / 'ramdisk.img')
    by_name = {e.name: e for e in entries}
    if any(n.split('/')[0] in PRIVATE for n in by_name):
        sys.exit('the export ramdisk already carries device files')
    init_sha = sha(by_name['init'].data)
    # Storage on image files of the user store (storage=testfile): /data is the data image's mapping.
    by_name['fstab.pw3'].data = by_name['fstab.pw3-testfile'].data
    storage = by_name['pw3-testfile-init']
    pack.static_arm_elf(storage.data, 'storage entry')
    # The command line below is written from this tree, and the entry that has
    # to accept it comes out of the export. An export made before the three-key
    # command line carries an entry that requires five keys: it would pack, and
    # it would not boot. The contract string is what tells them apart.
    if pack.CURRENT_TESTFILE_CONTRACT not in storage.data:
        raise SystemExit('the export\'s storage entry does not carry '
                         + pack.CURRENT_TESTFILE_CONTRACT.decode()
                         + '; it predates the three-key command line and the entry '
                           'it would build cannot boot. Build the export again.')
    storage.name = 'pw3-testfile-init.real'    # run by the restore program
    entries += [pack.Entry('pw3-testfile-init', storage.mode, storage.uid, storage.gid, programs['pw3-handoff-init']),
                pack.Entry('pw3-handoff', stat.S_IFDIR | 0o755),
                pack.Entry('pw3-handoff/mxc_epdc_fb.ko', stat.S_IFREG | 0o600, 0, 0, epdc)]
    taken = {e.name for e in entries}
    if any(name in taken for name, _, _ in pack.LOG_NODES):
        sys.exit('this initramfs already carries a log node; refusing a duplicate cpio member')
    entries += pack.log_nodes()
    entries.sort(key=lambda e: (e.name.count('/'), e.name))
    public = gzip.compress(pack.cpio_bytes(entries), mtime=0)
    # Read the archive back rather than trust what went in: the log nodes are
    # the one thing a storage entry needs before it can mount anything, so what
    # is checked is the bytes a kernel will unpack. read_ramdisk_bytes() is the
    # same reader, and it is the one that rejects a wrong member type or device.
    pack.verify_log_nodes(pack.read_ramdisk_bytes(public))
    (out / 'public-initramfs.cpio.gz').write_bytes(public)

    cmdline = ' '.join([profile['cmdline_base']] + CMDLINE_ANDROID)
    (out / 'cmdline.txt').write_text(cmdline + '\n')
    # The entry refuses a system image outside these bounds and then waits for
    # ever (preinit/TestFileFormat.h, pw3_test_size), so the build says so here
    # rather than letting the device be the one to find out.
    if not 256 << 20 <= system['bytes'] <= 1024 << 20 or system['bytes'] % 4096:
        sys.exit('the system image is %d bytes; the entry takes 256 MiB to 1 GiB, '
                 'a whole number of 4 KiB blocks' % system['bytes'])
    # pw3-preinit.conf without the lines of the device (devicedata.c adds fat_offset, fat_sizelimit, block=).
    conf = ''.join('%s=%s\n' % kv for kv in [
        ('schema', 1), ('system_path', a.system_path), ('system_size', system['bytes']), ('system_sha256', system['sha256']),
        ('init_sha256', init_sha), ('cmdline_sha256', sha(cmdline.encode())), ('kernel_release', release),
        ('adb_key_sha256', 'none')])   # adbd asks on the device and keeps the answer in /data/misc/adb
    (out / 'pw3-preinit.conf').write_text(conf)
    shutil.copyfile(a.kernel / 'zImage', out / 'zImage')

    run([sys.executable, str(HERE / 'build_module.py'), str(a.profile), '--kernel', str(out / 'zImage'),
         '--public-initramfs', str(out / 'public-initramfs.cpio.gz'), '--cmdline', str(out / 'cmdline.txt'),
         '--output', str(out / 'module')])
    run([sys.executable, str(HERE / 'handoff/audit_cache_boundary.py'), '--module', str(out / 'module/pw3_first_kernel.ko'),
         '--profile', 'stock', '--output', str(out / 'module/cache-audit')], stdout=subprocess.DEVNULL)
    audit = json.loads((out / 'module/cache-audit/result.json').read_text())
    if not audit['passed']:
        sys.exit('cache audit failed: ' + json.dumps(audit['checks']))
    run([sys.executable, str(HERE / 'build_launcher.py'), '--module', str(out / 'module'), '--kernel', str(out / 'zImage'),
         '--public-initramfs', str(out / 'public-initramfs.cpio.gz'), '--cmdline', str(out / 'cmdline.txt'),
         '--preinit-conf', str(out / 'pw3-preinit.conf'), '--data', a.data, '--name', a.name, '--title', a.title,
         '--profile', str(a.profile), '--output', str(out / 'launcher'),
         '--installer', str(a.installer), '--screens', str(a.screens),
         *[arg for u in a.release_url for arg in ('--release-url', u)],
         '--system-image', str(a.export / 'system.raw.img'), '--ca-bundle', str(a.ca_bundle),
         '--zstd-archive', str(a.zstd_archive)])

    result = {'schema': 3, 'entry': 'release', 'launcher': 'launcher/' + a.name,
              'export_receipt_sha256': sha((a.export / 'build-result.json').read_bytes()),
              'system': {'path': a.system_path, **system}, 'data': a.data, 'kernel_release': release, 'kernel_sha256': sha(kernel),
              'public_initramfs': {'bytes': len(public), 'sha256': sha(public), 'entries': len(entries)},
              'epdc_module_sha256': sha(epdc), 'programs_sha256': {n: sha(d) for n, d in programs.items()},
              'system_modules_checked': len(system_modules),
              'cmdline': cmdline, 'module_cache_audit_checks': len(audit['checks']),
              'installer': json.loads((out / 'launcher/build-result.json').read_text())['installer'], 'contains_device_data': False}
    (out / 'build-result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=1))


if __name__ == '__main__':
    main()
