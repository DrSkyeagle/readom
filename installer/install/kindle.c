/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "kindle.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <sys/ioctl.h>
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
#define REPAINT_MS 30000

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
		} else if (errno == ENOENT) {
			/* No part yet -- connecting, or a TLS handshake that will time
			 * out: no bytes, and still a report, because the report is also
			 * what repaints the screen when nothing else does (show). */
			progress(pctx, 0);
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
	/* A new screen at once; the same screen when a whole percent more is done,
	 * at most every few seconds -- and the same screen again after half a
	 * minute without either. The stock UI can still paint over this one (its
	 * own dialogs, a quick-settings panel opened before the touch screen was
	 * taken), and a download standing at 0% would otherwise never paint it
	 * back: 2026-11-05, a white screen for minutes. */
#if CHECK_PROFILE == 1
	kindle_take_touch();
#endif
	if ((int)id == k->shown_screen && t - k->shown_at < REPAINT_MS &&
	    (permille / 10 == k->shown_permille / 10 || (t - k->shown_at < PROGRESS_MS && done != total)))
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

/* ---- holding the last screen ----
 *
 * The library runs a script through the jailbreak's sh_integration launcher,
 * and when the script exits the launcher tells appmgrd to stop it and runs
 * xrefresh, which repaints every window: the library comes back over whatever
 * eips drew, 1.1-1.2 s after the exit, with Wi-Fi on or off (the screen probe
 * of 2026-11-04, launcher v4.1.0). While the script was still running the same
 * rows did not change once in 25 s. So a screen that ends a run stays only as
 * long as the run does, and the run waits for the reader.
 *
 * The touch screen is grabbed (EVIOCGRAB) for the wait. Without it the tap
 * that dismisses this screen would also reach the stock UI underneath, and a
 * tap on a library that cannot be seen opens whatever book is under the
 * finger. The grab is all or nothing for a touch: the input core hands a grab
 * every event of the device and the others none, and gives the others nothing
 * back when it is let go (evdev_ungrab replays nothing). So it is only ever
 * taken and let go with no finger on the screen -- a touch that began before
 * the grab is the stock UI's to finish, and the one that ends the wait is
 * finished here. "No finger" is the driver's BTN_TOUCH, which the stock
 * cyttsp4 driver reports as "any contact" on every frame (cyttsp4_mt_common.c:
 * num_cur_tch > 0); a quiet spell is not a lifted finger, because the input
 * core drops repeated positions and a finger held still sends nothing. A
 * device without BTN_TOUCH is not taken at all.
 *
 * The power button is not taken: it still puts the Kindle to sleep, and a wait
 * that finds it has slept ends at once, because what comes back on waking is
 * the stock UI's screen, not this one. Ten minutes is the end of it either
 * way: the stock idle timer cannot see a grabbed touch screen, and a Kindle
 * left with its touch screen held for an hour is a Kindle that looks frozen. */
#define HOLD_MAX_MS (10 * 60 * 1000)
#define HOLD_SLEPT_MS 2000	/* the boot clock ran ahead of the monotonic one by this much */

/* struct input_event as this kernel (3.0.35, ARM32) writes it: a 32-bit
 * timeval. Spelled out rather than taken from linux/input.h, whose timeval
 * follows the C library's time_t, and musl's is 64 bits wide. */
struct hold_event {
	uint32_t sec, usec;
	uint16_t type, code;
	int32_t value;
};
_Static_assert(sizeof(struct hold_event) == 16, "input_event of a 32-bit kernel");
#define HOLD_EV_SYN 0x00
#define HOLD_EV_KEY 0x01
#define HOLD_EV_ABS 0x03
#define HOLD_SYN_REPORT 0
#define HOLD_SYN_DROPPED 3
#define HOLD_BTN_TOUCH 0x14a
#define HOLD_ABS_X 0x00
#define HOLD_ABS_MT_POSITION_X 0x35
#define HOLD_KEY_BYTES (0x300 / 8)	/* KEY_MAX + 1 bits */
#define HOLD_EVIOCGBIT(ev, len) _IOC(_IOC_READ, 'E', 0x20 + (ev), len)
#define HOLD_EVIOCGKEY(len) _IOC(_IOC_READ, 'E', 0x18, len)
#define HOLD_EVIOCGRAB _IOW('E', 0x90, int)
#define HOLD_MAX_DEVICES 8

/* Cannot fail for the monotonic clock it is asked for; zeroed so that a
 * failure would read as time zero, not as whatever the stack held. */
static long long clock_ms(clockid_t id)
{
	struct timespec ts = {0, 0};
	clock_gettime(id, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int bit(const unsigned char *bits, unsigned n)
{
	return (bits[n / 8] >> (n % 8)) & 1;
}

/* A touch screen this can wait on: absolute positions and a BTN_TOUCH. */
static int is_touch_screen(int fd)
{
	unsigned char types[4] = {0}, axes[8] = {0}, keys[HOLD_KEY_BYTES] = {0};
	if (ioctl(fd, HOLD_EVIOCGBIT(0, sizeof(types)), types) < 0 || !bit(types, HOLD_EV_ABS) ||
	    !bit(types, HOLD_EV_KEY))
		return 0;
	if (ioctl(fd, HOLD_EVIOCGBIT(HOLD_EV_ABS, sizeof(axes)), axes) < 0 ||
	    !(bit(axes, HOLD_ABS_MT_POSITION_X) || bit(axes, HOLD_ABS_X)))
		return 0;
	return ioctl(fd, HOLD_EVIOCGBIT(HOLD_EV_KEY, sizeof(keys)), keys) >= 0 && bit(keys, HOLD_BTN_TOUCH);
}

/* Whether a finger is on this device now: 1, 0, or -1 when it cannot say. */
static int touching(int fd)
{
	unsigned char keys[HOLD_KEY_BYTES] = {0};
	if (ioctl(fd, HOLD_EVIOCGKEY(sizeof(keys)), keys) < 0)
		return -1;
	return bit(keys, HOLD_BTN_TOUCH);
}

static int64_t clock_us(clockid_t id)
{
	struct timespec ts;
	if (clock_gettime(id, &ts))
		return -1;
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* Microseconds since boot, sleep included, from /proc/uptime (in hundredths);
 * -1 when it cannot be read. Not CLOCK_BOOTTIME: the stock kernel's
 * get_monotonic_boottime() adds three 32-bit nanosecond fields before widening
 * them (timekeeping.c:999), so once the Kindle has slept, a reading taken in
 * part of every second comes back short by 2^32 ns, 4.29 s -- the fault that
 * 0103 fixes in the Android kernel, and that the stock kernel this runs on
 * still has. /proc/uptime reaches the same clock through
 * monotonic_to_bootbased(), which adds two fields at a time. */
static int64_t uptime_us(void)
{
	char buf[64];
	const int fd = open("/proc/uptime", O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	const ssize_t got = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (got <= 0)
		return -1;
	buf[got] = 0;
	char *end;
	errno = 0;
	const unsigned long long sec = strtoull(buf, &end, 10);
	if (errno || end == buf || end[0] != '.' || end[1] < '0' || end[1] > '9' || end[2] < '0' ||
	    end[2] > '9')
		return -1;
	return (int64_t)sec * 1000000 + ((end[1] - '0') * 10 + (end[2] - '0')) * 10000;
}

/* The two clocks a sleep is told by, read together: since boot with the
 * sleeps (boot) and without them (mono); boot - mono moves only when the
 * Kindle sleeps. mono is read between two readings of boot and refused when
 * those are 100 ms or more apart: a delay or a sleep between the readings
 * would otherwise be read as a sleep. Asked a few times; 0 when the clocks
 * could not be read together. */
struct hold_clocks {
	int64_t boot, mono;
};

static int read_clocks(struct hold_clocks *c)
{
	for (int tries = 0; tries < 8; tries++) {
		const int64_t before = uptime_us();
		const int64_t mono = clock_us(CLOCK_MONOTONIC);
		const int64_t after = uptime_us();
		if (before >= 0 && mono >= 0 && after >= before && after - before < 100000) {
			c->boot = after;
			c->mono = mono;
			return 1;
		}
	}
	return 0;
}

/* Reads what is waiting on fd. Returns 1 when a frame ended with no finger on
 * the screen after one had been on it during this hold (*down says whether one
 * is), 0 otherwise, -1 when the device is gone. *down: 0 no touch counted, 1 a
 * counted touch, 2 that touch lifted (settled at the end of its frame), 3 a
 * touch from before the line (draw_line), let finish without counting. */
static int drain(int fd, int *down)
{
	struct hold_event e[16];
	ssize_t got;
	int lifted = 0, dropped = 0;
	while ((got = read(fd, e, sizeof(e))) > 0) {
		for (size_t j = 0; j < (size_t)got / sizeof(e[0]); j++) {
			if (e[j].type == HOLD_EV_SYN && e[j].code == HOLD_SYN_DROPPED) {
				dropped = 1;	/* the queue overflowed: what was in it is gone */
			} else if (e[j].type == HOLD_EV_KEY && e[j].code == HOLD_BTN_TOUCH) {
				if (e[j].value) {
					if (*down != 3)
						*down = 1;
				}
				else if (*down == 3)
					*down = 0;	/* the touch from before the line ended */
				else if (*down)
					*down = 2;	/* lifted; settled at the end of the frame */
			} else if (e[j].type == HOLD_EV_SYN && e[j].code == HOLD_SYN_REPORT && *down == 2) {
				*down = 0;
				lifted = 1;
			}
		}
	}
	if (got < 0 && errno != EAGAIN && errno != EINTR)
		return -1;
	/* After an overflow the events cannot say whether a finger is down, and
	 * the driver can: a counted press whose release was lost is a touch that
	 * ended. A press that was lost is not made up from the driver's state --
	 * that state cannot say when the finger came down, and a finger from
	 * before the hold would then end it when it lifts. A tap lost to an
	 * overflow is a tap the reader makes again. */
	if (dropped) {
		const int now = touching(fd);
		if (now < 0)
			return -1;
		if (!now && *down) {
			lifted = *down != 3;
			*down = 0;
		}
	}
	return lifted;
}

/* The touch screen, once found, for the rest of this program: taken at the first
 * screen of a run and kept to the end of it (kindle_take_touch), and used by
 * the hold at the end. */
static struct {
	struct pollfd fds[HOLD_MAX_DEVICES];
	int n, opened, grabbed, refused;
} touch;

static int touch_open(void)
{
	if (touch.opened)
		return touch.n;
	touch.opened = 1;
	for (int i = 0; i < 16 && touch.n < HOLD_MAX_DEVICES; i++) {
		char path[32];
		snprintf(path, sizeof(path), "/dev/input/event%d", i);
		int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;
		if (is_touch_screen(fd)) {
			touch.fds[touch.n].fd = fd;
			touch.fds[touch.n].events = POLLIN;
			touch.n++;
		} else {
			close(fd);
		}
	}
	return touch.n;
}

/* Takes every touch device, only between touches, and asks again once taken:
 * a finger that landed between the question and the grab was seen by the
 * stock UI first, and is given back to it. 1 when held, 0 when a finger is on
 * the screen now (ask again later), -1 when the kernel refuses the grab or
 * will not say whether a finger is down. What is queued from before is read
 * away after the question, as in draw_line, so that it holds every touch the
 * question says is over: the stock UI saw those too, and every press read
 * while the grab is held is one that only this saw. */
static int touch_take(int *down)
{
	if (touch.grabbed)
		return 1;
	int busy = 0, failed = 0, taken = 0;
	for (int i = 0; i < touch.n && !busy && !failed; i++) {
		const int t = touching(touch.fds[i].fd);
		failed = t < 0;
		busy = t > 0;
	}
	for (int i = 0; i < touch.n && !busy && !failed; i++) {
		drain(touch.fds[i].fd, &down[i]);
		down[i] = 0;
	}
	while (taken < touch.n && !busy && !failed) {
		if (ioctl(touch.fds[taken].fd, HOLD_EVIOCGRAB, 1)) {
			failed = 1;
			break;
		}
		const int t = touching(touch.fds[taken++].fd);
		failed = t < 0;
		busy = t > 0;
	}
	if (!busy && !failed) {
		touch.grabbed = 1;
		return 1;
	}
	while (taken-- > 0)
		ioctl(touch.fds[taken].fd, HOLD_EVIOCGRAB, 0);
	return failed ? -1 : 0;
}

/* Lets go -- not in the middle of a touch where that can be helped: the rest
 * of a touch held then would go to the stock UI without its beginning. Two
 * seconds for it to end; a finger still there after that is given up on.
 *
 * What this cannot close is the instant between the grab and the check after
 * it (touch_take), and between the last check here and the release: the stock
 * evdev (evdev.c, evdev_grab/evdev_ungrab) has no way to take a device and ask
 * its state in one step, and replays nothing to the other readers. A finger
 * landing in that window -- two system calls wide, and as wide as the
 * scheduler makes it, with no bound on that -- is split between this and the
 * stock UI. It is left as it is: what would close it is a grab that can ask
 * and take in one step, and this kernel has none. Taking it once, at the start
 * of a run, rather than at its end, makes it one window instead of two. */
static void touch_release(int *down)
{
	for (long long until = clock_ms(CLOCK_MONOTONIC) + 2000; touch.grabbed && clock_ms(CLOCK_MONOTONIC) < until;) {
		int any = 0;
		for (int i = 0; i < touch.n; i++)
			any |= touching(touch.fds[i].fd) > 0;
		if (!any)
			break;
		poll(touch.fds, touch.n, 100);
		for (int i = 0; i < touch.n; i++)
			drain(touch.fds[i].fd, &down[i]);
	}
	for (int i = 0; i < touch.n; i++) {
		if (touch.grabbed)
			ioctl(touch.fds[i].fd, HOLD_EVIOCGRAB, 0);
		close(touch.fds[i].fd);
	}
	touch.n = 0;
	touch.grabbed = 0;
}

/* For the whole of a run, not only its last screen. The installer's screens
 * are drawn over a stock UI that is still there underneath and still takes
 * touches: a swipe from the top opens the stock quick settings over the
 * progress (2026-11-05, during a download that stood at 0% -- the screen was
 * white until the next draw), and a tap lands on a library the reader cannot
 * see. So the first screen takes the touch screen and the run keeps it: until
 * the handover, where the stock kernel goes, or the end of the run, where the
 * hold lets it go. Asked again at every screen while a finger is down; a grab
 * the kernel refuses is said once and not asked again. */
void kindle_take_touch(void)
{
	int down[HOLD_MAX_DEVICES] = {0};
	if (touch.grabbed || touch.refused || !touch_open())
		return;
	const int r = touch_take(down);
	if (r > 0)
		printf("PW3_INSTALL touch screen taken for the run (%d device(s))\n", touch.n);
	else if (r < 0) {
		touch.refused = 1;
		printf("PW3_INSTALL touch screen could not be taken for the run\n");
	}
}

/* Draws the line that the tap ending the hold has to begin after, by the order
 * of the queue rather than by the stamps on it, which are wall-clock time and
 * move when the Kindle sets its clock. The driver is asked first whether a
 * finger is down, and the queue is read away after. A touch that began before
 * the question and is over by then has a press whose frame was already
 * complete -- the driver ends the frame with a press (input_sync, after each
 * contact or in cyttsp4_final_sync, cyttsp4_mtb.c) before it reports the lift
 * (cyttsp4_mt_common.c) -- so it is in what is read away. A finger still
 * down is let finish without counting (3); one that lifts while the queue is
 * read is asked about again, and the queue read away again after that answer,
 * which by the same order takes in its press. Whatever is read after this
 * began after the last question. The order is all this rests on: nothing
 * here assumes the driver cannot be stopped partway through a frame. */
static void draw_line(int *down)
{
	for (int i = 0; i < touch.n; i++) {
		int finger = touching(touch.fds[i].fd);
		drain(touch.fds[i].fd, &down[i]);
		if (finger > 0 && touching(touch.fds[i].fd) == 0) {
			drain(touch.fds[i].fd, &down[i]);
			finger = 0;
		}
		down[i] = finger != 0 ? 3 : 0;
	}
}

void kindle_hold_screen(volatile sig_atomic_t *stop)
{
	/* The script that ran this, which is what exits to put the library back.
	 * Taken first; and one that is already gone -- this kernel hands its
	 * children to init, it has no subreapers -- leaves nothing to wait for. */
	const pid_t parent = getppid();
	int down[HOLD_MAX_DEVICES] = {0};
	if (parent == 1) {
		printf("PW3_INSTALL screen not held: the script that ran this is gone\n");
		touch_release(down);
		return;
	}
	/* The 90-second guard of the run is not a reason to stop waiting for the
	 * reader. A request to stop is, and *stop is only ever set by one. */
	alarm(0);
	if (!touch_open()) {
		printf("PW3_INSTALL screen not held: no touch screen with BTN_TOUCH\n");
		return;
	}
	/* What a sleep is told by, taken at the start: a sleep is only seen
	 * against a reading from before it. Without one the wait could not end
	 * on a sleep, and a Kindle that wakes with its touch screen held looks
	 * frozen; so there is no wait. */
	struct hold_clocks at;
	if (!read_clocks(&at)) {
		printf("PW3_INSTALL screen not held: the clocks could not be read together\n");
		touch_release(down);
		return;
	}
	const int64_t asleep = at.boot - at.mono;
	const int held_for_the_run = touch.grabbed;
	const int n = touch.n;
	draw_line(down);
	const long long start = clock_ms(CLOCK_MONOTONIC);
	int waits = 0;
	const char *why = "ten minutes";
	for (;;) {
		const long long now = clock_ms(CLOCK_MONOTONIC);
		struct hold_clocks c;
		if (read_clocks(&c) && c.boot - c.mono - asleep > HOLD_SLEPT_MS * 1000LL) {
			why = "the Kindle slept";
			break;
		}
		if (*stop) {
			why = "a request to stop";
			break;
		}
		if (getppid() != parent) {
			why = "the script that ran this went away";
			break;
		}
		if (now - start >= HOLD_MAX_MS) {
			/* The clock does not end a touch: asked of the driver, not of
			 * what was read, which can be a frame behind. */
			int pressed = 0;
			for (int i = 0; touch.grabbed && i < n; i++)
				pressed |= touching(touch.fds[i].fd) != 0;
			if (!pressed)
				break;
		}
		if (!touch.grabbed) {
			/* A grab the kernel refuses, or a state it will not report,
			 * ends the wait: the screen cannot be dismissed without one,
			 * and every tap would go to a library the reader cannot see. */
			const int r = touch_take(down);
			if (r < 0) {
				why = "the touch screen could not be taken";
				break;
			}
			if (r == 0)
				waits++;
		}
		const int ready = poll(touch.fds, n, touch.grabbed ? 1000 : 200);
		if (ready < 0 && errno == EINTR)
			continue;
		if (ready < 0) {
			why = "poll failed";
			break;
		}
		if (ready == 0)
			continue;
		int lifted = 0, gone = 0;
		for (int i = 0; i < n; i++) {
			if (touch.fds[i].revents & (POLLERR | POLLHUP | POLLNVAL))
				gone = 1;
			int r = drain(touch.fds[i].fd, &down[i]);
			if (r < 0)
				gone = 1;
			if (touch.grabbed && r > 0)
				lifted = 1;
		}
		if (gone) {
			why = "the touch screen went away";
			break;
		}
		if (lifted) {
			int any = 0;
			for (int i = 0; i < n; i++)
				any |= touching(touch.fds[i].fd) != 0;
			if (!any) {
				why = "a tap";
				break;
			}
		}
	}
	touch_release(down);
	printf("PW3_INSTALL screen held %lld ms on %d device(s)%s, until %s; waited %d time(s) for a touch to end\n",
	       clock_ms(CLOCK_MONOTONIC) - start, n, held_for_the_run ? " held since the run began" : "", why, waits);
}
