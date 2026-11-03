/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "install.h"
#include "fatcheck.h"
#include "ReportArea.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "unpack.h"

/* Downloads: attempts in a row that may add nothing before giving up. */
#define DOWNLOAD_ATTEMPTS 6
#define DOWNLOAD_IDLE_ATTEMPTS 2
#define CLOCK_SKEW_SECONDS (24 * 3600)
/* curl's answer when the transfer stayed under the floor the download is given
 * (--speed-limit 1024 --speed-time 60, kindle.c): under 1 KB/s for a whole
 * minute. Bytes may well have arrived; the rate is the answer. */
#define CURL_TOO_SLOW 100

enum image_state { IMG_NONE, IMG_WRITING, IMG_READY };

struct state {
	enum image_state system, data;
	char version[64];
	char system_sha256[65];
	char boot_sha256[65];	/* the installed system's boot image; empty when not recorded */
	unsigned data_format;
	/* The applications in the kept data image may still have to be compiled
	 * against the boot image now in place, so the start is told so (the screen
	 * that says this one compiles). Set when a system with a different boot
	 * image is installed over a data image that is kept, and cleared when a
	 * report says an Android start got all the way to boot_completed: the
	 * compiling happens in SystemServer.startOtherServices() --
	 * updatePackagesIfNeeded(), which is 700 lines before systemReady() in the
	 * same function -- so a start that completed is a start that finished
	 * compiling.
	 *
	 * It used to be a count of launches, three of them, because nothing on this
	 * side could ask the Android side whether it had finished. The report area
	 * is that answer (docs/design/diagnostics.md), and a count was
	 * wrong in the direction that hurts: three starts cut short left the fourth
	 * being told "about three minutes, hold the power button after ten" while
	 * the device was still compiling, which is advice to interrupt it again
	 * (docs/known-issues.md, 八). */
	int compiling;
	/* The count the area held when this notice was set. A report is evidence
	 * that the compiling is done when it comes from the system installed here
	 * (`build` in struct report) *and* it was written after the install that
	 * owes the work -- and "after" is what this number answers, because the
	 * same system may have completed a start before that install.
	 *
	 * The sequence it refuses: A completes a start and its report is kept on a
	 * computer; B is installed over it (another boot image, so another notice);
	 * A is installed again, which owes compiling again; A's old report is put
	 * back. The build matches, boot_completed is in it, and without this it
	 * would take the notice away while the work runs.
	 *
	 * Set from the area's own count at the tap that sets the notice, and gone
	 * when the notice goes. Not a watermark that outlives it: that was tried
	 * for two passes of review and each fix opened the next hole (open-items,
	 * 二十). The cost here is a notice that stays up when the counter restarts
	 * -- an area that was not a report leaves the next write counting from 1 --
	 * and that is the cheap direction: one notice too many is one look at a
	 * screen, one too few is somebody holding the power button in the middle of
	 * dexopt. It is bounded by the count at the install, not by everything the
	 * device has ever done. */
	uint32_t compiling_after;
	int compiling_after_known;	/* the state file carried a compiling_after= line */
	/* The report this side has already acted on, as the area's own count of
	 * Android starts (ReportArea.h). A report with a different count is one no
	 * earlier tap has acted on. Zero is "none seen", which is also what a device
	 * that has never started Android has.
	 *
	 * Kept here rather than derived from the report copies in the log folders
	 * that the launcher writes: those are rotated, a reader may delete them,
	 * and the launcher skips writing one when the store is nearly full
	 * (helper.c). A decision about installing cannot rest on files the design
	 * treats as disposable.
	 *
	 * This count is what the refusal path is asked about, and nothing else:
	 * "have I acted on this report already". It may move in either direction,
	 * because the count restarts at 1 whenever the area it was read from was
	 * not a report, and a baseline that could only rise would have such a
	 * device go silent for good.
	 *
	 * It was also, for two passes of review, what told a completed start from a
	 * stale one, by way of a second number that only ever rose ("the greatest
	 * count ever seen"). That was a count standing in for an identity, and it
	 * cost what proxies cost: the number lives in the report's first line,
	 * which the checksum does not cover, so one bit turned over in it could
	 * leave a watermark no honest report could ever pass; a remake of the area
	 * restarted the counter and left the watermark measuring the sequence
	 * before it; and the two numbers on one line put the one that may not fall
	 * inside the condition that drops the one that may. The identity was in the
	 * report all along -- see `build` in struct report -- so the second number
	 * is gone and this one answers only the question it can. */
	uint32_t android_boot;
	int android_known;	/* the state file carried an android= line */
	int known;	/* the file was there and readable */
};

struct run {
	const struct inst_release *rel;
	const struct inst_paths *paths;
	const struct inst_ops *ops;
	struct inst_result *res;
	struct state st;
	char dir[PATH_MAX], state_path[PATH_MAX], system_path[PATH_MAX], data_path[PATH_MAX],
		download_dir[PATH_MAX], documents_payload[PATH_MAX], download_payload[PATH_MAX], part_path[PATH_MAX],
		ca_path[PATH_MAX], template_path[PATH_MAX], report_path[PATH_MAX];
	enum inst_step step;
	/* Whether the last write_state() got its folder onto the disk; see there. */
	int state_synced;
};

static void say(struct run *r, const char *fmt, ...)
{
	char line[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	if (r->ops->log)
		r->ops->log(r->ops->ctx, line);
}

static enum inst_code stop(struct run *r, enum inst_code code, int err, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(r->res->detail, sizeof(r->res->detail), fmt, ap);
	va_end(ap);
	r->res->code = code;
	r->res->sys_errno = err;
	say(r, "stop E%d: %s%s%s", (int)code, r->res->detail, err ? ": " : "", err ? strerror(err) : "");
	return code;
}

static void show(struct run *r, enum inst_step step, uint64_t done, uint64_t total)
{
	if (r->ops->show)
		r->ops->show(r->ops->ctx, step, done, total);
}

static int path(char *out, const char *a, const char *b)
{
	int n = snprintf(out, PATH_MAX, "%s/%s", a, b);
	return n < 0 || n >= PATH_MAX ? -1 : 0;
}

static void hex(const uint8_t sha[32], char out[65])
{
	for (int i = 0; i < 32; i++)
		snprintf(out + 2 * i, 3, "%02x", sha[i]);
}

/* The size of a regular file, FILE_ABSENT when it is not there or is not a
 * regular file, and FILE_UNREADABLE when the user store could not say. The last
 * two are kept apart because two decisions below turn on them: an image that
 * cannot be read is not an image that is gone, and treating one as the other
 * writes a blank data image over everything the user has read. */
#define FILE_ABSENT (-1)
#define FILE_UNREADABLE (-2)
static int64_t file_size(const char *p)
{
	struct stat st;
	if (lstat(p, &st))
		return errno == ENOENT || errno == ENOTDIR ? FILE_ABSENT : FILE_UNREADABLE;
	return S_ISREG(st.st_mode) ? st.st_size : FILE_ABSENT;
}

static int remove_file(struct run *r, const char *p)
{
	if (unlink(p) && errno != ENOENT) {
		say(r, "cannot delete %s: %s", p, strerror(errno));
		return -1;
	}
	return 0;
}

/* ---- the state file ---- */

/* 0 when the state is as good as read -- which includes there being no state
 * file at all -- and -1 when the store would not say, errno set.
 *
 * Those are not one answer. "There is no state file" is what the first install
 * on a device looks like, and ensure() below reads it together with a data image
 * that is already there as "an image made before there were state files, of the
 * first format". A first install interrupted while the data image was being made
 * leaves a half-made image and a state file that says `data=writing`, and that
 * is remade on the next tap. Fold a failure to read into "there is no state
 * file" and the next tap instead infers the half-made image to be a complete one
 * of the old format, finds it the wrong size, and stops with E304 -- which tells
 * the reader to carry on with their previous version, of which on a first
 * install there is none. The reading was never confirmed; it just was not
 * attempted. */
static int read_state(struct run *r)
{
	struct state *s = &r->st;
	memset(s, 0, sizeof(*s));
	FILE *f = fopen(r->state_path, "re");
	if (!f)
		return errno == ENOENT || errno == ENOTDIR ? 0 : -1;
	char line[256];
	int schema = 0;
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\n")] = 0;
		char version[64], sha[65];
		unsigned format;
		unsigned long long boot;
		if (!strcmp(line, "schema=1"))
			schema = 1;
		else if (!strcmp(line, "system=writing"))
			s->system = IMG_WRITING;
		else if (sscanf(line, "system=ready %63s %64s", version, sha) == 2 && strlen(sha) == 64) {
			s->system = IMG_READY;
			strcpy(s->version, version);
			strcpy(s->system_sha256, sha);
		} else if (sscanf(line, "boot=%64s", sha) == 1 && strlen(sha) == 64)
			strcpy(s->boot_sha256, sha);
		else if (!strcmp(line, "data=writing"))
			s->data = IMG_WRITING;
		else if (sscanf(line, "data=ready %u", &format) == 1) {
			s->data = IMG_READY;
			s->data_format = format;
		} else if (!strcmp(line, "compiling=1"))
			s->compiling = 1;
		else if (sscanf(line, "compiling_after=%llu", &boot) == 1 && boot <= 0xffffffffull) {
			s->compiling_after = (uint32_t)boot;
			s->compiling_after_known = 1;
		}
		else if (sscanf(line, "android=%llu", &boot) == 1 && boot <= 0xffffffffull) {
			/* Whatever follows the number is read past, and a `seen=` line
			 * is one of the lines read past: both are where the greatest
			 * count ever seen was written, and no decision asks for it any
			 * more (struct state). */
			s->android_boot = (uint32_t)boot;
			s->android_known = 1;
		}
	}
	int bad = ferror(f);
	const int why = bad ? (errno ? errno : EIO) : 0;
	fclose(f);
	if (bad) {
		memset(s, 0, sizeof(*s));
		errno = why;
		return -1;
	}
	if (schema != 1) {
		/* Read through, and not a state file this installer wrote: a file
		 * from before the schema, or one whose first line never got
		 * written. That is a fact about the file, not a failure to read
		 * it, and it is what "no state" has always meant here. */
		say(r, "state file carries no schema=1 line; treated as absent");
		memset(s, 0, sizeof(*s));
		return 0;
	}
	s->known = 1;
	return 0;
}

/* Appends one line of the state file, and stops for good once a line would not
 * fit. Every line of that file has to be in it, so a short one is refused by
 * the caller rather than written.
 *
 * The lines come to 265 bytes of the 512 today, so this is not a truncation
 * waiting to happen -- it is a *write past the buffer* waiting for one more
 * field. The pattern it replaces accumulated `n += snprintf(text + n,
 * sizeof(text) - n, ...)`, and snprintf returns the length it would have
 * written: one truncated line leaves n past the end, the next call is handed a
 * negative remaining length as a very large size_t, and the write(2) at the
 * bottom is then given a length past the end of the buffer as well. */
