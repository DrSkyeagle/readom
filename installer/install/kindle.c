/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "kindle.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define LIPC_GET "/usr/bin/lipc-get-prop"
#define LIPC_SET "/usr/bin/lipc-set-prop"
#define CURL "/usr/bin/curl"
/* A stock tool that has not answered by then is taken as failed. */
#define TOOL_SECONDS 15
#define PROGRESS_MS 3000

static long long monotonic_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void wait_ms(long ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000 }, left;
	while (nanosleep(&ts, &left) && errno == EINTR)
		ts = left;
}

/* Starts argv[0] with standard output into a pipe (*out_fd, when out_fd is given)
 * and standard error into ours (the entry's log). */
/* fsize, when not zero, is a hard ceiling the kernel puts on anything this
 * child writes. It is the backstop for the one case the parent's polling
 * cannot cover; see the call in download().
 *
 * The result is not checked, and the child goes on to exec either way: this is
 * a second line and not the mechanism. setrlimit can refuse -- raising an
 * inherited hard limit without CAP_SYS_RESOURCE is the documented way, and
 * this kernel's sys.c has that exit -- and refusing to run curl because a
 * backstop could not be installed would turn an install that would have
 * worked into one that does not, for a case the polling in download() answers
 * on its own (a Codex review, 2026-10-30). What is lost when it refuses is the
 * margin for a draw that holds that polling for seconds, not the check. */
static pid_t start(char *const argv[], int *out_fd, unsigned seconds, uint64_t fsize)
{
	int p[2] = { -1, -1 };
	if (out_fd && pipe2(p, O_CLOEXEC))
		return -1;
	pid_t pid = fork();
	if (pid == 0) {
		/* The download's size limit is enforced by this process watching the
		 * part grow, so a tool that outlives it would go on writing into the
		 * user store with nothing left to stop it. The kernel ends the child
		 * when its parent does; this is the only thing that holds if the
		 * installer is killed rather than ended. */
		prctl(PR_SET_PDEATHSIG, SIGKILL);
		if (getppid() == 1)	/* the parent went between the fork and the line above */
			_exit(128 + SIGKILL);
		if (out_fd)
			dup2(p[1], STDOUT_FILENO);
		else {
			int null = open("/dev/null", O_WRONLY);
			if (null >= 0)
				dup2(null, STDOUT_FILENO);
		}
		if (fsize) {
			struct rlimit fl = { (rlim_t)fsize, (rlim_t)fsize };
			setrlimit(RLIMIT_FSIZE, &fl);	/* kept across exec */
		}
		if (seconds)
			alarm(seconds);	/* kept across exec: the tool ends if it hangs */
		execv(argv[0], argv);
		_exit(127);
	}
	if (out_fd) {
		close(p[1]);
		if (pid < 0)
			close(p[0]);
		else
			*out_fd = p[0];
	}
	return pid;
}

