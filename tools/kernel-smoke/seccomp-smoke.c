/* Interface smoke test for the actual kernel under test; not a security audit.
 * Run as an ordinary user. Each sandbox case is isolated in a child process.
 * Build on a Linux libc target: cc -O2 -Wall -Wextra -pthread -o seccomp-smoke seccomp-smoke.c
 * qemu-user does NOT execute the patched PW3 kernel.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <linux/filter.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PR_SET_NO_NEW_PRIVS
#define PR_SET_NO_NEW_PRIVS 38
#define PR_GET_NO_NEW_PRIVS 39
#endif
#ifndef SECCOMP_MODE_FILTER
#define SECCOMP_MODE_FILTER 2
#endif
#ifndef __NR_seccomp
#if defined(__arm__)
#define __NR_seccomp 383
#else
#error "This target libc does not define its seccomp syscall number"
#endif
#endif
#define SC_KILL 0x00000000U
#define SC_TRAP 0x00030000U
#define SC_ERRNO 0x00050000U
#define SC_ALLOW 0x7fff0000U
#define SC_FILTER 1
#define SC_TSYNC 1
struct sc_data { int nr; uint32_t arch; uint64_t ip; uint64_t args[6]; };

static int no_new_privs(void)
{
    return prctl(PR_SET_NO_NEW_PRIVS, 1UL, 0UL, 0UL, 0UL);
}

static int install_nr(int nr, unsigned int action, int tsync)
{
    struct sock_filter insns[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct sc_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (unsigned int)nr, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, action),
        BPF_STMT(BPF_RET | BPF_K, SC_ALLOW),
    };
    struct sock_fprog p = { sizeof(insns)/sizeof(insns[0]), insns };
    if (no_new_privs()) return -1;
    return tsync ? (int)syscall(__NR_seccomp, SC_FILTER, SC_TSYNC, &p)
                 : prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &p);
}

static int denied_getpid(void)
{
    errno = 0;
    return syscall(__NR_getpid) == -1 && errno == EPERM;
}

static int errno_and_nnp(void)
{
    if (install_nr(__NR_getpid, SC_ERRNO | EPERM, 0)) return 1;
    if (!denied_getpid()) return 2;
    if (prctl(PR_GET_NO_NEW_PRIVS, 0UL, 0UL, 0UL, 0UL) != 1) return 3;
    errno = 0;
    if (prctl(PR_SET_NO_NEW_PRIVS, 0UL, 0UL, 0UL, 0UL) != -1 || errno != EINVAL) return 4;
    return 0;
}

static int inherited(void)
{
    int status;
    pid_t p;
    if (install_nr(__NR_getpid, SC_ERRNO | EPERM, 0)) return 1;
    p = fork();
    if (p == -1) return 2;
    if (!p) _exit(denied_getpid() ? 0 : 3);
    if (waitpid(p, &status, 0) != p) return 4;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 5;
}

static pthread_barrier_t ready, applied;
static int worker_denied;
static void *worker(void *unused)
{
    (void)unused;
    pthread_barrier_wait(&ready);
    pthread_barrier_wait(&applied);
    worker_denied = denied_getpid();
    return NULL;
}

static int synchronized_threads(void)
{
    pthread_t t;
    int installed;
    if (pthread_barrier_init(&ready, NULL, 2) || pthread_barrier_init(&applied, NULL, 2)) return 1;
    if (pthread_create(&t, NULL, worker, NULL)) return 2;
    pthread_barrier_wait(&ready);
    installed = install_nr(__NR_getpid, SC_ERRNO | EPERM, 1);
    pthread_barrier_wait(&applied);
    if (pthread_join(t, NULL)) return 3;
    if (installed) return 4;
    return worker_denied && denied_getpid() ? 0 : 5;
}

static volatile sig_atomic_t trapped, trapped_nr, trapped_code;
static void trap_handler(int sig, siginfo_t *info, void *ctx)
{
    (void)ctx;
    trapped = sig == SIGSYS;
#ifdef si_syscall
    trapped_nr = info->si_syscall;
#else
    trapped_nr = -1;
#endif
    trapped_code = info->si_code;
}

/* Parent observes an anonymous pipe: a denied write must leave it empty.
 * This catches the old ARM entry bug even if the syscall return is misleading.
 */
static int denied_write(unsigned int action)
{
    int fds[2], status;
    pid_t p;
    char byte = 'X', readback = 0;
    ssize_t count;
    if (pipe(fds)) return 1;
    p = fork();
    if (p == -1) return 2;
    if (!p) {
        long result;
        close(fds[0]);
        if (action == SC_TRAP) {
            struct sigaction sa = {0};
            sa.sa_sigaction = trap_handler;
            sa.sa_flags = SA_SIGINFO;
            sigemptyset(&sa.sa_mask);
            if (sigaction(SIGSYS, &sa, NULL)) _exit(3);
        }
        if (install_nr(__NR_write, action, 0)) _exit(4);
        errno = 0;
        result = syscall(__NR_write, fds[1], &byte, 1);
        if (action == (SC_ERRNO | EACCES))
            _exit(result == -1 && errno == EACCES ? 0 : 5);
        if (action == SC_TRAP)
            _exit(trapped && trapped_nr == __NR_write && trapped_code == 1 ? 0 : 6);
        _exit(7); /* KILL must never return. */
    }
    close(fds[1]);
    if (waitpid(p, &status, 0) != p) return 8;
    count = read(fds[0], &readback, 1);
    close(fds[0]);
    if (count != 0) return 9;
    if (action == SC_KILL)
        return WIFSIGNALED(status) && WTERMSIG(status) == SIGSYS ? 0 : 10;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 11;
}
static int errno_write(void) { return denied_write(SC_ERRNO | EACCES); }
static int trap_write(void) { return denied_write(SC_TRAP); }
static int kill_write(void) { return denied_write(SC_KILL); }

static int run(const char *name, int (*test)(void))
{
    int status;
    pid_t p = fork();
    if (p == -1) { perror("fork"); return 1; }
    if (!p) _exit(test());
    if (waitpid(p, &status, 0) != p) { perror("waitpid"); return 1; }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        printf("PASS %s\n", name); return 0;
    }
    printf("FAIL %s wait_status=%d\n", name, status);
    return 1;
}

int main(void)
{
    struct utsname u;
    int failures = 0;
    if (!uname(&u)) printf("KERNEL_UNDER_TEST %s %s %s\n", u.sysname, u.release, u.machine);
    failures += run("filter syscall number and irreversible NNP", errno_and_nnp);
    failures += run("ERRNO write produces no side effect", errno_write);
    failures += run("TRAP reports syscall and produces no side effect", trap_write);
    failures += run("KILL produces no side effect", kill_write);
    failures += run("fork inherits filter", inherited);
    failures += run("syscall TSYNC reaches existing worker", synchronized_threads);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