static void add_line(char *text, size_t cap, size_t *at, const char *fmt, ...)
{
	va_list ap;
	int n;
	if (*at >= cap)
		return;
	va_start(ap, fmt);
	n = vsnprintf(text + *at, cap - *at, fmt, ap);
	va_end(ap);
	*at = n < 0 || (size_t)n >= cap - *at ? cap : *at + (size_t)n;
}

/* Whether this file's own directory entry is on the disk. After a rename that
 * entry is a different one, and vfat_rename() attaches the inode to it and --
 * outside dirsync, which this mount is not -- only marks the inode dirty
 * (fs/fat/namei_vfat.c), so the start cluster and the size it names are still
 * in memory alone. __fat_write_inode() is what puts them in that entry and
 * syncs the buffer, and fsync on this file is what reaches it:
 * generic_file_fsync syncs the inode it is given and fat_file_fsync adds the
 * FAT's own inode, so a fsync of the folder reaches neither. Read-only is
 * enough to ask for it. errno is the one that explains the answer, kept
 * across close(). */
static int file_on_disk(const char *path)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return 0;
	const int ok = !fsync(fd);
	const int err = errno;
	close(fd);
	errno = err;
	return ok;
}

/* And whether the folder naming it is: that the entry exists at all is the
 * folder's own buffer, hung off the directory inode by vfat_add_entry(). */
static int folder_on_disk(struct run *r)
{
	int dfd = open(r->dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (dfd < 0)
		return 0;
	const int ok = !fsync(dfd);
	const int err = errno;
	close(dfd);
	errno = err;
	return ok;
}

/* Both together are what "the state file says system=ready, and will still say
 * it after a power cut" rests on, and nothing deletes the payload without
 * asking. It is asked after writing that file and again on a later tap that
 * writes no state of its own, which would otherwise be trusting a page cache
 * that no reboot has cleared.
 *
 * vfat keeps no journal, so these two flushes are the whole of the ordering
 * available -- and they are also the whole of what can be asked from here. On
 * the device /mnt/us is fsp, a FUSE filesystem over the vfat at /mnt/base-us,
 * and this kernel's FUSE answers a fsync with success when the server replies
 * ENOSYS (fs/fuse/file.c), so a yes is "nothing refused me", not a proof that
 * the flash holds it. What rests on the answer is in proportion: being wrong
 * costs the reader the 196 MB again, not their Android. A tap that finds no
 * state file and a data image of this release's size keeps that image
 * (read_state, and the check "data image without a state file, release's size:
 * kept"), and only the system image is written afresh. */
static int state_on_disk(struct run *r)
{
	return file_on_disk(r->state_path) && folder_on_disk(r);
}

static int write_state(struct run *r)
{
	char tmp[PATH_MAX], text[512];
	const struct state *s = &r->st;
	size_t at = 0;
	add_line(text, sizeof(text), &at, "schema=1\n");
	if (s->system == IMG_WRITING)
		add_line(text, sizeof(text), &at, "system=writing\n");
	else if (s->system == IMG_READY) {
		add_line(text, sizeof(text), &at, "system=ready %s %s\n", s->version, s->system_sha256);
		if (s->boot_sha256[0])
			add_line(text, sizeof(text), &at, "boot=%s\n", s->boot_sha256);
	}
	if (s->data == IMG_WRITING)
		add_line(text, sizeof(text), &at, "data=writing\n");
	else if (s->data == IMG_READY)
		add_line(text, sizeof(text), &at, "data=ready %u\n", s->data_format);
	if (s->compiling) {
		add_line(text, sizeof(text), &at, "compiling=1\n");
		/* Zero is written too: it is a real answer (the area held no report when
		 * the notice was set), and leaving the line out would be the same file a
		 * version that did not write it at all produces. */
		add_line(text, sizeof(text), &at, "compiling_after=%u\n", s->compiling_after);
	}
	/* Zero included: what its absence says is that this tap had no baseline --
	 * either the file was written before reports were accounted for, or the
	 * area would not answer this time -- and that the count in the area is
	 * therefore not one this side has seen. */
	if (s->android_known)
		add_line(text, sizeof(text), &at, "android=%u\n", s->android_boot);
	if (at >= sizeof(text)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	if (snprintf(tmp, sizeof(tmp), "%s.new", r->state_path) >= (int)sizeof(tmp))
		return -1;
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		return -1;
	int ok = write(fd, text, at) == (ssize_t)at && !fsync(fd);
	ok = !close(fd) && ok;
	if (!ok || rename(tmp, r->state_path)) {
		int err = errno;
		unlink(tmp);
		errno = err;
		return -1;
	}
	/* Whether the rename above reached the disk and not only the page cache.
	 * The payload is deleted on the strength of this file saying system=ready,
	 * and it is the only copy of 196 MB the reader would otherwise fetch
	 * again -- so what cannot be confirmed is not acted on. A failure here is
	 * not made fatal: the state file is almost certainly there, and refusing an
	 * install over a flush would be worse than keeping one payload that a later
	 * tap deletes (ensure()). */
	r->state_synced = state_on_disk(r);
	if (!r->state_synced)
		say(r, "the state is written and could not be flushed: %s", strerror(errno));
	text[at - 1] = 0;	/* logged on one line, after "schema=1\n" */
	for (char *p = text; (p = strchr(p, '\n')); )
		*p = ' ';
	say(r, "state: %s", text + 9);
	return 0;
}

/* ---- unpacking ---- */

static void unpack_progress(void *ctx, enum pw3_unpack_phase phase, uint64_t done, uint64_t total)
{
	struct run *r = ctx;
	enum inst_step step = phase == PW3_UNPACK_VERIFYING ? INST_STEP_VERIFY : r->step;
	show(r, step, done, total);
}

static int system_before_write(void *ctx)
{
	struct run *r = ctx;
	r->st.system = IMG_WRITING;
	return write_state(r);
}

static enum pw3_unpack_result unpack(struct run *r, const char *input, const struct inst_image *in, const char *output,
				     const struct inst_image *out, enum inst_step step, int (*before_write)(void *),
				     struct pw3_unpack_job *job)
{
	memset(job, 0, sizeof(*job));
	job->input = input;
	job->input_bytes = in->bytes;
	memcpy(job->input_sha256, in->sha256, 32);
	job->output = output;
	job->output_bytes = out->bytes;
	memcpy(job->output_sha256, out->sha256, 32);
	job->progress = unpack_progress;
	job->progress_ctx = r;
	job->before_write = before_write;
	r->step = step;
	enum pw3_unpack_result e = pw3_unpack(job);
	say(r, "unpack %s -> %s: %d%s%s%s%s", input, output, (int)e, job->detail ? " " : "", job->detail ? job->detail : "",
	    job->sys_errno ? ": " : "", job->sys_errno ? strerror(job->sys_errno) : "");
	return e;
}

static enum inst_code unpack_failure(struct run *r, enum pw3_unpack_result e, const struct pw3_unpack_job *job,
				     const char *what)
{
	switch (e) {
	case PW3_UNPACK_NO_SPACE:
		r->res->short_bytes = job->space_needed;
		return stop(r, INST_E_NO_SPACE, job->sys_errno, "%s: %s", what, job->detail);
	case PW3_UNPACK_PAYLOAD_INVALID:
		return stop(r, INST_E_RELEASE, 0, "%s: %s", what, job->detail);
	case PW3_UNPACK_NO_MEMORY:
		return stop(r, INST_E_MEMORY, 0, "%s: %s", what, job->detail);
	default:
		return stop(r, INST_E_IO, job->sys_errno, "%s: %s", what, job->detail ? job->detail : "failed");
	}
}

/* ---- the system payload ---- */

static void download_progress(void *ctx, uint64_t bytes)
{
	struct run *r = ctx;
	show(r, INST_STEP_DOWNLOAD, bytes, r->rel->payload.bytes);
}

static int write_all(const char *p, const unsigned char *data, size_t bytes)
{
	int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0)
		return -1;
	size_t done = 0;
	while (done < bytes) {
		ssize_t n = write(fd, data + done, bytes - done);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			int err = n < 0 ? errno : EIO;
			close(fd);
			errno = err;
			return -1;
		}
		done += (size_t)n;
	}
	return close(fd);
}

