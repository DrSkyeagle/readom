/* SPDX-License-Identifier: GPL-2.0-only */
#include "userlog.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

/* The parts of the kernel this project drives or depends on, and the shapes a
 * kernel in trouble prints. Each is matched at the start of the line after the
 * level and timestamp. Measured against 201 logs captured on the device
 * (96,545 lines): 10.2% of the lines are kept, 3.1 KB of a 32 KB log.
 *
 * `mmc1` is deliberately absent. It is the Wi-Fi card's SDIO bus, whose
 * enumeration alone is 1,476 of those lines; the storage this project writes
 * to is mmc0/mmcblk0.
 *
 * `pw3-` is a prefix, not a tag: it is what this project's own patches and
 * programs print under, so a diagnostic that is added later is in the log that
 * gets read instead of being dropped by a list nobody remembered to extend.
 * Patch 1046 prints the cable and the pull-up under `pw3-usb:`, and that is
 * exactly what was missing from the one log that mattered. A tag of ours that
 * does not print its own fixed text goes in DENY below, which is checked first;
 * the identifier check is the second gate either way. */
static const char *const ALLOW[] = {
	"PM:", "suspend debug", "add wake up", "remove wake up",	/* the suspend path the handover rides on */
	"otg udc vbus", "fsl-usb2-udc", "g_file_storage",		/* the cable: a computer on it stops the start */
	"android_usb", "g_android",					/* and, under Android, the gadget on it */
	"pw3-",								/* every line this project prints itself (see below) */
	"mmc0", "mmcblk", "sdhci", "emmc", "EXT3-fs", "EXT4-fs",	/* the storage the images are on */
	"mxc_epdc", "imx_epdc",						/* the panel */
	"KERNEL: I pmic:",						/* the stock power events: charger, power button */
	"Unable to handle kernel", "Internal error", "Kernel panic",	/* a kernel in trouble */
	"Oops", "BUG:", "WARNING:", "Backtrace:", "watchdog",
	/* And the dump under those headings, which is the part that says where
	 * the kernel was and how it got there. Without these the folder a reader
	 * hands on carries "Kernel panic" and "Backtrace:" with nothing after
	 * them -- which is what the V78 start left behind on 2026-10-23: a NULL
	 * dereference in interrupt context on the first resume, 17 lines kept and
	 * 385 dropped, and not one of the dropped ones could be asked for again,
	 * because a kernel ring is 32 KB and that start was over.
	 *
	 * None of it is anybody's: kernel addresses, this kernel's own symbol
	 * names, its registers and the names of modules this project builds.
	 *
	 * Two lines of such a dump are deliberately not here. `Code:` is the
	 * instruction bytes, and the same line is printed for a user-mode fault,
	 * where they are the application's own (the test holds that, among "the
	 * stock system's own crashes"). `Process` is whatever was running, which
	 * is the reader's business and not this project's -- and an Oops in
	 * interrupt context is not about it anyway. */
	"[<",								/* a backtrace frame: [<c00144a0>] (symbol+0x0/0xf8) from ... */
	"pc : ", "sp : ", "r10:", "r7 : ", "r3 : ",			/* the register dump, four to a line */
	"PC is at ", "LR is at ", "pgd = ",
	"CPU: ", "Modules linked in:", "Flags: ", "Control: ",
	"Out of memory", "page allocation failure",			/* the memory the handover needs */
};

/* Printed by this project and still not a line to hand on. This list is a
 * whitelist -- a `pw3-` tag is handed on only by being named here -- and
 * `pw3-boot-report` is not named for two reasons that have both moved. It used
 * to relay up to four whole warning and error lines out of logcat, whose text
 * is whatever an application wrote: a file name, an address, a URL. On
 * 2026-10-29 that became counts per tag, from a fixed list of thirteen names
 * (BootReport.cpp, Relevant()), and the same day the kernel-log sink it wrote
 * to went with androidboot.pw3.diag, so the tag now reaches no kernel log at
 * all. It stays out because nothing has to be let in, not because its content
 * is still private; whoever opens that sink again decides then. Every other
 * `pw3-` tag prints this project's own fixed text and numbers, which
 * tools/tests/test_klog_tags.py holds to. */
static const char *const DENY[] = {
	"pw3-boot-report",
};

