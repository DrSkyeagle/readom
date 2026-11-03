#define _GNU_SOURCE
#include <dirent.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <linux/capability.h>
#include <linux/magic.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>

/* This is a normal exec/file-capability regression, not a device test.
 * No block device is opened. No socket is created. Only a private tmpfs
 * receives writes; all test processes and that mount are discarded on exit. */
#define WORK_DIR "/tmp/filecaps-work"
#define NICE_HELPER WORK_DIR "/nice-helper"
#define SYSLOG_HELPER WORK_DIR "/syslog-helper"
#define TEST_UID 10000
#define TEST_GID 10000
#define COPY_LIMIT (1024u * 1024u)
_Static_assert(sizeof(struct vfs_cap_data) == 20, "kernel revision-2 xattr size");
_Static_assert(sizeof(struct __user_cap_data_struct) == 12, "capget data UAPI");
static int checks, failures;
static const char *case_name = "runner";
static volatile sig_atomic_t worker_pid, timed_out;
struct test_mode { const char *name; int nosuid, nnp; };
static const struct test_mode modes[] = {
    {"ro-filecaps", 0, 0}, {"ro-nosuid", 1, 0}, {"ro-nnp", 0, 1}
};

static void report(int ok, const char *name) {
    printf("filecaps-smoke %s/%s: %s (errno=%d)\n", case_name, name,
           ok ? "PASS" : "FAIL", errno);
    ++checks;
    if (!ok) ++failures;
}
static int read_text(const char *path, char *buf, size_t cap) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    ssize_t n;
    if (fd < 0) return -1;
    n = read(fd, buf, cap - 1);
    close(fd);
    if (n < 0) return -1;
    buf[n] = 0;
    return (int)n;
}
static int ram_type(long type) {
    return (unsigned long)type == RAMFS_MAGIC || (unsigned long)type == TMPFS_MAGIC;
}
static int name_number(const char *name, const char *prefix) {
    size_t n = strlen(prefix);
    if (strncmp(name, prefix, n) || !name[n]) return 0;
    for (; name[n]; ++n) if (name[n] < '0' || name[n] > '9') return 0;
    return 1;
}
/* Every block device attached has to be one that lives in RAM, so that a file
 * capability set here cannot reach anything that survives the power-off. The
 * port's own zram device (config/android-core.config, the 256 MB compressed
 * swap the V32 memory measurements asked for) is one of those: it is backed by
 * the same RAM as ram0 and has no medium behind it. It was not in the kernel
 * when this was written, and this suite had not run since, so the whole test
 * refused with "diskless RAM-root disposable VExpress required". */