/* Fetches the payload to download/<name>; the part is kept when it fails. */
static enum inst_code download(struct run *r)
{
	const struct inst_release *rel = r->rel;
	if (!r->ops->online(r->ops->ctx))
		return stop(r, INST_E_OFFLINE, 0, "Wi-Fi is not connected");
	int64_t now = r->ops->now(r->ops->ctx);
	if (now < rel->built - CLOCK_SKEW_SECONDS)
		return stop(r, INST_E_CLOCK, 0, "the clock (%lld) is before the release (%lld)", (long long)now,
			    (long long)rel->built);
	if (write_all(r->ca_path, rel->ca_bundle, rel->ca_bundle_bytes))
		return stop(r, errno == ENOSPC ? INST_E_MEMORY : INST_E_IO, errno, "cannot write the CA bundle");
	/* One origin per attempt, in the order the entry lists them, and the one
	 * that last delivered at a usable rate is kept: an origin that cannot be
	 * reached, or that answers under the floor, costs one attempt and is left
	 * behind, while a working one is not swapped out, because every swap pays
	 * for another TLS handshake on a 1 GHz single core.
	 *
	 * Both budgets are per origin. `refused` counts the origins that have
	 * answered "this release is not here" since the last byte arrived; when
	 * every origin has said it, it is the release that is missing and not the
	 * route.
	 */
	const int origins = rel->payload_origins > 0 ? rel->payload_origins : 1;
	int origin = 0, idle = 0, refused = 0;
	for (int attempt = 1; attempt <= DOWNLOAD_ATTEMPTS * origins; attempt++) {
		int64_t before = file_size(r->part_path);
		/* A store that cannot say the length, at any of the three places this
		 * loop asks. Folded in with "no part" it would start curl on a file
		 * nothing here can read, and whatever curl then made of it would be
		 * reported as a download that would not finish -- the reader sent to
		 * look at Wi-Fi over a Kindle that cannot be read. It is the same
		 * mistake the end of this function made until a Codex review found it
		 * on 2026-10-30; file_size() keeps the two apart so that it can be
		 * asked, so it is asked everywhere and not only there. errno is the
		 * lstat behind it, and nothing is called in between. */
		if (before == FILE_UNREADABLE)
			return stop(r, INST_E_IO, errno, "cannot read the size of the part");
		if (before > (int64_t)rel->payload.bytes) {
			say(r, "part is larger than the payload (%lld); starting over", (long long)before);
			if (remove_file(r, r->part_path))
				return stop(r, INST_E_IO, errno, "cannot delete the part");
			before = -1;
		}
		if (before == (int64_t)rel->payload.bytes)
			break;
		show(r, INST_STEP_DOWNLOAD, before > 0 ? (uint64_t)before : 0, rel->payload.bytes);
		int http = 0;
		char why[160] = "";
		const char *url = rel->payload_urls[origin];
		int status = r->ops->download(r->ops->ctx, url, r->part_path, r->ca_path, rel->payload.bytes,
					      download_progress, r, &http);
		int64_t after = file_size(r->part_path);
		/* Kept across the log line below, which clobbers errno. The attempt is
		 * logged first because an attempt that happened is worth saying even
		 * when what stops the install is the answer to this question rather
		 * than anything curl did. */
		const int after_why = after == FILE_UNREADABLE ? errno : 0;
		say(r, "download attempt %d: origin %d of %d, curl %d, HTTP %d, part %lld -> %lld bytes", attempt,
		    origin + 1, origins, status, http, (long long)before, (long long)after);
		/* Before any of the classification below, all of which would read this
		 * as a length: the test for status 0 would call it "the server's file
		 * is -2 bytes" and blame the release for a store that cannot be read. */
		if (after == FILE_UNREADABLE)
			return stop(r, INST_E_IO, after_why, "cannot read the size of the part after this attempt");
		if (status == 0 && after == (int64_t)rel->payload.bytes)
			break;
		/* Before the three below, which are one chain: CURLE_PEER_FAILED_VERIFICATION
		 * happens before any HTTP answer, so none of those can fire on the same
		 * attempt and the order between them does not matter.
		 *
		 * The check further up only catches a clock
		 * before the release; a clock far enough ahead meets an expired
		 * certificate instead, and that fails here. Retrying cannot help, and
		 * E201 would send the user to look at Wi-Fi that is working. The
		 * certificate is still verified: this says which failure it was. */
		if (status == 60)
			return stop(r, INST_E_CLOCK, 0, "the certificate did not verify (curl 60); the clock reads %lld",
				    (long long)now);
		/* 416: the range asked for starts at or past the end of the server's
		 * object (RFC 9110 -- equal counts, and the website session measured
		 * its own route answering 416 at exactly equal, 2026-10-30). The two
		 * checks at the top of this attempt leave the part shorter than the
		 * manifest says, so the range asked for began inside the file the
		 * manifest describes: a 416 means the object on the server is shorter
		 * still, and it is not that file. Deleting the part and asking again
		 * cannot change that -- the next attempt downloads the short object
		 * whole and lands on the same answer one attempt later -- so this says
		 * it here. Keyed on the answer rather than the exit code because curl
		 * has no settled one: with --continue-at it reports a 416 as success
		 * on some versions, since "the range is unsatisfiable" and "you have
		 * the whole file" are the same sentence to it, and --fail does not
		 * change that. The check below for status 0 would otherwise take this
		 * and name the part's own length as the server's. */
		if (http == 416)
			snprintf(why, sizeof(why),
				 "the file there is shorter than the manifest's %llu bytes (HTTP 416 at %lld)",
				 (unsigned long long)rel->payload.bytes, (long long)after);
		else if (status == 0 || status == 63 || after > (int64_t)rel->payload.bytes)
			snprintf(why, sizeof(why), "the file there is %s%lld bytes, the manifest's %llu",
				 status == 63 ? "longer than " : "", (long long)after,
				 (unsigned long long)rel->payload.bytes);
		/* The server's answer decides this, not curl's exit code. --fail gives
		 * 22 on HTTP/1.1 and 56 on HTTP/2, where aborting the stream is a
		 * receive failure, and the release address is now this project's own
		 * (docs/design/release-build.md, "下载线路"), which negotiates HTTP/2:
		 * measured 2026-10-30 against the real address with the entry's own CA
		 * bundle -- --http1.1 gives 22, --http2 gives 56, same 404. Keyed on
		 * the exit code, a missing release file would spend six attempts and
		 * then send the reader to look at Wi-Fi that is working, which is the
		 * one thing this screen exists to avoid. http is 0 unless the server
		 * answered, and --fail writes no body on a 4xx, so the code alone is
		 * the whole of the question. */
		if (!*why && (http == 404 || http == 410))
			snprintf(why, sizeof(why), "the release file is not there (HTTP %d)", http);
		/* An answer about the release rather than about the route: this origin
		 * does not have the file the manifest describes. It takes the origin out
		 * of the running, not the release, until every origin has said the same
		 * since the last byte arrived -- otherwise one stale mirror refuses an
		 * install the other origins can serve. That is not hypothetical: on
		 * 2026-10-31 the Worker's GitHub fallback still held the previous
		 * release's payload, 497 bytes shorter, while R2 held the right one. */
		if (*why) {
			if (++refused >= origins)
				return stop(r, INST_E_RELEASE, 0, "%s", why);
			say(r, "origin %d of %d: %s; asking the next one", origin + 1, origins, why);
			origin = (origin + 1) % origins;
			continue;
		}
		if (status == 23)
			return stop(r, INST_E_IO, 0, "curl could not write the part");
		/* No resuming: start over. curl says so with 33 or 36. */
		if (status == 33 || status == 36) {
			if (remove_file(r, r->part_path))
				return stop(r, INST_E_IO, errno, "cannot delete the part");
			continue;
		}
		/* Progress is more bytes than there were, and "there were none" is one
		 * number: a part that was absent (-1) and is now an empty file (0) is
		 * not a byte received, though -1 < 0 said it was, which gave the first
		 * attempt one retry the idle count was meant to deny it (a Codex
		 * review, 2026-10-30). Neither value can be FILE_UNREADABLE here; that
		 * is answered above.
		 *
		 * Bytes at a rate that cannot finish are not progress either. On
		 * 2026-10-31 this project's own device spent 26 minutes on the one
		 * origin the entry then carried: four attempts in a row came back
		 * CURL_TOO_SLOW having moved a few hundred KB of 191 MB, every one of
		 * them "more bytes than there were", so the origin was kept and the
		 * rate was never the question. At that rate the payload needs more than
		 * two days and the screen's own advice is to use a computer, so a
		 * minute under the floor is an answer about this route: the next origin
		 * is asked, and it counts as idle as well, so a single origin stops
		 * after two such attempts instead of six. The part is kept either way
		 * and tapping again continues it. */
		const int moved = after > (before > 0 ? before : 0);
		/* `refused` is reset by bytes arriving whatever their rate, because what
		 * it counts is origins answering "this release is not here" and bytes
		 * are an origin answering the opposite. Folded into the rule below it
		 * would report E303, the release missing, for a release one origin was
		 * serving slowly while another answered 404 twice. */
		if (moved)
			refused = 0;
		if (moved && status != CURL_TOO_SLOW) {
			idle = 0;
		} else {
			idle++;
			/* Nothing usable arrived from this one. Ask the next, so that an
			 * origin that cannot be reached, or that cannot deliver, costs one
			 * attempt instead of all of them. */
			origin = (origin + 1) % origins;
		}
		if (idle >= DOWNLOAD_IDLE_ATTEMPTS * origins || attempt == DOWNLOAD_ATTEMPTS * origins)
			return stop(r, INST_E_DOWNLOAD, 0, "curl exit %d (HTTP %d) after %d attempts", status, http, attempt);
		r->ops->wait(r->ops->ctx, 5);
	}
	/* Running out of attempts is not success. The two breaks above are the only
	 * way the part is whole, and every other way out of the loop returns. The
	 * exception was 33/36 -- no resuming, so the part is deleted and the next
	 * attempt starts over -- on the *last* attempt: the loop simply ended, and
	 * the rename below then failed on the file that had just been deleted,
	 * reporting a storage fault (E402) for what is a network one, and sending
	 * the reader to look at a Kindle that is fine (a Codex review, 2026-10-30).
	 * The length is what the rename needs to be true anyway, so it is asked
	 * here rather than inferred from how the loop ended.
	 *
	 * A store that cannot say the length is a third answer, and it is the one
	 * this check first got wrong: folded into "not the manifest's length" it
	 * reported a download failure for a Kindle that could not be read, sending
	 * the reader to look at Wi-Fi over a storage fault (a Codex review,
	 * 2026-10-30). file_size() keeps the two apart for exactly this reason. */
	int64_t whole = file_size(r->part_path);
	if (whole == FILE_UNREADABLE)
		return stop(r, INST_E_IO, errno, "cannot read the size of the downloaded part");
	if (whole != (int64_t)rel->payload.bytes)
		return stop(r, INST_E_DOWNLOAD, 0, "the download did not finish in %d attempts",
			    DOWNLOAD_ATTEMPTS * origins);
	if (rename(r->part_path, r->download_payload))
		return stop(r, INST_E_IO, errno, "cannot rename the downloaded payload");
	show(r, INST_STEP_DOWNLOAD, rel->payload.bytes, rel->payload.bytes);
	return INST_READY;
}

/* Deletes what is left of payloads in download/ and this release's payload in documents/. */
/* Everything this installer put on the user store that is not one of the two
 * images: the payload, wherever it came from, and whatever a download left in
 * the folder of its own -- a part among them. The whole of that folder goes,
 * because prepare_paths() is what makes it and download() is the only thing
 * that writes there, so nothing in it is anyone else's; documents/ is the
 * reader's own folder, and there only this release's payload is touched, by
 * its exact name.
 *
 * Returns how many files it deleted, not how many it found: a caller that
 * cleans up on the off chance then says so only when there was something to
 * clean, and never says a file went when it did not -- remove_file() has
 * already said which one and why. Only regular files are offered to it. A
 * directory would be an unlink that always fails, and nothing here makes one
 * or is asked to take one away. */
static int clean_payloads(struct run *r)
{
	int gone = 0;
	DIR *d = opendir(r->download_dir);
	if (d) {
		struct dirent *e;
		char p[PATH_MAX];
		struct stat st;
		while ((e = readdir(d)))
			if (e->d_name[0] != '.' && !path(p, r->download_dir, e->d_name) &&
			    !lstat(p, &st) && S_ISREG(st.st_mode) && !remove_file(r, p))
				gone++;
		closedir(d);
	}
	/* Not "> 0": file_size() tells a regular file of no bytes (0) apart from
	 * one that is not there and one that is not a regular file (both
	 * negative), and an empty payload someone left behind is a file this
	 * deleted. */
	const int had_payload = file_size(r->documents_payload) >= 0;
	if (!remove_file(r, r->documents_payload) && had_payload)
		gone++;
	return gone;
}

