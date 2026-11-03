#define _GNU_SOURCE
#include <sched.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <grp.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

/* For a disposable QEMU/initramfs environment. clone(NEWPID|NEWNS) isolates
 * proc mount policy. All test files are on tmpfs. No block devices, modules,
 * power-state writes, or persistent filesystem writes are used. */
static char child_stack[128 * 1024];
static char shared_stack[64 * 1024];
static int original_fd, failures;

static void check(int ok, const char *name)
{
    printf("namespace-smoke %s: %s (errno=%d)\n", name, ok ? "PASS" : "FAIL", errno);
    fflush(stdout);
    if (!ok) failures++;
}
static int link_name(char *buf, size_t len)
{
    ssize_t n = readlink("/proc/self/ns/mnt", buf, len - 1);
    if (n < 0) return -1;
    buf[n] = 0;
    return 0;
}
static int wait_ok(pid_t pid)
{
    int st;
    return pid > 0 && waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0;
}
static int shared_fs_child(void *unused)
{
    (void)unused;
    errno = 0;
    return setns(original_fd, CLONE_NEWNS) == -1 && errno == EINVAL ? 0 : 1;
}
static int listed(const char *name)
{
    struct dirent *e;
    DIR *d = opendir("/proc");
    int found = 0;
    if (!d) return -1;
    while ((e = readdir(d)) != NULL) if (!strcmp(e->d_name, name)) found = 1;
    closedir(d);
    return found;
}
/* mode 0: hidepid=0/group exemption; 1: visible but inaccessible; 2: hidden. */
static int nonroot_check(int mode, int exempt)
{
    char own[64];
    struct stat st;
    gid_t group = 3009;
    int fd, rc, e;
    if (setgroups(exempt ? 1 : 0, exempt ? &group : NULL) || setgid(10000) || setuid(10000)) return 1;
    snprintf(own, sizeof(own), "/proc/%ld/status", (long)getpid());
    fd = open(own, O_RDONLY);
    if (fd < 0) return 2;
    close(fd);
    errno = 0; rc = stat("/proc/1", &st); e = errno;
    if (mode == 2 ? !(rc == -1 && e == ENOENT) : rc != 0) return 3;
    errno = 0; fd = open("/proc/1/status", O_RDONLY); e = errno;
    if (fd >= 0) close(fd);
    if (mode == 0 ? fd < 0 : !(fd == -1 && e == (mode == 2 ? ENOENT : EPERM))) return 4;
    if (listed("1") != (mode == 2 ? 0 : 1)) return 5;
    return 0;
}
static int run_nonroot(int mode, int exempt)
{
    pid_t p = fork();
    if (!p) _exit(nonroot_check(mode, exempt));
    return wait_ok(p);
}
static int isolated_main(void *unused)
{
    char first[80] = {0}, private_name[80] = {0}, returned[80] = {0};
    char cwd[256], mounts[8192];
    struct stat st;
    unsigned long ino;
    int fd, private_fd, n, rc;
    pid_t p;
    (void)unused;
    if (getpid() != 1 || mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) ||
        mount("tmpfs", "/tmp", "tmpfs", MS_NOSUID | MS_NODEV, "size=4m") ||
        mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, "hidepid=2,gid=3009")) {
        perror("namespace-smoke isolated setup"); return 1;
    }
    check(link_name(first, sizeof(first)) == 0 && !strncmp(first, "mnt:[", 5), "mnt-readlink");
    original_fd = open("/proc/self/ns/mnt", O_RDONLY | O_CLOEXEC);
    check(original_fd >= 0 && sscanf(first, "mnt:[%lu]", &ino) == 1 &&
          fstat(original_fd, &st) == 0 && (unsigned long)st.st_ino == ino, "fd-inode-identity");
    if (original_fd < 0) return 1;
    if (unshare(CLONE_NEWNS) || mount(NULL, "/", NULL, MS_REC | MS_SLAVE, NULL)) {
        perror("namespace-smoke unshare"); return 1;
    }
    check(link_name(private_name, sizeof(private_name)) == 0 && strcmp(first, private_name), "unshare-new-identity");
    private_fd = open("/proc/self/ns/mnt", O_RDONLY | O_CLOEXEC);
    if (private_fd < 0 || mkdir("/tmp/probe", 0700) ||
        mount("tmpfs", "/tmp/probe", "tmpfs", MS_NOSUID | MS_NODEV, "size=1m")) return 1;
    fd = open("/tmp/probe/private-marker", O_CREAT | O_WRONLY, 0600);
    if (fd < 0 || close(fd) || chdir("/tmp/probe")) return 1;
    rc = setns(original_fd, CLONE_NEWNS);
    check(rc == 0 && link_name(returned, sizeof(returned)) == 0 && !strcmp(first, returned), "setns-original");
    check(getcwd(cwd, sizeof(cwd)) != NULL && !strcmp(cwd, "/"), "setns-resets-pwd");
    errno = 0;
    check(stat("/tmp/probe/private-marker", &st) == -1 && errno == ENOENT, "private-mount-hidden");
    /* The private namespace has no tasks now; only its fd keeps it alive. */
    rc = setns(private_fd, CLONE_NEWNS);
    check(rc == 0 && link_name(returned, sizeof(returned)) == 0 && !strcmp(private_name, returned), "fd-keeps-empty-namespace");
    check(stat("/tmp/probe/private-marker", &st) == 0, "restored-private-mount");
    errno = 0;
    check(setns(original_fd, CLONE_NEWNET) == -1 && errno == EINVAL, "wrong-namespace-type-rejected");
    fd = open("/tmp/nsbind", O_CREAT | O_RDWR, 0600);
    if (fd < 0) return 1;
    errno = 0;
    check(setns(fd, CLONE_NEWNS) == -1 && errno == EINVAL, "ordinary-file-rejected");
    close(fd);
    errno = 0;
    check(mount("/proc/self/ns/mnt", "/tmp/nsbind", NULL, MS_BIND, NULL) == -1 && errno == EINVAL, "namespace-bind-cycle-rejected");
    p = clone(shared_fs_child, shared_stack + sizeof(shared_stack), CLONE_FS | SIGCHLD, NULL);
    check(wait_ok(p), "shared-fs-setns-rejected");
    if (setns(original_fd, CLONE_NEWNS)) return 1;
    close(private_fd); close(original_fd);
    check(run_nonroot(2, 0), "hidepid2-own-visible-pid1-hidden");
    check(run_nonroot(0, 1), "gid3009-exemption");
    check(mount(NULL, "/proc", "proc", MS_REMOUNT, "hidepid=1,gid=3009") == 0 && run_nonroot(1, 0), "hidepid1-visible-but-denied");
    check(mount(NULL, "/proc", "proc", MS_REMOUNT, "hidepid=0,gid=3009") == 0 && run_nonroot(0, 0), "hidepid0-standard-access");
    if (mount(NULL, "/proc", "proc", MS_REMOUNT, "hidepid=2,gid=3009")) return 1;
    errno = 0;
    check(mount(NULL, "/proc", "proc", MS_REMOUNT, "hidepid=0,unknown=1") == -1 && errno == EINVAL && run_nonroot(2, 0), "invalid-options-do-not-relax-policy");
    fd = open("/proc/mounts", O_RDONLY);
    n = fd < 0 ? -1 : read(fd, mounts, sizeof(mounts) - 1);
    if (fd >= 0) close(fd);
    if (n >= 0) mounts[n] = 0;
    check(n > 0 && strstr(mounts, "hidepid=2") && strstr(mounts, "gid=3009"), "reported-proc-options");
    printf("namespace-smoke failures=%d\n", failures); fflush(stdout);
    return failures != 0;
}
int main(void)
{
    pid_t p;
    if (geteuid() != 0) { fputs("namespace-smoke requires root with namespace/mount capabilities\n", stderr); return 2; }
    p = clone(isolated_main, child_stack + sizeof(child_stack), CLONE_NEWNS | CLONE_NEWPID | SIGCHLD, NULL);
    if (p < 0) { perror("namespace-smoke clone isolation"); return 1; }
    return wait_ok(p) ? 0 : 1;
}
