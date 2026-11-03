/* SPDX-License-Identifier: MIT
 *
 * A thread reading its own process's /proc/self/fd -- and nobody else's.
 *
 * This is the question that decided whether an application could be installed
 * on this device at all. Android 8.1 declares the size of the APK it is about
 * to write, so PackageInstallerSession.openWriteInternal() opens the staging
 * file O_WRONLY and hands the descriptor to StorageManager.allocateBytes(),
 * which asks ParcelFileDescriptor.getFile() for the path behind it --
 * readlink("/proc/self/fd/N"), from whichever binder thread took the call.
 *
 * On the pinned 3.0.35 that failed with EACCES, and every install failed with
 * it (PW3 Android V80, 2026-10-23). system_server changes uid at startup, so
 * it is not dumpable and the kernel gives its /proc entries to root; the
 * thread then meets two checks that compare it against the thread *group
 * leader* rather than against its own group, and loses both. Mainline fixed
 * them in 3.9 (96d0df79f264, 73af963f9f30) and this port backports them as
 * patches/android-core/1055-ptrace-proc-sub-threads.patch.
 *
 * So this program is the fix stated as behaviour rather than as a diff:
 *
 *   - a non-dumpable, unprivileged process can readlink its own write-only
 *     descriptor, from the leader and from any other thread;
 *   - another process cannot, neither one of a different uid nor one of the
 *     same uid, because the target is not dumpable. The second half is what
 *     keeps this a fix rather than a hole: same_thread_group() widens the
 *     answer to the threads that already share the ->mm and to nothing else.
 *
 * Verified in reverse (2026-10-23): run against a smoke kernel built before
 * the patch, the second check fails and the other three still pass, so this
 * program can tell the two kernels apart and is not merely agreeing with
 * whatever it is given.
 *
 * What it asserts is allowed or refused, never which errno: the refusal comes
 * back as ENOENT here, where /proc is mounted plainly, and as EACCES on the
 * device, where it is mounted hidepid=2 -- the same denial, reported by two
 * different layers.
 *
 * For a disposable QEMU/initramfs environment: the file it writes is on the
 * initramfs, it drops to an unused uid, and it touches no device.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define TEST_UID 10000
#define OTHER_UID 10001

static int failures;

static void check(int ok, const char *name)
{
    printf("thread-procfd-smoke %s: %s (errno=%d)\n", name, ok ? "PASS" : "FAIL", errno);
    fflush(stdout);
    if (!ok) failures++;
}

/* readlink of one descriptor of one process. 0 on success, -errno on refusal. */
static int link_of(pid_t pid, int fd)
{
    char path[64], target[256];
    snprintf(path, sizeof(path), "/proc/%d/fd/%d", (int)pid, fd);
    errno = 0;
    ssize_t n = readlink(path, target, sizeof(target) - 1);
    if (n < 0) return -errno;
    target[n] = 0;
    return 0;
}

static int self_link(int fd)
{
    char path[64], target[256];
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    errno = 0;
    ssize_t n = readlink(path, target, sizeof(target) - 1);
    if (n < 0) return -errno;
    target[n] = 0;
    return 0;
}

struct thread_arg {
    int fd;
    int result;
};

static void *in_thread(void *raw)
{
    struct thread_arg *arg = raw;
    arg->result = self_link(arg->fd);
    return NULL;
}

/* A child that becomes `uid` and then waits to be told to read the link.
 *
 * It has to be started before this process drops its own privileges, because
 * after that it could no longer become a different uid -- and it has to read
 * afterwards, because the state worth testing is an unprivileged, non-dumpable
 * process, which is what system_server is. Hence the pipe. Exits 0 when
 * refused, 1 when allowed, and 2 or 3 when it never got to try. */
static pid_t spawn_reader(uid_t uid, pid_t target, int fd, int *start)
{
    int ends[2];
    if (pipe(ends)) return -1;
    pid_t child = fork();
    if (child < 0) { close(ends[0]); close(ends[1]); return -1; }
    if (!child) {
        char byte;
        close(ends[1]);
        if (setgid(uid) || setuid(uid)) _exit(2);
        if (read(ends[0], &byte, 1) != 1) _exit(3);
        _exit(link_of(target, fd) == 0 ? 1 : 0);
    }
    close(ends[0]);
    *start = ends[1];
    return child;
}

static int refused(pid_t child, int start)
{
    char byte = 1;
    if (child < 0 || write(start, &byte, 1) != 1) return 0;
    close(start);
    int status = 0;
    if (waitpid(child, &status, 0) != child) return 0;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (geteuid() != 0) {
        fputs("thread-procfd-smoke: must start as root in the test initramfs\n", stderr);
        return 2;
    }

    /* Readable and writable by the unprivileged uid this becomes, so that the
     * descriptor below is the kind the installer holds: open for writing only. */
    int seed = open("/tmp/thread-procfd", O_CREAT | O_RDWR, 0666);
    if (seed < 0 || close(seed)) { perror("create"); return 2; }
    if (chmod("/tmp/thread-procfd", 0666)) { perror("chmod"); return 2; }

    int fd = open("/tmp/thread-procfd", O_WRONLY);
    if (fd < 0) { perror("open O_WRONLY"); return 2; }

    /* What a uid change does to system_server, done here on purpose so the
     * test does not depend on fs.suid_dumpable. */
    if (prctl(PR_SET_DUMPABLE, 0, 0, 0, 0)) { perror("PR_SET_DUMPABLE"); return 2; }

    int start_same = -1, start_other = -1;
    pid_t same = spawn_reader(TEST_UID, getpid(), fd, &start_same);
    pid_t other = spawn_reader(OTHER_UID, getpid(), fd, &start_other);

    if (setgid(TEST_UID) || setuid(TEST_UID)) { perror("setuid"); return 2; }
    if (prctl(PR_SET_DUMPABLE, 0, 0, 0, 0)) { perror("PR_SET_DUMPABLE after setuid"); return 2; }
    if (geteuid() != TEST_UID) { fputs("thread-procfd-smoke: still privileged\n", stderr); return 2; }

    check(self_link(fd) == 0, "the group leader reads its own write-only descriptor");

    struct thread_arg arg = { fd, -1 };
    pthread_t thread;
    if (pthread_create(&thread, NULL, in_thread, &arg) || pthread_join(thread, NULL)) {
        perror("pthread");
        return 2;
    }
    errno = -arg.result;
    check(arg.result == 0, "another thread of the same process reads it too");

    /* The boundary: a separate process, even of this very uid, still may not,
     * because this process is not dumpable. */
    check(refused(same, start_same), "a separate process of the same uid is refused");
    check(refused(other, start_other), "a separate process of another uid is refused");

    if (close(fd)) { perror("close"); return 2; }
    printf("thread-procfd-smoke: %d failures\n", failures);
    return failures ? 1 : 0;
}