/* The one diagnostic this project cannot print for itself.
 *
 * V65 installed and then refused to start, and the reason was an SELinux denial
 * on /dev/block/dm-3: the report area's device is labelled apart from the two
 * image devices so that only pw3_boot_report may write it, and pw3_init_check
 * had no rule for that label, so its stat() failed and the check reported a
 * mismatch of extents that had nothing to do with the cause. The kernel printed
 * the denial. This list dropped it, and the folder a person hands on therefore
 * said nothing at all -- the raw ring had to be read with a tool that is not
 * part of a release.
 *
 * So a denial is kept, under conditions that keep the user's own names out of a
 * folder meant to be handed on. Nothing is rewritten: a line that could name a
 * file of theirs is dropped whole.
 *
 *   scontext must be one of this project's domains -- a denial against somebody
 *   else's domain is not ours to pass on and its object could be anything;
 *   path=, if present, must be under /dev, /sys, /proc or /system, which this
 *   project owns the names of;
 *   name=, which is a bare filename inside a directory with no path to bound
 *   it, is always dropped: a book's title would look exactly like that.
 *
 * Returns -1 for "not a denial", 0 for "a denial, drop it", 1 for "keep". */
static int denial(const char *s, size_t rest)
{
	static const char *const SAFE[] = { "/dev/", "/sys/", "/proc/", "/system/" };
	/* The kernel writes the audit record first, so "avc:" is not at the start:
	 *   type=1400 audit(1792543841.720:4): avc:  denied  { getattr } for ...
	 * Both words are required so that a line merely mentioning one of them is
	 * not taken for a denial. */
	int seen_avc = 0, seen_denied = 0;
	for (size_t i = 0; i < rest; i++) {
		if (!seen_avc && i + 4 <= rest && !memcmp(s + i, "avc:", 4))
			seen_avc = 1;
		if (!seen_denied && i + 6 <= rest && !memcmp(s + i, "denied", 6))
			seen_denied = 1;
	}
	if (!seen_avc || !seen_denied)
		return -1;
	/* Our own domains only. */
	const char *scontext = NULL;
	for (size_t i = 0; i + 17 <= rest; i++)
		if (!memcmp(s + i, "scontext=u:r:pw3_", 17)) { scontext = s + i; break; }
	if (!scontext)
		return 0;
	/* A bare name has nothing bounding it. */
	for (size_t i = 0; i + 6 <= rest; i++)
		if (!memcmp(s + i, "name=\"", 6))
			return 0;
	/* A path, if there is one, has to be somewhere this project names. */
	for (size_t i = 0; i + 6 <= rest; i++) {
		if (memcmp(s + i, "path=\"", 6))
			continue;
		const char *value = s + i + 6;
		size_t left = rest - (i + 6);
		for (unsigned k = 0; k < sizeof(SAFE) / sizeof(SAFE[0]); k++) {
			size_t n = strlen(SAFE[k]);
			if (left >= n && !memcmp(value, SAFE[k], n))
				return 1;
		}
		return 0;
	}
	/* No path and no name: a capability or a socket, with nothing of theirs in
	 * it. The identifier gate still has the last word on the whole line. */
	return 1;
}

