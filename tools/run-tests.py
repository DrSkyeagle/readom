#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Every test of this project that needs nothing but the machine it runs on.

The project's tests grew where the code did -- Python beside the tools, C
beside the headers it checks -- and each knew how to run itself and nobody
knew how to run them all. This is that list. It runs what can run here, says
what it skipped and why, and returns non-zero if anything failed, so that
"did I break something" is one command before a change is published.

The tests it does not run are the ones that need something this machine does
not have on its own: a kernel build to compare against, a built system image,
QEMU, or the device. They are listed as skipped with what each one wants, so
that the list is the whole truth and not the convenient part of it.

Usage: run-tests.py [--list] [--cc gcc] [--filter SUBSTRING]
                    [--zstd-archive zstd-1.5.6.tar.gz]
"""
import argparse
import json
import os
import platform
import shutil
import subprocess
import sys
import tempfile
from importlib.util import find_spec
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
# -Werror because every one of these compiles cleanly today, and the
# sanitizers because these are the parsers of what a damaged device hands the
# start: a test that passes by reading past an array has told us nothing.
CFLAGS = ['-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
          '-fsanitize=address,undefined', '-fno-sanitize-recover=all']
# The same, for the C++ of the panel and the composer. Those tests existed from
# the start and were built only as device modules (device/amazon/pw3/tests/
# paneltest), so nothing ran them unless somebody asked: panel_test.cpp had
# been asserting fullEvery == 30 since the reader chose 20 on 2026-10-22, and
# the suite was green throughout. They are plain host C++ and belong here.
CXXFLAGS = ['-std=c++14', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fexceptions',
            '-fsanitize=address,undefined', '-fno-sanitize-recover=all']

# name, kind, what to run, whether it needs Linux. 'python': the script, run
# as itself. 'c': sources compiled together and run; a source entry beginning
# with '-I' is an include path instead. 'linux' marks a test that needs Linux
# headers (device mapper, FIBMAP) and is skipped elsewhere.
TESTS = [
    ('patch-series', 'python', ['tools/tests/test_patch_series.py'], False),
    # A patch header cannot be corrected after the fact (the series is
    # append-only), so what a patch says about itself is what was known the
    # day it was written. The ones that say they were not run on the device
    # are listed outside the series, and this keeps the two in step: 0008
    # said it, nobody went back, and the premise it had not measured stood
    # for twelve days (patches/VERIFICATION.md).
    ('patch-verification', 'python', ['tools/tests/test_patch_verification.py'], False),
    ('collect-sources', 'python', ['tools/tests/test_collect_sources.py'], False),
    ('klog-tags', 'python', ['tools/tests/test_klog_tags.py'], False),
    ('log-channels', 'python', ['tools/tests/test_log_channels.py'], False),
    ('usage-time-check', 'python', ['tools/tests/test_usage_time_check.py'], False),
    ('apk-package-name', 'python', ['tools/tests/test_apk_package_name.py'], False),
    ('sepolicy-device-paths', 'python', ['tools/tests/test_sepolicy_device_paths.py'], False),
    ('usb-gate', 'python', ['tools/tests/test_usb_gate.py'], False),
    ('release-url', 'python', ['tools/tests/test_release_url.py'], False),
    ('launcher-name', 'python', ['tools/tests/test_launcher_name.py'], False),
    ('font-config', 'python', ['tools/tests/test_font_config.py'], False),
    ('screens', 'python', ['tools/tests/test_screens.py'], False),
    ('doc-pairs', 'python', ['tools/tests/test_doc_pairs.py'], False),
    ('doc-links', 'python', ['tools/tests/test_doc_links.py'], False),
    # The kernel configuration is two files -- what this port needs of any
    # kernel, and what is true of one machine -- and nothing in a build notices
    # either half going wrong. Reads the two files and the build script, not the
    # kernel tree: whether Kconfig keeps an option is what `merge-kconfig.py
    # --check` answers during the build itself.
    ('kconfig-fragments', 'python', ['tools/tests/test_kconfig_fragments.py'], False),
    # The home screen's four marks are drawn by one script, three of them into
    # somebody else's resource and the fourth into the patch that puts it on the
    # screen. A mark regenerated and not carried into all four places is a home
    # screen that is no longer one set, which is the whole point of them.
    ('launcher-icons', 'python',
     ['device/amazon/pw3/launcher-icons/make_icons.py', '--check'], False),
    # And the size they are drawn at, against the one rule that decides whether
    # Launcher3 draws them as authored or shrinks them first. Three of the four
    # go through that rule and the fourth does not, which is how they reached
    # the screen at two sizes.
    ('icon-normalizer', 'python', ['tools/tests/test_icon_normalizer.py'], False),
    # The makefile half of check-image-references: every module this device
    # keeps out of a release has to say why. It needs no built image, so it
    # runs everywhere and on every change -- which is the point, because the
    # image half can only answer for the variant that happens to be staged,
    # and the staged tree after the userdebug smoke step is not a release.
    ('device-packages', 'python', ['tools/check-image-references.py', '--packages-only'], False),
    # A model's stock firmware is written down twice: in its boot profile, which
    # the build reads, and in one struct in devicedata.c, which the compiled
    # helper checks the device against. A build that has one without the other
    # is a Kindle that installs and then refuses to start, so the two are held
    # to each other before that.
    ('profile-devicedata', 'python', ['tools/tests/test_profile_matches_devicedata.py'], False),
    # And the check that keeps this list honest: a test file that is in the
    # tree and in nothing's list of what to run is a hole in a green suite,
    # which is how a wrong assertion about the refresh policy survived a day
    # of work on the refresh policy.
    ('every-test-runs', 'python', ['tools/tests/test_every_test_runs.py'], False),
    # Where the installer's screens break a line. The rule ran only inside a
    # full render, which needs Pillow and the AOSP tree's fonts, so nothing
    # looked at the rule itself until it put a closing quotation mark and a
    # full stop alone on a line of E202.
    ('wrap', 'python', ['tools/tests/test_wrap.py'], False),
    # The panel and the composer, as device/amazon/pw3/tests/paneltest builds
    # them for the device. Here they are built for the host: the code is the
    # same and the question -- which waveform a change asks for, what a luma
    # row converts to -- does not depend on the instruction set. (What does is
    # the vector code, which is why the device modules stay.)
    ('panel', 'cpp', ['-Idevice/amazon/pw3/epdc', 'device/amazon/pw3/epdc/tests/panel_test.cpp',
                      'device/amazon/pw3/epdc/Panel.cpp', 'device/amazon/pw3/epdc/Tone.cpp'], False),
    ('luma', 'cpp', ['-Idevice/amazon/pw3/epdc', 'device/amazon/pw3/epdc/tests/luma_test.cpp',
                     'device/amazon/pw3/epdc/Luma.cpp'], False),
    ('dither', 'cpp', ['-Idevice/amazon/pw3/epdc', 'device/amazon/pw3/epdc/tests/dither_test.cpp',
                       'device/amazon/pw3/epdc/Dither.cpp', 'device/amazon/pw3/epdc/Tone.cpp'], False),
    ('picture', 'cpp', ['-Idevice/amazon/pw3/epdc', 'device/amazon/pw3/epdc/tests/picture_test.cpp',
                        'device/amazon/pw3/epdc/Picture.cpp'], False),
    ('sleep-text', 'cpp', ['-Idevice/amazon/pw3/epdc', 'device/amazon/pw3/epdc/tests/sleep_text_test.cpp',
                           'device/amazon/pw3/epdc/SleepText.cpp', 'device/amazon/pw3/epdc/Picture.cpp'], False),
    # Two more that were written, compile on a host, and were run by nobody:
    # the low-battery rule (its own header says "g++ -std=c++14 -I.."), and the
    # USB gate, which only tools/kernel-hardware/tests/run_tests.py built -- a
    # macOS-only harness that also wants a kernel build directory, so the gate
    # went unbuilt wherever that was not at hand. Both pass.
    ('low-battery', 'cpp', ['-Idevice/amazon/pw3/health',
                            'device/amazon/pw3/health/tests/low_battery_test.cpp'], False),
    ('usb-gate', 'cpp', ['-Itools/kernel-hardware/native',
                         'tools/kernel-hardware/native/UsbGate.cpp',
                         'tools/kernel-hardware/native/PosixChecks.cpp',
                         'tools/kernel-hardware/tests/gate_test.cpp'], False),
    ('luma-compose', 'cpp', ['-Idevice/amazon/pw3/epdc', '-Idevice/amazon/pw3/composer',
                             'device/amazon/pw3/composer/tests/luma_compose_test.cpp',
                             'device/amazon/pw3/composer/LumaCompose.cpp',
                             'device/amazon/pw3/epdc/Luma.cpp'], False),
    # The sleep-power arithmetic, and the four ways a measurement can look
    # right and be wrong: a device that never suspended, one that was charging
    # when it was read, a restart between the samples, a field that moved.
    ('sleep-power', 'python', ['tools/tests/test_sleep_power.py'], False),
    ('reuse', 'python', ['tools/tests/test_reuse.py'], False),
    ('launcher-call-sites', 'python', ['tools/tests/test_launcher_call_sites.py'], False),
    ('verify-script', 'python', ['tools/tests/test_verify_script.py'], False),
    ('verify-device-answers', 'python', ['tools/tests/test_verify_device_answers.py'], False),
    ('export-refuses', 'python', ['tools/tests/test_export_refuses.py'], False),
    # Here and not only in the commit hook: a guard that runs once per clone, if
    # somebody remembered to enable it (README, `git config core.hooksPath`), is
    # not a guard. This repository is meant to be published, so what it would
    # publish is checked by the suite that has to pass anyway.
    ('private-data', 'python', ['tools/check-private-data.py'], False),
    ('private-data-scanner', 'python', ['tools/tests/test_private_data_scanner.py'], False),
    ('preinit-pack', 'python', ['tools/preinit-pack/tests/test_pack.py'], False),
    ('adbd-functions', 'python', ['tools/kernel-hardware/tests/test_adbd.py'], False),
    ('allocation-ranges', 'c', ['-Iboot/handoff', 'boot/handoff/tests/test-allocation-ranges.c',
                                'boot/handoff/pw3_plan.c'], False),
    ('device-data', 'c', ['boot/devicedata/tests/devicedata_test.c',
                          'boot/devicedata/devicedata.c', 'boot/handoff/pw3_plan.c'], False),
    ('user-log', 'c', ['boot/launcher/tests/userlog_test.c', 'boot/launcher/userlog.c'], False),
    ('report-area', 'c', ['-Idevice/amazon/pw3/report', 'boot/launcher/tests/report_test.c'], False),
    # The report PID 1 writes when it refuses to start. It is the path that only
    # runs after something has already gone wrong, so without this it would
    # first be executed on somebody's Kindle.
    ('report-stop', 'c', ['device/amazon/pw3/preinit/tests/report_stop_test.c'], False),
    ('system-stamp', 'c', ['device/amazon/pw3/preinit/tests/system_stamp_test.c'], True),
    ('system-recheck', 'c', ['device/amazon/pw3/preinit/tests/system_recheck_test.c'], False),
    ('fat-cross-link', 'c', ['device/amazon/pw3/preinit/tests/fat_crosslink_test.c'], True),
    # The mapping reader at its syscall seam. It lived in a README until the
    # report area made a third file of what the receipt holds: a test nobody
    # runs is how a refactor gets to look finished.
    ('file-mapper', 'c', ['tools/preinit-pack/tests/test_file_mapper.c'], True),
]

# The installer's own tests compile install.c and run its steps natively on
# small images, which needs the zstd source archive it links against and a
# Linux host to compile for. Given the archive, they are ordinary tests; the
# listing says what they want when they are not given it.
INSTALLER = ('installer', 'installer/install/test_install.py',
             'the zstd source archive (--zstd-archive), and Linux to compile on')

# The cross-link check at the seam the installer's own tests cannot reach: a FAT
# of its own, mounted, with the images on it, asked through FIBMAP, and
# cross-linked on purpose. Everything below the names -- which volume holds the
# file, what FIBMAP answers, which frame those answers are in -- is only
# answered by a running kernel, and mounting is root's, so this goes through
# sudo and is listed rather than run where that is not available.
FATCHECK = ('installer-fatcheck', 'installer/install/test_fatcheck.py',
            'Linux, mkfs.vfat, and sudo without a password (it mounts a FAT of its own)')

# pw3-unpack is what reads the payload on the device, and it is an ARM binary
# run here under qemu-arm-static. It takes the same archive the installer links
# against, so given that it can be built and run in one go; its own listing used
# to ask for "a built unpack binary", which nobody was going to build by hand.
UNPACK = ('installer-unpack', 'installer/unpack/test_unpack.py',
          'the zstd source archive (--zstd-archive), qemu-arm-static and Linux')

# The installer's screens are laid out from this project's text with the fonts
# of the AOSP tree, so this runs where that tree is: the build VM. It renders
# them, which is where text too tall for the panel is refused, and then checks
# that screen.c puts every screen together exactly as the render drew it.
# Nothing else renders them until a release is built, and that is how the start
# screen stood 90 px over the panel for three days.
AOSP_TREE = Path('/home/builder/android-src')
SCREEN_FONTS = AOSP_TREE / 'external'
SCREENS = ('installer-screens', 'installer/screens/render_screens.py',
           "Pillow, the AOSP tree's fonts and the zstd source archive (--zstd-archive)")
# The same screen, drawn a second time on the other side of the handover
# (boot/handoff-init/start-picture.h). It is a committed generated file, so
# what has to be checked is that it still is what the wording and the fonts
# give: a change to the installer's words that stops at the installer would
# leave the two screens saying different things.
START_PICTURE = ('handoff-start-picture', 'boot/handoff-init/make-start-picture.py',
                 "Pillow and the AOSP tree's fonts")
# The download screen says a size and two error screens say the payload's file
# name. A release takes both from its own manifest; this stands in for one, and
# both values are the worst case rather than today's, because what this run
# decides is whether the text fits the panel. The name is the longest a release
# can produce: the version is "v" and a number, and three digits is as far as
# that goes before the name is the least of the problems.
SCREEN_PAYLOAD = {'files': {'system': {'bytes': 196 * 1000 * 1000,
                                       'name': 'readom-system-v999.img.zst'}}}

# What a finished image points at and may not have: a program named by an init
# .rc file, a shared library something links against. Both are read out of the
# staged tree, so they see what a variant or a package list dropped; both have
# already caught a service that could not start at every boot.
IMAGE = ('image-references', 'tools/check-image-references.py',
         'a staged system image in the build VM')
IMAGE_PRODUCT = AOSP_TREE / 'out/target/product/pw3'

# The four marks as the packages carry them. launcher-icons above compares the
# resource files with the script that writes them; this compares the packages
# with the resource files, which is the half where a mistake is silent: the
# browser's icon is a runtime overlay, and an overlay entry in the wrong
# configuration is passed over without a line in any log.
HOME_ICONS = ('home-screen-icons', 'tools/tests/test_home_screen_icons.py',
              'a staged system image in the build VM, and aapt2 from its host tools')
AAPT2 = AOSP_TREE / 'out/host/linux-x86/bin/aapt2'

# The one C++ test of this project that does not stand on its own: it reads
# the allocator's PixelBytes.h, which includes AOSP's <system/graphics.h> for
# the format constants. That is the point of the test -- the allocator and the
# composer must size a pixel the same -- so the header comes from the tree
# rather than from a copy here.
FORMAT = ('buffer-format', 'device/amazon/pw3/composer/tests/buffer_format_test.cpp',
          "AOSP's system/core headers, which are in the build VM")
FORMAT_INCLUDE = AOSP_TREE / 'system/core/include'

# The repaint the EPDC device owes when the panel holds a picture the process
# does not know. It is reached only when the driver refuses an update or never
# ends one, so nothing on the device has run it: a release build has no root and
# the panel has no failure to inject. The test replaces the driver instead
# (gralloc/tests/recover_test.cpp), which needs the device's own translation
# unit, and so AOSP's HAL and log headers.
RECOVER = ('gralloc-recover', 'device/amazon/pw3/gralloc/tests/recover_test.cpp',
           "AOSP's HAL and system/core headers, which are in the build VM")
HAL_INCLUDE = AOSP_TREE / 'hardware/libhardware/include'

# A class that only a resource names has no reference a shrinker can see. The
# launcher crashed at every start of the first clean install for that reason
# and nothing before the device could have caught it: the flag that made the
# only Java reference dead was a source change, the dropped class was a build
# output, and the two only meet in the package. So the packages are opened.
XML_CLASSES = ('xml-named-classes', 'tools/tests/test_xml_named_classes.py',
               'a staged system image in the build VM, and aapt from its host tools')
AAPT = AOSP_TREE / 'out/host/linux-x86/bin/aapt'

# device/amazon/pw3/fonts/fonts.xml is AOSP's with whole families deleted, and
# only AOSP's own copy can show it is still exactly that. Without the tree,
# font-config above checks what the two files in this repository say about each
# other; this adds the deletions-only proof and the two module-name subsets.
FONT_AOSP = ('font-config-aosp', 'tools/tests/test_font_config.py',
             "the AOSP tree in the build VM")

# The home screen is seeded from a layout this repository keeps as a patch, and
# a row the launcher cannot build is dropped without a word. What the layout
# says has to agree with the code that answers it (the action of the all-apps
# entry, the activity the downloader's cell is handed over by), with the version
# that decides what a device which already has a home screen is given, and with
# what that delivery can carry at all. Read from the applied tree: the question
# is what the whole series leaves behind, not what one patch says.
LAUNCHER_TREE = AOSP_TREE / 'packages/apps/Launcher3'
HOME_SCREEN = ('home-screen-entries', 'tools/tests/test_home_screen_entries.py',
               "the AOSP tree in the build VM, with the patch series applied")

# The emoji font is rewritten on the way into the image, to carry the variation
# sequences Paint.hasGlyph() reads. It is a third-party binary, so this reads
# the result back rather than trusting the writer. Wants the fetched font
# (tools/fetch-prebuilts.py) and Unicode's own list, which is in the tree.
EMOJI_FONT = ROOT / 'device/amazon/pw3/emoji/NotoEmoji-VF.ttf'
EMOJI_SEQUENCES = AOSP_TREE / 'external/unicode/emoji-variation-sequences.txt'
EMOJI_VS = ('emoji-variation-sequences', 'tools/tests/test_emoji_vs.py',
            "the fetched emoji font (tools/fetch-prebuilts.py) and Unicode's "
            "sequence list in the AOSP tree")

# The built image run in QEMU: ART and the core libraries, the zygote, and the
# system services. Each is a real ARM boot of the image this build made, which
# is as close to the device as this machine gets.
# The export to run: the one holding the image this tree last built, which the
# build receipt names by hash. Picking "the newest export" instead would one
# day run an older image and report its pass as this build's, which is worse
# than not running at all; a name with a version in it would have to be edited
# for every build. PW3_EXPORT_DIR overrides, for an export made elsewhere.
BUILD_RECEIPT = Path('/mnt/build-share/logs/aosp-system-job.json')


def built_export():
    named = os.environ.get('PW3_EXPORT_DIR')
    if named:
        return Path(named)
    try:
        built = json.loads(BUILD_RECEIPT.read_text())['images']['system.img']
    except (OSError, KeyError, ValueError):
        return None
    for export in sorted(Path('/mnt/build-share').glob('aosp-images-device-test-*')):
        receipt = export / 'build-result.json'
        if not (export / 'filesystem-inspection.json').exists() or not receipt.exists():
            continue
        try:
            if json.loads(receipt.read_text())['files']['system.img']['sha256'] == built:
                return export
        except (OSError, KeyError, ValueError):
            continue
    return None


def built_payload(export):
    """The payload made from this export's image, which its manifest names by
    the hash of the image it unpacks to. Picking the newest payload instead
    would one day check an older build's seed and report it as this one's."""
    if export is None:
        return None
    try:
        raw = json.loads((export / 'build-result.json').read_text())['files']['system.raw.img']['sha256']
    except (OSError, KeyError, ValueError):
        return None
    for payload in sorted(Path('/mnt/build-share').glob('payload-*')):
        try:
            manifest = json.loads((payload / 'manifest.json').read_text())
            if manifest['files']['system']['unpacked_sha256'] == raw:
                return payload
        except (OSError, KeyError, ValueError):
            continue
    return None


# The rules that decide whether the data image a start finds may be used, read
# against a real seed: the one this build's payload carries. Unpacking it is
# cheap -- it is sparse -- and on the way through, the manifest's own record of
# what it unpacks to is checked (tools/tests/unpack-payload-file.py).
DATA_IMAGE = ('data-image', 'device/amazon/pw3/preinit/tests/data_image_test.c',
              "the payload built from this tree's image (installer/build_payload.py), "
              'which carries the seed data image')

SMOKE = [('runtime-smoke', 'tools/aosp-runtime-smoke/run.py'),
         ('zygote-smoke', 'tools/aosp-zygote-smoke/run.py'),
         ('service-smoke', 'tools/aosp-service-smoke/run.py')]

# Two tests compare one file as Amazon shipped it with the same file after a
# patch of this port's. The stock side comes out of the pinned archive
# (extract-stock-file.py) and the patched side out of the tree a build leaves
# behind, so both run wherever that tree is -- the build VM. They were skipped
# for weeks because supplying the stock side meant keeping a second tree by
# hand, which nobody was going to do twice.
KERNEL_ARCHIVE = Path(os.environ.get('PW3_KERNEL_ARCHIVE',
                                     '/mnt/kernel-inputs/amazon-5.16.2.1.1/linux-3.0.35.tar.gz'))
PATCHED_KERNEL = Path(os.environ.get('PW3_PATCHED_KERNEL_DIR',
                                     '/home/builder/pw3-build/linux-android-core'))
BEFORE_AFTER = [
    # GNU make's own hash escaping, which decides whether a changed compiler
    # command line is noticed at all: with the stock Kbuild.include a second
    # build recompiled what had not changed, and the patch is what stopped it.
    ('kbuild-cmd-cache', 'tools/tests/test_kbuild_cmd_cache.py',
     'scripts/Kbuild.include', []),
    # The three PMIC transfer helpers, compiled with a stub bus: the stock ones
    # report success for a transfer that moved fewer messages than asked.
    ('pmic-i2c', 'tools/tests/test_pmic_i2c.py',
     'drivers/mfd/max77696-i2c.c', ['--cc', '{cc}']),
]
BEFORE_AFTER_NEEDS = ('the pinned kernel archive (PW3_KERNEL_ARCHIVE) and a patched kernel '
                      'tree (PW3_PATCHED_KERNEL_DIR), which a build leaves in the build VM')

# The exported kernel component the AOSP build takes in, and the FunctionFS
# lifecycle of the stock gadget driver. Both wanted a hand-set variable, which
# is why neither had run in weeks; the build leaves the first where this looks,
# and the second needs one stock file, which comes out of the archive.
KERNEL_BUILD = Path(os.environ.get('PW3_KERNEL_BUILD_DIR', '/mnt/build-share/android-core-kernel'))
EXPORTER = ('kernel-exporter', 'tools/kernel-hardware/tests/test_exporter.py',
            'PW3_KERNEL_BUILD_DIR, a built kernel with its receipt')
FFS_FILE = 'drivers/usb/gadget/f_fs.c'
FFS = ('ffs-lifecycle', 'tools/kernel-hardware/tests/test_ffs_lifecycle.py',
       'the pinned kernel archive (PW3_KERNEL_ARCHIVE), which holds the stock driver')

# What each of the others needs before it can run, in the words of whoever
# would have to provide it.
ELSEWHERE = [
]

HISTORY = ('private-data-history', 'tools/check-private-data.py',
           'a commit to walk: this tree has no history yet')


# The handoff in QEMU: a first Linux loads the module, is refused what breaks
# the build's pins, and jumps to a second Linux. It builds its own module, so
# what it wants is the loader-stage kernel (tools/build-loader-stage-kernel.sh)
# and the RAM-only observer to jump to. Three minutes.
LOADER_KERNEL = Path('/mnt/build-share/loader-stage-kernel')
OBSERVER = Path('/mnt/build-share/minimal-linux-observer-qemu-v2')
HANDOFF_QEMU = ('handoff-in-qemu', 'boot/handoff/tests/run_qemu.py',
                'the loader-stage kernel (tools/build-loader-stage-kernel.sh) and the '
                'RAM-only observer, in the build VM')

# The launcher and its helper on ARM under the device owner's own shell. It
# builds the virtual profile of both itself, so what it wants is the kernel, the
# observer, and that shell -- which is the owner's own and is not published, so
# this is the one test here that cannot run on a machine without their device.
FIXTURE = Path('/home/builder/early-observe/private-fixture/root')
HANDOFF_CLIENT = ('handoff-client', 'boot/handoff/tests/run_client_test.py',
                  "the loader-stage kernel, the observer, and the device owner's own shell "
                  'and libc as the fixture, which is theirs and is not published')


def run_python(script, _cc, _work):
    return subprocess.run([sys.executable, str(ROOT / script[0])] + list(script[1:]),
                          cwd=str(ROOT), capture_output=True, text=True)


def run_steps(steps, cc, work):
    """Each command in turn, stopping at the first that fails. '{work}' in an
    argument is the directory of this run, which is how one step reaches what
    the step before it made; '{cc}' is the compiler this run was given.

    A step is a Python script and its arguments, or ('c', [sources], [arguments])
    for one compiled here with the same flags as a 'c' test -- which is how a C
    test gets an input another step had to make first."""
    for step in steps:
        if step[0] == 'c':
            result = run_c(step[1], cc, work, step[2])
        else:
            command = [sys.executable, str(ROOT / step[0])] + [s.format(work=work, cc=cc)
                                                               for s in step[1:]]
            result = subprocess.run(command, cwd=str(ROOT), capture_output=True, text=True)
        if result.returncode:
            return result
    return result


def run_cpp(sources, cxx, work, arguments=()):
    return run_c(sources, cxx, work, arguments, CXXFLAGS)


def cpp_compiler_works(cxx, work):
    """Whether cxx can build a C++14 program at all, and if not, why not.

    These tests used to be built only as device modules, so a host that cannot
    compile C++ never noticed. Now that they run here, a host with a half
    installed toolchain would report eight failures of this project's code,
    which is the wrong thing to tell someone: nothing in the repository is
    wrong, their compiler cannot find its own standard library. This Mac is
    one -- Command Line Tools 27 leaves eleven files in
    usr/include/c++/v1, the rest being in the SDK that clang does not look in
    -- and the honest answer is a skip that says so. Inventing an -I to reach
    around it would put an unknown libc++ ahead of the one the compiler
    chose, on every host, to accommodate one broken one.
    """
    source = work / 'cxx-probe.cpp'
    source.write_text('#include <cstdint>\n#include <vector>\n'
                      'int main() { return int(std::vector<uint8_t>{0}.size()) - 1; }\n')
    build = subprocess.run([cxx] + CXXFLAGS + [str(source), '-o', str(work / 'cxx-probe.bin')],
                           cwd=str(ROOT), capture_output=True, text=True)
    if not build.returncode:
        return None
    first = (build.stderr.strip().splitlines() or ['no output'])[0]
    return '%s cannot build a C++14 program here: %s' % (cxx, first)


def run_c(sources, cc, work, arguments=(), flags=None):
    includes = ['-I' + str(ROOT / s[2:]) for s in sources if s.startswith('-I')]
    files = [str(ROOT / s) for s in sources if not s.startswith('-I')]
    binary = work / (Path(files[0]).stem + '.bin')
    build = subprocess.run([cc] + (flags or CFLAGS) + includes + files + ['-o', str(binary)],
                           cwd=str(ROOT), capture_output=True, text=True)
    if build.returncode:
        return build
    return subprocess.run([str(binary)] + [s.format(work=work) for s in arguments],
                          cwd=str(work), capture_output=True, text=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--cc', default=os.environ.get('CC', 'cc'), help='the C compiler to use')
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'), help='the C++ compiler to use')
    parser.add_argument('--filter', default='', help='run only tests whose name contains this')
    parser.add_argument('--list', action='store_true', help='print the list and run nothing')
    parser.add_argument('--zstd-archive', help="the zstd release the installer links against")
    args = parser.parse_args()

    tests, elsewhere = list(TESTS), list(ELSEWHERE)
    # The exporter reads REUSE.toml with tomllib, which Python gained in 3.11;
    # the build VM runs 3.10. There the test is not "passed" and not "failed":
    # it cannot run, and it says so, which is what the skipped list is for. The
    # exporter itself is only ever run on a machine with a current Python (the
    # publish step), and the property stays checked wherever one is.
    if sys.version_info < (3, 11):
        for entry in [t for t in tests if t[0] == 'export-refuses']:
            tests.remove(entry)
            elsewhere.insert(0, (entry[0], entry[2][0],
                                 'Python 3.11 (tomllib); this is %d.%d'
                                 % sys.version_info[:2]))
    # The working tree is checked above; this is every version of every file in
    # the history, which is the other half of what publishing publishes: a value
    # deleted from a file is out of the working tree and still in `git log -p`,
    # and in every clone. Fixable only before the first publish, which is why it
    # runs with the suite rather than on the day.
    #
    # Conditional on there being a commit to walk. A tree with none -- the folder
    # the open-source export is assembled in, before its first commit -- used to
    # report "0 versions of files in 0 commits checked ... 0 findings" and count
    # as a pass, which is this check switched off wearing the face of this check
    # passing (seen 2026-10-31 in the export's dry run).
    #
    # Asked as "does the range this walks hold a commit", not "is there a HEAD":
    # the check itself reads `rev-list --all`, so a tree sitting on an unborn
    # branch while other branches and tags still carry commits has history to
    # walk and no HEAD to find (Codex 2026-10-31). A git that errors is not an
    # empty history either, and says so.
    try:
        walk = subprocess.run(['git', '-C', str(ROOT), 'rev-list', '--all', '-n', '1'],
                              capture_output=True, text=True)
    except OSError as e:
        # No git at all. --list promises a list, so it gets one with this item
        # marked unavailable rather than a traceback (Codex 2026-10-31); running
        # the suite without git is the caller's problem to see in that line.
        elsewhere.insert(0, (HISTORY[0], HISTORY[1], 'git on PATH: %s' % e.strerror))
        walk = None
    if walk is not None and walk.returncode and 'not a git repository' not in walk.stderr:
        print('git could not be asked for this tree\'s history: %s'
              % (walk.stderr.strip().splitlines() or ['no output'])[0], file=sys.stderr)
        return 2
    if walk is None:
        pass
    elif walk.stdout.strip():
        tests.append((HISTORY[0], 'python', [HISTORY[1], '--history'], False))
    else:
        elsewhere.insert(0, HISTORY)
    archive = str(Path(args.zstd_archive).resolve()) if args.zstd_archive else None
    if archive and platform.system() == 'Linux':
        tests.append((INSTALLER[0], 'python', [INSTALLER[1], '--zstd-archive', archive], True))
    else:
        elsewhere.insert(0, INSTALLER)
    if (platform.system() == 'Linux' and shutil.which('mkfs.vfat') and
            not subprocess.run(['sudo', '-n', 'true'], capture_output=True).returncode):
        tests.append((FATCHECK[0], 'python', [FATCHECK[1]], True))
    else:
        elsewhere.insert(0, FATCHECK)
    if archive and platform.system() == 'Linux' and Path('/usr/bin/qemu-arm-static').exists():
        tests.append((UNPACK[0], 'steps', [
            ['installer/build_unpack.py', '--zstd-archive', archive, '--output', '{work}/unpack'],
            [UNPACK[1], '{work}/unpack/pw3-unpack']], True))
    else:
        elsewhere.insert(0, UNPACK)
    if archive and platform.system() == 'Linux' and SCREEN_FONTS.is_dir() and find_spec('PIL'):
        tests.append((SCREENS[0], 'steps', [
            [SCREENS[1], '--manifest', '{work}/screen-payload.json',
             '--profile', 'boot/profiles/pw3-5.16.2.1.1.json',
             '--cjk-font', str(SCREEN_FONTS / 'noto-fonts/cjk/NotoSansCJK-Regular.ttc'),
             '--latin-font', str(SCREEN_FONTS / 'roboto-fonts/Roboto-Regular.ttf'),
             '--latin-title-font', str(SCREEN_FONTS / 'roboto-fonts/Roboto-Medium.ttf'),
             '--output', '{work}/screens'],
            ['installer/install/test_screen.py', '--screens', '{work}/screens',
             '--zstd-archive', archive]], True))
    else:
        elsewhere.insert(0, SCREENS)
    if (SCREEN_FONTS / 'noto-fonts/cjk/NotoSansCJK-Regular.ttc').is_file():
        tests.append((START_PICTURE[0], 'python', [
            START_PICTURE[1], '--check',
            '--cjk-font', str(SCREEN_FONTS / 'noto-fonts/cjk/NotoSansCJK-Regular.ttc'),
            '--latin-font', str(SCREEN_FONTS / 'roboto-fonts/Roboto-Regular.ttf'),
            '--latin-title-font', str(SCREEN_FONTS / 'roboto-fonts/Roboto-Medium.ttf')], False))
    else:
        elsewhere.insert(0, START_PICTURE)
    if (FORMAT_INCLUDE / 'system/graphics.h').is_file():
        tests.append((FORMAT[0], 'cpp',
                      ['-Idevice/amazon/pw3/epdc', '-Idevice/amazon/pw3/composer',
                       '-Idevice/amazon/pw3/gralloc', '-I' + str(FORMAT_INCLUDE),
                       FORMAT[1], 'device/amazon/pw3/composer/LumaCompose.cpp',
                       'device/amazon/pw3/epdc/Luma.cpp'], False))
    else:
        elsewhere.insert(0, FORMAT)
    if (HAL_INCLUDE / 'hardware/fb.h').is_file() and (FORMAT_INCLUDE / 'cutils/properties.h').is_file():
        tests.append((RECOVER[0], 'cpp',
                      ['-Idevice/amazon/pw3/gralloc/tests/include', '-I' + str(HAL_INCLUDE),
                       '-I' + str(FORMAT_INCLUDE), RECOVER[1],
                       'device/amazon/pw3/epdc/Panel.cpp', 'device/amazon/pw3/epdc/Tone.cpp',
                       'device/amazon/pw3/epdc/Luma.cpp', 'device/amazon/pw3/epdc/Dither.cpp',
                       'device/amazon/pw3/epdc/Picture.cpp',
                       'device/amazon/pw3/epdc/SleepText.cpp'], True))
    else:
        elsewhere.insert(0, RECOVER)
    if (IMAGE_PRODUCT / 'system').is_dir():
        tests.append((IMAGE[0], 'python', [IMAGE[1], str(IMAGE_PRODUCT)], True))
    else:
        elsewhere.insert(0, IMAGE)
    if (IMAGE_PRODUCT / 'system').is_dir() and AAPT2.is_file():
        tests.append((HOME_ICONS[0], 'python',
                      [HOME_ICONS[1], '--product', str(IMAGE_PRODUCT),
                       '--aapt2', str(AAPT2)], False))
    else:
        elsewhere.insert(0, HOME_ICONS)
    if (IMAGE_PRODUCT / 'system').is_dir() and AAPT.is_file():
        tests.append((XML_CLASSES[0], 'python',
                      [XML_CLASSES[1], '--product', str(IMAGE_PRODUCT),
                       '--aapt', str(AAPT)], False))
    else:
        elsewhere.insert(0, XML_CLASSES)
    # Gated on the Launcher3 checkout, not on the file this reads. Asking about
    # the object under test turns its absence into a skip: the layout deleted,
    # or this project's patch not applied, used to list as "needs the AOSP tree"
    # on a build machine that has the AOSP tree (Codex 2026-10-31). The test
    # reads the file and fails when it is not there.
    if LAUNCHER_TREE.is_dir():
        tests.append((HOME_SCREEN[0], 'python',
                      [HOME_SCREEN[1], '--launcher-tree', str(LAUNCHER_TREE)], False))
    else:
        elsewhere.insert(0, HOME_SCREEN)
    if (AOSP_TREE / 'frameworks/base/data/fonts').is_dir():
        tests.append((FONT_AOSP[0], 'python',
                      [FONT_AOSP[1], '--aosp-tree', str(AOSP_TREE)], False))
    else:
        elsewhere.insert(0, FONT_AOSP)
    if (LOADER_KERNEL / 'zImage').is_file() and (OBSERVER / 'initramfs.cpio.gz').is_file():
        tests.append((HANDOFF_QEMU[0], 'steps',
                      [[HANDOFF_QEMU[1], '--output', '{work}/handoff-qemu']], True))
    else:
        elsewhere.insert(0, HANDOFF_QEMU)
    if (LOADER_KERNEL / 'zImage').is_file() and (FIXTURE / 'bin/busybox').is_file():
        tests.append((HANDOFF_CLIENT[0], 'steps',
                      [[HANDOFF_CLIENT[1], '--output', '{work}/handoff-client']], True))
    else:
        elsewhere.insert(0, HANDOFF_CLIENT)
    if (KERNEL_BUILD / 'build-result.json').is_file():
        tests.append((EXPORTER[0], 'python', [EXPORTER[1], '--build-dir', str(KERNEL_BUILD)], True))
    else:
        elsewhere.insert(0, EXPORTER)
    if KERNEL_ARCHIVE.is_file():
        tests.append((FFS[0], 'steps', [
            ['tools/tests/extract-stock-file.py', '--archive', str(KERNEL_ARCHIVE),
             '--path', FFS_FILE, '--output', '{work}/stock-kernel/' + FFS_FILE],
            [FFS[1], '--kernel-source', '{work}/stock-kernel', '--cc', '{cc}']], True))
    else:
        elsewhere.insert(0, FFS)
    for name, script, member, extra in BEFORE_AFTER:
        if KERNEL_ARCHIVE.is_file() and (PATCHED_KERNEL / member).is_file():
            tests.append((name, 'steps', [
                ['tools/tests/extract-stock-file.py', '--archive', str(KERNEL_ARCHIVE),
                 '--path', member, '--output', '{work}/%s-stock' % name],
                [script, '{work}/%s-stock' % name, str(PATCHED_KERNEL / member),
                 '{work}/%s.json' % name] + extra], True))
        else:
            elsewhere.insert(0, (name, script, BEFORE_AFTER_NEEDS))
    if EMOJI_FONT.is_file() and EMOJI_SEQUENCES.is_file():
        tests.append((EMOJI_VS[0], 'python',
                      [EMOJI_VS[1], '--font', str(EMOJI_FONT),
                       '--sequences', str(EMOJI_SEQUENCES)], False))
    else:
        elsewhere.insert(0, EMOJI_VS)
    # The three that run the built image in QEMU. They take about a minute and
    # a half each and need the image's file listing, which only root can make
    # (inspect-system-image.py reads the file system directly), so they run
    # when an export with that listing is there and are named with what they
    # want when it is not.
    #
    # They also need that export to be a userdebug one. All three work the same
    # way: init copies a test payload into /data and starts it as a service in
    # the shell domain. A release image refuses that, and three of AOSP's own
    # neverallow rules are why -- there is no payload path through /data that a
    # user build will run, so this is not a matter of finding the right
    # directory (the attempt and the denials are in each of their READMEs).
    # The release image is covered by everything else here, by what the export
    # itself checks, and on the device.
    export = built_export()
    payload = built_payload(export)
    if payload is not None:
        tests.append((DATA_IMAGE[0], 'steps', [
            ['tools/tests/unpack-payload-file.py', '--payload', str(payload),
             '--name', 'data', '--output', '{work}/seed-data.img'],
            ('c', [DATA_IMAGE[1]], ['{work}/seed-data.img'])], True))
    else:
        elsewhere.insert(0, DATA_IMAGE)
    variant = None
    if export is not None:
        try:
            variant = json.loads((export / 'build-result.json').read_text())['variant']
        except (OSError, KeyError, ValueError):
            variant = None
    for name, script in SMOKE:
        if export is not None and variant == 'userdebug':
            tests.append((name, 'steps', [[script, '--system-image', str(export / 'system.raw.img'),
                                           '--output', '{work}/' + name]], True))
        elif export is not None:
            elsewhere.insert(0, (name, script, 'a userdebug export of this tree (the export found is '
                                 '%s): a release image runs no test payload out of /data, by three '
                                 'of AOSP\'s neverallow rules' % variant))
        else:
            elsewhere.insert(0, (name, script, 'an export of the image this tree built, with its file '
                                 'listing (tools/inspect-system-image.py, as root)'))

    if args.list:
        for name, kind, what, linux in tests:
            parts = what[0] if kind == 'steps' else what
            first = next(s for s in parts if not s.startswith('-I'))
            print('%-20s %-7s %s%s' % (name, kind, first, '   (Linux only)' if linux else ''))
        for name, where, needs in elsewhere:
            print('%-20s %-7s %s   (needs %s)' % (name, 'elsewhere', where, needs))
        return 0

    on_linux = platform.system() == 'Linux'
    if not shutil.which(args.cc):
        print('no C compiler at %r; use --cc' % args.cc, file=sys.stderr)
        return 2
    if not shutil.which(args.cxx):
        print('no C++ compiler at %r; use --cxx' % args.cxx, file=sys.stderr)
        return 2
    passed, failed, skipped = [], [], []
    runners = {'python': run_python, 'c': run_c, 'cpp': run_cpp, 'steps': run_steps}
    with tempfile.TemporaryDirectory(prefix='pw3-tests-') as temporary:
        work = Path(temporary)
        (work / 'screen-payload.json').write_text(json.dumps(SCREEN_PAYLOAD))
        no_cpp = cpp_compiler_works(args.cxx, work)
        for name, kind, what, linux in tests:
            if args.filter and args.filter not in name:
                continue
            if linux and not on_linux:
                skipped.append((name, 'needs Linux headers; this is ' + platform.system()))
                print('SKIP  %-20s needs Linux headers' % name, flush=True)
                continue
            if kind == 'cpp' and no_cpp:
                skipped.append((name, no_cpp))
                print('SKIP  %-20s %s' % (name, no_cpp), flush=True)
                continue
            result = runners[kind](what, args.cxx if kind == 'cpp' else args.cc, work)
            if result.returncode:
                failed.append((name, result))
                print('FAIL  %s' % name, flush=True)
            else:
                passed.append(name)
                last = [line for line in (result.stdout + result.stderr).splitlines() if line.strip()]
                print('ok    %-20s %s' % (name, last[-1][:80] if last else ''), flush=True)
    for name, where, needs in elsewhere:
        if args.filter and args.filter not in name:
            continue
        skipped.append((name, 'needs ' + needs))
        print('SKIP  %-20s needs %s' % (name, needs), flush=True)

    print('\n%d passed, %d failed, %d not run here' % (len(passed), len(failed), len(skipped)))
    for name, result in failed:
        print('\n--- %s ---\n%s%s' % (name, result.stdout[-4000:], result.stderr[-4000:]))
    # A --filter that selects nothing is a failure, not a clean run of no tests.
    # A misspelled filter in a script or a habit used to print "0 passed, 0
    # failed, 0 not run here" and exit 0, which reads as the suite agreeing
    # (Codex 2026-10-31). Selecting tests that cannot run here is a different
    # answer and stays a pass.
    if args.filter and not (passed or failed or skipped):
        print('--filter %r selected no test; the names are in --list' % args.filter, file=sys.stderr)
        return 2
    return 1 if failed else 0


if __name__ == '__main__':
    raise SystemExit(main())
