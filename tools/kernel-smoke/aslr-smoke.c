/* SPDX-License-Identifier: MIT
 * Real ARM mmap/exec/sysctl regression in the diskless test kernel only.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#define SYSCTL "/proc/sys/vm/mmap_rnd_bits"
#define PAGE 4096UL
#define SAMPLE_COUNT 32
#define MAP_COUNT 512
static int checks, failed, completed_samples;
struct sample { uint32_t address; int adjacent; int dense; };
static void check(int ok, const char *name) {
    ++checks; if (!ok) ++failed;
    printf("aslr-smoke %s: %s\n", name, ok ? "PASS" : "FAIL");
}
static int isolated(void) {
    struct utsname u; struct statfs s;
    return !uname(&u) && strstr(u.release, "-pw3-core-smoke") &&
        !strcmp(u.machine, "armv7l") && !statfs("/", &s) &&
        ((unsigned long)s.f_type == 0x858458f6UL || (unsigned long)s.f_type == 0x01021994UL);
}
static int read_bits(void) {
    char b[20]; int fd = open(SYSCTL, O_RDONLY); ssize_t n;
    if (fd < 0) return -1;
    n = read(fd, b, sizeof(b)-1); close(fd);
    if (n < 1) return -1;
    b[n] = 0; return atoi(b);
}
static int write_bits(const char *value) {
    int fd = open(SYSCTL, O_WRONLY); ssize_t n; size_t size = strlen(value);
    if (fd < 0) return -1;
    n = write(fd, value, size); close(fd);
    return n == (ssize_t)size ? 0 : -1;
}
static int sample_child(void) {
    struct sample s = {0, 0, 0}; void *maps[MAP_COUNT]; unsigned i;
    void *a = mmap(NULL, PAGE, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    void *b = mmap(NULL, PAGE, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (a == MAP_FAILED || b == MAP_FAILED) return 2;
    s.address = (uint32_t)(uintptr_t)a;
    s.adjacent = (uintptr_t)b == (uintptr_t)a + PAGE;
    for (i=0; i<MAP_COUNT; ++i) {
        maps[i] = mmap(NULL, 1UL<<20, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE, -1, 0);
        if (maps[i] == MAP_FAILED) return 3;
        if (i && (uintptr_t)maps[i] != (uintptr_t)maps[0] + ((uintptr_t)i<<20)) return 4;
    }
    s.dense = 1;
    return write(3, &s, sizeof(s)) == (ssize_t)sizeof(s) ? 0 : 5;
}
static int sample_exec(int no_randomize, struct sample *result) {
    int p[2], status; size_t used = 0; pid_t pid;
    if (pipe(p)) return -1;
    pid = fork();
    if (!pid) {
        close(p[0]);
        if (dup2(p[1], 3) < 0) _exit(120);
        if (p[1] != 3) close(p[1]);
        if (no_randomize && personality(personality(0xffffffffUL) | ADDR_NO_RANDOMIZE) == -1) _exit(121);
        execl("/aslr-smoke", "/aslr-smoke", "--sample", (char *)0);
        _exit(122);
    }
    close(p[1]);
    if (pid < 0) { close(p[0]); return -1; }
    while (used < sizeof(*result)) {
        ssize_t n = read(p[0], (char *)result+used, sizeof(*result)-used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        used += (size_t)n;
    }
    close(p[0]);
    if (waitpid(pid, &status, 0) != pid) return -1;
    if (used == sizeof(*result) && WIFEXITED(status) && !WEXITSTATUS(status)) {
        ++completed_samples; return 0;
    }
    return -1;
}
static int samples(int bits, uint32_t *span) {
    uint32_t low = UINT32_MAX, high = 0; struct sample s; unsigned i;
    for (i=0; i<SAMPLE_COUNT; ++i) {
        if (sample_exec(0, &s) || !s.adjacent || !s.dense) return -1;
        /* The test kernel deliberately matches PW3's 3G/1G split. */
        if (s.address < 0x40000000UL || s.address >= 0x40010000UL + ((1UL<<bits)*PAGE)) return -1;
        if (s.address < low) low = s.address;
        if (s.address > high) high = s.address;
    }
    *span = high-low; return 0;
}
int main(int argc, char **argv) {
    int original, status; struct stat st; struct sample a, b; uint32_t span8=0, span16=0; pid_t pid;
    if (!isolated()) return 77;
    if (argc==2 && !strcmp(argv[1], "--sample")) return sample_child();
    if (argc!=2 || strcmp(argv[1], "--run-isolated") || getuid() || getppid()!=1) return 77;
    original = read_bits();
    check(original==16, "default-is-16");
    if (original < 0) goto done;
    check(!stat(SYSCTL, &st) && (st.st_mode&0777)==0600 && !st.st_uid, "root-only-sysctl");
    check(write_bits("7\n") != 0 && read_bits()==original, "reject-below-arm-minimum");
    check(write_bits("17\n") != 0 && read_bits()==original, "reject-above-arm-maximum");
    check(!write_bits("8\n") && read_bits()==8, "set-8-bits");
    check(!samples(8, &span8) && span8>0 && span8<(1UL<<20), "8-bit-exec-range-and-dense-mappings");
    check(!write_bits("16\n") && read_bits()==16, "set-16-bits");
    check(!samples(16, &span16) && span16>(1UL<<20), "16-bit-exec-range-and-dense-mappings");
    check(!sample_exec(1, &a) && !sample_exec(1, &b) && a.address==b.address && a.adjacent && b.adjacent,
          "addr-no-randomize-remains-deterministic");
    pid = fork();
    if (!pid) {
        if (setgid(10000) || setuid(10000)) _exit(2);
        _exit(write_bits("8\n") != 0 ? 0 : 3);
    }
    check(pid>0 && waitpid(pid, &status, 0)==pid && WIFEXITED(status) && !WEXITSTATUS(status) && read_bits()==16,
          "ordinary-uid-cannot-change-aslr");
    { char value[20]; snprintf(value, sizeof(value), "%d\n", original);
      check(!write_bits(value) && read_bits()==original, "restore-original-setting"); }
done:
    printf("aslr-smoke SUMMARY: checks=%d failed=%d exec_samples=%d dense_mapping_bytes_per_sample=%lu\n",
           checks, failed, completed_samples, (unsigned long)MAP_COUNT<<20);
    return failed ? 1 : 0;
}