static int hex(char c)
{
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* Still an identifier, on a line we would otherwise keep: a MAC address, a long
 * run of hex digits, or a line naming a network or a serial number. None of the
 * 90,850 captured lines reaches this (the allow-list above drops the MAC lines
 * with it), which is the point: it is what catches the line nobody expected. */
static int identifier(const char *s, size_t len)
{
	unsigned run = 0;
	for (size_t i = 0; i < len; i++) {
		run = hex(s[i]) ? run + 1 : 0;
		if (run >= 12)
			return 1;
		/* Six hex pairs, separated by ':' or by '-': both are how a MAC
		 * address is written, and the hyphen form is the one the run of
		 * hex digits above cannot see, because the hyphens break it. */
		if (i + 17 <= len && hex(s[i]) && hex(s[i + 1])) {
			const char sep = s[i + 2];
			unsigned k = 0;
			if (sep == ':' || sep == '-')
				while (k < 5 && s[i + 2 + k * 3] == sep && hex(s[i + 3 + k * 3]) && hex(s[i + 4 + k * 3]))
					k++;
			if (k == 5)
				return 1;
		}
	}
	for (size_t i = 0; i < len; i++) {
		static const char *const words[] = {"ssid", "serial", "command line"};
		for (unsigned w = 0; w < sizeof(words) / sizeof(words[0]); w++) {
			size_t n = strlen(words[w]);
			if (i + n > len)
				continue;
			size_t k = 0;
			while (k < n && (s[i + k] | 0x20) == words[w][k])
				k++;
			if (k == n)
				return 1;
		}
	}
	return 0;
}

int pw3_klog_keep(const char *line, size_t len)
{
	/* "<6>[   32.659546] " -- the level and the timestamp say nothing about
	 * the device, and skipping them is what makes the list below readable. */
	size_t at = 0;
	if (at < len && line[at] == '<') {
		while (at < len && line[at] != '>')
			at++;
		if (at < len)
			at++;
	}
	if (at < len && line[at] == '[') {
		while (at < len && line[at] != ']')
			at++;
		if (at < len)
			at++;
	}
	while (at < len && line[at] == ' ')
		at++;
	const char *s = line + at;
	size_t rest = len - at;
	for (unsigned i = 0; i < sizeof(DENY) / sizeof(DENY[0]); i++) {
		size_t n = strlen(DENY[i]);
		if (rest >= n && !memcmp(s, DENY[i], n))
			return 0;
	}
	int avc = denial(s, rest);
	if (avc >= 0)
		return avc && !identifier(s, rest);
	for (unsigned i = 0; i < sizeof(ALLOW) / sizeof(ALLOW[0]); i++) {
		size_t n = strlen(ALLOW[i]);
		if (rest >= n && !memcmp(s, ALLOW[i], n))
			return !identifier(s, rest);
	}
	return 0;
}

int pw3_report_keep(const char *line, size_t len)
{
	return !identifier(line, len);
}

int pw3_report_write(FILE *stream, const char *text, size_t bytes, unsigned boot,
		     unsigned *kept, unsigned *left_out)
{
	*kept = *left_out = 0;
	fprintf(stream, "pw3-android-report-v1 boot=%u bytes=%zu\n", boot, bytes);
	for (size_t at = 0; at < bytes;) {
		size_t end = at;
		while (end < bytes && text[end] != '\n')
			end++;
		if (end > at && pw3_report_keep(text + at, end - at)) {
			++*kept;
			fprintf(stream, "%.*s\n", (int)(end - at), text + at);
		} else if (end > at) {
			++*left_out;
		}
		at = end + 1;
	}
	fprintf(stream, "pw3-android-report-v1 kept=%u left_out=%u\n", *kept, *left_out);
	/* Flushed here for the reason pw3_klog_end gives: until the buffer goes
	 * out, "it was written" only means nothing has refused it yet. */
	return fflush(stream) || ferror(stream) ? -1 : 0;
}

void pw3_klog_begin(struct pw3_klog_out *out, FILE *stream, const char *source)
{
	out->stream = stream;
	out->kept = out->left_out = 0;
	out->held = 0;
	fprintf(stream, "pw3-kernel-log-filtered-v1 source=%s\n", source);
}

static void klog_line(struct pw3_klog_out *out)
{
	if (!out->held)
		return;
	if (pw3_klog_keep(out->line, out->held)) {
		out->kept++;
		fprintf(out->stream, "%.*s\n", (int)out->held, out->line);
	} else {
		out->left_out++;
	}
	out->held = 0;
}

void pw3_klog_feed(struct pw3_klog_out *out, const char *text, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		char c = text[i];
		if (c == '\n' || c == 0 || out->held == sizeof(out->line))
			klog_line(out);
		if (c != '\n' && c != 0)
			out->line[out->held++] = c;
	}
}

int pw3_klog_end(struct pw3_klog_out *out)
{
	klog_line(out);
	fprintf(out->stream, "pw3-kernel-log-filtered-v1 kept=%u left_out=%u\n",
		out->kept, out->left_out);
	/* Flushed here, not left to the caller: a full user store refuses the
	 * write when the buffer goes out, and a C library is free to keep it
	 * until then (glibc does, with a stream this test fills; the macOS one
	 * fails at the fprintf). Without this, "it was written" would mean
	 * "nothing has refused it yet". */
	return fflush(out->stream) || ferror(out->stream) ? -1 : 0;
}

#define COURTESY_FREE_MIN (1ull << 20)