/* ---- space ---- */

static uint64_t clusters(uint64_t bytes, uint64_t unit)
{
	return (bytes + unit - 1) / unit * unit;
}

/* Bytes already on the store at p, rounded up to whole clusters: room a write to
 * that name does not have to find. A store that cannot say is not an absence,
 * and this used to read it as one -- the credit was lost, the room asked for
 * grew by whatever that file holds, and the reader was told to delete books
 * over a file that is on the drive and cannot be read (a Codex review,
 * 2026-10-30). It cannot be answered here, because the answer is a number, so
 * it is passed out through *unreadable for the caller to stop on. */
static uint64_t existing(const char *p, uint64_t unit, int *unreadable)
{
	int64_t n = file_size(p);
	if (n == FILE_UNREADABLE) {
		if (!*unreadable)
			*unreadable = errno ? errno : EIO;
		return 0;
	}
	return n > 0 ? clusters((uint64_t)n, unit) : 0;
}

/* The order is: download, system image, payload deleted, data image. The most the
 * user store must hold at once is either the payload and system image, or (after the
 * payload is gone) the system and data images. */
static enum inst_code check_space(struct run *r, int need_system, const char *payload, int need_data)
{
	struct statvfs vfs;
	if (statvfs(r->dir, &vfs))
		return stop(r, INST_E_IO, errno, "cannot read the free space");
	uint64_t unit = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
	uint64_t free_bytes = (uint64_t)vfs.f_bavail * unit;
	uint64_t s = 0, p = 0, p_freed = 0, d = 0;
	int unreadable = 0;
	if (need_system) {
		uint64_t want = clusters(r->rel->system.bytes, unit), have = existing(r->system_path, unit, &unreadable);
		s = want > have ? want - have : 0;
		uint64_t full = clusters(r->rel->payload.bytes, unit);
		p_freed = full;
		if (!payload) {
			uint64_t part = existing(r->part_path, unit, &unreadable);
			p = full > part ? full - part : 0;
		}
	}
	if (need_data) {
		uint64_t want = clusters(r->rel->data.bytes, unit), have = existing(r->data_path, unit, &unreadable);
		d = want > have ? want - have : 0;
	}
	/* Before the arithmetic is believed: a credit that could not be read makes
	 * every number below too large, and the answer E401 gives is "delete books". */
	if (unreadable)
		return stop(r, INST_E_IO, unreadable,
			    "cannot read the size of a file already on the store, so the room it saves cannot be counted");
	uint64_t first = p + s, second = p + s + d > p_freed ? p + s + d - p_freed : 0;
	uint64_t need = (first > second ? first : second) + INST_RESERVE_BYTES;
	say(r, "space: free %llu, payload %llu, system %llu, data %llu, freed %llu, need %llu", (unsigned long long)free_bytes,
	    (unsigned long long)p, (unsigned long long)s, (unsigned long long)d, (unsigned long long)p_freed,
	    (unsigned long long)need);
	if (need > free_bytes) {
		r->res->short_bytes = need - free_bytes;
		return stop(r, INST_E_NO_SPACE, 0, "%llu more bytes are needed", (unsigned long long)(need - free_bytes));
	}
	return INST_READY;
}

/* ---- the steps ---- */

static enum inst_code prepare_paths(struct run *r)
{
	const char *u = r->paths->user;
	char documents[PATH_MAX];
	if (path(r->dir, u, "android") || path(r->state_path, r->dir, "state") || path(r->system_path, r->dir, "system.img") ||
	    path(r->data_path, r->dir, "data.img") || path(r->download_dir, r->dir, "download") ||
	    path(documents, u, "documents") || path(r->documents_payload, documents, r->rel->payload_name) ||
	    path(r->download_payload, r->download_dir, r->rel->payload_name) ||
	    snprintf(r->part_path, PATH_MAX, "%s.part", r->download_payload) >= PATH_MAX ||
	    path(r->ca_path, r->paths->work, "ca-bundle.pem") || path(r->template_path, r->paths->work, "data-template.zst") ||
	    path(r->report_path, r->dir, PW3_REPORT_NAME))
		return stop(r, INST_E_IO, ENAMETOOLONG, "path too long");
	if ((mkdir(r->dir, 0755) && errno != EEXIST) || (mkdir(r->download_dir, 0755) && errno != EEXIST))
		return stop(r, INST_E_IO, errno, "cannot make the android folder");
	return INST_READY;
}

/* What Android left in the report area for this side to read: whether there is
 * a report, which Android start wrote it, and the reason PID 1 refused to start
 * when that is what the report is (PW3_REPORT_STOP). The body holds a great
 * deal more, and all of it is for people -- the launcher has already copied it
 * into the folder a reader can hand on (helper.c) before this runs.
 *
 * Nothing here fails a tap. An area that is absent, unreadable, zeroed or not a
 * report is the ordinary state of a device that has never started Android, and
 * on such a device this must come to the same thing it came to before the area
 * existed: no report, nothing learnt.
 */
/* The line of the report that names the build that wrote it. Not in
 * ReportArea.h with PW3_REPORT_STOP and PW3_REPORT_BOOT_COMPLETED, although
 * that is where the names the two sides must spell alike belong, because this
 * one is not a name this project chose: it is an Android property, and its
 * writer is the list of property names in BootReport.cpp, not a constant. The
 * header is also a device source, and the system image is rebuilt whenever one
 * changes -- a 194 MB download for everybody, to add a line of text that the
 * Android side would not read. What holds the two spellings together instead is
 * a check in test_install.py over both files. */
#define PW3_REPORT_BUILD "ro.build.version.incremental="

struct report {
	/* The area answered: it is not there, or it is and this is what it holds.
	 * Told apart from "the store would not read it out", because the second
	 * leaves this side without a baseline rather than with an empty one, and
	 * claiming a baseline it does not have is how a report about the system
	 * before this one gets applied to the system after it. */
	int readable;
	int found;
	uint32_t boot;
	char stopped[192];	/* empty unless PID 1 refused to start */
	int completed;		/* the start reached sys.boot_completed=1 */
	int made;		/* the area this installer wrote, with no start in it yet */
	/* Which build wrote it: ro.build.version.incremental, as the sampler read
	 * it from the running system (PW3_REPORT_BUILD). This is the whole answer
	 * to the only question the compiling notice asks -- did *the system this
	 * entry installs* finish compiling -- and it was in the report from the
	 * first version of it, put there on purpose: "a report reaches whoever
	 * reads it without the boot it came from, and every other line means
	 * something else depending on the answer" (BootReport.cpp, Properties).
	 *
	 * Empty when the report carries no such line. A stop report is the usual
	 * case: PID 1 writes those before there is a property service to ask, so a
	 * refusal is told from an already-answered refusal by its count, not by
	 * this. */
	char build[48];
};

static void read_report(struct run *r, struct report *out)
{
	static unsigned char area[PW3_REPORT_BYTES];
	struct pw3_report parsed;
	size_t got = 0;
	int why = 0;
	memset(out, 0, sizeof(*out));
	const int fd = open(r->report_path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0) {
		/* Not there is an answer: no Android start has written one, which is
		 * where every device begins. Anything else is the store declining to
		 * say, and this side then has no baseline at all. */
		if (errno == ENOENT)
			out->readable = 1;
		else
			say(r, "report area cannot be read: %s", strerror(errno));
		return;
	}
	while (got < sizeof(area)) {
		const ssize_t count = read(fd, area + got, sizeof(area) - got);
		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0) {
			why = errno;
			break;
		}
		if (count == 0)
			break;
		got += (size_t)count;
	}
	close(fd);
	if (why) {
		say(r, "report area unreadable after %llu bytes: %s", (unsigned long long)got, strerror(why));
		return;
	}
	out->readable = 1;
	if (!pw3_report_parse(area, got, &parsed))
		/* Read through and not a report: an area of zeros that no start has
		 * written, or one cut off mid-write. That is an answer too, and it is
		 * the answer this side had before the area existed. */
		return;
	if (pw3_report_is_made(&parsed)) {
		/* The area this installer made, saying so in its body, and no start has
		 * written over it yet. Not a report from Android, so nothing to act on
		 * -- but its count matters: it is the count this installer planted, and
		 * the next start's report is that count plus one.
		 *
		 * Known by the body and not by the count. The count is in the first
		 * line, which the body's checksum does not cover, so one bit turned
		 * over in a start's `boot=1` used to make its report a placeholder to
		 * this reader. */
		say(r, "the report area is there and no Android start has written to it");
		out->made = 1;
		out->boot = parsed.boot;
		return;
	}
	out->found = 1;
	out->boot = parsed.boot;
	pw3_report_field(parsed.text, parsed.bytes, PW3_REPORT_STOP, out->stopped, sizeof(out->stopped));
	/* A report is not by itself a completed start: the sampler is started on
	 * the earliest property that means the storage is up, on purpose, because
	 * a start that never completes is the start whose report is worth having
	 * (pw3-boot-report.rc). It samples for two minutes and writes what it has.
	 * So an upgrade whose compiling takes longer than that writes a report
	 * while it is still compiling, and reading "there is a new report" as
	 * "compiling is done" would clear the notice in the middle of the work --
	 * which is the mistake the tap counter made and this replaced. */
	char value[16];
	out->completed = pw3_report_field(parsed.text, parsed.bytes, PW3_REPORT_BOOT_COMPLETED,
					  value, sizeof(value)) && !strcmp(value, "1");
	pw3_report_field(parsed.text, parsed.bytes, PW3_REPORT_BUILD, out->build, sizeof(out->build));
}

/* The report area (device/amazon/pw3/report/ReportArea.h): 64 KiB that Android
 * writes by sector, so its size and its clusters must already be there and must
 * never change. Made or repaired on every tap, because an installation from
 * before it existed has to gain one, and because a file somebody deleted or
 * truncated from a computer must not leave Android writing into clusters that
 * are no longer its own -- the first kernel maps it by its extents, and a short
 * file would map short.
 *
 * It is a diagnostic, and it gives way to everything: nothing here fails an
 * installation, nothing here overwrites a file that is not ours, and nothing
 * here spends the last of the user store (report_area_is_ours,
 * REPORT_AREA_FREE_MIN). A tap that does not make it says so in the log and
 * goes on, and Android then finds no area and writes no report.
 */
