#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Tests for the device-side installer (install.c), in the build VM: the installer's
steps run natively (test_driver.c) on small images, with a fake battery, network,
clock and server, and the user store is checked after each run.

Usage: test_install.py --zstd-archive zstd-1.5.6.tar.gz
"""
import argparse, hashlib, json, os, random, re, shutil, subprocess, sys, tarfile, tempfile, zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
ZSTD_FLAGS = ['-DZSTD_LEGACY_SUPPORT=0', '-DZSTD_TRACE=0', '-DZSTD_DISABLE_ASM', '-DDEBUGLEVEL=0']
BUILT = 1_800_000_000
E = {'ready': 0, 'battery': 102, 'download': 201, 'offline': 202, 'clock': 203, 'payload': 302, 'release': 303,
     'format': 304, 'space': 401, 'io': 402}
REPORT_HEADER = (ROOT / 'device/amazon/pw3/report/ReportArea.h').read_text()
# The report area's size and the two names the installer matches in a report, all
# from the header both sides compile, so this test cannot drift from them.
REPORT_BYTES = int(re.search(r'define PW3_REPORT_BYTES (\d+)', REPORT_HEADER).group(1))
REPORT_STOP = re.search(r'define PW3_REPORT_STOP "([^"]*)"', REPORT_HEADER).group(1)
REPORT_STOP_SYSTEM = re.search(r'define PW3_REPORT_STOP_SYSTEM_IMAGE "([^"]*)"', REPORT_HEADER).group(1)
REPORT_STOP_FORMAT = re.search(r'define PW3_REPORT_STOP_SYSTEM_FORMAT "([^"]*)"', REPORT_HEADER).group(1)
REPORT_COMPLETED = re.search(r'define PW3_REPORT_BOOT_COMPLETED "([^"]*)"', REPORT_HEADER).group(1)
REPORT_MADE = re.search(r'define PW3_REPORT_MADE "([^"]*)"', REPORT_HEADER).group(1).replace('\\n', '\n')
# The line that names the build a report came from. This one is the installer's
# own, not the shared header's, because its writer is the sampler's list of
# Android property names; the two spellings are held together by a check below.
INSTALL_C = (HERE / 'install.c').read_text()
REPORT_BUILD = re.search(r'define PW3_REPORT_BUILD "([^"]*)"', INSTALL_C).group(1)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def report_area(body, boot):
    """A report area as Android leaves it: the first line, the body, zeros to the end.

    Built here from the format's own rules rather than by calling the C, so that a
    change to pw3_report_build which the installer still accepts is caught by this
    side refusing it (ReportArea.h).
    """
    text = body.encode()
    head = ('PW3-REPORT-1 bytes=%d crc32=%08x boot=%d\n'
            % (len(text), zlib.crc32(text) & 0xffffffff, boot)).encode()
    area = head + text
    assert len(area) <= REPORT_BYTES, 'the report does not fit the area'
    return area + bytes(REPORT_BYTES - len(area))


def counts(acted):
    """The line the installer writes about report counts: which report it has
    acted on. One number, because one question is asked of it -- whether a
    report has already been acted on -- and that one may move in either
    direction. The greatest count ever seen was a second number here for two
    passes of review, standing in for the identity of the system that wrote a
    report, which the report carries itself (REPORT_BUILD).
    """
    return 'android=%d\n' % acted


def done_report(boot, rel, first=True):
    """A report from a start that reached boot_completed, as the sampler leaves it.

    The build is the release's own: a report says which system wrote it, and that
    is what tells a start of the system now installed from one of whatever was
    installed before it.
    """
    body = ''
    if first:
        body += 'snapshot=0 uptime_s=0\n%s%s\n%sunset\n' % (REPORT_BUILD, rel.incremental, REPORT_COMPLETED)
    body += 'snapshot=3 uptime_s=120\n%s%s\n%s1\n' % (REPORT_BUILD, rel.incremental, REPORT_COMPLETED)
    return report_area(body, boot)


# The count the installer plants in an area it makes: the clock, never below the
# release's build time (install.c, plant_count). The test clock is the build time
# unless a case passes now=.
PLANT = BUILT


def made_area(plant=PLANT):
    """An area as the installer makes it: a report of no start, saying so.

    Not zeros. An area has to be recognisable as ours from its own bytes, which
    is what lets a damaged one be repaired rather than left for dead, and 64 KiB
    of zeros is recognisable as nothing (ReportArea.h, PW3_REPORT_MADE).
    """
    return report_area(REPORT_MADE, plant)


def stop_report(boot, reason):
    """The area PID 1 leaves when it refuses to start (PreInit.c, report_stop)."""
    return report_area(REPORT_STOP + reason + '\n', boot)


def build(tmp, archive):
    lock = json.loads((ROOT / 'source-lock.json').read_text())['installer_zstd']
    if sha(archive.read_bytes()) != lock['archive_sha256']:
        sys.exit('the zstd archive is not the one in source-lock.json')
    with tarfile.open(archive) as tar:
        tar.extractall(tmp / 'zstd', filter='data')
    lib = tmp / 'zstd' / ('zstd-' + lock['version']) / 'lib'
    sources = sorted(list((lib / 'common').glob('*.c')) + list((lib / 'decompress').glob('*.c')))
    binary = tmp / 'test_driver'
    # zstd is built as for the device; our code with the address and undefined-behaviour sanitizers.
    objects = []
    for c in sources:
        o = tmp / ('zstd-' + c.parent.name + '-' + c.stem + '.o')
        subprocess.run(['gcc', '-std=gnu11', '-O2', '-c', *ZSTD_FLAGS, '-I', str(lib), '-I', str(lib / 'common'), str(c),
                        '-o', str(o)], check=True)
        objects.append(str(o))
    subprocess.run(['gcc', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-I', str(HERE), '-I', str(HERE.parent / 'unpack'), '-I', str(ROOT / 'boot/handoff'),
                    '-I', str(ROOT / 'device/amazon/pw3/report'),
                    '-I', str(ROOT / 'device/amazon/pw3/preinit'), '-I', str(lib),
                    str(HERE / 'install.c'), str(HERE / 'test_driver.c'), str(HERE.parent / 'unpack/unpack.c'),
                    str(HERE / 'fatcheck.c'), str(ROOT / 'boot/handoff/sha256.c'), *objects,
                    '-o', str(binary)], check=True)
    return binary


def store_shim(tmp):
    """A library that makes the user store behave in two ways this host cannot.

    `PW3_TEST_UNREADABLE` makes lstat fail with EIO for one named path. The
    installer has to tell "the store says there is no image" from "the store
    could not say", because it makes a new data image for the first and that
    would write over everything the user has read. No filesystem here can be
    made to fail lstat on one file while open still works, so the failure is
    injected around the real call instead of in the installer.

    `PW3_TEST_UNOPENABLE` makes open and fopen fail with EIO for one named path,
    while everything else about it -- its size, its contents -- stays as it is.
    That is the one way the installer can be shown the difference between "this
    file says X" and "I was not allowed to read this file", and both answers it
    used to give were wrong: a payload it could not open was deleted as a bad one
    (installer/unpack/unpack.c), and a state file it could not open was taken for
    there being no state file (read_state).

    `PW3_TEST_UNDELETABLE` makes unlink fail with EROFS for one named path, so
    that the installer can be caught saying a file was deleted when it was not.

    `PW3_TEST_FAIL_NTH` is `call:path:n`, and fails the nth call of `call` on
    `path` while every call before it goes through. Failing from the first call,
    which is all the switches above can do, never reaches the branches that come
    after a successful open: the `fstat` of the input, the *second* open of it in
    pw3_unpack, and a read that stops part way. Those were the three places the
    review of 2026-10-21 could not reach, and the three that classified an
    operation failure as a damaged package and deleted it.

    `PW3_TEST_FREE_BYTES` makes the store report that much free space. A store
    down to its last kilobytes is what the report area has to give way to, and
    filling a real filesystem to prove it would take as long as the store is
    large.
    """
    source = tmp / 'unreadable.c'
    source.write_text("""
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>
static int shut(const char *path) {
    const char *fail = getenv("PW3_TEST_UNOPENABLE");
    return fail && *fail && path && !strcmp(path, fail);
}
/* PW3_TEST_FAIL_NTH=call:path:n -- the nth call of `call` on `path` fails, the
 * ones before it do not. One counter per call name is enough: no test here needs
 * two different calls counted at once. `path` is compared only when the caller
 * has one; a read or an fstat is matched by the descriptor it was opened on. */
static int nth_hit(const char *call, const char *path, int fd) {
    static char watched_call[32];
    static char watched_path[512];
    static long watched_n;
    static long seen;
    static int parsed;
    if (!parsed) {
        const char *spec = getenv("PW3_TEST_FAIL_NTH");
        parsed = 1;
        if (spec && *spec) {
            const char *colon = strrchr(spec, ':');
            const char *first = strchr(spec, ':');
            if (colon && first && colon != first) {
                size_t call_len = (size_t)(first - spec);
                size_t path_len = (size_t)(colon - first - 1);
                if (call_len < sizeof(watched_call) && path_len < sizeof(watched_path)) {
                    memcpy(watched_call, spec, call_len);
                    memcpy(watched_path, first + 1, path_len);
                    watched_n = strtol(colon + 1, NULL, 10);
                }
            }
        }
    }
    if (!watched_n || strcmp(call, watched_call))
        return 0;
    /* A call with a path is matched by it; one with only a descriptor has
     * already been matched against watched_descriptor by its caller. There was a
     * second, never-assigned `watched_fd` compared here, which made every fstat
     * and every read miss -- found because the two cases failed. */
    if (path && strcmp(path, watched_path))
        return 0;
    (void)fd;
    if (++seen != watched_n)
        return 0;
    return 1;
}
/* The descriptor the watched path was last opened on, so that a read or an fstat
 * on it can be matched without a path of its own. */
static int watched_descriptor = -1;
/* Which open of the watched path that descriptor came from, so that a read can
 * be aimed at the reader that comes after a chosen open. pw3_unpack() opens the
 * input a second time to unpack from, and its first read is a different branch
 * from verify_input()'s -- failing "the first read" only ever reaches the first
 * of the two. */
static long open_sequence;
static long watched_open_sequence = -1;
static const char *watched_name(void) {
    static char path[512];
    const char *spec = getenv("PW3_TEST_FAIL_NTH");
    const char *first = spec ? strchr(spec, ':') : NULL;
    const char *colon = spec ? strrchr(spec, ':') : NULL;
    if (!first || !colon || colon == first) return NULL;
    if ((size_t)(colon - first - 1) >= sizeof(path)) return NULL;
    memcpy(path, first + 1, (size_t)(colon - first - 1));
    path[colon - first - 1] = 0;
    return path;
}
int open(const char *path, int flags, ...) {
    static int (*real)(const char *, int, mode_t);
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap);
    }
    if (!real) real = dlsym(RTLD_NEXT, "open");
    if (shut(path)) { errno = EIO; return -1; }
    if (nth_hit("open", path, -1)) { errno = EIO; return -1; }
    int fd = real(path, flags, mode);
    const char *watched = watched_name();
    if (watched && path && !strcmp(path, watched)) {
        open_sequence++;
        if (fd >= 0) {
            watched_descriptor = fd;
            watched_open_sequence = open_sequence;
        }
    }
    return fd;
}
/* glibc has exported fstat itself since 2.33 (2.35 here), so there is one symbol
 * to interpose and no __fxstat chain to get wrong. If a host ever has only the
 * old symbol this returns from dlsym as NULL and the test says so rather than
 * passing: PW3_SHIM_CHECK below proves the interposition on every run. */
int fstat(int fd, struct stat *out) {
    static int (*real)(int, struct stat *);
    if (!real) real = dlsym(RTLD_NEXT, "fstat");
    if (fd == watched_descriptor && nth_hit("fstat", NULL, fd)) { errno = EIO; return -1; }
    if (!real) { errno = ENOSYS; return -1; }
    return real(fd, out);
}
/* Both of them: a read into a buffer whose size the compiler knows becomes
 * __read_chk under _FORTIFY_SOURCE, which these flags turn on, so interposing
 * only `read` left every read in the installer going straight to libc. */