int pw3_store_has_room(const char *dir, uint64_t wanted, uint64_t *free)
{
	struct statvfs vfs;
	if (statvfs(dir, &vfs)) {
		*free = UINT64_MAX;
		return 0;
	}
	uint64_t unit = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
	*free = (uint64_t)vfs.f_bavail * unit;
	return *free >= wanted + COURTESY_FREE_MIN;
}

/* The launcher's own folder name, and nothing else: the fifteen characters of
 * the timestamp, a hyphen, and the digits of the process id -- what
 * launcher-template.sh makes out of `date +%Y%m%dT%H%M%S` and `$$`, and the only
 * shape anything in a release writes here.
 *
 * The shell there also lets a name be passed to it, and checks that one by its
 * alphabet ([0-9TZ-]); this used to check the same alphabet, and it is the wrong
 * question. What the shell is guarding is the path it is about to write. What
 * this decides is whether a folder may be deleted, and by the alphabet `2026`
 * was the launcher's own: four older folders were enough for rotation to drop it
 * and delete every plain file a reader had put in it. A folder that is not of
 * this shape is now left exactly as it is, which is the safe direction -- a name
 * passed in by hand is kept rather than rotated. */
static int ours(const char *name)
{
	size_t n = strlen(name);
	if (n < 17 || n > 80)
		return 0;
	for (size_t i = 0; i < 15; i++) {
		if (i == 8) {
			if (name[i] != 'T')
				return 0;
		} else if (name[i] < '0' || name[i] > '9')
			return 0;
	}
	if (name[15] != '-')
		return 0;
	for (size_t i = 16; i < n; i++)
		if (name[i] < '0' || name[i] > '9')
			return 0;
	return 1;
}

/* 1 a directory, 0 not one, -1 the question could not be answered. The third
 * answer is not the second: a scan that cannot tell would otherwise skip the
 * entry, which drops the newest run out of the set rotation keeps and then
 * deletes it. Every caller here stops instead. */
static int is_directory(int parent, const char *name)
{
	struct stat st;
	if (fstatat(parent, name, &st, AT_SYMLINK_NOFOLLOW))
		return -1;
	return S_ISDIR(st.st_mode) ? 1 : 0;
}

/* readdir() returns NULL both at the end of a directory and on an error, so
 * every scan below clears errno first and reads it when the loop ends: a scan
 * cut short would otherwise read as a complete one, and rotation would delete by
 * a keep-set it had not finished building. */

/* One run folder: its plain files, then itself. Anything else inside means it
 * is not what this entry wrote, and then it is left exactly as it is. */
static int remove_run(int parent, const char *name)
{
	int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return 0;
	/* Not while somebody is still writing it. A run holds a shared lock on its
	 * own folder for as long as it lives (pw3_logs_hold, helper.c), and the
	 * kernel takes that lock back when the process goes, so a folder left by a
	 * run the OOM killer took is free again and is reclaimed here as before.
	 * Which folder is whose cannot be decided by name: the names are times, and
	 * a clock that went back gives the run that started later the smaller name,
	 * so the newest-by-name keep-set does not know it is looking at a folder in
	 * use. Only the kernel knows, and this is how it is asked.
	 *
	 * A folder goes only when this holds the exclusive lock on it. Any request
	 * that does not succeed keeps the folder, whatever the reason: this kernel
	 * allocates the lock before it looks for a conflicting one, so a folder
	 * somebody holds can be refused with ENOMEM, and reading "any other reason"
	 * as "nobody holds it" would delete it. Keeping costs one folder until the
	 * next tap, which rotates again; the other way round costs a run its folder.
	 *
	 * There is no branch for a store that does not answer lock requests, and
	 * there was one until 2026-10-27. It was a guess, and the guess was wrong:
	 * neither vfat nor fuse gives the directory operations a `flock` of its own
	 * on this kernel, so both go through the generic one, which answers. What
	 * the branch did instead was give every transient refusal a way past the
	 * lock -- which is the thing the lock is for.
	 *
	 * The lock has to outlive the directory, not just the reading of it:
	 * closedir() closes this descriptor and with it the lock, and rmdir does
	 * not consult an advisory lock, so a holder could take the folder back in
	 * between and still lose it. `held` is a second descriptor on the same open
	 * file description -- the same lock -- and it is closed after the rmdir. */
	if (flock(fd, LOCK_EX | LOCK_NB)) {
		close(fd);
		return 0;
	}
	const int held = dup(fd);
	if (held < 0) {
		close(fd);
		return 0;
	}
	DIR *d = fdopendir(fd);
	if (!d) {
		close(held);
		close(fd);
		return 0;
	}
	int ok = 1;
	struct dirent *e;
	errno = 0;
	while ((e = readdir(d))) {
		struct stat st;
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		if (fstatat(fd, e->d_name, &st, AT_SYMLINK_NOFOLLOW) || !S_ISREG(st.st_mode)) {
			ok = 0;
			break;
		}
		errno = 0;
	}
	if (ok && errno)
		ok = 0;
	if (ok) {
		rewinddir(d);
		errno = 0;
		while ((e = readdir(d))) {
			if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
				continue;
			if (unlinkat(fd, e->d_name, 0))
				ok = 0;
			errno = 0;
		}
		if (errno)
			ok = 0;
	}
	closedir(d);
	const int gone = ok && !unlinkat(parent, name, AT_REMOVEDIR);
	close(held);
	return gone;
}