/* Whether the open file is a report area of the wrong size rather than somebody's
 * file that happens to be called that. Takes the descriptor the caller is about
 * to write through, not a name, so that the file asked about is the file written
 * over. Two things are ours and nothing else is:
 *
 *   - it begins with the magic: Android wrote a report, and a computer then
 *     truncated or extended the file;
 *   - it is no longer than an area and every byte of it is zero, which includes
 *     a file of no bytes at all: made and never written, then truncated.
 *
 * Every byte, not the first few: a file whose first bytes are zeros and whose
 * rest is somebody's data is somebody's data. Reading it costs at most 64 KiB,
 * because anything longer than an area cannot be one that was never written.
 * Anything else is left exactly as it is -- `android/` is this installer's
 * folder, but that is a reason to own the five names it writes, not a reason to
 * overwrite a sixth file because the name matches.
 *
 * A file that cannot be read is not ours either: not knowing is the case this
 * exists for. */
/* Through pw3_report_claim_*, not with a second copy of the rule: the header
 * says the two sides have to apply one rule, and this file used to answer it
 * again in its own words. The two then drifted -- a file cut off inside the
 * magic became ours in the header and stayed somebody else's here, which is
 * the report channel dead for good on a device whose area was truncated. */
static int report_area_is_ours(int fd)
{
	struct pw3_report_claim claim;
	unsigned char chunk[4096];
	pw3_report_claim_begin(&claim);
	for (;;) {
		ssize_t n = read(fd, chunk, sizeof(chunk));
		if (n < 0 && errno == EINTR)
			continue;
		/* Could not read it. This is the one branch here with no test
		 * behind it -- no file on the machine the installer's tests run
		 * on can be made to fail a read, and the library they preload to
		 * fail an lstat cannot reach read, whose name the address
		 * sanitizer already owns. */
		if (n < 0)
			return 0;
		if (n == 0)
			break;
		pw3_report_claim_feed(&claim, chunk, (size_t)n);
		/* Longer than an area, so not one that was made and never written;
		 * and a file this long is read no further. */
		if (claim.seen > PW3_REPORT_BYTES)
			return 0;
		/* The whole magic is the only answer that does not need the rest of
		 * the file. A prefix of it is an answer about a file *shorter* than
		 * the magic, and how long a file is is only known at the end: a read
		 * that came back short of a longer file would otherwise have its
		 * first byte taken for one. `blank` is excluded because an area of
		 * zeros is likewise only known at the end. */
		if (!claim.blank && claim.seen >= PW3_REPORT_MAGIC_LEN &&
		    pw3_report_claim_ours(&claim))
			return 1;
	}
	return pw3_report_claim_ours(&claim);
}

/* Free space the store must still have after the area is there. It is not the
 * installer's 64 MiB reserve: this runs on the path that has nothing to install
 * and so never checked the space at all, and a store with 64 MiB free is not a
 * store that needs the area withheld. What it has to prevent is the area taking
 * the last of the room and the start then failing on a file it cannot do
 * without -- this run's own log, which is written after this and whose writes
 * are required to succeed. A megabyte is far more than those files need. */
#define REPORT_AREA_FREE_MIN (1ull << 20)

/* `reclaim` is what the file being replaced gives back, which only the caller
 * holding that file open can say. */
static int report_area_has_room(struct run *r, uint64_t reclaim)
{
	struct statvfs vfs;
	if (statvfs(r->dir, &vfs)) {
		say(r, "report area: cannot read the free space: %s", strerror(errno));
		return 0;
	}
	const uint64_t unit = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
	const uint64_t free_bytes = (uint64_t)vfs.f_bavail * unit + reclaim;
	if (free_bytes < (uint64_t)PW3_REPORT_BYTES + REPORT_AREA_FREE_MIN) {
		say(r, "report area: %llu bytes free, not made", (unsigned long long)free_bytes);
		return 0;
	}
	return 1;
}

/* The count this installer plants in a new area, and so the floor for every
 * report written after it (pw3_report_build_made). The clock, not a random
 * number: what has to be true is that a report from *before* this install
 * carries a smaller count, and the clock is the one number on this device that
 * only goes forward. Two random plants would be out of order half the time.
 *
 * Never below the release's own build time, which the installer already refuses
 * to work below (INST_E_CLOCK), and never above 2^31 so that the counts after
 * it cannot reach the wrap at 2^32 in any life of the device.
 */
static uint32_t plant_count(struct run *r)
{
	const int64_t now = r->ops->now(r->ops->ctx);
	int64_t pick = now > r->rel->built ? now : r->rel->built;
	if (pick < 1)
		pick = 1;
	if (pick > 0x7fffffff)
		pick = 0x7fffffff;
	return (uint32_t)pick;
}

/* `plant` is the count the placeholder carries, and so the count the next start
 * writes, plus one (pw3_report_build_made). Returns 1 when it wrote one, so that
 * the caller can record that count: the report this side has acted on is no
 * longer anywhere, and the start that follows writes `plant + 1` -- which, left
 * unrecorded against a stale `android=1`, is the number a first refusal usually
 * carries and would be passed over for one already acted on.
 *
 * Reported back for that reason and no other. It is not the watermark of the
 * seventh pass returning: nothing about the compiling notice is decided here. */
static int ensure_report_area(struct run *r, uint32_t plant, int force)
{
	const int64_t have = file_size(r->report_path);
	if (have == FILE_UNREADABLE) {
		say(r, "report area: the store cannot say what is at that name: %s", strerror(errno));
		return 0;
	}
	/* One descriptor for the rest of it, and everything decided through that
	 * descriptor: what is asked about and what is written over then cannot be
	 * two different files. A name that was free a moment ago is created
	 * exclusively, so a file that appeared since is not this program's to
	 * truncate -- and the room is asked for before that create, because a
	 * store with none should not even gain a directory entry. A name that was
	 * taken is opened without O_CREAT and without O_TRUNC, which creates and
	 * destroys nothing, so there the room is asked for afterwards, with the
	 * size of the file in hand: how much comes back is a fact about that
	 * file, not about what a name pointed at earlier. Neither open follows a
	 * symbolic link: what is at the end of one would be someone else's file
	 * even when the link is at a name of ours. */
	int fd;
	if (have < 0) {
		if (!report_area_has_room(r, 0))
			return 0;
		fd = open(r->report_path, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
	} else {
		fd = open(r->report_path, O_RDWR | O_NOFOLLOW | O_CLOEXEC);
	}
	if (fd < 0) {
		say(r, "report area: cannot make it: %s", strerror(errno));
		return 0;
	}
	struct stat st;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode)) {
		say(r, "report area: cannot say what it is: %s", strerror(errno));
		close(fd);
		return 0;
	}
	/* A file that is already the area's size still has to be looked at: being
	 * that size is not being a usable area. PID 1 maps only one that begins
	 * with the magic or is all zeros (pw3_report_claim_ours in TestFileInit.h),
	 * so a report that lost one of the thirteen bytes of its magic -- the same
	 * kind of accident the system image is guarded against -- is a file this
	 * side used to keep because the size was right and PID 1 refuses. That is
	 * the channel dead for good: no report is ever written again, so the
	 * compiling notice can never clear and a damaged system image can never be
	 * found.
	 *
	 * Such a file is known from the file itself and not from a record that this
	 * installer made one: its first line still parses and its body still
	 * checksums to what that line declares (pw3_report_damaged_magic), which
	 * somebody else's file would have to carry a parseable header and a CRC32
	 * over exactly the length it names to imitate. A record of the path would
	 * have said yes to whatever was put at that name afterwards.
	 *
	 * `force` changes one answer here and no other: "this one is usable, leave
	 * it alone" becomes "write a new one anyway", because the caller needs a
	 * count planted in it. Whose file it is is asked the same way either way --
	 * a file at that name that is not ours is not this program's to write over,
	 * however much it would like to plant a count in it. Keeping the two
	 * questions in one place is the point: they were separate for an hour, and
	 * the forced path then called a report of ours with a damaged magic
	 * somebody else's file, which is the one file that most needs replacing. */
	if (st.st_size == (off_t)PW3_REPORT_BYTES) {
		static unsigned char area[PW3_REPORT_BYTES];
		struct pw3_report_claim claim;
		size_t got = 0;
		int why = 0;
		while (got < sizeof(area)) {
			const ssize_t n = read(fd, area + got, sizeof(area) - got);
			if (n < 0 && errno == EINTR)
				continue;
			if (n < 0) { why = errno; break; }
			if (n == 0)
				break;
			got += (size_t)n;
		}
		if (why) {
			/* Could not read it, which is not the same as "it is not a
			 * report": a store that failed one read may hold a perfectly
			 * good area, and emptying it would throw away the diagnostic
			 * and the count over a fault that may not even repeat. Left as
			 * it is, like a file that would not open. */
			say(r, "report area: %u bytes at that name and unreadable after %llu: %s",
			    PW3_REPORT_BYTES, (unsigned long long)got, strerror(why));
			close(fd);
			return 0;
		}
		/* The question is PID 1's: will it map this file (pw3_report_claim_ours
		 * -- the magic at the front, or nothing but zeros)? Three answers, each
		 * with its own reason:
		 *
		 *   the magic is there, so the channel is alive whatever the rest of
		 *     the file says -- a report cut off mid-write has a checksum that
		 *     no reader believes, and the next start writes over it;
		 *   nothing but zeros is an area made by an installer from before they
		 *     said what they were, and it is made again so that a byte turned
		 *     over in it stays recoverable;
		 *   otherwise PID 1 will not map it, and then either it is a report of
		 *     ours whose magic alone was damaged -- made again, because nothing
		 *     else would ever fix it -- or it is somebody's file at that name,
		 *     and is left exactly as it is.
		 */
		pw3_report_claim_begin(&claim);
		pw3_report_claim_feed(&claim, area, got);
		if (pw3_report_claim_ours(&claim) && !claim.blank) {
			if (!force) {
				close(fd);
				return 0;
			}
			say(r, "report area: %u bytes, ours, written again to plant the start count",
			    PW3_REPORT_BYTES);
		} else if (claim.blank)
			say(r, "report area: %u bytes of zeros, from an installer that wrote those; made again",
			    PW3_REPORT_BYTES);
		else if (pw3_report_damaged_magic(area, got))
			say(r, "report area: a report whose magic is damaged; made again so that it can be written");
		else {
			say(r, "report area: %u bytes at that name and not ours, left alone", PW3_REPORT_BYTES);
			close(fd);
			return 0;
		}
		if (lseek(fd, 0, SEEK_SET) != 0) {
			say(r, "report area: cannot rewind it: %s", strerror(errno));
			close(fd);
			return 0;
		}
	} else if (st.st_size && !report_area_is_ours(fd)) {
		say(r, "report area: %lld bytes at that name and not ours, left alone", (long long)st.st_size);
		close(fd);
		return 0;
	}
	/* Asked for whatever this file is, an empty one included: a name that is
	 * already taken still costs the area's own bytes to fill. */
	if (!report_area_has_room(r, (uint64_t)st.st_size)) {
		close(fd);
		return 0;
	}
	if (ftruncate(fd, 0) || lseek(fd, 0, SEEK_SET) != 0) {
		say(r, "report area: cannot empty it: %s", strerror(errno));
		close(fd);
		return 0;
	}
	/* A report saying the area was made and no start has written to it, rather
	 * than 64 KiB of zeros. Zeros say nothing about whose file they are, so one
	 * byte turned over in them left a file this side would not touch and PID 1
	 * would not map: the channel dead for good before Android had written to it
	 * once (PW3_REPORT_MADE). A report of ours is recognisable even with a byte
	 * of its magic lost, which is the one place a byte can be lost and matter. */
	static unsigned char made[PW3_REPORT_BYTES];
	int ok = pw3_report_build_made(made, sizeof(made), plant);
	for (size_t written = 0; written < sizeof(made) && ok; ) {
		const ssize_t n = write(fd, made + written, sizeof(made) - written);
		if (n < 0 && errno == EINTR)
			continue;
		ok = n > 0;
		if (ok)
			written += (size_t)n;
	}
	ok = ok && !fsync(fd);
	if (!ok) {
		/* Emptied rather than deleted: a name is not a file, and the file
		 * this program has in its hands is the one that must not be left
		 * holding clusters. What stays is a file of no bytes, which the
		 * next tap sees as an area to make -- the same place a removed one
		 * would have left it. */
		say(r, "report area: cannot fill it: %s", strerror(errno));
		if (ftruncate(fd, 0))
			say(r, "report area: and cannot empty it: %s", strerror(errno));
		close(fd);
		return 0;
	}
	if (close(fd)) {
		say(r, "report area: cannot close it: %s", strerror(errno));
		return 0;
	}
	int dfd = open(r->dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (dfd >= 0) { fsync(dfd); close(dfd); }
	say(r, "report area: %u bytes, %s, start count planted at %u", PW3_REPORT_BYTES,
	    have < 0 ? "made" : "remade", plant);
	return 1;
}