static int finish(pid_t pid)
{
	int status;
	pid_t got;
	do
		got = waitpid(pid, &status, 0);
	while (got < 0 && errno == EINTR);
	if (got != pid)
		return -1;
	return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

/* Runs a tool and keeps the first line of its output. 0 when it exited with 0. */
static int run(char *const argv[], char *line, size_t size)
{
	int fd = -1;
	pid_t pid = start(argv, line ? &fd : NULL, TOOL_SECONDS, 0);
	if (pid < 0)
		return -1;
	size_t n = 0;
	if (line) {
		ssize_t k;
		while (n + 1 < size && (k = read(fd, line + n, size - 1 - n)) != 0) {
			if (k < 0 && errno == EINTR)
				continue;
			if (k < 0)
				break;
			n += (size_t)k;
		}
		char drain[256];
		while (read(fd, drain, sizeof(drain)) > 0)
			;
		close(fd);
		line[n] = 0;
		line[strcspn(line, "\r\n")] = 0;
	}
	return finish(pid) == 0 ? 0 : -1;
}

static int lipc_get(const char *service, const char *property, char *value, size_t size)
{
	char *argv[] = { LIPC_GET, (char *)service, (char *)property, NULL };
	return run(argv, value, size);
}

static int lipc_number(const char *service, const char *property, int *value)
{
	char text[32], *end;
	if (lipc_get(service, property, text, sizeof(text)))
		return -1;
	long v = strtol(text, &end, 10);
	if (end == text || *end)
		return -1;
	*value = (int)v;
	return 0;
}

static int battery(void *ctx, int *percent, int *charging)
{
	(void)ctx;
	if (lipc_number("com.lab126.powerd", "battLevel", percent))
		return -1;
	if (lipc_number("com.lab126.powerd", "isCharging", charging))
		*charging = 0;
	return 0;
}

static int online(void *ctx)
{
	(void)ctx;
	char state[64];
	return !lipc_get("com.lab126.wifid", "cmState", state, sizeof(state)) && !strcmp(state, "CONNECTED");
}

static int64_t now(void *ctx)
{
	(void)ctx;
	return (int64_t)time(NULL);
}

static void wait_seconds(void *ctx, unsigned seconds)
{
	(void)ctx;
	wait_ms((long)seconds * 1000);
}

static int download(void *ctx, const char *url, const char *part, const char *ca_file, uint64_t limit,
		    void (*progress)(void *, uint64_t), void *pctx, int *http)
{
	(void)ctx;
	char cacert[PATH_MAX + 16], maxsize[32];
	snprintf(cacert, sizeof(cacert), "%s", ca_file);
	snprintf(maxsize, sizeof(maxsize), "%llu", (unsigned long long)limit);
	/* Only HTTPS, also after redirects; a transfer slower than 1 KB/s for a minute
	 * ends and is resumed by the installer's next attempt. --max-filesize turns an
	 * answer longer than the manifest away before it is written; the part is also
	 * measured while it arrives, below, because the size curl is told about is the
	 * one the server states. */
	char *argv[] = { CURL, "--fail", "--location", "--max-redirs", "5", "--proto", "=https", "--proto-redir", "=https",
			 "--silent", "--show-error", "--connect-timeout", "30", "--speed-limit", "1024", "--speed-time", "60",
			 "--max-filesize", maxsize,
			 "--cacert", cacert, "--continue-at", "-", "--output", (char *)part, "--write-out", "%{http_code}",
			 (char *)url, NULL };
	int fd = -1;
	*http = 0;
	/* The ceiling is deliberately above the manifest's size rather than equal to
	 * it, and the difference is the whole point (a Codex review, 2026-10-30).
	 *
	 * What bounds the part normally is the loop below: it stats the file and,
	 * if it is past limit, kills curl and returns 63, which the installer
	 * reports as "the server's file is longer than the manifest" -- the true
	 * reason. That check runs before the progress report because drawing on the
	 * panel can hold this loop for seconds (eips is allowed 15), and the file
	 * grows the whole time. So the bound is limit plus whatever arrives in one
	 * draw, and --max-filesize does not close it either: curl 7.86 checks the
	 * length the answer states, and an answer that states none (chunked) has no
	 * length to check.
	 *
	 * RLIMIT_FSIZE closes it in the kernel, where no draw can delay it. Setting
	 * it to limit exactly would be worse than it looks: the stat above could
	 * then never see a file past limit, so the accurate 63 would be replaced by
	 * curl dying on a write error, which the installer reports as a fault in
	 * the Kindle's storage. With headroom, the kernel caps the damage and the
	 * real reason is still what comes back -- not by winning a race. The loop
	 * below does call waitpid() first, but it stats the part and returns 63 on
	 * a file past the limit before it acts on what waitpid() said, so a curl
	 * the kernel has already ended with SIGXFSZ is still reported as a file
	 * longer than the manifest, which is what it is. */
	pid_t pid = start(argv, &fd, 0, limit + (4u << 20));
	if (pid < 0)
		return -1;
	fcntl(fd, F_SETFL, O_NONBLOCK);
	char code[16] = "";
	size_t n = 0;
	for (;;) {
		int status;
		pid_t got = waitpid(pid, &status, WNOHANG);
		struct stat st;
		if (!stat(part, &st)) {
			if ((uint64_t)st.st_size > limit) {
				/* The answer is longer than the manifest says, whatever it
				 * stated: it stops here rather than filling the user store.
				 * Before the progress report, which draws on the panel and
				 * can hold this loop for seconds while the file grows. */
				kill(pid, SIGKILL);
				while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
					;
				close(fd);
				*http = 0;
				return 63;
			}
			progress(pctx, (uint64_t)st.st_size);
		}
		ssize_t k;
		while (n + 1 < sizeof(code) && (k = read(fd, code + n, sizeof(code) - 1 - n)) > 0)
			n += (size_t)k;
		if (got == pid) {
			while (n + 1 < sizeof(code) && (k = read(fd, code + n, sizeof(code) - 1 - n)) > 0)
				n += (size_t)k;
			code[n] = 0;
			close(fd);
			*http = atoi(code);
			return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
		}
		if (got < 0 && errno != EINTR) {
			close(fd);
			return -1;
		}
		wait_ms(1000);
	}
}

static void show(void *ctx, enum inst_step step, uint64_t done, uint64_t total)
{
	struct kindle *k = ctx;
	static const enum pw3_screen screens[] = {
		[INST_STEP_CHECK] = PW3_SCREEN_CHECK, [INST_STEP_DOWNLOAD] = PW3_SCREEN_DOWNLOAD,
		[INST_STEP_VERIFY] = PW3_SCREEN_VERIFY, [INST_STEP_SYSTEM] = PW3_SCREEN_SYSTEM,
		[INST_STEP_DATA] = PW3_SCREEN_DATA, [INST_STEP_REPAIR] = PW3_SCREEN_REPAIR,
	};
	/* The array is as long as its largest index, so a step added to install.h
	 * and not added here would be read from past its end. */
	if (step < 0 || (size_t)step >= sizeof(screens) / sizeof(screens[0])) {
		printf("PW3_INSTALL no screen for step %d\n", (int)step);
		return;
	}
	enum pw3_screen id = screens[step];
	int permille = step == INST_STEP_CHECK || step == INST_STEP_REPAIR ? -1 : total ? (int)(done * 1000 / total) : 0;
	long long t = monotonic_ms();
	/* A new screen at once; the same screen when a whole percent more is done, at most every few seconds. */
	if ((int)id == k->shown_screen && (permille / 10 == k->shown_permille / 10 || (t - k->shown_at < PROGRESS_MS && done != total)))
		return;
	k->shown_at = t;
	k->shown_screen = id;
	k->shown_permille = permille;
	if (screen_show(k->screen, id, permille, 0))
		printf("PW3_INSTALL cannot show screen %d\n", (int)id);
}

static void log_line(void *ctx, const char *line)
{
	(void)ctx;
	printf("PW3_INSTALL %s\n", line);
	fflush(stdout);
}

void kindle_ops(struct kindle *k, struct screen *screen, struct inst_ops *ops)
{
	memset(k, 0, sizeof(*k));
	k->screen = screen;
	k->shown_screen = -1;
	k->shown_at = -PROGRESS_MS;
	*ops = (struct inst_ops){ .ctx = k, .battery = battery, .online = online, .now = now, .download = download,
				  .wait = wait_seconds, .show = show, .log = log_line };
}

int kindle_lock(const char *user)
{
	char dir[PATH_MAX];
	if (snprintf(dir, sizeof(dir), "%s/android", user) >= (int)sizeof(dir)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	if (mkdir(dir, 0755) && errno != EEXIST)
		return -1;
	int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	if (flock(fd, LOCK_EX | LOCK_NB)) {
		int err = errno;
		close(fd);
		errno = err;
		return -1;
	}
	return fd;
}

void kindle_keep_awake(struct kindle *k)
{
	if (lipc_get("com.lab126.powerd", "preventScreenSaver", k->awake_before, sizeof(k->awake_before)))
		snprintf(k->awake_before, sizeof(k->awake_before), "0");
	char *argv[] = { LIPC_SET, "com.lab126.powerd", "preventScreenSaver", "1", NULL };
	k->awake_changed = !run(argv, NULL, 0);
	printf("PW3_INSTALL screen saver held: %d (was %s)\n", k->awake_changed, k->awake_before);
}

void kindle_restore_awake(struct kindle *k)
{
	if (!k->awake_changed)
		return;
	char *argv[] = { LIPC_SET, "com.lab126.powerd", "preventScreenSaver", k->awake_before, NULL };
	if (run(argv, NULL, 0))
		printf("PW3_INSTALL cannot restore preventScreenSaver=%s\n", k->awake_before);
	k->awake_changed = 0;
}

int kindle_show_error(struct kindle *k, const struct inst_result *r)
{
	static const struct { enum inst_code code; enum pw3_screen screen; } map[] = {
		{ INST_E_LOW_BATTERY, PW3_SCREEN_E102 }, { INST_E_RUNNING, PW3_SCREEN_E103 },
		{ INST_E_DOWNLOAD, PW3_SCREEN_E201 }, { INST_E_OFFLINE, PW3_SCREEN_E202 }, { INST_E_CLOCK, PW3_SCREEN_E203 },
		{ INST_E_PAYLOAD_BAD, PW3_SCREEN_E302 }, { INST_E_RELEASE, PW3_SCREEN_E303 },
		{ INST_E_DATA_FORMAT, PW3_SCREEN_E304 }, { INST_E_NO_SPACE, PW3_SCREEN_E401 }, { INST_E_IO, PW3_SCREEN_E402 },
		{ INST_E_MEMORY, PW3_SCREEN_E403 }, { INST_E_FAT_CROSS, PW3_SCREEN_E404 },
	};
	enum pw3_screen id = PW3_SCREEN_E402;
	for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
		if (map[i].code == r->code)
			id = map[i].screen;
	if (screen_show(k->screen, id, -1, (r->short_bytes + 999999) / 1000000)) {
		printf("PW3_INSTALL cannot show the error screen\n");
		return 0;
	}
	return 1;
}