int pw3_logs_hold(int fd, const char *path)
{
	/* A refusal is one of two different things, and they are not retried the
	 * same way. EWOULDBLOCK is a rotation holding this folder: it may go on to
	 * remove it, or keep it (one it cannot read, or an rmdir that fails), so
	 * this is the answer "being removed" rather than "removed", and the caller
	 * is what decides. Waiting here is of no use either way: the rotation
	 * holds the lock until it is done with the folder, and what the caller
	 * needs to know it can ask for later, under the install lock, where no
	 * rotation can be running at all.
	 *
	 * Any other failure is this kernel declining to allocate the lock -- it
	 * allocates before it looks for a conflict, so this says nothing about who
	 * holds what -- and it passes. Five tries, roughly half a second; roughly,
	 * because a signal can cut a sleep short and the allocation inside the
	 * call can take its own time. What must not happen is a run that can never
	 * install because a lock could not be taken, so it then carries on and
	 * says so, no better protected than every release up to V88 was. */
	int locked = 0;
	for (int tries = 0; !locked && tries < 5; tries++) {
		if (!flock(fd, LOCK_SH | LOCK_NB)) {
			locked = 1;
			break;
		}
		if (errno == EWOULDBLOCK)
			return -1;
		usleep(100000);
	}
	/* Taking the lock is not the whole answer. Between this folder being opened
	 * and this call, another run's rotation can have held it, read it and
	 * removed it, and the shared lock then succeeds on an inode with no name
	 * left -- after which every file this run owes its folder fails, far from
	 * here. So the name is asked whether it still leads to this folder.
	 *
	 * With the lock given, the answer holds for as long as this descriptor
	 * does: the only taker of the exclusive lock is a rotation, and it holds
	 * that lock from before it reads the folder until after the folder is
	 * gone. Without it (the retries ran out), the answer is true of this
	 * moment only, and the caller asks again where it can be made to hold --
	 * under the install lock, which is the one thing a rotation needs. */
	struct stat by_fd, by_name;
	if (fstat(fd, &by_fd) || stat(path, &by_name) ||
	    by_fd.st_dev != by_name.st_dev || by_fd.st_ino != by_name.st_ino)
		return -1;
	return locked;
}

#define KEEP_MAX 16

/* The counter in one folder's report, taken from the whole header line that
 * pw3_report_write prints above -- the trailing newline has to be there. A
 * report cut short by a flat battery can end at `boot=1` of `boot=12`, and a
 * parse that stopped at the first field read that as the counter 1.
 *
 * 1 read, 0 not there or not whole, -1 there and unreadable (errno set). The
 * third answer is not the second, for the same reason the scan above needs its
 * own: a folder whose report could not be read would otherwise drop out of the
 * maximum silently, and a counter that has already been seen would come back as
 * a smaller one -- which reads as a report that is new. */