/* Whether writing the images would write into another file as well, asked
 * before anything is written and of the filesystem that is mounted now.
 *
 * The check this calls is the one the start uses on the images it maps
 * (device/amazon/pw3/preinit/FatCrossLink.h). Until this, it ran only there --
 * after the handover, on the other side of the write. It would refuse to start
 * a system whose image shared clusters with a book, which is the right answer
 * and far too late: the installer had already written those clusters, and what
 * was in the book was gone. The answer has to be had by the program that does
 * the writing, before it writes, from the filesystem state it is about to write
 * into. That is all this moves.
 *
 * The filesystem it asks is not the one these paths are on: on the device the
 * store is a FUSE proxy over the vfat that holds the bytes, and fatcheck.h says
 * how the one is named from the other. What is passed from here is what this
 * program knows -- the store it was given and the paths it is about to write. */
static enum inst_code check_cross_links(struct run *r, int need_system, int need_data)
{
	const char *system_path = NULL, *data_path = NULL;
	int64_t system_bytes = 0, data_bytes = 0;
	struct inst_fat_note note;
	enum inst_fat answer;

	/* Every image that is already there, at the length it is there with -- not
	 * at the length this release gives it. A mapping is a fact about allocated
	 * clusters, and those are the clusters the write goes through: pw3_unpack
	 * opens the image as it is and writes it from the first byte, so one left
	 * shorter by an earlier install or by a repair tool is written through its
	 * own chain just the same. Asking only about files that are already the
	 * release's length would have skipped exactly those.
	 *
	 * A file that is not there at all is skipped, and that is the boundary
	 * fatcheck.h states: its clusters are chosen out of the free map by the
	 * write itself, so there is nothing to ask about yet. */
	if (need_system && (system_bytes = file_size(r->system_path)) > 0)
		system_path = r->system_path;
	if (need_data && (data_bytes = file_size(r->data_path)) > 0)
		data_path = r->data_path;
	if (!system_path && !data_path)
		return INST_READY;

	answer = inst_fat_exclusive(r->paths->user, system_path, (uint64_t)system_bytes,
				    data_path, (uint64_t)data_bytes, &note);

	if (answer == INST_FAT_SHARED)
		return stop(r, INST_E_FAT_CROSS, 0,
			    "a cluster this install would write over belongs to another file as well: %s",
			    note.where ? note.where : "shared");
	if (answer == INST_FAT_UNREADABLE) {
		/* "Could not look" is not "looked and it is fine", and reporting the
		 * first as the second sends whoever reads it after the wrong thing. It
		 * is not a refusal either: the fault this guards against needs a FAT
		 * that is already inconsistent, and refusing every install whose store
		 * this cannot model would turn installs that would have worked into
		 * installs that do not, for a risk that is conditional. So it is said in
		 * a line of its own, and the install goes on exactly as it did before
		 * this check existed. */
		say(r, "cross-link check did not run: %s%s%s", note.where ? note.where : "unknown",
		    note.err ? ": " : "", note.err ? strerror(note.err) : "");
		return INST_READY;
	}
	/* Which volume, because this is the one line that says the question was
	 * asked of the filesystem that will hold the bytes rather than of the proxy
	 * in front of it -- and on a reader's Kindle it is the only way to see it:
	 * the shape of the store is not something this program can be shown here. */
	say(r, "no cluster of the images belongs to another file (%s on %s)", note.mount, note.volume);
	return INST_READY;
}

static enum inst_code install_system(struct run *r)
{
	const struct inst_release *rel = r->rel;
	const char *payload = NULL;
	/* A size the store would not give is not a size that differs. Taken as one,
	 * a payload sitting in documents/ that could not be stat'ed sent the run on
	 * to downloading it -- and on a device with no Wi-Fi that ends at E202,
	 * "connect to Wi-Fi", about a file the reader had already copied across.
	 *
	 * Asked in order, and each answer acted on before the next is asked. Asking
	 * both and then testing the pair stops on an unreadable copy in
	 * android/download/ that this run was never going to use, and reports the
	 * errno of whichever call ran last rather than of the one that failed. */
	const int64_t in_documents = file_size(r->documents_payload);
	if (in_documents == FILE_UNREADABLE)
		return stop(r, INST_E_IO, errno, "cannot tell whether %s is on the store",
			    r->documents_payload);
	if (in_documents == (int64_t)rel->payload.bytes) {
		payload = r->documents_payload;
	} else {
		const int64_t downloaded = file_size(r->download_payload);
		if (downloaded == FILE_UNREADABLE)
			return stop(r, INST_E_IO, errno, "cannot tell whether %s is on the store",
				    r->download_payload);
		if (downloaded == (int64_t)rel->payload.bytes)
			payload = r->download_payload;
	}
	if (!payload) {
		enum inst_code c = download(r);
		if (c)
			return c;
		payload = r->download_payload;
	}
	say(r, "system payload: %s", payload);
	struct pw3_unpack_job job;
	enum pw3_unpack_result e = unpack(r, payload, &rel->payload, r->system_path, &rel->system, INST_STEP_SYSTEM,
					  system_before_write, &job);
	if (e == PW3_UNPACK_INPUT_INCOMPLETE || e == PW3_UNPACK_INPUT_CORRUPT) {
		/* Said, not assumed. remove_file() reports why it could not delete,
		 * and a log that says "deleted" about a file still sitting there
		 * sends the next look in the wrong direction. */
		const int gone = !remove_file(r, payload);
		return stop(r, INST_E_PAYLOAD_BAD, 0, "%s: %s; %s", payload, job.detail,
			    gone ? "deleted" : "and it could not be deleted");
	}
	if (e == PW3_UNPACK_ABORTED)
		return stop(r, INST_E_IO, errno, "cannot write the state");
	if (e)
		return unpack_failure(r, e, &job, "system image");
	r->st.system = IMG_READY;
	snprintf(r->st.version, sizeof(r->st.version), "%s", rel->version);
	hex(rel->system.sha256, r->st.system_sha256);
	hex(rel->boot_image, r->st.boot_sha256);
	if (write_state(r))
		return stop(r, INST_E_IO, errno, "cannot write the state");
	/* Only once that file is known to be on the disk. This deletes the one copy
	 * of 196 MB that can install this system again, and a state file still in
	 * the page cache when the power goes would leave the next tap with neither.
	 * An unconfirmed flush keeps it instead, and costs nothing lasting: the tap
	 * after this one finds both images in place and deletes it there. */
	if (r->state_synced)
		clean_payloads(r);
	else
		say(r, "the payload is kept: the state file's folder was not flushed");
	return INST_READY;
}

static enum inst_code install_data(struct run *r)
{
	const struct inst_release *rel = r->rel;
	if (write_all(r->template_path, rel->template_data, rel->template_file.bytes))
		return stop(r, errno == ENOSPC ? INST_E_MEMORY : INST_E_IO, errno, "cannot write the data template");
	r->st.data = IMG_WRITING;
	if (write_state(r))
		return stop(r, INST_E_IO, errno, "cannot write the state");
	struct pw3_unpack_job job;
	enum pw3_unpack_result e = unpack(r, r->template_path, &rel->template_file, r->data_path, &rel->data, INST_STEP_DATA,
					  NULL, &job);
	if (e == PW3_UNPACK_INPUT_INCOMPLETE || e == PW3_UNPACK_INPUT_CORRUPT)
		return stop(r, INST_E_RELEASE, 0, "the built-in data template: %s", job.detail);
	if (e)
		return unpack_failure(r, e, &job, "data image");
	r->st.data = IMG_READY;
	r->st.data_format = rel->data_format;
	if (write_state(r))
		return stop(r, INST_E_IO, errno, "cannot write the state");
	return INST_READY;
}

/* The one thing that may not be handed over to Android on a state file that got
 * no further than the page cache: a data image the state calls ready. A tap
 * that reads data=writing finds an image of exactly the right size and remakes
 * it from the empty template, so everything the reader did in between is gone.
 * For the system an unconfirmed state costs time and nothing else -- the tap
 * after it reads system=writing and writes the system again from the payload --
 * and write_state() is right not to make that flush fatal.
 *
 * Asked at the way out rather than of the tap that wrote the line, because the
 * tap that writes it need not be the tap that hands over. Until 2026-10-27 this
 * was `need_data && !state_synced`, which is only the first of them: a tap
 * killed between install_data() and that check, or a reader who taps again
 * without the restart E402 asked for, reaches Android through the next tap,
 * which makes no data image, asks nothing, and returns ready on a data=ready
 * that is still only in the cache.
 *
 * Asking is also the repair: state_on_disk() fsyncs the file and its folder, so
 * a write that was merely waiting is on the disk by the time this returns, and
 * only a store that will not take it at all stops the tap. Stopping takes
 * nothing from the reader either way -- an image this tap made is empty and the
 * next tap remakes it; one from before is theirs, untouched, and still there
 * after the restart. */