static int diskless_surface(void) {
    DIR *d = opendir("/sys/class/block");
    struct dirent *entry;
    int ok = 1;
    if (!d) return 0;
    while ((entry = readdir(d))) {
        if (entry->d_name[0] == '.') continue;
        if (!name_number(entry->d_name, "ram") && !name_number(entry->d_name, "loop") &&
            !name_number(entry->d_name, "zram")) {
            ok = 0;
            break;
        }
    }
    closedir(d);
    return ok;
}
static int caps_match(int allowed, int cap) {
    struct __user_cap_header_struct header = {_LINUX_CAPABILITY_VERSION_3, 0};
    struct __user_cap_data_struct data[_LINUX_CAPABILITY_U32S_3];
    unsigned i;
    memset(data, 0, sizeof(data));
    if (syscall(SYS_capget, &header, data)) return 0;
    for (i = 0; i < _LINUX_CAPABILITY_U32S_3; ++i) {
        uint32_t wanted = allowed && i == (unsigned)CAP_TO_INDEX(cap)
            ? (uint32_t)CAP_TO_MASK(cap) : 0;
        if (data[i].effective != wanted || data[i].permitted != wanted || data[i].inheritable)
            return 0;
    }
    return 1;
}
static int identity_matches(void) {
    uid_t ruid, euid, suid;
    gid_t rgid, egid, sgid;
    return !getresuid(&ruid, &euid, &suid) && !getresgid(&rgid, &egid, &sgid) &&
        ruid == TEST_UID && euid == TEST_UID && suid == TEST_UID &&
        rgid == TEST_GID && egid == TEST_GID && sgid == TEST_GID && getgroups(0, NULL) == 0;
}
static int helper_main(int argc, char **argv) {
    int cap, allowed, expected_nnp, rc, saved_errno, old_nice, after;
    struct rlimit limit;
    if (argc != 6 || (strcmp(argv[2], "nice") && strcmp(argv[2], "syslog")) ||
        (strcmp(argv[3], "0") && strcmp(argv[3], "1")) ||
        (strcmp(argv[4], "0") && strcmp(argv[4], "1"))) return 64;
    cap = !strcmp(argv[2], "nice") ? CAP_SYS_NICE : CAP_SYSLOG;
    allowed = argv[3][0] == '1';
    expected_nnp = argv[4][0] == '1';
    case_name = argv[5];
    alarm(5);
    if (!identity_matches()) {
        report(0, "nonroot-uid-gid-unchanged-after-exec");
        return 2;
    }
    report(1, "nonroot-uid-gid-unchanged-after-exec");
    report(caps_match(allowed, cap), "exact-effective-permitted-and-empty-inheritable");
    report(prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) == expected_nnp, "nnp-after-exec");
    report(prctl(PR_CAPBSET_READ, cap, 0, 0, 0) == 1, "selected-cap-still-in-bounding-set");
    report(!getrlimit(RLIMIT_NICE, &limit) && !limit.rlim_cur && !limit.rlim_max,
           "nice-limit-zero");
    errno = 0;
    old_nice = getpriority(PRIO_PROCESS, 0);
    report(old_nice == 0 && !errno, "initial-own-priority-zero");
    errno = 0;
    rc = setpriority(PRIO_PROCESS, 0, -1);
    saved_errno = errno;
    errno = 0;
    after = getpriority(PRIO_PROCESS, 0);
    if (cap == CAP_SYS_NICE && allowed) {
        report(rc == 0 && after == -1 && !errno, "own-priority-authorization");
    } else {
        report(rc == -1 && saved_errno == EACCES && after == 0 && !errno,
               "own-priority-authorization");
    }
    printf("filecaps-smoke %s helper checks=%d failures=%d\n", case_name, checks, failures);
    return failures ? 1 : 0;
}
static int write_all(int fd, const void *buf, size_t n) {
    const char *p = buf;
    while (n) {
        ssize_t written = write(fd, p, n);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return -1;
        p += written;
        n -= (size_t)written;
    }
    return 0;
}
static int copy_helper(const char *target, int cap) {
    int input = -1, output = -1, rc = -1;
    struct stat before, after;
    struct statfs fs;
    struct vfs_cap_data attrs, observed;
    char buf[8192], verify[8192];
    off_t at;
    input = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
    if (input < 0 || fstat(input, &before) || !S_ISREG(before.st_mode) ||
        before.st_size <= 0 || before.st_size > COPY_LIMIT) goto done;
    output = open(target, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (output < 0 || fstatfs(output, &fs) || (unsigned long)fs.f_type != TMPFS_MAGIC) goto done;
    for (at = 0; at < before.st_size;) {
        size_t want = (size_t)(before.st_size - at);
        ssize_t n;
        if (want > sizeof(buf)) want = sizeof(buf);
        n = pread(input, buf, want, at);
        if (n <= 0 || write_all(output, buf, (size_t)n)) goto done;
        at += n;
    }
    if (fchmod(output, 0755) || fstat(output, &after) || after.st_size != before.st_size ||
        after.st_uid || after.st_gid || (after.st_mode & 07777) != 0755) goto done;
    for (at = 0; at < before.st_size;) {
        size_t want = (size_t)(before.st_size - at);
        if (want > sizeof(buf)) want = sizeof(buf);
        if (pread(input, buf, want, at) != (ssize_t)want ||
            pread(output, verify, want, at) != (ssize_t)want || memcmp(buf, verify, want)) goto done;
        at += (off_t)want;
    }
    memset(&attrs, 0, sizeof(attrs));
    attrs.magic_etc = htole32(VFS_CAP_REVISION_2 | VFS_CAP_FLAGS_EFFECTIVE);
    attrs.data[CAP_TO_INDEX(cap)].permitted = htole32((uint32_t)CAP_TO_MASK(cap));
    if (fsetxattr(output, "security.capability", &attrs, sizeof(attrs), XATTR_CREATE) ||
        fgetxattr(output, "security.capability", &observed, sizeof(observed)) != sizeof(observed) ||
        memcmp(&attrs, &observed, sizeof(attrs))) goto done;
    rc = 0;
done:
    if (output >= 0) close(output);
    if (input >= 0) close(input);
    return rc;
}
static int read_token(int fd, char expected) {
    char token;
    ssize_t n;
    do n = read(fd, &token, 1); while (n < 0 && errno == EINTR);
    return n == 1 && token == expected ? 0 : -1;
}
static int wait_child(pid_t child) {
    int status;
    pid_t got;
    do got = waitpid(child, &status, 0); while (got < 0 && errno == EINTR);
    return got == child && WIFEXITED(status) && !WEXITSTATUS(status);
}
static void child_exec(const char *path, const char *cap_name, const struct test_mode *mode,
                       const char *label) {
    struct rlimit limit = {0, 0};
    const char *allowed = mode->nosuid || mode->nnp ? "0" : "1";
    char *args[] = {(char *)path, "--helper", (char *)cap_name, (char *)allowed,
                    mode->nnp ? "1" : "0", (char *)label, NULL};
    char *env[] = {"PATH=/sbin:/bin", NULL};
    int cap = !strcmp(cap_name, "nice") ? CAP_SYS_NICE : CAP_SYSLOG;
    case_name = label;
    alarm(5);
    if (setrlimit(RLIMIT_NICE, &limit) || setpriority(PRIO_PROCESS, 0, 0) ||
        setgroups(0, NULL) || setresgid(TEST_GID, TEST_GID, TEST_GID) ||
        setresuid(TEST_UID, TEST_UID, TEST_UID)) {
        perror("filecaps-smoke lower credentials");
        _exit(2);
    }
    if (!identity_matches() || !caps_match(0, cap) ||
        prctl(PR_CAPBSET_READ, cap, 0, 0, 0) != 1 ||
        prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 0) {
        report(0, "nonroot-empty-caps-before-exec");
        _exit(2);
    }
    report(1, "nonroot-empty-caps-before-exec");
    if (mode->nnp && prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) {
        perror("filecaps-smoke set NNP");
        _exit(2);
    }
    execve(path, args, env);
    perror("filecaps-smoke exec helper");
    _exit(2);
}
static int worker_main(int ready_fd, int continue_fd) {
    struct statfs fs;
    struct statvfs flags;
    unsigned i, c;
    int mounted = 0, bad = 0, rc = 2;
    const char *paths[] = {NICE_HELPER, SYSLOG_HELPER};
    const char *caps[] = {"nice", "syslog"};
    if (unshare(CLONE_NEWNS) || mount(NULL, "/", NULL, MS_PRIVATE | MS_REC, NULL)) {
        perror("filecaps-smoke private mount namespace");
        goto cleanup;
    }
    if (mount("filecaps-test-ram", "/tmp", "tmpfs", MS_NODEV, "size=8m,mode=0755")) {
        perror("filecaps-smoke private tmpfs");
        goto cleanup;
    }
    mounted = 1;
    if (statfs("/tmp", &fs) || (unsigned long)fs.f_type != TMPFS_MAGIC || mkdir(WORK_DIR, 0755))
        goto cleanup;
    if (write_all(ready_fd, "R", 1) || read_token(continue_fd, 'C')) goto cleanup;
    close(ready_fd);
    ready_fd = -1;
    close(continue_fd);
    continue_fd = -1;
    report(copy_helper(NICE_HELPER, CAP_SYS_NICE) == 0, "copy-nice-helper-and-exact-xattr");
    report(copy_helper(SYSLOG_HELPER, CAP_SYSLOG) == 0, "copy-syslog-helper-and-exact-xattr");
    if (failures) goto cleanup;
    for (i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
        unsigned long mflags = MS_REMOUNT | MS_RDONLY | MS_NODEV;
        if (modes[i].nosuid) mflags |= MS_NOSUID;
        if (mount(NULL, "/tmp", "tmpfs", mflags, NULL) || statvfs(WORK_DIR, &flags) ||
            !(flags.f_flag & ST_RDONLY) || !(flags.f_flag & ST_NODEV) ||
            (flags.f_flag & ST_NOEXEC) || !!(flags.f_flag & ST_NOSUID) != modes[i].nosuid) {
            report(0, "exact-test-mount-flags");
            goto cleanup;
        }
        case_name = modes[i].name;
        report(1, "readonly-nodev-executable-mount-flags");
        for (c = 0; c < 2; ++c) {
            char label[64];
            pid_t child;
            snprintf(label, sizeof(label), "%s-%s", modes[i].name, caps[c]);
            child = fork();
            if (child < 0) goto cleanup;
            if (!child) child_exec(paths[c], caps[c], &modes[i], label);
            case_name = label;
            report(wait_child(child), "helper-exit-zero");
            if (failures) ++bad;
        }
    }
    rc = bad || failures ? 1 : 0;
cleanup:
    case_name = "cleanup";
    if (ready_fd >= 0) close(ready_fd);
    if (continue_fd >= 0) close(continue_fd);
    if (mounted) {
        int ok = umount("/tmp") == 0;
        report(ok, "private-tmpfs-unmounted");
        if (!ok) {
            (void)umount2("/tmp", MNT_DETACH);
            rc = 1;
        }
    }
    printf("filecaps-smoke worker checks=%d failures=%d; namespace exits now\n", checks, failures);
    return rc;
}
static void parent_timeout(int sig) {
    const char text[] = "filecaps-smoke FAIL: bounded runner timeout; discarding private process group\n";
    (void)sig;
    timed_out = 1;
    (void)write(STDERR_FILENO, text, sizeof(text) - 1);
    if (worker_pid > 0) (void)kill(-(pid_t)worker_pid, SIGKILL);
}
static int tmp_same(int fd, const struct stat *before) {
    struct stat after, marker;
    if (fstat(fd, &after) || after.st_dev != before->st_dev || after.st_ino != before->st_ino)
        return 0;
    errno = 0;
    return fstatat(fd, "filecaps-work", &marker, AT_SYMLINK_NOFOLLOW) == -1 && errno == ENOENT;
}
int main(int argc, char **argv) {
    struct utsname un;
    struct stat tmp_before, check, module;
    struct statfs root, tmpfs;
    char cpu[8192];
    int ready[2], control[2], tmp_fd, ok = 0;
    pid_t child;
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGPIPE, SIG_IGN);
    if (argc > 1 && !strcmp(argv[1], "--helper")) return helper_main(argc, argv);
    if (argc != 2 || strcmp(argv[1], "--run-isolated")) return 64;
    if (geteuid() || uname(&un) || !strstr(un.release, "-pw3-core-smoke") ||
        strncmp(un.machine, "arm", 3) || read_text("/proc/cpuinfo", cpu, sizeof(cpu)) < 0 ||
        !strstr(cpu, "ARM-Versatile Express") || statfs("/", &root) || !ram_type(root.f_type) ||
        !diskless_surface() || lstat("/tmp", &tmp_before) || !S_ISDIR(tmp_before.st_mode) ||
        statfs("/tmp", &tmpfs) || !ram_type(tmpfs.f_type)) {
        fputs("filecaps-smoke REFUSED: diskless RAM-root disposable VExpress required\n", stderr);
        return 77;
    }
    if (stat("/proc/modules", &module) == 0 || errno != ENOENT ||
        prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 0) {
        fputs("filecaps-smoke REFUSED: CONFIG_MODULES=n and initially unset NNP required\n", stderr);
        return 77;
    }
    tmp_fd = open("/tmp", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (tmp_fd < 0 || !tmp_same(tmp_fd, &tmp_before) || pipe2(ready, O_CLOEXEC) ||
        pipe2(control, O_CLOEXEC)) return 2;
    child = fork();
    if (child < 0) return 2;
    if (!child) {
        close(tmp_fd);
        close(ready[0]);
        close(control[1]);
        if (setpgid(0, 0)) _exit(2);
        _exit(worker_main(ready[1], control[0]));
    }
    close(ready[1]);
    close(control[0]);
    if (setpgid(child, child) && errno != EACCES) {
        (void)kill(child, SIGKILL);
        (void)wait_child(child);
        return 2;
    }
    worker_pid = child;
    signal(SIGALRM, parent_timeout);
    alarm(45);
    if (!read_token(ready[0], 'R')) {
        ok = tmp_same(tmp_fd, &tmp_before) && lstat("/tmp", &check) == 0 &&
            check.st_dev == tmp_before.st_dev && check.st_ino == tmp_before.st_ino;
        report(ok, "parent-tmp-unchanged-while-child-tmpfs-mounted");
        if (ok && write_all(control[1], "C", 1)) ok = 0;
    }
    close(ready[0]);
    close(control[1]);
    if (!ok) (void)kill(-child, SIGKILL);
    report(wait_child(child) && !timed_out && ok, "worker-exit-zero");
    alarm(0);
    worker_pid = 0;
    report(tmp_same(tmp_fd, &tmp_before) && lstat("/tmp", &check) == 0 &&
        check.st_dev == tmp_before.st_dev && check.st_ino == tmp_before.st_ino,
        "parent-tmp-unchanged-after-worker-exit");
    close(tmp_fd);
    printf("filecaps-smoke SUMMARY: cases=6 parent_checks=%d parent_failures=%d; RAM-only normal exec\n",
           checks, failures);
    return failures ? 1 : 0;
}
