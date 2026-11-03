/* SPDX-License-Identifier: MIT */
/* PID 1 for a diskless isolated kernel test VM. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int run(const char *program) {
    int status;
    pid_t child = fork();
    if (child < 0) return 1;
    if (!child) {
        if (!strcmp(program,"/seccomp-smoke") && (setgid(10000) || setuid(10000))) _exit(126);
        if (!strcmp(program, "/network-smoke") || !strcmp(program, "/binder-sg-smoke") || !strcmp(program, "/binder-hardening-smoke") ||
            !strcmp(program, "/network-gid-smoke") || !strcmp(program, "/filecaps-smoke") || !strcmp(program, "/inet-permission-smoke") || !strcmp(program, "/aslr-smoke") || !strcmp(program, "/siginfo-smoke") || !strcmp(program, "/process-vm-smoke"))
            execl(program, program, "--run-isolated", (char *)0);
        else execl(program, program, (char *)0);
        perror(program); _exit(127);
    }
    if (waitpid(child, &status, 0) != child) return 1;
    printf("SUITE %s wait_status=%d\n", program, status);
    return !WIFEXITED(status) || WEXITSTATUS(status) != 0;
}
__attribute__((noreturn)) static void finish(void) {
    sync();
    // QEMU handles its poweroff MMIO request asynchronously. Linux 3.0 calls
    // do_exit() if machine_power_off() returns, so issue reboot from a child
    // and keep PID 1 alive until the emulator exits. A timeout remains failure.
    pid_t child = fork();
    if (!child) {
        reboot(RB_POWER_OFF);
        perror("test poweroff"); puts("PW3_KERNEL_SMOKE_FAIL"); _exit(2);
    }
    if (child < 0) { perror("poweroff fork"); puts("PW3_KERNEL_SMOKE_FAIL"); }
    for (;;) pause();
}
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    if (getpid() != 1) { fputs("Refusing to run outside a test initramfs.\n", stderr); return 2; }
    mkdir("/proc", 0755); mkdir("/sys", 0755); mkdir("/dev", 0755); mkdir("/tmp", 0700);
    if (mount("proc", "/proc", "proc", 0, NULL) ||
        mount("sysfs", "/sys", "sysfs", 0, NULL) ||
        mount("devtmpfs", "/dev", "devtmpfs", 0, NULL)) {
        perror("test mounts"); puts("PW3_KERNEL_SMOKE_FAIL");
        finish();
    }
    // Diskless test VM only: Binder's quota regression reads its own stats.
    mkdir("/sys/kernel/debug", 0700);
    if (mount("debugfs", "/sys/kernel/debug", "debugfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL)) {
        perror("test debugfs mount"); puts("PW3_KERNEL_SMOKE_FAIL");
        finish();
    }
    puts("KERNEL_TEST_PLATFORM=vexpress-a9; NOT_PW3_HARDWARE");
    const char *programs[] = {"/platform-smoke", "/seccomp-smoke", "/alarmfd-smoke",
        "/namespace-hidepid-smoke", "/binder-sg-smoke", "/binder-hardening-smoke", "/network-wire-selftest", "/network-smoke", "/network-gid-smoke", "/inet-permission-smoke", "/filecaps-smoke", "/aslr-smoke", "/siginfo-smoke", "/process-vm-smoke", "/thread-procfd-smoke"};
    int failures = 0;
    for (size_t i = 0; i < sizeof(programs)/sizeof(programs[0]); ++i) failures += run(programs[i]);
    puts(failures ? "PW3_KERNEL_SMOKE_FAIL" : "PW3_KERNEL_SMOKE_PASS");
    finish();
}
