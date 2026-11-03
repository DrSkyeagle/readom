#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

static int checks, failures;
static sigset_t blocked;

static void check(int ok, const char *name)
{
    ++checks;
    if (!ok) ++failures;
    printf("siginfo-smoke %s: %s\n", name, ok ? "PASS" : "FAIL");
}

static siginfo_t information(int code)
{
    siginfo_t value;
    memset(&value, 0, sizeof(value));
    value.si_signo = SIGUSR1;
    value.si_code = code;
    value.si_pid = getpid();
    value.si_uid = getuid();
    value.si_value.sival_int = 0x3456;
    return value;
}

static void self_signal(int threaded, int code)
{
    siginfo_t info = information(code), got;
    struct timespec timeout = {1, 0};
    long result = threaded ? syscall(SYS_rt_tgsigqueueinfo, getpid(),
                                     syscall(SYS_gettid), SIGUSR1, &info)
                           : syscall(SYS_rt_sigqueueinfo, getpid(), SIGUSR1, &info);
    check(result == 0, threaded ? "thread-self-send" : "process-self-send");
    if (result != 0) return;
    memset(&got, 0, sizeof(got));
    check(sigtimedwait(&blocked, &got, &timeout) == SIGUSR1 && got.si_code == code,
          "self-delivery-preserves-code");
}

struct peer_state {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    pid_t tid;
    int done;
};

static void *peer_thread(void *pointer)
{
    struct peer_state *state = pointer;
    /* A non-leader thread must also be able to rethrow its own signal. */
    self_signal(1, SI_TKILL);
    pthread_mutex_lock(&state->lock);
    state->tid = (pid_t)syscall(SYS_gettid);
    pthread_cond_signal(&state->changed);
    while (!state->done) pthread_cond_wait(&state->changed, &state->lock);
    pthread_mutex_unlock(&state->lock);
    return NULL;
}

int main(int argc, char **argv)
{
    struct utsname uts;
    struct statfs fs;
    if (argc != 2 || strcmp(argv[1], "--run-isolated") || getuid() != 0 ||
        getppid() != 1 || uname(&uts) || strcmp(uts.machine, "armv7l") ||
        !strstr(uts.release, "-pw3-core-smoke") || statfs("/", &fs) ||
        ((unsigned long)fs.f_type != 0x858458f6UL &&
         (unsigned long)fs.f_type != 0x01021994UL)) {
        fprintf(stderr, "siginfo-smoke: isolated ARM RAM-root guard refused\n");
        return 77;
    }
    alarm(12);
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGUSR1);
    if (sigprocmask(SIG_BLOCK, &blocked, NULL)) return 2;
    const int codes[] = {SI_USER, SI_KERNEL, SI_TKILL};
    for (unsigned i = 0; i < sizeof(codes) / sizeof(codes[0]); ++i) {
        self_signal(0, codes[i]);
        self_signal(1, codes[i]);
    }

    struct peer_state state = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0};
    pthread_t peer;
    int created = pthread_create(&peer, NULL, peer_thread, &state) == 0;
    if (!created) check(0, "create-owned-peer-thread");
    if (created) {
        pthread_mutex_lock(&state.lock);
        while (!state.tid) pthread_cond_wait(&state.changed, &state.lock);
        check(1, "create-owned-peer-thread");
        siginfo_t info = information(SI_KERNEL);
        errno = 0;
        check(syscall(SYS_rt_tgsigqueueinfo, getpid(), state.tid, SIGUSR1, &info) == -1 &&
              errno == EPERM, "other-thread-positive-code-still-denied");
        state.done = 1;
        pthread_cond_signal(&state.changed);
        pthread_mutex_unlock(&state.lock);
        check(pthread_join(peer, NULL) == 0, "join-owned-peer-thread");
    }

    int ready[2], go[2];
    if (pipe(ready) || pipe(go)) return 2;
    pid_t child = fork();
    if (child == 0) {
        close(ready[0]); close(go[1]);
        char byte = 'R';
        if (write(ready[1], &byte, 1) != 1 || read(go[0], &byte, 1) != 1) _exit(2);
        siginfo_t got;
        struct timespec timeout = {1, 0};
        memset(&got, 0, sizeof(got));
        int ok = sigtimedwait(&blocked, &got, &timeout) == SIGUSR1 &&
                 got.si_code == SI_QUEUE && got.si_value.sival_int == 0x3456;
        _exit(ok ? 0 : 1);
    }
    close(ready[1]); close(go[0]);
    check(child > 0, "create-owned-peer-process");
    if (child > 0) {
        char byte;
        check(read(ready[0], &byte, 1) == 1, "peer-signal-mask-ready");
        siginfo_t info = information(SI_USER);
        errno = 0;
        check(syscall(SYS_rt_sigqueueinfo, child, SIGUSR1, &info) == -1 && errno == EPERM,
              "other-process-positive-code-still-denied");
        info = information(SI_KERNEL);
        errno = 0;
        check(syscall(SYS_rt_tgsigqueueinfo, child, child, SIGUSR1, &info) == -1 && errno == EPERM,
              "other-process-thread-send-still-denied");
        info = information(SI_QUEUE);
        check(syscall(SYS_rt_tgsigqueueinfo, child, child, SIGUSR1, &info) == 0,
              "ordinary-cross-process-queue-retained");
        check(write(go[1], "G", 1) == 1, "release-owned-peer-process");
        int status;
        check(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "ordinary-cross-process-queue-delivered");
    }
    close(ready[0]); close(go[1]);

    siginfo_t info = information(SI_USER);
    errno = 0;
    check(syscall(SYS_rt_tgsigqueueinfo, 0, syscall(SYS_gettid), SIGUSR1, &info) == -1 &&
          errno == EINVAL, "zero-thread-group-rejected");
    errno = 0;
    check(syscall(SYS_rt_tgsigqueueinfo, getpid() + 100000, syscall(SYS_gettid), SIGUSR1, &info) == -1 &&
          errno == ESRCH, "self-id-does-not-bypass-thread-group-check");
    alarm(0);
    printf("siginfo-smoke SUMMARY: checks=%d failed=%d\n", checks, failures);
    return failures ? 1 : 0;
}
