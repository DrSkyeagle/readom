#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""The storage entry built outside AOSP, and the three things that make it
trustworthy.

`tools/build-preinit.py` is the port's answer to gap G1: `pw3-testfile-init` is
normally an AOSP module (clang, bionic, BoringSSL), so a Voyage entry could not
be produced while there is no AOSP tree. The script compiles the same
`PreInit.c` with musl and a project-owned SHA-256, and proves three things
before it calls the build good: the SHA-256 passes the NIST vectors, the board
gate accepts this board's kernel release and refuses the other model's, and the
output is a static ARM EABI5 executable carrying the V12 contract string.

Each of those is checked here again on the build machine. The first two need
only a host C compiler; the cross build needs the musl toolchain and the ARM
kernel headers, and is skipped with the reason when either is missing (a
machine without them can still run this test, it just cannot make the binary).

Nothing here starts QEMU or touches the network.
"""
import importlib.util
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BUILDER = ROOT / 'tools/build-preinit.py'
# The tool's defaults, repeated so a test run says what it skipped on.
MUSL = Path('/home/builder/toolchains/musl-arm/bin/musl-gcc')
KERNEL_HEADERS = Path('/usr/arm-linux-gnueabi/include')
# The one program contract string (preinit/TestFileInit.h, pw3_pack.py
# CURRENT_TESTFILE_CONTRACT). A change here is a change to what PID 1 accepts.
CONTRACT = ('PW3_TESTFILE_INIT_V12_DM_CLUSTER_EXTENTS_FAT_READONLY_VERIFY_THEN_RW_ANDROID_TMPFS_COVERED'
            '_KINDLE_ROOT_VIEW_READ_ONLY_DATA_SIZED_IMAGES_SYSTEM_STAMP_BACKGROUND_RECHECK'
            '_FAT_CROSSLINK_OPTIONAL_REPORT_AREA_IMAGE_OWNS_APP_CODE_THREE_KEY_CMDLINE_NO_PARTITIONING')

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
    spec = importlib.util.spec_from_file_location('pw3_build_preinit', BUILDER)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    # run() appends to this global; main() sets it up, and so does the test.
    module.transcript = []
    return module


def host_compiler():
    return shutil.which('gcc') or shutil.which('cc')


def sha_selftest(module, include, work):
    """Compile and run the same NIST vectors the tool runs."""
    try:
        module.host_sha_selftest(include, work)
    except SystemExit as failure:
        check(False, 'the SHA-256 self-test refused: %s' % (failure,))
        return
    binary = work / 'preinit-sha256-selftest'
    check(binary.is_file(), 'the SHA-256 self-test binary was not produced')
    if not binary.is_file():
        return
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    check(result.returncode == 0, 'the SHA-256 self-test exited %d: %s'
          % (result.returncode, result.stdout[-400:]))
    for vector in ('empty', 'abc', '448-bit message', "one million 'a'"):
        check('ok %s' % vector in result.stdout,
              'the SHA-256 self-test did not report the %s vector' % vector)
    check('FAIL' not in result.stdout, 'a SHA-256 vector failed: %s' % result.stdout[-400:])
    # A wrong digest in PID 1 is reported as a damaged image on the device, so
    # the vectors are the check that matters; the text above is its evidence.


def board_gate(module, board, other, include, work):
    try:
        module.host_board_gate(board, other, include, work)
    except SystemExit as failure:
        check(False, 'release_is_this_board() for board %r refused its own check: %s'
              % (board, failure))
        return
    check(True, 'release_is_this_board() accepts %s and refuses %s' % (board, other))


def cross_build(module, include, work):
    if not MUSL.is_file() or not KERNEL_HEADERS.is_dir():
        skip('the static ARM build needs the musl toolchain (%s) and the ARM kernel '
             'headers (%s); one is missing here' % (MUSL, KERNEL_HEADERS))
        return
    out = work / 'out'
    out.mkdir()
    try:
        facts = module.build_one('voyage', out, include, work, str(MUSL), str(KERNEL_HEADERS))
    except SystemExit as failure:
        check(False, 'the cross build of the voyage preinit refused: %s' % (failure,))
        return
    binary = out / facts['binary']['name']
    check(binary.is_file() and binary.stat().st_size == facts['binary']['bytes'],
          'the built binary is not the file its facts describe')
    elf = facts['elf']
    check(elf['class'] == 'ELF32' and elf['type'] == 'EXEC' and elf['machine'] == 'ARM',
          'the preinit is not a 32-bit ARM executable: %r' % (elf,))
    check('Version5 EABI' in (elf['flags'] or ''), 'the preinit is not EABI5: %r' % elf['flags'])
    check(elf['has_interp'] is False, 'the preinit has an INTERP segment, so it is not static')
    check(bool(elf['exec_loads']), 'the preinit has no executable LOAD segment')
    check(elf['problems'] == [], 'the tool found problems it kept: %r' % elf['problems'])
    strings = facts['strings']
    check(strings['contract'] and strings['problems'] == [],
          'the preinit does not carry the V12 contract string: %r' % strings['problems'])
    check(module.CONTRACT == CONTRACT,
          'the build used a contract string other than the V12 one')
    stripped = out / facts['stripped']['name']
    check(stripped.is_file() and stripped.stat().st_size < binary.stat().st_size,
          'the stripped copy of the preinit is missing or not smaller')
    check('voyage' in facts['compile'],
          'the compile command does not name the board it built for: %s' % facts['compile'])


def main():
    check(BUILDER.is_file(), '%s is missing' % BUILDER.name)
    if not BUILDER.is_file():
        return report()
    module = load()
    check(module.CONTRACT == CONTRACT,
          'build-preinit.py carries a contract string other than the V12 one')
    check('-static' in module.CROSS_FLAGS,
          'the cross build does not link statically: %r' % module.CROSS_FLAGS)
    check('-Werror' in module.CROSS_FLAGS,
          'the cross build does not treat warnings as errors: %r' % module.CROSS_FLAGS)

    compiler = host_compiler()
    if compiler is None:
        skip('the SHA-256 self-test and the board gate need a host C compiler')
    with tempfile.TemporaryDirectory(prefix='pw3-preinit-test-') as temporary:
        work = Path(temporary)
        include = work / 'include'
        include.mkdir()
        header = module.write_sha_header(include)
        text = header.read_text() if header.is_file() else ''
        check(header.is_file(), 'the generated <openssl/sha.h> is missing')
        for name in ('SHA256_CTX', 'SHA256_Init', 'SHA256_Update', 'SHA256_Final', 'SHA256_DIGEST_LENGTH'):
            check(name in text, 'the generated <openssl/sha.h> has no %s' % name)
        if compiler is not None:
            sha_selftest(module, include, work)
            # The gate is compiled from the untouched PreInit.c (main renamed),
            # so this is the source that runs as PID 1, not a copy of it.
            board_gate(module, 'voyage', 'pw3', include, work)
            board_gate(module, 'pw3', 'voyage', include, work)
        cross_build(module, include, work)
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