static enum inst_code hand_over(struct run *r)
{
	if (r->st.data == IMG_READY && !state_on_disk(r))
		return stop(r, INST_E_IO, errno,
			    "the data image is ready and the state saying so would not flush");
	return INST_READY;
}

static enum inst_code ensure(struct run *r)
{
	const struct inst_release *rel = r->rel;
	const struct inst_ops *ops = r->ops;
	enum inst_code c;
	if (read_state(r))
		return stop(r, INST_E_IO, errno, "cannot read %s, so nothing is known about what is installed",
			    r->state_path);
	struct state *s = &r->st;
	/* What Android did, before anything is decided about what to install. Two
	 * things used to be guessed here and are now read (docs/known-issues.md,
	 * 七 and 八); both need a report that no earlier tap has already acted on,
	 * which is what the count is for. */
	struct report rep;
	read_report(r, &rep);
	int accounted = 0, notice_cleared = 0;
	if (!rep.readable) {
		/* No baseline this tap, so none is claimed: the android= line is left
		 * out of whatever this tap writes, and the tap after this one adopts
		 * the count it finds without concluding anything from it.
		 *
		 * Keeping the old baseline instead is what lets a report about the
		 * system before this one be applied to the system after it: this tap
		 * may install a new system, and a report already in the area is then
		 * about the old one, with a count this side has not seen. The cost of
		 * giving the baseline up is one report passed over -- the start after
		 * it writes another, with a different count, and that one is acted on,
		 * so a damaged image is found one tap later rather than never. */
		say(r, "the report area did not answer; this tap records no report count");
		s->android_known = 0;
		accounted = 1;
	} else if (!s->android_known) {
		/* A state file written before reports were accounted for, or none at
		 * all, against an area that may hold a report from any time. The count
		 * is adopted and nothing is concluded from it: this tap does not know
		 * whether that report is about the system now on the store, and
		 * guessing either way is what this whole mechanism replaces. An area
		 * that holds no report makes the baseline 0, which is what a device
		 * that has never started Android has. */
		if (rep.found)
			say(r, "report %u found, and no earlier tap recorded one: taken as seen", rep.boot);
		s->android_boot = rep.found ? rep.boot : 0;
		s->android_known = 1;
		accounted = 1;
	} else if (rep.made && s->android_boot != rep.boot) {
		/* The area was made again -- by this program, which is the only thing
		 * that writes that -- so the count in the state refers to a report that
		 * is no longer anywhere. What this side has acted on in *this* area is
		 * the placeholder, so that is what is recorded, and the start that
		 * follows writes one more than it. */
		say(r, "the report area was made again, with the start count at %u; the %u recorded refers to no report that is still there",
		    rep.boot, s->android_boot);
		s->android_boot = rep.boot;
		s->android_known = 1;
		accounted = 1;
	} else if (rep.found && rep.boot != s->android_boot) {
		/* Different, not greater. The count restarts at 1 when the area it
		 * was read from was not a report -- a write cut off by a flat battery
		 * leaves a checksum that does not match, and the start after that
		 * counts from the beginning (ReportStop.h). Against "greater" such a
		 * device would never be heard from again: every report it wrote would
		 * be numbered below the one this side had already accounted for, and
		 * a damaged system image would be back to the loop with no way out.
		 * Equal is the only reading that means "the report I acted on".
		 *
		 * The cost of "different" is that an area restored from a copy on a
		 * computer is acted on a second time: one repair, announced on the
		 * screen. That is the right way round against a device that goes
		 * silent for good. */
		s->android_boot = rep.boot;
		accounted = 1;
		if (!strncmp(rep.stopped, PW3_REPORT_STOP_SYSTEM_IMAGE, strlen(PW3_REPORT_STOP_SYSTEM_IMAGE)) ||
		    !strncmp(rep.stopped, PW3_REPORT_STOP_SYSTEM_FORMAT, strlen(PW3_REPORT_STOP_SYSTEM_FORMAT))) {
			/* The system image on the store is not the image its recorded
			 * hash says it is. Nothing here reads 512 MiB on every tap to
			 * find that out -- some 19 seconds every time, 20.9 s measured
			 * when the image was 576 MiB -- so the
			 * record stayed believed and every tap handed over to the same
			 * refusal, with no error code and no way out but a computer.
			 * Installing it again is the answer, and these are the reports
			 * that ask for it. The data image beside it is untouched.
			 *
			 * Two sentences and not one: the hash says it when it runs, and
			 * the superblock says it when the hash was skipped because the
			 * image's stamp still matched -- which is where bit rot under an
			 * unchanged size and mtime lands (ReportArea.h). Both mean the
			 * image on the store is not the one this release wrote. */
			say(r, "Android refused the system image on start %u: %s", rep.boot, rep.stopped);
			s->system = IMG_NONE;
			s->version[0] = 0;
			s->system_sha256[0] = 0;
			r->res->repaired = 1;
			/* Said before the checks that can stop this tap, and held, so
			 * that it is read rather than passed through on the way to the
			 * installing screens. The recorded boot image is kept: writing
			 * this release's image again brings no new compiling with it. */
			show(r, INST_STEP_REPAIR, 0, 0);
			ops->wait(ops->ctx, 4);
		} else if (rep.stopped[0]) {
			/* A refusal this side cannot act on: the reason is this project's
			 * own sentence and it is in the folder the launcher just wrote,
			 * which is what a reader needs. Nothing about compiling follows
			 * from a start that never got that far. */
			say(r, "Android did not start on start %u: %s", rep.boot, rep.stopped);
		}
	}
	/* Whether the applications are compiled is asked of whatever the area
	 * holds, and not of the count: the count answers the refusal path's
	 * question ("have I acted on this report already"), and this one is "did
	 * the system this entry installs finish compiling". A report answers that
	 * when it comes from that system, which it says itself.
	 *
	 * boot_completed is after updatePackagesIfNeeded() in
	 * SystemServer.startOtherServices(), so a start that reached it compiled
	 * the applications in the data image against the boot image in place.
	 *
	 * The two directions do not cost the same: clearing this notice late is one
	 * notice too many, clearing it early has somebody hold the power button in
	 * the middle of dexopt. A copy of this file put back from a computer is the
	 * one thing anybody can restore, and the report it holds is then about some
	 * other system -- which is the whole reason the build is compared instead
	 * of a count of starts. Two passes of review went into making a count carry
	 * that weight, and each fix opened the next hole, because a count cannot
	 * say whose work it counted.
	 *
	 * A start whose compiling outlasts the sampler's two minutes reports
	 * without boot_completed, and the notice is then shown once more, on the
	 * tap after a start that had in fact finished. */
	if (s->compiling && rep.found && !rep.stopped[0]) {
		const int ours = rel->build_incremental[0] && !strcmp(rep.build, rel->build_incremental);
		if (rep.completed && ours && rep.boot > s->compiling_after) {
			say(r, "Android start %u of %s reached boot_completed; the applications are compiled",
			    rep.boot, rep.build);
			s->compiling = 0;
			s->compiling_after = 0;
			s->compiling_after_known = 0;
			/* A tap that installs nothing and accounts for nothing still has to
			 * write this down. It did not, for as long as the clearing lived
			 * inside the branch that records a report: the notice went out of
			 * memory and stayed on the store, to be shown again by the next tap
			 * that could not read the area. */
			notice_cleared = 1;
		} else if (rep.completed && ours)
			say(r, "report %u is not later than the %u this install was owed from; the compiling notice stays",
			    rep.boot, s->compiling_after);
		else if (rep.completed && rep.build[0])
			say(r, "report %u completed, but it was written by %s and this entry installs %s; the compiling notice stays",
			    rep.boot, rep.build, rel->build_incremental);
		else if (rep.completed)
			say(r, "report %u completed and does not say which build wrote it; the compiling notice stays", rep.boot);
		else
			say(r, "Android start %u did not reach boot_completed; the applications may still be compiling",
			    rep.boot);
	}
	char sha[65], boot[65];
	hex(rel->system.sha256, sha);
	hex(rel->boot_image, boot);
	/* The boot image of the system in place, before this run replaces it. Empty
	 * when nothing recorded it -- a state file from before it was written, or no
	 * state file at all -- and then it is not known whether the applications in
	 * /data face compiling, so the screen that says they do is shown. A warning
	 * too many is read and ignored; one too few leaves the user waiting out half
	 * an hour with no idea why. */
	char installed_boot[sizeof(s->boot_sha256)];
	memcpy(installed_boot, s->boot_sha256, sizeof(installed_boot));