/* `read-after-open:path:k` -- the first read on the descriptor the kth open of
 * that path returned. One counter, so it fires once. */
static int read_after_open(int fd) {
    static int fired;
    const char *spec = getenv("PW3_TEST_FAIL_NTH");
    if (fired || fd != watched_descriptor || !spec || strncmp(spec, "read-after-open:", 16))
        return 0;
    const char *colon = strrchr(spec, ':');
    if (!colon || strtol(colon + 1, NULL, 10) != watched_open_sequence)
        return 0;
    fired = 1;
    return 1;
}
ssize_t read(int fd, void *buf, size_t count) {
    static ssize_t (*real)(int, void *, size_t);
    if (!real) real = dlsym(RTLD_NEXT, "read");
    if (read_after_open(fd)) { errno = EIO; return -1; }
    if (fd == watched_descriptor && nth_hit("read", NULL, fd)) { errno = EIO; return -1; }
    return real(fd, buf, count);
}
ssize_t __read_chk(int fd, void *buf, size_t count, size_t cap) {
    static ssize_t (*real)(int, void *, size_t, size_t);
    if (!real) real = dlsym(RTLD_NEXT, "__read_chk");
    if (read_after_open(fd)) { errno = EIO; return -1; }
    if (fd == watched_descriptor && nth_hit("read", NULL, fd)) { errno = EIO; return -1; }
    return real(fd, buf, count, cap);
}
FILE *fopen(const char *path, const char *mode) {
    static FILE *(*real)(const char *, const char *);
    if (!real) real = dlsym(RTLD_NEXT, "fopen");
    if (shut(path)) { errno = EIO; return NULL; }
    return real(path, mode);
}
int unlink(const char *path) {
    static int (*real)(const char *);
    const char *fail = getenv("PW3_TEST_UNDELETABLE");
    if (!real) real = dlsym(RTLD_NEXT, "unlink");
    if (fail && *fail && path && !strcmp(path, fail)) { errno = EROFS; return -1; }
    return real(path);
}
int lstat(const char *path, struct stat *out) {
    static int (*real)(const char *, struct stat *);
    const char *fail = getenv("PW3_TEST_UNREADABLE");
    if (!real) real = dlsym(RTLD_NEXT, "lstat");
    if (fail && *fail && !strcmp(path, fail)) { errno = EIO; return -1; }
    return real(path, out);
}
int statvfs(const char *path, struct statvfs *out) {
    static int (*real)(const char *, struct statvfs *);
    const char *free_bytes = getenv("PW3_TEST_FREE_BYTES");
    if (!real) real = dlsym(RTLD_NEXT, "statvfs");
    int got = real(path, out);
    if (!got && free_bytes && *free_bytes) {
        out->f_frsize = out->f_bsize = 512;
        out->f_bavail = out->f_bfree = strtoull(free_bytes, NULL, 10) / 512;
    }
    return got;
}
""")
    library = tmp / 'unreadable.so'
    subprocess.run(['gcc', '-shared', '-fPIC', '-O1', '-o', str(library), str(source), '-ldl'], check=True)
    # The installer is built with the address sanitizer, whose runtime insists on
    # coming first in the preload list.
    asan = subprocess.run(['gcc', '-print-file-name=libasan.so'], capture_output=True, text=True,
                          check=True).stdout.strip()
    return '%s:%s' % (asan, library)


def zstd(data):
    return subprocess.run(['zstd', '-q', '-c', '-19'], input=data, capture_output=True, check=True).stdout


class Release:
    def __init__(self, tmp, name, system, payload, template, data, **override):
        self.payload_name = 'readom-system-%s.img.zst' % name
        self.payload, self.system, self.data = payload, system, data
        template_path = tmp / ('template-%s.zst' % name)
        template_path.write_bytes(template)
        fields = {'version': name, 'built': BUILT, 'data_format': 1, 'payload_name': self.payload_name,
                  'payload_url': 'https://example.invalid/' + self.payload_name, 'payload_bytes': len(payload),
                  'payload_sha256': sha(payload), 'system_bytes': len(system), 'system_sha256': sha(system),
                  'template': template_path, 'template_bytes': len(template), 'template_sha256': sha(template),
                  'data_bytes': len(data), 'data_sha256': sha(data),
                  # Releases differ in their boot image unless a test says otherwise,
                  # which is what makes an upgrade between two of them compile the
                  # applications in /data again.
                  'boot_image': sha(b'boot image of ' + name.encode()),
                  # ro.build.version.incremental, as the sampler would read it out
                  # of this release's system. One per release, because that is what
                  # makes a report from one of them evidence about that one only.
                  'build_incremental': 'eng.test.' + name}
        fields.update(override)
        self.boot_image = fields['boot_image']
        got = fields['payload_url']
        self.payload_urls = list(got) if isinstance(got, (list, tuple)) else [got]
        self.incremental = fields['build_incremental']
        self.path = tmp / ('release-%s.txt' % name)
        # A list writes one line per element, which is how a release with more
        # than one origin is expressed: `payload_url` is the only repeatable key
        # the driver takes, and a dict cannot hold the same key twice.
        lines = []
        for key, value in fields.items():
            for one in (value if isinstance(value, (list, tuple)) else [value]):
                lines.append('%s=%s\n' % (key, one))
        self.path.write_text(''.join(lines))
        self.serve = tmp / ('serve-%s' % self.payload_name)
        self.serve.write_bytes(payload)


class Tests:
    def __init__(self, binary, tmp):
        self.binary, self.tmp, self.count, self.failures, self.n = binary, tmp, 0, 0, 0

    def store(self):
        self.n += 1
        user, work = self.tmp / ('user%d' % self.n), self.tmp / ('work%d' % self.n)
        (user / 'documents').mkdir(parents=True)
        work.mkdir()
        return user, work

    def run(self, rel, user, work, *options, env=None):
        p = subprocess.run([str(self.binary), str(rel.path), str(user), str(work), 'serve=%s' % rel.serve, *options],
                           capture_output=True, text=True, env={**os.environ, **env} if env else None)
        out = p.stdout
        result = [l for l in out.splitlines() if l.startswith('result ')]
        downloads = [l for l in out.splitlines() if l.startswith('downloads ')]
        code, short, upgraded, repaired = ((int(x) for x in result[0].split()[1:]) if result
                                           else (None, None, None, None))
        return {'exit': p.returncode, 'code': code, 'short': short, 'upgraded': upgraded, 'repaired': repaired,
                'out': out, 'err': p.stderr, 'downloads': int(downloads[0].split()[1]) if downloads else None}

    def check(self, name, condition, run=None):
        self.count += 1
        print('%-4s %s' % ('ok' if condition else 'FAIL', name))
        if not condition:
            self.failures += 1
            if run:
                print(run['out'][-3000:], run['err'][-3000:])


def state(user):
    p = user / 'android/state'
    return p.read_text() if p.exists() else None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--zstd-archive', type=Path, required=True)
    a = ap.parse_args()
    rnd = random.Random(11)
    with tempfile.TemporaryDirectory(prefix='pw3-install-test-') as t:
        tmp = Path(t)
        (tmp / 'build').mkdir()
        binary = build(tmp / 'build', a.zstd_archive)
        shim = store_shim(tmp / 'build')
        sys1 = rnd.randbytes(4 << 20) + bytes(18 << 20) + rnd.randbytes(2 << 20)
        sys2 = rnd.randbytes(4 << 20) + bytes(18 << 20) + rnd.randbytes(2 << 20)
        data = b'PW3 empty data image' + bytes((20 << 20) - 20)
        p1, p2, template = zstd(sys1), zstd(sys2), zstd(data)
        v1 = Release(tmp, 'v1', sys1, p1, template, data)
        v2 = Release(tmp, 'v2', sys2, p2, template, data)
        v2f = Release(tmp, 'v2f', sys2, p2, template, data, data_format=2)
        huge = Release(tmp, 'vh', sys1, p1, template, data, data_bytes=1 << 42)
        T = Tests(binary, tmp)
        img = lambda u, n: (u / 'android' / n).read_bytes() if (u / 'android' / n).exists() else None

        # A first install from a payload copied into documents/.
        u, w = T.store()
        (u / 'documents' / v1.payload_name).write_bytes(p1)
        r = T.run(v1, u, w, 'online=0')
        T.check('install from documents/: ready, not an upgrade', r['code'] == 0 and r['downloads'] == 0 and r['upgraded'] == 0, r)
        T.check('install from documents/: images are the release\'s', img(u, 'system.img') == sys1 and img(u, 'data.img') == data, r)
        T.check('install from documents/: state',
                state(u) == 'schema=1\nsystem=ready v1 %s\nboot=%s\ndata=ready 1\n'
                % (sha(sys1), v1.boot_image) + counts(0), r)
        T.check('install from documents/: payload deleted, work left empty',
                not (u / 'documents' / v1.payload_name).exists() and not any((u / 'android/download').iterdir()) and
                not any(w.iterdir()), r)
        r = T.run(v1, u, w, 'battery=5', 'online=0')
        T.check('second tap: ready without checks or writes', r['code'] == 0 and 'system ready, data kept' in r['out'] and
                r['upgraded'] == 0, r)

        # The report area: Android writes it by sector, so the installer owes it a
        # file of exactly that size, with clusters of its own, on every tap.
        report = u / 'android/report.bin'
        T.check('install: the report area is made, 64 KiB of zeros',
                report.stat().st_size == REPORT_BYTES and report.read_bytes() == made_area(), r)
        first = b'PW3-REPORT-1 bytes=5 crc32=00000000 boot=1\nbody\n'
        written = first + bytes(REPORT_BYTES - len(first))
        report.write_bytes(written)
        r = T.run(v1, u, w, 'online=0')
        T.check('a report Android wrote: the next tap leaves it alone',
                report.read_bytes() == written and 'report area:' not in r['out'], r)
        report.write_bytes(bytes(100))
        r = T.run(v1, u, w, 'online=0')
        T.check('an area truncated from a computer: remade at its size',
                r['code'] == 0 and report.read_bytes() == made_area() and 'report area: 65536 bytes, remade' in r['out'], r)
        report.unlink()
        r = T.run(v1, u, w, 'online=0')
        T.check('an area deleted from a computer: made again',
                r['code'] == 0 and report.read_bytes() == made_area() and 'report area: 65536 bytes, made' in r['out'], r)
        report.unlink()
        report.mkdir()
        r = T.run(v1, u, w, 'online=0')
        T.check('an area that cannot be made: said in the log, the tap still ready',
                r['code'] == 0 and 'report area: cannot make it' in r['out'], r)
        report.rmdir()
        # Not every file at that name is ours. A book somebody copied there is
        # left as it is, and so is one the store cannot say anything about.
        book = b'%PDF-1.4 a book somebody put there' + bytes(5000)
        report.write_bytes(book)
        r = T.run(v1, u, w, 'online=0')
        T.check('a file at that name that is not ours: left alone',
                r['code'] == 0 and report.read_bytes() == book and 'not ours, left alone' in r['out'], r)
        # Zeros at the front do not make it ours: every byte of it has to be
        # zero, and no more of them than an area holds.
        leading = bytes(20) + b'somebody else\'s data' + bytes(900)
        report.write_bytes(leading)
        r = T.run(v1, u, w, 'online=0')
        T.check('zeros at the front and data after them: left alone',
                r['code'] == 0 and report.read_bytes() == leading and 'not ours, left alone' in r['out'], r)
        longer = bytes(REPORT_BYTES + 4096)
        report.write_bytes(longer)
        r = T.run(v1, u, w, 'online=0')
        T.check('more zeros than an area holds: left alone',
                r['code'] == 0 and report.read_bytes() == longer and 'not ours, left alone' in r['out'], r)
        # An area truncated to less than its first line is still an area.
        report.write_bytes(bytes(5))
        r = T.run(v1, u, w, 'online=0')
        T.check('an area truncated shorter than its own first line: remade',
                r['code'] == 0 and report.read_bytes() == made_area(), r)
        report.unlink()
        r = T.run(v1, u, w, 'online=0', env={'LD_PRELOAD': str(shim), 'PW3_TEST_UNREADABLE': str(report)})
        T.check('an area the store cannot say anything about: not made, the tap still ready',
                r['code'] == 0 and not report.exists() and 'cannot say what is at that name' in r['out'], r)
        # And it never takes the last of the store: this run's own log is
        # written after it and may not be the write that fails.
        r = T.run(v1, u, w, 'online=0', env={'LD_PRELOAD': str(shim), 'PW3_TEST_FREE_BYTES': '%d' % (1 << 16)})
        T.check('a store down to its last bytes: no area, the tap still ready',
                r['code'] == 0 and not report.exists() and 'report area: 65536 bytes free, not made' in r['out'], r)
        report.write_bytes(b'')
        r = T.run(v1, u, w, 'online=0', env={'LD_PRELOAD': str(shim), 'PW3_TEST_FREE_BYTES': '%d' % (1 << 16)})
        T.check('an empty area and a store down to its last bytes: not filled',
                r['code'] == 0 and report.read_bytes() == b'' and 'report area: 65536 bytes free, not made' in r['out'], r)
        r = T.run(v1, u, w, 'online=0')
        T.check('and with the room back: made', r['code'] == 0 and report.read_bytes() == made_area(), r)

        # What the installer reads out of a report. Before the area, a system
        # image damaged without changing size left the tap handing over to a
        # start that refused it, for ever, with no error code and no way out but
        # a computer (docs/known-issues.md, 七).
        #
        # The two sides have to spell the refusal the same way, and nothing makes
        # them: PID 1 hands stop() a literal, and the installer matches the
        # beginning of it. So the agreement is checked here, where it matters --
        # a sentence reworded on one side and not the other brings the loop back
        # and nothing would fail.
        preinit = (ROOT / 'device/amazon/pw3/preinit/TestFileInit.h').read_text()
        installer = (HERE / 'install.c').read_text()
        # The sentence as a quoted literal, not the whole stop() call: PID 1
        # chooses between this one and "could not be read through" on whether
        # the image was read all the way, because only a real mismatch is worth
        # installing over.
        T.check('PID 1 stops with the sentence the installer looks for',
                '"%s"' % REPORT_STOP_SYSTEM in preinit and 'could not be read through' in preinit)
        # A quoted literal, not a mention: a comment that names the line is how
        # the next reader of install.c finds out what it is matching.
        T.check('and the installer takes it from the shared header, not a copy of its own',
                '"%s"' % REPORT_STOP_SYSTEM not in installer and 'PW3_REPORT_STOP_SYSTEM_IMAGE' in installer)
        # The second sentence that asks for the same repair. It is judged after
        # the hash and not before the report area, which is where it used to be:
        # a stop there wrote nowhere, and it pre-empted the hash, so a damaged
        # image left no reason behind and every tap handed over to the same
        # refusal. Checked the same way, on both sides.
        T.check('PID 1 stops with the format sentence the installer also looks for',
                '"%s"' % REPORT_STOP_FORMAT in preinit)
        T.check('and the installer takes that one from the shared header too',
                '"%s"' % REPORT_STOP_FORMAT not in installer and 'PW3_REPORT_STOP_SYSTEM_FORMAT' in installer)
        # Where it sits in PID 1 is the whole point of the change, so it is
        # checked and not left to the comment: after the SHA256 sentence, which
        # is after the report area is mapped.
        T.check('and PID 1 judges the superblock after the hash, not before the report area',
                preinit.index('"%s"' % REPORT_STOP_FORMAT) > preinit.index('"%s"' % REPORT_STOP_SYSTEM))
        # And the superblock keeps the two findings apart, as the hash does. A
        # pread that returns short or fails says nothing about the image: the
        # store may be momentarily unwell and the image perfectly good. Said
        # with the sentence above, the next tap would throw a good image away
        # and ask for 196 MB the reader may be offline for, where reading it
        # again is all that was ever needed. So it has a sentence of its own,
        # the installer matches neither it nor anything it begins with, and it
        # is reached first -- the read before what the read found.
        T.check('PID 1 says a superblock it could not read in words of its own',
                '"test system image superblock could not be read"' in preinit)
        T.check('and the installer does not act on that one',
                'superblock could not be read' not in installer)
        T.check('and PID 1 reads the superblock before it judges it',
                preinit.index('"test system image superblock could not be read"') <
                preinit.index('"%s"' % REPORT_STOP_FORMAT))
        # The launcher rotates the log folders under the install lock. Before
        # 2026-10-27 it rotated first: five taps during one long install were
        # enough for the fifth to delete the installing tap's own folder, and
        # the next write_result() there failed on a directory with no name. The
        # order is the fix, so the order is what is checked.
        launcher = (ROOT / 'boot/launcher/helper.c').read_text()
        T.check('the launcher takes the install lock before it rotates the log folders',
                launcher.index('install_prepare();') < launcher.index('pw3_logs_rotate('))
        # And settles its own folder in between. Before the install lock a run
        # that was not given the shared lock -- this kernel allocates a lock
        # before it looks for a conflicting one, and that allocation can fail
        # -- has nothing holding its folder, and a rotation can take it. After
        # the lock no rotation can run at all, so this is the one point where
        # "the folder is still ours" can be settled rather than hoped for, and
        # it has to come before this run's own rotation writes anything.
        T.check('and settles that its own folder is still there in between',
                launcher.index('install_prepare();')
                < launcher.index('"output_directory_gone"')
                < launcher.index('pw3_logs_rotate('))
        # Whose file is at the area's name has to be one rule, not one per
        # reader: this file answered it again in its own words once, the two
        # drifted, and a file cut off inside the magic then became ours in the
        # header and stayed somebody else's here -- the report channel dead for
        # good on a device whose area was truncated. So no reader outside the
        # header may compare the magic itself.
        for name, text in (('the installer', installer),
                           ('PID 1', preinit),
                           ('the launcher', (ROOT / 'boot/launcher/helper.c').read_text())):
            # The magic's *length* is a bound, not a comparison: a reader may
            # ask how many bytes the magic has without deciding for itself what
            # they are.
            T.check('%s asks whose file it is through the shared rule' % name,
                    not re.search(r'PW3_REPORT_MAGIC(?!_LEN)', text))

        # The same for the line that says the start completed: the sampler
        # decides which properties go into a report, and if it stopped putting
        # this one in, every tap would go on showing the compiling notice with
        # nothing failing anywhere.
        sampler = (ROOT / 'device/amazon/pw3/initcheck/BootReport.cpp').read_text()
        T.check('the sampler records the property the installer reads completion from',
                '"%s"' % REPORT_COMPLETED.rstrip('=') in sampler)
        T.check('and the installer reads it through the shared header too',
                '"%s"' % REPORT_COMPLETED not in installer and 'PW3_REPORT_BOOT_COMPLETED' in installer)
        # The build that wrote a report is the other line read from the body, and
        # the one the compiling notice now rests on. Its name is not in the shared
        # header: it is an Android property, its writer is the sampler's own list
        # of property names, and the header is a device source -- changing one
        # rebuilds the system image, which is a 194 MB download for everybody to
        # add a line the Android side would not read. So the two spellings are
        # held together here instead.
        T.check('the sampler records the property the installer reads the build from',
                '"%s"' % REPORT_BUILD.rstrip('=') in sampler)
        T.check('and the installer matches it through one name of its own',
                installer.count('"%s"' % REPORT_BUILD) == 1 and
                installer.count('PW3_REPORT_BUILD') > 1)
        ru, rw = T.store()
        (ru / 'documents' / v1.payload_name).write_bytes(p1)
        r = T.run(v1, ru, rw, 'online=0')
        T.check('a fresh install: nothing read, and the count recorded as none',
                r['code'] == 0 and r['repaired'] == 0 and counts(0) in state(ru), r)
        rrep = ru / 'android/report.bin'
        good = img(ru, 'system.img')
        with open(ru / 'android/system.img', 'r+b') as f:
            f.seek(len(good) // 2)
            f.write(bytes([good[len(good) // 2] ^ 0xff]))
        damaged = img(ru, 'system.img')
        T.check('a system image damaged without changing size: the record still believes it',
                len(damaged) == len(good) and damaged != good and
                r'system=ready v1 ' + sha(good) in state(ru), r)
        # Without a report, that is exactly the dead end: nothing to install.
        r = T.run(v1, ru, rw, 'online=0')
        T.check('and with no report, the tap hands over to it again',
                r['code'] == 0 and r['repaired'] == 0 and img(ru, 'system.img') == damaged, r)
        rrep.write_bytes(stop_report(1, REPORT_STOP_SYSTEM))
        (ru / 'documents' / v1.payload_name).write_bytes(p1)
        r = T.run(v1, ru, rw, 'online=0')
        T.check('a report saying Android refused it: written again from the payload',
                r['code'] == 0 and r['repaired'] == 1 and img(ru, 'system.img') == good and
                'Android refused the system image on start 1' in r['out'] and counts(1) in state(ru), r)
        # And not a second time: the refusal is still in the area until the next
        # Android start writes over it, and 512 MiB is not written again for a
        # report that has already been acted on.
        (ru / 'documents' / v1.payload_name).write_bytes(p1)
        r = T.run(v1, ru, rw, 'online=0')
        # The payload planted for this tap goes with it. A repair that is owed
        # asks for the system image again, and the cleanup only runs when it is
        # not; this tap owes none, so the 196 MB feeds nothing and is not left
        # on the reader's Kindle. Until 2026-10-27 the tap turned back before
        # the cleanup and this check read the payload as proof that nothing was
        # written -- the image itself says that, and says it of the right thing.
        T.check('the same refusal on the next tap: nothing installed',
                r['code'] == 0 and r['repaired'] == 0 and r['downloads'] == 0 and
                img(ru, 'system.img') == good and
                not (ru / 'documents' / v1.payload_name).exists(), r)
        # A refusal for a reason this side cannot act on is read and left alone.
        rrep.write_bytes(stop_report(2, 'test data mount or final protection check failed'))
        r = T.run(v1, ru, rw, 'online=0')
        T.check('a refusal about something else: recorded, nothing installed',
                r['code'] == 0 and r['repaired'] == 0 and img(ru, 'system.img') == good and
                'Android did not start on start 2' in r['out'] and counts(2) in state(ru), r)
        # A start that refused the image says nothing about compiling, so a tap
        # that repairs one must not take the compiling screen away.
        (ru / 'android/state').write_text(state(ru).replace(counts(2), 'compiling=1\n' + counts(2)))
        rrep.write_bytes(stop_report(3, REPORT_STOP_SYSTEM))
        (ru / 'documents' / v1.payload_name).write_bytes(p1)
        r = T.run(v1, ru, rw, 'online=0')
        T.check('repairing an image does not clear the compiling screen',
                r['code'] == 0 and r['repaired'] == 1 and r['upgraded'] == 1 and 'compiling=1\n' in state(ru), r)
        # The cross-link check runs on every tap that is about to write over an
        # image already there, which is this one. This host is not the device:
        # the store is on no FAT, so the whole of what the check can do here is
        # say that it could not look and leave the install alone -- which is the
        # branch every tap in this file takes, and the one that has to be said
        # out loud, because an install that silently stopped being checked looks
        # exactly like an install that was. The answer itself, on a mounted FAT
        # with a cross-link in it, is test_fatcheck.py's.
        T.check('the cross-link check says it could not look, and the tap installs anyway',
                r['code'] == 0 and
                'cross-link check did not run: no mounted vfat holds this image' in r['out'], r)
        # The other sentence that means the same thing about the image: the
        # superblock, which PID 1 judges when the hash was skipped because the
        # image's stamp still matched. Until 2026-10-27 it judged it before the
        # report area was mapped, so the refusal wrote nowhere and pre-empted
        # the hash as well; a reader whose image rotted had a device that would
        # not start Android and no way back but a computer.
        damaged_sb = bytearray(good)
        damaged_sb[1024 + 56:1024 + 58] = b'\x00\x00'	# s_magic, the ext4 signature
        (ru / 'android/system.img').write_bytes(bytes(damaged_sb))
        rrep.write_bytes(stop_report(4, REPORT_STOP_FORMAT))
        (ru / 'documents' / v1.payload_name).write_bytes(p1)
        r = T.run(v1, ru, rw, 'online=0')
        T.check('a report saying the superblock was refused: the image is written again too',
                r['code'] == 0 and r['repaired'] == 1 and img(ru, 'system.img') == good and
                'Android refused the system image on start 4' in r['out'], r)
        # An area that is not a report tells this side nothing, whichever way it
        # is not one. None of these may be read as a refusal or as a completed
        # start, and none of them may fail the tap.
        state_before = state(ru)
        valid = done_report(9, v1)
        # Padded by what the line actually measures, not by a number written out
        # beside it: as a hand-counted 41 this area was 65537 bytes, and a file
        # of ours that is not an area's size is remade whatever its content says,
        # so the case meant here -- an area's size, first line unreadable -- was
        # never once run.
        no_newline = b'PW3-REPORT-1 bytes=5 crc32=00000000 boot=1'
        for name, area in (('a body whose checksum does not match', valid[:60] + bytes([valid[60] ^ 1]) + valid[61:]),
                           ('a first line with no newline',
                            no_newline + bytes(REPORT_BYTES - len(no_newline))),
                           ('a length past the end of the area',
                            report_area('x\n', 4)[:13] + b'bytes=99999 ' + report_area('x\n', 4)[25:])):
            assert len(area) == REPORT_BYTES, 'this case is about content, so the size must be right: ' + name
            rrep.write_bytes(area)
            r = T.run(v1, ru, rw, 'online=0')
            T.check('an area that is %s: nothing read' % name,
                    r['code'] == 0 and r['repaired'] == 0 and state(ru) == state_before, r)
        # An area of zeros is what installers before 2026-10-22 made. It is
        # remade as a report, so that a byte turned over in it stays
        # recoverable -- zeros carry no evidence of whose file they are.
        rrep.write_bytes(bytes(REPORT_BYTES))
        r = T.run(v1, ru, rw, 'online=0')
        T.check('an area of zeros from an older installer: made again, and read as no report',
                r['code'] == 0 and rrep.read_bytes() == made_area() and
                'from an installer that wrote those' in r['out'] and r['repaired'] == 0, r)

        # An area as the installer makes it is a report, so one byte turned over
        # anywhere in it is recoverable: in the magic it is recognised as ours
        # and remade, anywhere else the magic is intact and PID 1 maps it. The
        # 64 KiB of zeros this replaced was recognisable as nothing, and a byte
        # turned over in it left the channel dead before Android had written to
        # it once.
        fresh = made_area()
        rrep.write_bytes(bytes([fresh[0] ^ 0xff]) + fresh[1:])
        r = T.run(v1, ru, rw, 'online=0')
        T.check('a new area whose magic lost a byte: remade',
                r['code'] == 0 and rrep.read_bytes() == made_area() and 'magic is damaged' in r['out'], r)
        rrep.write_bytes(fresh[:4096] + bytes([fresh[4096] ^ 1]) + fresh[4097:])
        r = T.run(v1, ru, rw, 'online=0')
        T.check('a new area whose zeros lost a byte: left for PID 1 to map and write over',
                r['code'] == 0 and rrep.read_bytes()[4096] == (fresh[4096] ^ 1), r)
        rrep.write_bytes(fresh)

        # The area the installer makes is not a report from Android: its count is
        # the one no start writes, and it says so in its body.
        kept_lines = [line for line in state(ru).splitlines()
                      if not line.startswith(('compiling=', 'compiling_after=', 'upgrade=', 'android='))]
        (ru / 'android/state').write_text(
            ''.join(line + '\n' for line in kept_lines) + 'compiling=1\n' + counts(50))
        rrep.write_bytes(made_area())
        r = T.run(v1, ru, rw, 'online=0')
        T.check('the area the installer made: not accounted for as a start',
                r['code'] == 0 and r['upgraded'] == 1 and 'compiling=1\n' in state(ru) and
                'no Android start has written to it' in r['out'] and
                # and the count it refers to is given up with it: that report is
                # not in this area, whatever its number was.
                counts(PLANT) in state(ru), r)
        # A report put back from a computer after that: it was written by the
        # release that was installed before this one, and says so, so it is no
        # evidence about this one however its count compares.
        rrep.write_bytes(done_report(2, v2))
        r = T.run(v1, ru, rw, 'online=0')
        T.check('an old release\'s completed report after it cannot clear the notice',
                r['code'] == 0 and r['upgraded'] == 1 and 'compiling=1\n' in state(ru) and
                'this entry installs' in r['out'], r)
        # And the first real start after a remade area clears it at once, with a
        # count far below anything this side has ever recorded: the counter
        # restarted by this installer's own hand, and nothing about the notice is
        # measured against it.
        rrep.write_bytes(done_report(PLANT + 1, v1))
        r = T.run(v1, ru, rw, 'online=0')
        T.check('while the first start after the remade area clears it',
                r['code'] == 0 and r['upgraded'] == 0 and 'compiling=1' not in state(ru) and
                counts(PLANT + 1) in state(ru), r)
        # A new area cut off inside its magic -- which is also what a write of it
        # that stopped early leaves. Zeros covered this while a new area was
        # zeros; one that says what it is has a magic to lose.
        rrep.write_bytes(made_area()[:5])
        r = T.run(v1, ru, rw, 'online=0')
        T.check('an area cut off inside its magic: made again',
                r['code'] == 0 and rrep.read_bytes() == made_area(), r)
        (ru / 'android/state').write_text(
            ''.join(line + '\n' for line in kept_lines) + 'compiling=1\n' + counts(50))

        # A report area of the right size whose first line lost a byte of its
        # magic. Every other check kept such a file -- the size is right -- and
        # PID 1 refuses to map it, so no report could ever be written again: the
        # compiling notice could never clear and a damaged system image could
        # never be found. It is known from the file itself: the first line still
        # parses and the body still checksums to what it declares, which
        # somebody else's file does not do.
        made = done_report(20, v1)
        rrep.write_bytes(bytes([made[0] ^ 0xff]) + made[1:])
        T.check('a damaged report area keeps its size', rrep.stat().st_size == REPORT_BYTES)
        r = T.run(v1, ru, rw, 'online=0')
        T.check('a report area whose magic lost a byte: emptied, not left dead',
                r['code'] == 0 and rrep.read_bytes() == made_area() and
                'magic is damaged' in r['out'], r)
        # A file of exactly that size at that name whose content is not a report
        # of ours is still not this installer's to write over, however the state
        # file got there. A record of having made an area at that path would
        # have said yes to this.
        book = bytes(range(256)) * (REPORT_BYTES // 256)
        rrep.write_bytes(book)
        r = T.run(v1, ru, rw, 'online=0')
        T.check('a file of the area\'s size that is not a report of ours: left alone',
                r['code'] == 0 and rrep.read_bytes() == book and 'not ours, left alone' in r['out'], r)
        # And a read that fails is not a damaged report either: a store that
        # failed one read may hold a perfectly good area, and emptying it would
        # throw the diagnostic and the count away over a fault that may not
        # repeat.
        # The second read of that file in a tap is the one this check makes: the
        # first is read_report's, at the top of the same tap.
        rrep.write_bytes(bytes([made[0] ^ 0xff]) + made[1:])
        r = T.run(v1, ru, rw, 'online=0',
                  env={'LD_PRELOAD': str(shim), 'PW3_TEST_FAIL_NTH': 'read:%s:2' % rrep})
        T.check('a damaged area the store will not read through: left alone',
                r['code'] == 0 and rrep.read_bytes()[0] == made[0] ^ 0xff and
                'unreadable after' in r['out'], r)
        r = T.run(v1, ru, rw, 'online=0')
        T.check('and with the read working again: emptied',
                r['code'] == 0 and rrep.read_bytes() == made_area(), r)

        # An area the store will not open at all.        # An area the store will not open at all. That is not "no report": this
        # side then has no baseline, and claiming the old one is how a report
        # about the system before this tap gets applied to the system after it.
        # So no count is recorded, and the tap after this one adopts whatever it
        # finds without concluding anything -- which costs one report and finds
        # a damaged image a tap later rather than never.
        rrep.write_bytes(done_report(30, v1))
        (ru / 'android/state').write_text(state_before)
        r = T.run(v1, ru, rw, 'online=0',
                  env={'LD_PRELOAD': str(shim), 'PW3_TEST_UNOPENABLE': str(rrep)})
        T.check('an area the store will not open: no count recorded, tap still ready',
                r['code'] == 0 and r['repaired'] == 0 and 'android=' not in state(ru) and
                'did not answer' in r['out'], r)
        r = T.run(v1, ru, rw, 'online=0')
        T.check('and the tap after it adopts the count without concluding',
                r['code'] == 0 and r['repaired'] == 0 and counts(30) in state(ru), r)
        (ru / 'android/state').write_text(state_before)

        # A state file from before reports were accounted for, against an area
        # that holds one: the count is adopted from the area, and the `upgrade=`
        # line such a file carries is read past. That line was the one piece of
        # compatibility with installers older than 2026-10-21, and it went on
        # 2026-10-29 with the rest of them: a device whose state file is that
        # old loses the "still compiling" notice for one tap and sees the
        # ordinary starting screen instead; the tap is not counted as an upgrade
        # either, because the only thing that said so was that line. The system
        # it already had is the one being installed, so nothing is rewritten.
        legacy = ''.join(line + '\n' for line in state_before.splitlines()
                         if not line.startswith(('compiling=', 'compiling_after=', 'upgrade=', 'android=')))
        (ru / 'android/state').write_text(legacy + 'upgrade=2\n')
        rrep.write_bytes(done_report(7, v2))
        r = T.run(v1, ru, rw, 'online=0')
        T.check('a state file from before the count: adopted, the old line read past',
                r['code'] == 0 and r['upgraded'] == 0 and
                counts(7) in state(ru) and 'taken as seen' in r['out'], r)
        rrep.write_bytes(done_report(PLANT + 1, v1))
        r = T.run(v1, ru, rw, 'online=0')
        T.check('and the start of this release after it clears it', r['code'] == 0 and r['upgraded'] == 0 and
                'compiling=1' not in state(ru) and counts(PLANT + 1) in state(ru), r)
        # Which start cleared the notice is a question about *whose* start it
        # was, and a report answers it: the one line of it that names the build
        # that wrote it (REPORT_BUILD). A copy of this file put back from a
        # computer is the one thing anybody can restore, and the report in it is
        # then about the system that was installed then -- which is the direction
        # that has somebody hold the power button in the middle of dexopt.
        #
        # Two passes of review went into making a count carry this instead, as
        # "the greatest count ever seen". Each fix opened the next hole, because
        # the number is in the report's first line, which the checksum does not
        # cover; because a remade area restarts the counter the watermark was
        # measuring; and because the two numbers went on one line, which put the
        # one that may not fall inside the condition that drops the one that may.
        # None of that is asked any more.
        kept_lines = [line for line in state(ru).splitlines()
                      if not line.startswith(('compiling=', 'compiling_after=', 'upgrade=', 'android='))]
        (ru / 'android/state').write_text(
            ''.join(line + '\n' for line in kept_lines) + 'compiling=1\n' + counts(50))
        rrep.write_bytes(done_report(2, v2))
        r = T.run(v1, ru, rw, 'online=0')
        T.check('a completed report from the release before this one: the notice stays',
                r['code'] == 0 and r['upgraded'] == 1 and 'compiling=1\n' in state(ru) and
                'this entry installs' in r['out'], r)
        # Not even with a count far ahead of anything this side has recorded: a
        # count says nothing about whose work it counted.
        rrep.write_bytes(done_report(99, v2))
        r = T.run(v1, ru, rw, 'online=0')
        T.check('nor one from that release with a count far ahead',
                r['code'] == 0 and r['upgraded'] == 1 and 'compiling=1\n' in state(ru), r)
        # And a report of the installed release clears it, wherever its count sits
        # against the ones before it.
        rrep.write_bytes(done_report(PLANT + 3, v1))
        r = T.run(v1, ru, rw, 'online=0')
        T.check('while one from the release installed here clears it',
                r['code'] == 0 and r['upgraded'] == 0 and 'compiling=1' not in state(ru) and
                counts(PLANT + 3) in state(ru), r)
        # The same release can have completed a start *before* the install that
        # owes this compiling: install A, install B over it (another boot image),
        # install A again. A's old report matches the build and says
        # boot_completed, and the only thing that tells it from the start this
        # install is waiting for is that it was written earlier -- which is what
        # compiling_after records, from the area's own count at the tap that set
        # the notice.
        (ru / 'android/state').write_text(
            ''.join(line + '\n' for line in kept_lines) +
            'compiling=1\ncompiling_after=50\n' + counts(50))
        rrep.write_bytes(done_report(2, v1))
        r = T.run(v1, ru, rw, 'online=0')
        T.check('this release\'s own report from before the install: the notice stays',
                r['code'] == 0 and r['upgraded'] == 1 and 'compiling=1\n' in state(ru) and
                'is not later than' in r['out'], r)
        # And the start this install is actually waiting for clears it.
        rrep.write_bytes(done_report(51, v1))
        r = T.run(v1, ru, rw, 'online=0')
        T.check('while the start after the install clears it',
                r['code'] == 0 and r['upgraded'] == 0 and 'compiling=1' not in state(ru) and
                'compiling_after' not in state(ru), r)
        # Clearing it has to reach the store. This tap installs nothing, accounts
        # for no new report -- the count in the area is the one it already acted
        # on -- and for as long as the clearing lived inside the branch that
        # records a report, the state file was never written: the notice went out
        # of memory and stayed on the store.
        (ru / 'android/state').write_text(
            ''.join(line + '\n' for line in kept_lines) + 'compiling=1\ncompiling_after=4\n' + counts(7))
        rrep.write_bytes(done_report(7, v1))
        r = T.run(v1, ru, rw, 'online=0')
        T.check('a tap that only clears the notice writes it down',
                r['code'] == 0 and 'compiling=1' not in state(ru) and counts(7) in state(ru), r)
        r = T.run(v1, ru, rw, 'online=0')
        T.check('and the tap after it does not show it again',
                r['code'] == 0 and r['upgraded'] == 0 and 'compiling=1' not in state(ru), r)
        # A completed report that names no build at all is not evidence either:
        # it is a file this side cannot place, and the cheap mistake is to leave
        # the notice up.
        (ru / 'android/state').write_text(
            ''.join(line + '\n' for line in kept_lines) + 'compiling=1\n' + counts(50))
        rrep.write_bytes(report_area('snapshot=3 uptime_s=120\n%s1\n' % REPORT_COMPLETED, 60))
        r = T.run(v1, ru, rw, 'online=0')
        T.check('a completed report that names no build: the notice stays',
                r['code'] == 0 and r['upgraded'] == 1 and 'compiling=1\n' in state(ru) and
                'does not say which build' in r['out'], r)

        # A tap that could not read the area records no count, and the next tap
        # adopts whatever it finds without concluding anything from the number --
        # the build is what it concludes from.
        (ru / 'android/state').write_text(
            ''.join(line + '\n' for line in kept_lines) + 'compiling=1\n' + counts(50))
        rrep.write_bytes(done_report(2, v2))
        r = T.run(v1, ru, rw, 'online=0',
                  env={'LD_PRELOAD': str(shim), 'PW3_TEST_UNOPENABLE': str(rrep)})
        T.check('a tap that could not read the area: no count recorded',
                r['code'] == 0 and 'android=' not in state(ru), r)
        r = T.run(v1, ru, rw, 'online=0')
        T.check('and the old release\'s report after it still cannot clear the notice',
                r['code'] == 0 and r['upgraded'] == 1 and 'compiling=1\n' in state(ru) and
                counts(2) in state(ru), r)

        # An area holding nothing but the report this installer writes when it
        # makes one: the count in the state refers to a report that is no longer
        # anywhere, and the next start will write count 1. Left standing, that 1
        # would look like the report already acted on, and the first refusal
        # after a remake would be passed over -- two minutes and a reboot for
        # nothing, until the count reached 2.
        (ru / 'android/state').write_text(
            ''.join(line + '\n' for line in kept_lines) + counts(1))
        rrep.write_bytes(made_area())
        r = T.run(v1, ru, rw, 'online=0')
        T.check('an area that was made again: the count it refers to is given up',
                r['code'] == 0 and counts(PLANT) in state(ru) and
                'refers to no report that is still there' in r['out'], r)
        rrep.write_bytes(stop_report(1, REPORT_STOP_SYSTEM))
        (ru / 'documents' / v1.payload_name).write_bytes(p1)
        r = T.run(v1, ru, rw, 'online=0')
        T.check('so the first refusal after it is acted on, not passed over',
                r['code'] == 0 and r['repaired'] == 1 and counts(1) in state(ru), r)

        # Making the area again changes no count other than that one: nothing the
        # installer decides rests on how far the counter had got.
        (ru / 'android/state').write_text(
            ''.join(line + '\n' for line in kept_lines) + 'compiling=1\n' + counts(50))
        damaged = made_area()
        rrep.write_bytes(bytes([damaged[0] ^ 0xff]) + damaged[1:])
        r = T.run(v1, ru, rw, 'online=0')
        T.check('making the area again leaves the count alone',
                r['code'] == 0 and counts(50) in state(ru) and 'seen=' not in state(ru) and
                rrep.read_bytes() == made_area(), r)

        # A count that went backwards is still a report this side has not seen.
        # It goes backwards when the area it was read from was not a report: a
        # write cut off by a flat battery leaves a checksum that does not match,
        # and the start after that counts from 1 again (ReportStop.h). Against
        # "greater than what I have accounted for" such a device would never be
        # heard from again, and a damaged system image would be back to the loop
        # with no way out -- which is the defect this whole mechanism replaced.
        kept_lines = [line for line in state(ru).splitlines()
                      if not line.startswith(('compiling=', 'compiling_after=', 'upgrade=', 'android='))]
        (ru / 'android/state').write_text(
            ''.join(line + '\n' for line in kept_lines) + counts(50))
        rrep.write_bytes(stop_report(1, REPORT_STOP_SYSTEM))
        (ru / 'documents' / v1.payload_name).write_bytes(p1)
        r = T.run(v1, ru, rw, 'online=0')
        # The report acted on follows the count down, which is the whole of what
        # this number is for.
        T.check('a count that restarted at 1 after a cut-off write: read, not passed over',
                r['code'] == 0 and r['repaired'] == 1 and counts(1) in state(ru), r)

        # Upgrades keep the data image.
        dpath = u / 'android/data.img'
        with open(dpath, 'r+b') as f:
            f.seek(1 << 20)
            f.write(b'user data')
        user_data = dpath.read_bytes()
        bad = bytearray(p2)
        bad[len(bad) // 2] ^= 1
        (u / 'documents' / v2.payload_name).write_bytes(bad)
        r = T.run(v2, u, w, 'online=0')
        T.check('upgrade with a damaged payload: E302, payload deleted', r['code'] == E['payload'] and
                not (u / 'documents' / v2.payload_name).exists(), r)
        T.check('upgrade with a damaged payload: old system still ready', img(u, 'system.img') == sys1 and
                state(u).startswith('schema=1\nsystem=ready v1 '), r)
        r = T.run(v2, u, w)
        T.check('upgrade by download: ready, one download, an upgrade', r['code'] == 0 and r['downloads'] == 1 and
                r['upgraded'] == 1, r)
        T.check('upgrade by download: new system, data kept', img(u, 'system.img') == sys2 and dpath.read_bytes() == user_data, r)
        r = T.run(v2f, u, w)
        T.check('release of another data format: E304, nothing changed', r['code'] == E['format'] and
                img(u, 'system.img') == sys2 and dpath.read_bytes() == user_data and 'system=ready v2 ' in state(u), r)

        # A state file cut short -- what a flat battery during a write leaves -- still
        # says schema=1, and read_state calls that known. The data image it no longer
        # mentions is the user's and must not be written over for the lack of a line.
        (u / 'android/state').write_text('schema=1\nsystem=ready v2 %s\n' % sha(sys2))
        r = T.run(v2, u, w, 'online=0')
        # The data line is put back; the boot image is not invented for a record
        # that does not have one (see "state with no boot image" below).
        T.check('state without a data line: data kept, state written again',
                dpath.read_bytes() == user_data and
                state(u) == 'schema=1\nsystem=ready v2 %s\ndata=ready 1\n' % sha(sys2) + counts(0), r)
        # The same for a release of another format, when the image is the size that
        # release uses. The format of an image the state file does not account for
        # cannot be recovered, so it is taken to be this release's and the size
        # decides: taking it to be format 1 would leave every device whose record was
        # lost after it was installed unable to start Android at all, because this
        # check is ahead of the hand-over (install.c, "data image the state file does
        # not account for"). An image of a size this release does not use is still
        # refused -- "data image without a state file, other size" below.
        (u / 'android/state').write_text('schema=1\nsystem=ready v2 %s\n' % sha(sys2))
        (u / 'documents' / v2f.payload_name).write_bytes(p2)
        r = T.run(v2f, u, w, 'online=0')
        T.check('state without a data line, another format of the same size: data kept, format recorded',
                r['code'] == 0 and dpath.read_bytes() == user_data and 'data=ready 2\n' in state(u), r)

        # An upgrade that brings the boot image already in place. Most releases of
        # this project are such (the 45 files of /system/framework/arm are compared
        # after every build): the applications in /data keep their compiled code, so
        # the screen that says this start compiles them must not be shown.
        u, w = T.store()
        (u / 'documents' / v1.payload_name).write_bytes(p1)
        T.run(v1, u, w, 'online=0')
        same = Release(tmp, 'v1s', sys2, p2, template, data, boot_image=v1.boot_image)
        (u / 'documents' / same.payload_name).write_bytes(p2)
        r = T.run(same, u, w, 'online=0')
        T.check('upgrade with the boot image already in place: installed, not an upgrade',
                r['code'] == 0 and img(u, 'system.img') == sys2 and r['upgraded'] == 0, r)
        T.check('upgrade with the boot image already in place: the state keeps it',
                state(u) == 'schema=1\nsystem=ready v1s %s\nboot=%s\ndata=ready 1\n'
                % (sha(sys2), v1.boot_image) + counts(PLANT), r)
        r = T.run(same, u, w, 'online=0')
        T.check('and the tap after it owes no upgrade screen either', r['code'] == 0 and r['upgraded'] == 0, r)

        # A state file from before boot images were recorded, with nothing to
        # install. The record is left alone: filling it in from this release would
        # rest on that same record naming what is on the store, and an image put
        # there by hand of the same size passes every check made here.
        kept = 'schema=1\nsystem=ready v1s %s\ndata=ready 1\n' % sha(sys2)
        (u / 'android/state').write_text(kept)
        r = T.run(same, u, w, 'online=0')
        # The record is left as it is; the report count is put in, because a
        # baseline has to exist before the first report can be acted on at all.
        T.check('state with no boot image, nothing to install: left as it is, no warning',
                r['code'] == 0 and r['upgraded'] == 0 and state(u) == kept + counts(0), r)
        # With a system to install and no record, what the applications were compiled
        # against is unknown, and the warning is shown rather than left out.
        (u / 'android/state').write_text(kept)
        (u / 'documents' / v2.payload_name).write_bytes(p2)
        r = T.run(v2, u, w, 'online=0')
        T.check('state with no boot image and a system to install: an upgrade',
                r['code'] == 0 and r['upgraded'] == 1 and 'compiling=1\n' in state(u), r)

        # The compiling an upgrade started is owed until it is done. Installing a
        # release that brings the boot image already in place adds no compiling of
        # its own, and must not take that away: the applications in /data may still
        # be compiled against the one before.
        u, w = T.store()
        (u / 'documents' / v1.payload_name).write_bytes(p1)
        T.run(v1, u, w, 'online=0')
        (u / 'documents' / v2.payload_name).write_bytes(p2)
        r = T.run(v2, u, w, 'online=0')
        T.check('an upgrade that changes the boot image: owed until a start completes',
                r['upgraded'] == 1 and 'compiling=1\n' in state(u), r)
        after = Release(tmp, 'v2b', sys1, p1, template, data, boot_image=v2.boot_image)
        (u / 'documents' / after.payload_name).write_bytes(p1)
        r = T.run(after, u, w, 'online=0')
        T.check('then one with the same boot image: still owed, and this start is told',
                r['code'] == 0 and img(u, 'system.img') == sys1 and r['upgraded'] == 1 and
                'compiling=1\n' in state(u), r)

        # Checks before anything is written.
        u, w = T.store()
        r = T.run(v1, u, w, 'battery=20')
        T.check('battery 20%, not charging: E102, no images', r['code'] == E['battery'] and img(u, 'system.img') is None, r)
        r = T.run(v1, u, w, 'battery=20', 'charging=1')
        T.check('battery 20%, charging: installs', r['code'] == 0 and img(u, 'system.img') == sys1, r)
        u, w = T.store()
        r = T.run(v1, u, w, 'battery=unknown')
        T.check('battery unknown: installs', r['code'] == 0, r)
        u, w = T.store()
        r = T.run(v1, u, w, 'online=0')
        T.check('no payload, offline: E202', r['code'] == E['offline'] and r['downloads'] == 0, r)
        r = T.run(v1, u, w, 'now=%d' % (BUILT - 3 * 86400))
        T.check('clock before the release: E203', r['code'] == E['clock'] and r['downloads'] == 0, r)
        r = T.run(huge, u, w)
        T.check('not enough space: E401 with the shortfall', r['code'] == E['space'] and r['short'] > (1 << 41), r)

        # Downloads.
        u, w = T.store()
        r = T.run(v1, u, w, 'attempt=18:200:100000', 'attempt=56:200:200000')
        T.check('download resumed over three attempts: ready', r['code'] == 0 and r['downloads'] == 3 and
                img(u, 'system.img') == sys1, r)
        u, w = T.store()
        r = T.run(v1, u, w, 'attempt=56:200:300000', 'attempt=7:0:0', 'attempt=7:0:0')
        part = u / 'android/download' / (v1.payload_name + '.part')
        T.check('download stalls: E201, part kept', r['code'] == E['download'] and r['downloads'] == 3 and
                part.stat().st_size == 300000, r)
        r = T.run(v1, u, w)
        T.check('next tap resumes the part: ready', r['code'] == 0 and r['downloads'] == 1 and img(u, 'system.img') == sys1 and
                not part.exists(), r)
        u, w = T.store()
        r = T.run(v1, u, w, 'attempt=22:404:0')
        T.check('release file missing (HTTP 404): E303', r['code'] == E['release'], r)
        # The same 404 as curl reports it over HTTP/2, where --fail aborts the
        # stream and that is a receive failure (56) rather than 22. The release
        # address is this project's own and negotiates HTTP/2, so this is the
        # code the device will actually see; keyed on 22, the reader would get
        # six attempts and then "check your Wi-Fi" for a file that is not there.
        u, w = T.store()
        r = T.run(v1, u, w, 'attempt=56:404:0')
        T.check('release file missing over HTTP/2 (curl 56, HTTP 404): E303', r['code'] == E['release'], r)
        # A receive failure with no answer from the server is still just a
        # failed attempt: it must not be read as a missing release.
        u, w = T.store()
        r = T.run(v1, u, w, 'attempt=56:0:0')
        T.check('a receive failure with no HTTP answer is not a missing release',
                r['code'] != E['release'], r)
        # "No resuming, start over" (curl 33/36) on every attempt, the last one
        # included. That branch deletes the part and continues, so the last one
        # used to leave the loop with nothing to rename and report a storage
        # fault for a network failure -- the Kindle blamed for the server.
        u, w = T.store()
        r = T.run(v1, u, w, *(['attempt=33:0:0'] * 6))
        T.check('no resuming on every attempt, the last included: E201, not a storage fault',
                r['code'] == E['download'], r)
        # 416 is the server saying the range asked for began at or past the end
        # of its object -- equal counts (RFC 9110), and the website session
        # measured its own route answering 416 at exactly equal, 2026-10-30.
        # A part as long as the manifest never gets here (the attempt breaks out
        # above it), so a 416 is the object being shorter than the manifest: it
        # is not that file, and starting over only downloads the short object
        # whole and says the same thing an attempt later. curl has no settled
        # exit code for it -- with --continue-at it reports success on some
        # versions, since "the range is unsatisfiable" and "you have it all"
        # read the same to it, and --fail does not change that -- so both codes
        # are asked for here, and neither may be read as "the server's file is
        # <the part's own length> bytes".
        for code, how in ((0, 'curl reporting success'), (22, 'curl failing')):
            u, w = T.store()
            part = u / 'android/download' / (v1.payload_name + '.part')
            part.parent.mkdir(parents=True, exist_ok=True)
            part.write_bytes(p1[:100000])
            r = T.run(v1, u, w, 'attempt=%d:416:0' % code)
            T.check('HTTP 416 with %s: E303, the part is left alone' % how,
                    r['code'] == E['release'] and part.exists() and part.stat().st_size == 100000, r)
        # Several origins. Reachability is not a property of the release: on
        # 2026-10-31 this project's own device got 515 KB of 191 MB in 26 minutes
        # from the one address the entry carried, and the same bytes came back at
        # 390 KB/s from another network. So the entry carries a list, and what is
        # pinned here is that the list is walked and that one bad origin cannot
        # condemn the release.
        #
        # Both budgets are per origin, so a one-origin release behaves exactly as
        # it did before any of this existed -- every test above this point is that
        # case, and none of them changed.
        # Its own name, because the name is the release description's filename
        # (release-<name>.txt): built as another 'v1' it overwrote v1's own file,
        # and every test after this point went on using `v1` while running
        # against two origins. Nothing failed, so nothing said so.
        two = Release(tmp, 'v1two', sys1, p1, template, data,
                      payload_url=['https://first.invalid/' + v1.payload_name,
                                   'https://second.invalid/' + v1.payload_name])
        u, w = T.store()
        r = T.run(two, u, w, 'attempt=22:404:0')
        asked = [l.split(' ', 1)[1] for l in r['out'].splitlines() if l.startswith('download ')]
        T.check('the first origin does not have it: the second is asked and it installs',
                r['code'] == 0 and r['downloads'] == 2 and asked == two.payload_urls and
                'asking the next one' in r['out'] and img(u, 'system.img') == sys1, r)
        # Every origin saying the same thing is the release missing, which is the
        # one answer that must still reach E303 rather than "check your Wi-Fi".
        u, w = T.store()
        r = T.run(two, u, w, 'attempt=22:404:0', 'attempt=22:404:0')
        T.check('every origin answers 404: E303, not a download failure',
                r['code'] == E['release'] and r['downloads'] == 2, r)
        # The stale-mirror case, and it is not hypothetical: the Worker's GitHub
        # fallback held the previous release's payload on 2026-10-31, 497 bytes
        # shorter than the one R2 held. A 416 from that origin takes the origin
        # out of the running, not the release.
        u, w = T.store()
        part = u / 'android/download' / (two.payload_name + '.part')
        part.parent.mkdir(parents=True, exist_ok=True)
        part.write_bytes(p1[:100000])
        r = T.run(two, u, w, 'attempt=0:416:0')
        T.check('one origin holds a shorter file: the next one finishes it',
                r['code'] == 0 and r['downloads'] == 2 and img(u, 'system.img') == sys1, r)
        # Nothing arrived: ask the next one rather than spending the whole budget
        # on an origin that cannot be reached. This is the shape the device hit.
        u, w = T.store()
        r = T.run(two, u, w, 'attempt=7:0:0')
        asked = [l.split(' ', 1)[1] for l in r['out'].splitlines() if l.startswith('download ')]
        T.check('nothing arrived from the first origin: the second is asked',
                r['code'] == 0 and asked == two.payload_urls, r)
        # And when no origin moves a byte it still stops, at two dead attempts
        # per origin -- the same rule as before, counted over the list.
        u, w = T.store()
        r = T.run(two, u, w, *(['attempt=7:0:0'] * 8))
        T.check('no origin moves a byte: E201 after two dead attempts each',
                r['code'] == E['download'] and r['downloads'] == 4, r)
        # Bytes at a rate that cannot finish. curl 100 is the floor the download
        # is given (--speed-limit 1024 --speed-time 60): under 1 KB/s for a
        # minute, which for 191 MB is more than two days. Counted as progress it
        # keeps the origin, which is what spent 26 minutes on one address on
        # 2026-10-31 -- four attempts, each one "more bytes than there were".
        u, w = T.store()
        r = T.run(two, u, w, 'attempt=100:200:1000')
        asked = [l.split(' ', 1)[1] for l in r['out'].splitlines() if l.startswith('download ')]
        T.check('the first origin is under the speed floor: the second is asked',
                r['code'] == 0 and r['downloads'] == 2 and asked == two.payload_urls and
                img(u, 'system.img') == sys1, r)
        # And the bytes it did deliver are kept: the second origin resumes the
        # part rather than starting the 191 MB again.
        T.check('what the slow origin delivered is resumed, not thrown away',
                'part 1000 ->' in r['out'], r)
        one = Release(tmp, 'v1one', sys1, p1, template, data)
        u, w = T.store()
        r = T.run(one, u, w, *(['attempt=100:200:1000'] * 8))
        T.check('one origin, always under the floor: E201 after two attempts, not six',
                r['code'] == E['download'] and r['downloads'] == 2, r)
        # A release one origin serves slowly while another answers 404 is a
        # release that is there. `refused` counts origins saying it is not, so
        # bytes reset it whatever their rate; folded into the rule above, this
        # reached E303 on the second 404.
        u, w = T.store()
        r = T.run(two, u, w, 'attempt=22:404:0', 'attempt=100:200:1000', 'attempt=22:404:0', 'attempt=100:200:1000')
        T.check('one origin 404s while another serves slowly: not E303',
                r['code'] == 0 and img(u, 'system.img') == sys1, r)

        u, w = T.store()
        v1.serve.write_bytes(p1 + b'x' * (4 << 20))
        r = T.run(v1, u, w)
        v1.serve.write_bytes(p1)
        part = u / 'android/download' / (v1.payload_name + '.part')
        # Bounded, not exact: the size is looked at between writes here and once a
        # second on the device, so one of those may land before the kill. What must
        # not happen is the answer being written until the store is full.
        T.check('server file larger than the manifest: E303, the part stays bounded',
                r['code'] == E['release'] and (not part.exists() or part.stat().st_size <= len(p1) + (1 << 20)), r)
        u, w = T.store()
        (u / 'documents' / v1.payload_name).write_bytes(p1[:1000])
        r = T.run(v1, u, w)
        T.check('partial copy in documents/: downloaded, copy deleted', r['code'] == 0 and r['downloads'] == 1 and
                not (u / 'documents' / v1.payload_name).exists(), r)
        u, w = T.store()
        (u / 'android/download').mkdir(parents=True)
        (u / 'android/download' / v1.payload_name).write_bytes(bytes(len(p1)))
        r = T.run(v1, u, w)
        T.check('damaged downloaded payload: E302, deleted', r['code'] == E['payload'] and
                not (u / 'android/download' / v1.payload_name).exists(), r)
        r = T.run(v1, u, w)
        T.check('next tap downloads again: ready', r['code'] == 0 and r['downloads'] == 1, r)

        # Power lost while writing.
        u, w = T.store()
        (u / 'documents' / v1.payload_name).write_bytes(p1)
        r = T.run(v1, u, w, 'die=system:%d' % (8 << 20))
        T.check('stopped while writing the system image: state says writing', r['exit'] == 99 and
                state(u) == 'schema=1\nsystem=writing\n' + counts(0), r)
        r = T.run(v1, u, w)
        T.check('next tap writes it again: ready', r['code'] == 0 and img(u, 'system.img') == sys1 and img(u, 'data.img') == data, r)
        u, w = T.store()
        (u / 'documents' / v1.payload_name).write_bytes(p1)
        r = T.run(v1, u, w, 'die=data:%d' % (8 << 20))
        T.check('stopped while writing the data image: state says writing', r['exit'] == 99 and
                state(u).endswith('data=writing\n' + counts(0)) and 'system=ready v1' in state(u), r)
        r = T.run(v1, u, w, 'online=0')
        T.check('next tap makes the data image again: ready', r['code'] == 0 and img(u, 'data.img') == data, r)
        u, w = T.store()
        (u / 'documents' / v1.payload_name).write_bytes(p1)
        r = T.run(v1, u, w, 'die=system:%d' % len(sys1))
        T.check('stopped after the system image, before the state: writing', r['exit'] == 99 and
                state(u) == 'schema=1\nsystem=writing\n' + counts(0), r)
        r = T.run(v1, u, w)
        T.check('next tap: ready', r['code'] == 0 and img(u, 'system.img') == sys1, r)

        # Data images the installer did not record.
        u, w = T.store()
        (u / 'android').mkdir()
        (u / 'android/data.img').write_bytes(data[:-1] + b'x')
        (u / 'documents' / v1.payload_name).write_bytes(p1)
        r = T.run(v1, u, w)
        # A system image over a data image that is kept is an upgrade, so the
        # record also says the applications may still be compiling.
        T.check('data image without a state file, release\'s size: kept', r['code'] == 0 and
                img(u, 'data.img') == data[:-1] + b'x' and r['upgraded'] == 1 and
                state(u).endswith('data=ready 1\ncompiling=1\ncompiling_after=%d\n' % PLANT + counts(PLANT)), r)
        # The floor is in the area as well as in the state file: the install that
        # set the notice wrote the area itself, carrying the count it planted, so
        # the next start reports that count plus one. A floor recorded in the
        # state and not planted in the area would be a number nothing on the
        # device had to agree with.
        T.check('the install that set the notice planted its count in the area',
                (u / 'android/report.bin').read_bytes() == made_area(PLANT), r)
        # And the replay the plant refuses: a completed report of this very
        # build, kept since the last time it was installed, carries a count from
        # before this install -- which is now, by construction, smaller.
        (u / 'android/report.bin').write_bytes(done_report(50, v1))
        r = T.run(v1, u, w, 'online=0')
        T.check('this build\'s own report from an earlier install: the notice stays',
                r['code'] == 0 and r['upgraded'] == 1 and 'compiling=1\n' in state(u) and
                'is not later than' in r['out'], r)
        (u / 'android/report.bin').write_bytes(made_area(PLANT))
        # However many taps are cut short, the compiling screen stays: counting
        # them is what made the fourth tap say "about three minutes" to a device
        # that was still compiling (docs/known-issues.md, 八).
        for tap in range(1, 6):
            r = T.run(v1, u, w, 'online=0')
            T.check('the compiling screen survives interrupted start %d' % tap,
                    r['code'] == 0 and r['upgraded'] == 1 and 'compiling=1\n' in state(u), r)
        # A new report is not by itself a completed start: the sampler starts
        # when the storage is up, on purpose, and writes after two minutes --
        # so an upgrade still compiling at that point reports while it compiles.
        # Reading that as "finished" would clear the notice in the middle of the
        # work, which is the mistake the tap counter made.
        (u / 'android/report.bin').write_bytes(
            report_area('snapshot=3 uptime_s=120\n%s%s\n%sunset\n'
                        % (REPORT_BUILD, v1.incremental, REPORT_COMPLETED), 1))
        r = T.run(v1, u, w, 'online=0')
        T.check('a report from a start that did not complete: still compiling',
                r['code'] == 0 and r['upgraded'] == 1 and 'compiling=1\n' in state(u) and
                counts(1) in state(u) and 'did not reach boot_completed' in r['out'], r)
        # And one that did takes it away, on that tap and on every tap after it.
        # The last reading of the property is the one that counts: the first
        # round of every start there has ever been says unset.
        (u / 'android/report.bin').write_bytes(done_report(PLANT + 2, v1))
        r = T.run(v1, u, w, 'online=0')
        T.check('a report from a completed start: the ordinary starting screen',
                r['code'] == 0 and r['upgraded'] == 0 and 'compiling=1' not in state(u) and
                counts(PLANT + 2) in state(u) and 'reached boot_completed' in r['out'], r)
        r = T.run(v1, u, w, 'online=0')
        T.check('and the tap after it, with the same report: still ordinary',
                r['code'] == 0 and r['upgraded'] == 0 and counts(PLANT + 2) in state(u), r)
        u, w = T.store()
        (u / 'android').mkdir()
        (u / 'android/data.img').write_bytes(data + b'x')
        r = T.run(v1, u, w)
        T.check('data image without a state file, other size: E304, kept', r['code'] == E['format'] and
                img(u, 'data.img') == data + b'x' and img(u, 'system.img') is None, r)
        (u / 'android/data.img').unlink()
        (u / 'android/state').write_text('schema=1\ndata=ready 1\n')
        (u / 'documents' / v1.payload_name).write_bytes(p1)
        r = T.run(v1, u, w)
        T.check('state has a data image the store does not: a new one', r['code'] == 0 and img(u, 'data.img') == data, r)


        # The store cannot say whether an image is there. Not the same as its
        # not being there: making a new data image over one that is only
        # unreadable loses everything the user has read. The file is there and
        # writable throughout; only lstat on it fails, as a bad sector or a
        # busy controller would make it fail.
        u, w = T.store()
        (u / 'documents' / v1.payload_name).write_bytes(p1)
        r = T.run(v1, u, w, 'online=0')
        mine = b'what the user read' + data[18:]
        (u / 'android/data.img').write_bytes(mine)
        blind = {'LD_PRELOAD': str(shim), 'PW3_TEST_UNREADABLE': str(u / 'android/data.img')}
        r = T.run(v1, u, w, 'online=0', env=blind)
        T.check('an image the store cannot stat: E402, nothing written',
                r['code'] == E['io'] and img(u, 'data.img') == mine, r)
        r = T.run(v1, u, w, 'online=0')
        T.check('and the next tap, with the store readable again: ready, still the user\'s',
                r['code'] == 0 and img(u, 'data.img') == mine, r)

        # The state file cannot be opened. Not the same as there not being one:
        # with no state file a data image already on the store is read as a
        # complete one of the first format, so a first install interrupted while
        # that image was being made -- half of it written, the state file saying
        # data=writing -- came out as E304 instead, which tells the reader to
        # carry on with the previous version that a first install does not have.
        u, w = T.store()
        (u / 'documents' / v1.payload_name).write_bytes(p1)
        r = T.run(v1, u, w, 'online=0')
        T.check('an install, to have a state file and an image to cut short', r['code'] == 0, r)
        with (u / 'android/data.img').open('r+b') as f:
            f.truncate(8 << 20)
        deaf = {'LD_PRELOAD': str(shim), 'PW3_TEST_UNOPENABLE': str(u / 'android/state')}
        r = T.run(v1, u, w, 'online=0', env=deaf)
        T.check('a state file the store cannot open, over a half-made image: E402, not E304',
                r['code'] == E['io'] and img(u, 'data.img') is not None, r)
        r = T.run(v1, u, w, 'online=0')
        T.check('and with it readable again: E304, which is what the state file actually says',
                r['code'] == E['format'], r)

        # A payload the store will not open is not a bad payload. It used to be
        # deleted and the screen used to say the package was damaged: a reader
        # with no Wi-Fi then has to go back to a computer for a 200 MB file,
        # over a failure that said nothing about the bytes in it.
        u, w = T.store()
        payload = u / 'documents' / v1.payload_name
        payload.write_bytes(p1)
        closed = {'LD_PRELOAD': str(shim), 'PW3_TEST_UNOPENABLE': str(payload)}
        r = T.run(v1, u, w, 'online=0', env=closed)
        T.check('a payload the store cannot open: E402, and the payload is still there',
                r['code'] == E['io'] and payload.exists() and payload.read_bytes() == p1, r)
        r = T.run(v1, u, w, 'online=0')
        T.check('and with it readable again: installed from it, nothing downloaded',
                r['code'] == 0 and r['downloads'] == 0, r)

        # A payload whose size the store will not give is not a payload of the
        # wrong size. Taken as one, the run went on to download it instead, and
        # on a device with no Wi-Fi that ends at E202 -- "connect to Wi-Fi",
        # about a file the reader had already copied across.
        u, w = T.store()
        payload = u / 'documents' / v1.payload_name
        payload.write_bytes(p1)
        unseen = {'LD_PRELOAD': str(shim), 'PW3_TEST_UNREADABLE': str(payload)}
        r = T.run(v1, u, w, 'online=0', env=unseen)
        T.check('a payload the store cannot stat: E402, not E202, and it is still there',
                r['code'] == E['io'] and payload.exists() and payload.read_bytes() == p1, r)
        # And the copy this run was never going to use does not stop it. Asking
        # about both and then testing the pair made an unreadable leftover in
        # android/download/ fail an install that had its payload in documents/.
        (u / 'android/download').mkdir(parents=True, exist_ok=True)
        leftover = u / 'android/download' / v1.payload_name
        leftover.write_bytes(p1)
        blind_leftover = {'LD_PRELOAD': str(shim), 'PW3_TEST_UNREADABLE': str(leftover)}
        r = T.run(v1, u, w, 'online=0', env=blind_leftover)
        T.check('an unreadable leftover in android/download/ while documents/ has the payload: installed',
                r['code'] == 0 and r['downloads'] == 0, r)
        # The part's own length, when the store cannot say it. The download loop
        # asks for it in three places and each is its own branch; the one at the
        # end folded this in with "not the manifest's length" and reported a
        # download that would not finish, sending the reader to look at Wi-Fi
        # over a Kindle that cannot be read (a Codex review, 2026-10-30). This
        # reaches the first of the three, before curl is started at all: there is
        # no point asking a tool to resume a file nothing here can stat.
        u, w = T.store()
        blind = u / 'android/download' / (v1.payload_name + '.part')
        blind.parent.mkdir(parents=True, exist_ok=True)
        blind.write_bytes(p1[:100000])
        r = T.run(v1, u, w, env={'LD_PRELOAD': str(shim), 'PW3_TEST_UNREADABLE': str(blind)})
        T.check('a part the store cannot stat: E402, not a download or a release fault',
                r['code'] == E['io'] and r['downloads'] == 0, r)
        # The space check counts what is already on the store as room it does
        # not have to find. A file whose size the store will not give used to
        # count as nothing there, which makes every number too large -- and the
        # answer E401 gives is "delete books", over a file that is on the drive
        # (a Codex review, 2026-10-30).
        u, w = T.store()
        (u / 'documents' / v1.payload_name).write_bytes(p1)
        u_system = u / 'android' / 'system.img'
        u_system.parent.mkdir(parents=True, exist_ok=True)
        u_system.write_bytes(b'\0' * 4096)
        r = T.run(v1, u, w, 'online=0',
                  env={'LD_PRELOAD': str(shim), 'PW3_TEST_UNREADABLE': str(u_system)})
        T.check('a system image the store cannot stat: E402, not a space fault',
                r['code'] == E['io'], r)
        # A leftover part is only this run's business when this run may have to
        # resume it. Read as fatal wherever it appeared, an unreadable one
        # stopped an install whose payload was already in documents/ -- the same
        # mistake the payload choice was fixed for, one file further on (a Codex
        # review, 2026-10-30).
        u, w = T.store()
        (u / 'documents' / v1.payload_name).write_bytes(p1)
        stale = u / 'android/download' / (v1.payload_name + '.part')
        stale.parent.mkdir(parents=True, exist_ok=True)
        stale.write_bytes(p1[:100000])
        r = T.run(v1, u, w, 'online=0',
                  env={'LD_PRELOAD': str(shim), 'PW3_TEST_UNREADABLE': str(stale)})
        T.check('an unreadable leftover part while documents/ has the payload: installed',
                r['code'] == 0 and r['downloads'] == 0, r)

        # The three branches that come after a successful open, which a switch
        # that fails from the first call cannot reach. Each used to be
        # PW3_UNPACK_INPUT_INCOMPLETE, which is the caller's instruction to
        # delete the payload and blame it.
        #
        # verify_input() opens the input, fstats it, then reads it through;
        # pw3_unpack() opens it a second time to unpack from. So on the watched
        # path: fstat #1 is verify_input's, read #1 is its first read, and
        # open #2 is the one pw3_unpack does.
        # The fourth branch of that group, "the input is not a plain file", is
        # not here: install_system() only reaches unpack() through file_size(),
        # which requires S_ISREG, so the payload path cannot be a directory. It
        # is classified correctly and there is nothing that can produce it.
        for nth, what in (('fstat:%s:1', 'the fstat of the input fails'),
                          ('read:%s:1', 'the first read of the input fails'),
                          ('open:%s:2', 'the second open of the input fails'),
                          ('read-after-open:%s:2',
                           'the first read of the second open fails')):
            u, w = T.store()
            payload = u / 'documents' / v1.payload_name
            payload.write_bytes(p1)
            env = {'LD_PRELOAD': str(shim), 'PW3_TEST_FAIL_NTH': nth % payload}
            r = T.run(v1, u, w, 'online=0', env=env)
            T.check('%s: E402, and the payload is still there' % what,
                    r['code'] == E['io'] and payload.exists() and payload.read_bytes() == p1, r)
            r = T.run(v1, u, w, 'online=0')
            T.check('and then it installs from the same payload', r['code'] == 0 and r['downloads'] == 0, r)

        # And when the package really is damaged but the store will not delete
        # it, the log says that. It used to say "deleted" without looking, and a
        # log that says a file went while the file is still sitting there sends
        # the next look in the wrong direction.
        u, w = T.store()
        payload = u / 'documents' / v1.payload_name
        payload.write_bytes(p1[:-1] + bytes([p1[-1] ^ 0xff]))
        stuck = {'LD_PRELOAD': str(shim), 'PW3_TEST_UNDELETABLE': str(payload)}
        r = T.run(v1, u, w, 'online=0', env=stuck)
        T.check('a damaged payload the store will not delete: E302, and the log does not claim it went',
                r['code'] == E['payload'] and payload.exists() and 'could not be deleted' in r['out'], r)

        # A part left over from a server that sent too much is deleted before the
        # space check, which would otherwise stop first and ask the user to delete
        # books while the installer's own leftover is what is in the way.
        u, w = T.store()
        (u / 'android/download').mkdir(parents=True)
        part = u / 'android/download' / (v1.payload_name + '.part')
        part.write_bytes(bytes(len(p1) + (1 << 20)))
        (u / 'documents' / v1.payload_name).write_bytes(p1)
        r = T.run(v1, u, w, 'online=0')
        # Before the space check, which is the point: the part counts against the
        # free space, so the old order stopped at E401 and asked the user to
        # delete books. The log says which came first.
        lines = r['out'].splitlines()
        left = [i for i, l in enumerate(lines) if 'part larger than the payload is left over' in l]
        space = [i for i, l in enumerate(lines) if l.startswith('log space:')]
        T.check('oversized part is deleted, and before the space check',
                r['code'] == 0 and not part.exists() and img(u, 'system.img') == sys1 and
                left and space and left[0] < space[0], r)

        # The payload that install_system() deletes is left behind when a tap was
        # cut off between the state file saying system=ready and that deletion. It
        # then holds 196 MB that check_space() counts as reclaimable only while
        # the system still has to be written, so the data image was refused for
        # want of room the installer's own leftover was holding, and the screen
        # asked for books to be deleted.
        u, w = T.store()
        (u / 'android').mkdir()
        (u / 'android/system.img').write_bytes(sys1)
        (u / 'android/state').write_text('schema=1\nsystem=ready v1 %s\nboot=%s\n' % (sha(sys1), v1.boot_image))
        (u / 'documents' / v1.payload_name).write_bytes(p1)
        r = T.run(v1, u, w, 'online=0')
        lines = r['out'].splitlines()
        left = [i for i, l in enumerate(lines) if 'file(s) of this installer were left over' in l]
        space = [i for i, l in enumerate(lines) if l.startswith('log space:')]
        T.check('a payload left over with the system in place is deleted, before the space check',
                r['code'] == 0 and not (u / 'documents' / v1.payload_name).exists() and
                img(u, 'data.img') == data and left and space and left[0] < space[0], r)

        # And with the data image in place as well, which is where this was
        # missed until 2026-10-27: an upgrade keeps its data, so the tap after
        # an interrupted one has nothing to install, turned back at the "both in
        # place" return, and never reached the cleanup -- and neither did any
        # tap after that, so the 196 MB stayed for good. A part is planted with
        # it, because the whole folder goes and not the payload by name.
        u, w = T.store()
        (u / 'documents' / v1.payload_name).write_bytes(p1)
        T.run(v1, u, w, 'online=0')
        (u / 'documents' / v1.payload_name).write_bytes(p1)
        part = u / 'android/download' / (v1.payload_name + '.part')
        part.write_bytes(b'x' * 1024)
        r = T.run(v1, u, w, 'online=0')
        T.check('nothing left to install, and an interrupted tap\'s leftovers still go',
                r['code'] == 0 and not (u / 'documents' / v1.payload_name).exists() and
                not part.exists() and img(u, 'system.img') == sys1 and img(u, 'data.img') == data, r)

        # A part with no payload beside it. The condition used to be "is there a
        # payload", so a part on its own was left behind for good by the same
        # return; the folder is what is asked about now.
        u, w = T.store()
        (u / 'documents' / v1.payload_name).write_bytes(p1)
        T.run(v1, u, w, 'online=0')
        part = u / 'android/download' / (v1.payload_name + '.part')
        part.write_bytes(b'x' * 1024)
        r = T.run(v1, u, w, 'online=0')
        T.check('a part on its own, with nothing left to install: it goes too',
                r['code'] == 0 and not part.exists() and img(u, 'data.img') == data, r)

        # The payload is deleted on the strength of the state file saying
        # system=ready, so it goes only once the folder holding that file is
        # known to be on the disk. With the folder unopenable the flush cannot
        # be asked for at all, which is one of the two ways folder_on_disk()
        # answers no -- the other is the fsync itself failing, which this shim
        # has no way to produce. The 196 MB is kept rather than destroyed on a
        # promise.
        #
        # The tap is also not handed over, and this is an upgrade whose data
        # image came from an earlier release: the state file cannot say which
        # data=ready reached the disk and which is still in the page cache, and
        # a store that will not confirm anything is not one to decide that on.
        # The reader loses nothing by the refusal -- their image is untouched
        # and still there after the restart E402 asks for -- where guessing
        # wrong the other way loses everything in it.
        u, w = T.store()
        (u / 'documents' / v1.payload_name).write_bytes(p1)
        T.run(v1, u, w, 'online=0')
        (u / 'documents' / v2.payload_name).write_bytes(p2)
        r = T.run(v2, u, w, 'online=0',
                  env={'LD_PRELOAD': str(shim), 'PW3_TEST_UNOPENABLE': str(u / 'android')})
        T.check('an upgrade whose state folder will not flush: installed, kept, and not handed over',
                r['code'] == E['io'] and img(u, 'system.img') == sys2 and img(u, 'data.img') == data and
                (u / 'documents' / v2.payload_name).exists() and 'the payload is kept' in r['out'], r)
        # The tap after it, with the folder answering again. Stated as its own
        # check first: without it this one passes when the keeping and the
        # cleaning are both gone, because then the payload was already deleted
        # by the tap above and "it is not there" is true for the wrong reason.
        T.check('the kept payload is there to be taken away',
                (u / 'documents' / v2.payload_name).exists(), r)
        r = T.run(v2, u, w, 'online=0')
        T.check('and the tap after it, which can flush, takes the payload away',
                r['code'] == 0 and not (u / 'documents' / v2.payload_name).exists(), r)

        # The tap that makes a data image is held to a harder rule. A state file
        # saying data=ready that got no further than the page cache lets the
        # reader into Android on an image the next tap will read as data=writing
        # and remake from the empty template -- everything they did in between,
        # gone. So that tap is refused rather than handed over, which costs
        # nothing: nothing has been written into the image yet.
        u, w = T.store()
        (u / 'documents' / v1.payload_name).write_bytes(p1)
        r = T.run(v1, u, w, 'online=0',
                  env={'LD_PRELOAD': str(shim), 'PW3_TEST_UNOPENABLE': str(u / 'android')})
        T.check('a new data image whose state will not flush: refused, not handed over',
                r['code'] == E['io'] and 'would not flush' in r['out'], r)
        # The tap after it, with the store no better. This one makes no data
        # image of its own: it reads the data=ready the tap above wrote, so the
        # question "did this tap flush what it wrote" has nothing to say about
        # it, and the answer until 2026-10-27 was to hand over on a line that
        # may never have left the page cache. The reader taps again because the
        # screen told them to restart and they did not, and what they do in
        # Android after that is what the next tap remakes from the empty
        # template. So the way out asks the disk, whatever this tap did.
        r = T.run(v1, u, w, 'online=0',
                  env={'LD_PRELOAD': str(shim), 'PW3_TEST_UNOPENABLE': str(u / 'android')})
        T.check('and the tap after it, with nothing of its own to write, is refused as well',
                r['code'] == E['io'] and 'would not flush' in r['out'], r)
        # And with the folder answering, the same tap finishes the job.
        r = T.run(v1, u, w, 'online=0')
        T.check('and the tap after that one installs and hands over',
                r['code'] == 0 and img(u, 'system.img') == sys1 and img(u, 'data.img') == data, r)

        # And an upgrade whose folder still will not answer keeps the payload
        # again, rather than taking the first "both in place" as leave enough.
        u, w = T.store()
        (u / 'documents' / v1.payload_name).write_bytes(p1)
        T.run(v1, u, w, 'online=0')
        blind_folder = {'LD_PRELOAD': str(shim), 'PW3_TEST_UNOPENABLE': str(u / 'android')}
        (u / 'documents' / v2.payload_name).write_bytes(p2)
        T.run(v2, u, w, 'online=0', env=blind_folder)
        r = T.run(v2, u, w, 'online=0', env=blind_folder)
        T.check('nothing to install and the folder still will not flush: kept again',
                r['code'] == E['io'] and (u / 'documents' / v2.payload_name).exists() and
                'would not flush' in r['out'], r)

        # A certificate that does not verify is not a Wi-Fi problem; retrying
        # cannot help, and the clock is what the user can do something about.
        u, w = T.store()
        r = T.run(v1, u, w, 'attempt=60:0:0')
        T.check('curl 60 (the certificate did not verify): E203, not E201',
                r['code'] == E['clock'], r)

        print('%d of %d passed' % (T.count - T.failures, T.count))
        sys.exit(1 if T.failures else 0)


if __name__ == '__main__':
    main()