static int report_boot(int parent, const char *folder, const char *leaf, unsigned *boot)
{
	char path[256];
	if ((size_t)snprintf(path, sizeof(path), "%s/%s", folder, leaf) >= sizeof(path))
		return 0;
	int fd = openat(parent, path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return errno == ENOENT || errno == ENOTDIR ? 0 : -1;
	char line[128];
	ssize_t n;
	do {
		n = read(fd, line, sizeof(line) - 1);
	} while (n < 0 && errno == EINTR);
	if (n < 0) {
		const int why = errno;
		close(fd);
		errno = why;
		return -1;
	}
	close(fd);
	if (n == 0)
		return 0;
	line[n] = 0;
	unsigned value;
	unsigned long long bytes;
	char tail = 0;
	if (sscanf(line, "pw3-android-report-v1 boot=%u bytes=%llu%c", &value, &bytes, &tail) != 3 || tail != '\n')
		return 0;
	(void)bytes;
	*boot = value;
	return 1;
}

const char *pw3_logs_start_word(int found, unsigned before, unsigned now)
{
	if (found < 0)
		return "unknown";
	if (!found)
		return "no_previous_report";
	return before < now ? "reported" : "no_new_report";
}

int pw3_logs_report_max(const char *dir, const char *current, const char *leaf, unsigned *boot)
{
	if (!boot || !leaf || strchr(leaf, '/'))
		return -1;
	*boot = 0;
	int fd = open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return -1;
	DIR *d = fdopendir(fd);
	if (!d) {
		const int why = errno;
		close(fd);
		errno = why;
		return -1;
	}
	struct dirent *e;
	int found = 0, failed = 0;
	errno = 0;
	while ((e = readdir(d))) {
		const char *name = e->d_name;
		int state = (ours(name) && !(current && !strcmp(name, current))) ? is_directory(fd, name) : 0;
		if (state < 0) {
			failed = 1;
			break;
		}
		if (state > 0) {
			unsigned value;
			const int read_it = report_boot(fd, name, leaf, &value);
			if (read_it < 0) {
				failed = 1;
				break;
			}
			if (read_it && (!found || value > *boot)) {
				*boot = value;
				found = 1;
			}
		}
		errno = 0;
	}
	/* The reason is carried out past closedir(), which has an errno of its own
	 * and would otherwise be the one the caller prints. */
	const int why = errno;
	if (why)
		failed = 1;
	closedir(d);
	errno = why;
	return failed ? -1 : found;
}

int pw3_logs_rotate(const char *dir, const char *current, unsigned keep)
{
	char newest[KEEP_MAX][81];
	unsigned held = 0;
	/* One of the kept folders is the one being written, which is not a
	 * candidate here, so the rest of them share the remaining slots. */
	keep = keep > KEEP_MAX ? KEEP_MAX - 1 : keep ? keep - 1 : 0;
	int fd = open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return -1;
	DIR *d = fdopendir(fd);
	if (!d) {
		close(fd);
		return -1;
	}
	/* The `keep` newest names, by name: the folder name is the launcher's
	 * timestamp, so sorting it is sorting by time, and a clock that went
	 * backwards cannot make this delete the run writing now -- `current` is
	 * never a candidate. */
	struct dirent *e;
	errno = 0;
	while ((e = readdir(d))) {
		int state = (ours(e->d_name) && strcmp(e->d_name, current)) ? is_directory(fd, e->d_name) : 0;
		if (state < 0) {
			closedir(d);
			return -1;
		}
		if (state > 0) {
			unsigned at = held;
			while (at && strcmp(newest[at - 1], e->d_name) < 0) {
				if (at < keep)
					memcpy(newest[at], newest[at - 1], sizeof(newest[0]));
				at--;
			}
			if (at < keep) {
				memcpy(newest[at], e->d_name, strlen(e->d_name) + 1);
				if (held < keep)
					held++;
			}
		}
		errno = 0;
	}
	if (errno) {
		closedir(d);
		return -1;
	}
	closedir(d);

	/* A second pass over a directory nobody is iterating: readdir may skip
	 * entries while they are being removed, and a run that survives one tap
	 * is removed by the next. */
	int removed = 0;
	fd = open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return -1;
	d = fdopendir(fd);
	if (!d) {
		close(fd);
		return -1;
	}
	errno = 0;
	while ((e = readdir(d))) {
		int state = (ours(e->d_name) && strcmp(e->d_name, current)) ? is_directory(fd, e->d_name) : 0;
		if (state < 0) {
			closedir(d);
			return -1;
		}
		if (state > 0) {
			unsigned i = 0;
			while (i < held && strcmp(newest[i], e->d_name))
				i++;
			if (i >= held && remove_run(fd, e->d_name))
				removed++;
		}
		errno = 0;
	}
	if (errno) {
		closedir(d);
		return -1;
	}
	closedir(d);
	return removed;
}