	int64_t data_bytes = file_size(r->data_path);
	if (data_bytes == FILE_UNREADABLE)
		return stop(r, INST_E_IO, errno, "cannot tell whether there is a data image");
	int inferred = 0;
	if (s->data == IMG_NONE && data_bytes >= 0) {
		/* A data image the state file does not account for. It was made before there
		 * were state files, the state file was cut short -- a write interrupted by a
		 * flat battery can leave "schema=1" and no data line, which read_state still
		 * reports as known -- or there is no state file at all. Either way it holds
		 * what the user has read and is not written over for the lack of a line about
		 * it. An image the state file says was being written (IMG_WRITING) is not this
		 * case: that one is half made and is remade.
		 *
		 * Its format is the one thing that cannot be recovered, so it is taken to be
		 * this release's and the check below decides on the size, which is the only
		 * evidence there is. Taking it to be format 1 -- what this did while 1 was the
		 * only format -- refuses every device whose record was lost after it was
		 * installed, and refusing it repairs nothing: the check below is ahead of the
		 * hand-over too, so that device starts no Android at all until the reader
		 * deletes the data image and loses what is in it. The size settles it as long
		 * as no two formats are the same size. A format that reuses a size has to be
		 * stamped into the image itself -- the ext4 volume label is where it would go
		 * -- because this inference cannot tell two such images apart. */
		s->data = IMG_READY;
		s->data_format = rel->data_format;
		inferred = 1;
		say(r, "data image the state file does not account for: taken as format %u, this release's",
		    rel->data_format);
	}
	if (s->data == IMG_READY && data_bytes == FILE_ABSENT) {
		say(r, "state has a data image, the user store does not: a new one is made");
		s->data = IMG_NONE;
	}
	if (s->data == IMG_READY && (s->data_format != rel->data_format || data_bytes != (int64_t)rel->data.bytes))
		return stop(r, INST_E_DATA_FORMAT, 0, "data image of format %u and %lld bytes; this release uses format %u, %llu bytes",
			    s->data_format, (long long)data_bytes, rel->data_format, (unsigned long long)rel->data.bytes);
	int need_data = s->data != IMG_READY;
	int64_t system_bytes = file_size(r->system_path);
	if (system_bytes == FILE_UNREADABLE)
		return stop(r, INST_E_IO, errno, "cannot tell whether there is a system image");
	int need_system = !(s->system == IMG_READY && !strcmp(s->version, rel->version) && !strcmp(s->system_sha256, sha) &&
			    system_bytes == (int64_t)rel->system.bytes);
	say(r, "release %s: system %s, data %s", rel->version, need_system ? "to install" : "ready",
	    need_data ? "to make" : "kept");
	/* The system is in place, so no payload feeds anything from here: a data
	 * image is made from the template the entry carries (install_data). One is
	 * still there when a tap was cut off between the state file saying
	 * system=ready and install_system() deleting it, and when install_system()
	 * kept it because that file's folder would not flush. It is 196 MB, and
	 * check_space() counts it as reclaimable only while the system still has to
	 * be written -- so a data image could be refused for want of room the
	 * installer's own leftover is holding, and the screen would ask the reader
	 * to delete books.
	 *
	 * Above the "nothing to do" return and not below it, which is where this
	 * stood until 2026-10-27: an upgrade keeps its data image, so the tap after
	 * an interrupted one finds both in place and returns there -- and so does
	 * every tap after that, leaving the 196 MB on the reader's Kindle for good.
	 * The whole folder goes, not the payload by name, so a part left beside it
	 * goes with it.
	 *
	 * On the same terms as install_system(): the state file is what says the
	 * system is in place, and this tap may have written no state of its own,
	 * so the folder is asked before anything goes. Without that, a tap whose
	 * predecessor was killed between the state file and the payload -- the one
	 * case that reaches here with a cache no reboot has cleared -- would
	 * delete the payload on a rename that is still only in memory. */
	if (!need_system) {
		if (!state_on_disk(r))
			say(r, "anything this installer left over is kept: the state would not flush: %s",
			    strerror(errno));
		else {
			const int gone = clean_payloads(r);
			if (gone)
				say(r, "the system is in place and %d file(s) of this installer were left over; deleted", gone);
		}
	}
	if (!need_system && !need_data) {
		/* An upgrade whose first start was cut short faces the same compiling
		 * again, so the screen that says so is shown until a report says a
		 * start got through it -- not for a fixed number of launches, which is
		 * what the block above has just read the area to replace. */
		if (s->compiling) {
			r->res->upgraded = 1;
			say(r, "the applications may still be compiling; no completed Android start since the upgrade");
		}
		/* A state file with no boot image -- one written before they were
		 * recorded -- is left without one. Filling it in from this release would
		 * rest on the record naming the image on the store, and the record is
		 * what is in question: an image put there by hand, of the same size,
		 * passes every check made here, because need_system compares the recorded
		 * SHA-256 and not the file. The cost of leaving it empty is one warning
		 * too many at the next upgrade, which is the direction to err in.
		 *
		 * The record is brought up to date even with nothing to install: a state
		 * file that was cut short would otherwise stay that way for good.
		 *
		 * `accounted` is in here because a report that was read and acted on has
		 * to be written down on this path too: it is the path almost every tap
		 * takes, and a count that is not kept would have the next tap treat the
		 * same report as new again. */
		if ((inferred || accounted || notice_cleared || r->res->upgraded) && write_state(r))
			return stop(r, INST_E_IO, errno, "cannot write the state");
		/* The path almost every tap takes, so the one that has to make the
		 * report area for an installation from before it existed -- and the one
		 * that has to empty one whose magic was damaged, because that file is
		 * kept by every other check and refused by PID 1.
		 *
		 * Forced when a notice is owed with no count behind it: a state file
		 * written by an installer from before that line existed, or one an older
		 * installer rewrote without it. Without a floor, any report of this
		 * build kept on a computer clears that notice; with one, the notice
		 * waits for a start later than this moment. */
		const uint32_t plant = plant_count(r);
		const int floor_owed = s->compiling && !s->compiling_after_known;
		if (ensure_report_area(r, plant, floor_owed) && floor_owed) {
			s->compiling_after = plant;
			s->compiling_after_known = 1;
			if (write_state(r))
				say(r, "the report area was made and the state could not be written: %s",
				    strerror(errno));
		}
		return hand_over(r);
	}

	int percent = 0, charging = 0;
	if (ops->battery(ops->ctx, &percent, &charging))
		say(r, "battery level unknown; not checked");
	else if (percent < INST_MIN_BATTERY && !charging)
		return stop(r, INST_E_LOW_BATTERY, 0, "battery %d%%, not charging", percent);

	const char *payload = NULL;
	if (need_system) {
		/* A store that cannot say a payload's length is a storage fault, and
		 * taken for "that payload is not here" it becomes the wrong answer two
		 * steps later: the space check asks for room to download 191 MB that is
		 * in fact already on the drive, does not find it, and tells the reader
		 * to delete books (a Codex review, 2026-10-30). The copy in download/
		 * is only asked about when documents/ did not answer, which is what
		 * keeps an unreadable leftover there from stopping an install whose
		 * payload is in documents/. */
		int64_t here = file_size(r->documents_payload);
		if (here == FILE_UNREADABLE)
			return stop(r, INST_E_IO, errno, "cannot read the size of the payload in documents");
		if (here == (int64_t)rel->payload.bytes) {
			payload = r->documents_payload;
		} else {
			here = file_size(r->download_payload);
			if (here == FILE_UNREADABLE)
				return stop(r, INST_E_IO, errno, "cannot read the size of the payload in download");
			if (here == (int64_t)rel->payload.bytes)
				payload = r->download_payload;
		}
	}
	/* A part left behind by a server that sent more than the manifest says can
	 * neither become the payload nor be resumed. Deleting it here rather than in
	 * download() is what lets the next run get that far: it takes room, so the
	 * space check below would stop first and tell the user to delete books while
	 * the installer's own leftover is what is in the way. */
	/* A store that cannot say the leftover part's length only stops this run
	 * when this run may have to resume that part. With the payload already
	 * found, or with no system image to write, nothing reads it -- and read as
	 * fatal either way it stopped an install whose payload was sitting in
	 * documents/ (a Codex review, 2026-10-30), which is the same mistake the
	 * payload choice above was just fixed for. download() asks again, and
	 * check_space() only counts this file when it is going to be resumed. */
	int64_t leftover = file_size(r->part_path);
	if (need_system && !payload && leftover == FILE_UNREADABLE)
		return stop(r, INST_E_IO, errno, "cannot read the size of the part left over");
	if (leftover > (int64_t)rel->payload.bytes) {
		say(r, "a part larger than the payload is left over; deleting it");
		if (remove_file(r, r->part_path))
			return stop(r, INST_E_IO, errno, "cannot delete the part");
	}
	if ((c = check_space(r, need_system, payload, need_data)))
		return c;
	if (!s->known && write_state(r))	/* from here on the state file says what is on the user store */
		return stop(r, INST_E_IO, errno, "cannot write the state");
	s->known = 1;
	/* A kept data image holds applications, and they are compiled again only if
	 * the boot image they were compiled against is gone. Most releases of this
	 * project keep it byte for byte (the 45 files of /system/framework/arm are
	 * compared after every build), and those upgrades cost the user nothing.
	 *
	 * Decided before anything is written, so that install_system() commits it in
	 * the same state file as the system it belongs to. Two commits leave a window
	 * -- the payload, 196 MB, is deleted between them -- and a power cut inside it
	 * would lose the warning for good: the next tap would find everything in place
	 * and say three minutes, and to hold the power button after ten, in the middle
	 * of the compiling. */
	r->res->upgraded = need_system && !need_data && strcmp(installed_boot, boot) != 0;
	if (r->res->upgraded) {
		s->compiling = 1;
		/* The floor this install's own start has to be above. Planted rather
		 * than read: a new area is written here carrying this count, so the
		 * next start reports it plus one, and every report written before this
		 * moment carries less (plant_count). Reading the count that happens to
		 * be in the area instead is what let a report of this same build, kept
		 * on a computer since the last time it was installed, take the notice
		 * away -- and what let one bit turned over in an unprotected count
		 * leave a notice that could never clear at all.
		 *
		 * If the area cannot be written, the floor falls back to the count in
		 * it: the degraded case keeps the ordering it can and is no worse than
		 * the version before this one. */
		const uint32_t plant = plant_count(r);
		if (ensure_report_area(r, plant, 1)) {
			s->compiling_after = plant;
			s->android_boot = plant;
			s->android_known = 1;
		} else {
			s->compiling_after = rep.found ? rep.boot : 0;
		}
		s->compiling_after_known = 1;
	} else if (need_data) {
		/* Nothing in a new data image to compile, so nothing is owed from before. */
		s->compiling = 0;
		s->compiling_after = 0;
		s->compiling_after_known = 0;
	} else if (s->compiling)
		/* An earlier upgrade's compiling was never seen through, and this release
		 * brings the boot image already in place: it adds no compiling of its own,
		 * and it does not finish what the earlier one started either. So this
		 * start is told, like the starts before it, and the earlier notice keeps
		 * the count it was set with. */
		r->res->upgraded = 1;
	if ((c = check_cross_links(r, need_system, need_data)))
		return c;
	if (need_system && (c = install_system(r)))
		return c;
	if (need_data && (c = install_data(r)))
		return c;
	if (write_state(r))
		return stop(r, INST_E_IO, errno, "cannot write the state");
	if ((c = hand_over(r)) != INST_READY)
		return c;
	say(r, "installed %s%s", rel->version,
	    !need_system || need_data ? "" :
	    r->res->upgraded ? " over a kept data image, whose applications are compiled again"
			     : " over a kept data image, with the same boot image");
	/* Last, and never in the way: the images have their room, and 64 KiB is
	 * inside the 64 MiB this check keeps free (INST_RESERVE_BYTES), so it is
	 * not part of the space arithmetic above. An install that set the notice
	 * already made the area with its own planted count, and this leaves that
	 * one alone. */
	ensure_report_area(r, plant_count(r), 0);
	return INST_READY;
}

enum inst_code inst_ensure(const struct inst_release *rel, const struct inst_paths *paths, const struct inst_ops *ops,
			   struct inst_result *result)
{
	struct run run = { .rel = rel, .paths = paths, .ops = ops, .res = result };
	memset(result, 0, sizeof(*result));
	show(&run, INST_STEP_CHECK, 0, 0);
	enum inst_code c = prepare_paths(&run);
	if (c)
		return c;
	c = ensure(&run);
	unlink(run.ca_path);
	unlink(run.template_path);
	return c;
}
