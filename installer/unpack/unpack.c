/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "unpack.h"

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "sha256.h"
#define ZSTD_STATIC_LINKING_ONLY
#include "zstd.h"
#include "zstd_errors.h"

#define BUFFER_BYTES (1u << 20)
/* Written data is pushed to the device and dropped from the page cache in windows of
 * this size: the stock Kindle system keeps running with little free memory, and the
 * progress shown is what is on the device rather than in the cache. */
#define WINDOW_BYTES (8ull << 20)
/* build_payload.py compresses with zstd -19: an 8 MiB window. A frame asking for more
 * is refused instead of allocating it. */
#define ZSTD_WINDOW_LOG_MAX 23

struct reader {
	int fd;
	unsigned char *buf;
	size_t pos, len;
	uint64_t consumed;	/* bytes handed out */
};

struct writer {
	struct pw3_unpack_job *job;
	int fd;
	uint64_t offset;	/* bytes written */
	uint64_t pushed;	/* bytes whose write-back has been started */
	uint64_t settled;	/* bytes on the device and dropped from the cache */
	SHA256_CTX sha;
};

static enum pw3_unpack_result fail(struct pw3_unpack_job *job, enum pw3_unpack_result r, int err,
				   const char *detail)
{
	job->sys_errno = err;
	job->detail = detail;
	return r;
}

static void report(struct pw3_unpack_job *job, enum pw3_unpack_phase phase, uint64_t done, uint64_t total)
{
	if (job->progress)
		job->progress(job->progress_ctx, phase, done, total);
}

/* ---- input ---- */

static enum pw3_unpack_result verify_input(struct pw3_unpack_job *job, unsigned char *buf)
{
	/* Why these are four branches and not one.
	 *
	 * PW3_UNPACK_INPUT_INCOMPLETE is the caller's instruction to delete the
	 * file and tell the reader the package was bad (installer/install, E302).
	 * That is right when the size has been measured and does not match. It is
	 * not right when the store simply would not let the file be opened or
	 * stat'ed: then a good 200 MB package is destroyed over a failure that
	 * says nothing about its contents, and somebody with no Wi-Fi has to go
	 * back to a computer for a copy while the screen blames the copy they had.
	 * The output's open failure a few lines down was already classified as
	 * what it is; the input's was not. ENOENT stays with INPUT_INCOMPLETE --
	 * "not there" is a statement about the file, and deleting what is not
	 * there costs nothing. */
	struct stat st;
	int fd = open(job->input, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return fail(job, errno == ENOENT ? PW3_UNPACK_INPUT_INCOMPLETE : PW3_UNPACK_IO_ERROR, errno,
			    errno == ENOENT ? "the input is not there" : "cannot open the input");
	if (fstat(fd, &st)) {
		const int err = errno;
		close(fd);
		return fail(job, PW3_UNPACK_IO_ERROR, err, "cannot read the input's size");
	}
	if (!S_ISREG(st.st_mode)) {
		close(fd);
		return fail(job, PW3_UNPACK_IO_ERROR, 0, "the input is not a plain file");
	}
	if ((uint64_t)st.st_size != job->input_bytes) {
		close(fd);
		return fail(job, PW3_UNPACK_INPUT_INCOMPLETE, 0, "the input is not its full size");
	}
	posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
	SHA256_CTX sha;
	sha256_init(&sha);
	uint64_t done = 0, dropped = 0;
	for (;;) {
		ssize_t n = read(fd, buf, BUFFER_BYTES);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0) {
			int err = errno;
			close(fd);
			return fail(job, PW3_UNPACK_IO_ERROR, err, "reading the input failed");
		}
		if (n == 0)
			break;
		sha256_update(&sha, buf, (size_t)n);
		done += (uint64_t)n;
		if (done - dropped >= WINDOW_BYTES) {
			posix_fadvise(fd, (off_t)dropped, (off_t)(done - dropped), POSIX_FADV_DONTNEED);
			dropped = done;
			report(job, PW3_UNPACK_VERIFYING, done, job->input_bytes);
		}
	}
	posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
	close(fd);
	unsigned char digest[32];
	sha256_final(&sha, digest);
	if (done != job->input_bytes)
		return fail(job, PW3_UNPACK_INPUT_INCOMPLETE, 0, "the input changed while it was read");
	if (memcmp(digest, job->input_sha256, 32))
		return fail(job, PW3_UNPACK_INPUT_CORRUPT, 0, "the input's SHA-256 is not the manifest's");
	report(job, PW3_UNPACK_VERIFYING, done, job->input_bytes);
	return PW3_UNPACK_OK;
}

/* Fills up to want bytes; *got < want only at the end of the file. */
static enum pw3_unpack_result read_some(struct pw3_unpack_job *job, struct reader *r, size_t want,
					const unsigned char **data, size_t *got)
{
	if (r->pos == r->len) {
		ssize_t n;
		do
			n = read(r->fd, r->buf, BUFFER_BYTES);
		while (n < 0 && errno == EINTR);
		if (n < 0)
			return fail(job, PW3_UNPACK_IO_ERROR, errno, "reading the input failed");
		r->pos = 0;
		r->len = (size_t)n;
	}
	*data = r->buf + r->pos;
	*got = r->len - r->pos < want ? r->len - r->pos : want;
	r->pos += *got;
	r->consumed += *got;
	return PW3_UNPACK_OK;
}

/* ---- output ---- */

static int is_enospc(int err)
{
	return err == ENOSPC || err == EFBIG || err == EDQUOT;
}

/* check_space() ran and passed, and the store filled up anyway: the stock system
 * downloads and indexes while this runs. The caller shows space_needed on the
 * screen ("delete some books to free this much"), and every one of these paths
 * used to leave it at the zero it starts at, so the screen asked for 0 MB and no
 * amount of deleting could satisfy it. What has not reached the device yet is the
 * least that is missing. The last sector is a metadata write with nothing left to
 * settle, and a cluster is the least that can help there. */
#define UNPACK_MIN_SPACE (1024u * 1024u)
static enum pw3_unpack_result no_space(struct writer *w, int err, const char *detail)
{
	uint64_t left = w->job->output_bytes > w->settled ? w->job->output_bytes - w->settled : 0;
	w->job->space_needed = left > UNPACK_MIN_SPACE ? left : UNPACK_MIN_SPACE;
	return fail(w->job, PW3_UNPACK_NO_SPACE, err, detail);
}

/* Starts the write-back of what was written since the last call, in whole windows, and
 * waits for the window before it (it had a window's time to reach the device). */
static enum pw3_unpack_result settle(struct writer *w, int all)
{
	uint64_t target = all ? w->offset : w->offset - w->offset % WINDOW_BYTES;
	if (target > w->pushed) {
		if (sync_file_range(w->fd, (off_t)w->pushed, (off_t)(target - w->pushed), SYNC_FILE_RANGE_WRITE) &&
		    errno != ENOSYS && errno != EINVAL && errno != ESPIPE) {
			if (is_enospc(errno))
				return no_space(w, errno, "the store filled up while writing");
			return fail(w->job, PW3_UNPACK_IO_ERROR, errno, "writing the output failed");
		}
		w->pushed = target;
	}
	uint64_t ready = all ? w->pushed : (w->pushed >= WINDOW_BYTES ? w->pushed - WINDOW_BYTES : 0);
	if (ready > w->settled) {
		if (sync_file_range(w->fd, (off_t)w->settled, (off_t)(ready - w->settled),
				    SYNC_FILE_RANGE_WAIT_BEFORE | SYNC_FILE_RANGE_WRITE | SYNC_FILE_RANGE_WAIT_AFTER) &&
		    errno != ENOSYS && errno != EINVAL && errno != ESPIPE) {
			if (is_enospc(errno))
				return no_space(w, errno, "the store filled up while writing");
			return fail(w->job, PW3_UNPACK_IO_ERROR, errno, "writing the output failed");
		}
		posix_fadvise(w->fd, (off_t)w->settled, (off_t)(ready - w->settled), POSIX_FADV_DONTNEED);
		w->settled = ready;
		report(w->job, PW3_UNPACK_WRITING, w->settled, w->job->output_bytes);
	}
	return PW3_UNPACK_OK;
}

static enum pw3_unpack_result emit(struct writer *w, const void *data, size_t n)
{
	if (n > w->job->output_bytes - w->offset)
		return fail(w->job, PW3_UNPACK_PAYLOAD_INVALID, 0, "the input unpacks to more than the manifest's size");
	sha256_update(&w->sha, data, n);
	const unsigned char *p = data;
	while (n) {
		ssize_t k = write(w->fd, p, n);
		if (k < 0 && errno == EINTR)
			continue;
		if (k < 0) {
			if (is_enospc(errno))
				return no_space(w, errno, "the store filled up while writing");
			return fail(w->job, PW3_UNPACK_IO_ERROR, errno, "writing the output failed");
		}
		uint64_t before = w->offset;
		p += k;
		n -= (size_t)k;
		w->offset += (uint64_t)k;
		if (w->offset / WINDOW_BYTES != before / WINDOW_BYTES) {
			enum pw3_unpack_result e = settle(w, 0);
			if (e)
				return e;
		}
	}
	return PW3_UNPACK_OK;
}

/* ---- zstd ---- */

static enum pw3_unpack_result unpack_zstd(struct writer *w, struct reader *r, unsigned char *out_buf)
{
	struct pw3_unpack_job *job = w->job;
	ZSTD_DCtx *dctx = ZSTD_createDCtx();
	if (!dctx)
		return fail(job, PW3_UNPACK_NO_MEMORY, 0, "no memory for the zstd decoder");
	enum pw3_unpack_result e = PW3_UNPACK_OK;
	if (ZSTD_isError(ZSTD_DCtx_setParameter(dctx, ZSTD_d_windowLogMax, ZSTD_WINDOW_LOG_MAX))) {
		e = fail(job, PW3_UNPACK_NO_MEMORY, 0, "cannot limit the zstd window");
		goto out;
	}
	size_t last = 0;	/* 0: the last frame is complete */
	int seen = 0;
	for (;;) {
		const unsigned char *data;
		size_t got;
		e = read_some(job, r, BUFFER_BYTES, &data, &got);
		if (e)
			goto out;
		if (!got)
			break;
		seen = 1;
		ZSTD_inBuffer in = { data, got, 0 };
		int full = 0;
		/* A full output buffer may leave more of the frame to flush; once the frame is
		 * complete (0), another call would only start waiting for the next one. */
		while (in.pos < in.size || (full && last != 0)) {
			ZSTD_outBuffer out = { out_buf, BUFFER_BYTES, 0 };
			last = ZSTD_decompressStream(dctx, &out, &in);
			if (ZSTD_isError(last)) {
				e = fail(job, ZSTD_getErrorCode(last) == ZSTD_error_memory_allocation ? PW3_UNPACK_NO_MEMORY
								: PW3_UNPACK_PAYLOAD_INVALID,
					 0, ZSTD_getErrorName(last));
				goto out;
			}
			e = emit(w, out_buf, out.pos);
			if (e)
				goto out;
			full = out.pos == out.size;
		}
	}
	if (!seen || last != 0)
		e = fail(job, PW3_UNPACK_PAYLOAD_INVALID, 0, "the zstd stream ends inside a frame");
out:
	ZSTD_freeDCtx(dctx);
	return e;
}

/* ---- the job ---- */

static enum pw3_unpack_result check_space(struct pw3_unpack_job *job, int fd)
{
	struct stat st;
	struct statvfs vfs;
	char dir[PATH_MAX];
	if (fstat(fd, &st))
		return fail(job, PW3_UNPACK_IO_ERROR, errno, "cannot read the output's size");
	if (strlen(job->output) >= sizeof(dir))
		return fail(job, PW3_UNPACK_IO_ERROR, ENAMETOOLONG, "output path too long");
	strcpy(dir, job->output);
	if (statvfs(dirname(dir), &vfs))
		return fail(job, PW3_UNPACK_IO_ERROR, errno, "cannot read the free space");
	uint64_t unit = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
	uint64_t have = (uint64_t)st.st_size, want = job->output_bytes;
	have = (have + unit - 1) / unit * unit;	/* what the file already holds, in whole clusters */
	want = (want + unit - 1) / unit * unit;
	uint64_t need = want > have ? want - have : 0, free_bytes = (uint64_t)vfs.f_bavail * unit;
	if (need > free_bytes) {
		job->space_needed = need - free_bytes;
		return fail(job, PW3_UNPACK_NO_SPACE, 0, "not enough free space");
	}
	return PW3_UNPACK_OK;
}

static enum pw3_unpack_result finish(struct writer *w)
{
	struct pw3_unpack_job *job = w->job;
	if (w->offset != job->output_bytes)
		return fail(job, PW3_UNPACK_PAYLOAD_INVALID, 0, "the input unpacks to less than the manifest's size");
	enum pw3_unpack_result e = settle(w, 1);
	if (e)
		return e;
	if (ftruncate(w->fd, (off_t)job->output_bytes) || fsync(w->fd)) {
		if (is_enospc(errno))
			return no_space(w, errno, "the store filled up while finishing the output");
		return fail(job, PW3_UNPACK_IO_ERROR, errno, "finishing the output failed");
	}
	char dir[PATH_MAX];
	strcpy(dir, job->output);	/* length checked in check_space() */
	int dfd = open(dirname(dir), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (dfd >= 0) {
		fsync(dfd);	/* the directory entry's size; vfat writes it with the file's inode */
		close(dfd);
	}
	unsigned char digest[32];
	sha256_final(&w->sha, digest);
	if (memcmp(digest, job->output_sha256, 32))
		return fail(job, PW3_UNPACK_PAYLOAD_INVALID, 0, "the output's SHA-256 is not the manifest's");
	report(job, PW3_UNPACK_WRITING, job->output_bytes, job->output_bytes);
	return PW3_UNPACK_OK;
}

enum pw3_unpack_result pw3_unpack(struct pw3_unpack_job *job)
{
	job->space_needed = 0;
	job->sys_errno = 0;
	job->detail = NULL;
	unsigned char *in_buf = malloc(BUFFER_BYTES), *work_buf = malloc(BUFFER_BYTES);
	enum pw3_unpack_result e;
	if (!in_buf || !work_buf) {
		e = fail(job, PW3_UNPACK_NO_MEMORY, 0, "no memory for buffers");
		goto out;
	}
	if ((e = verify_input(job, in_buf)))
		goto out;

	/* Opened a second time, and classified the same way as the first: a
	 * failure to open is not evidence about the bytes inside. */
	struct reader r = { .fd = open(job->input, O_RDONLY | O_CLOEXEC), .buf = in_buf };
	if (r.fd < 0) {
		e = fail(job, errno == ENOENT ? PW3_UNPACK_INPUT_INCOMPLETE : PW3_UNPACK_IO_ERROR, errno,
			 errno == ENOENT ? "the input is not there" : "cannot open the input");
		goto out;
	}
	posix_fadvise(r.fd, 0, 0, POSIX_FADV_SEQUENTIAL);
	struct writer w = { .job = job, .fd = open(job->output, O_WRONLY | O_CREAT | O_CLOEXEC, 0644) };
	if (w.fd < 0) {
		e = fail(job, is_enospc(errno) ? PW3_UNPACK_NO_SPACE : PW3_UNPACK_IO_ERROR, errno, "cannot open the output");
		close(r.fd);
		goto out;
	}
	sha256_init(&w.sha);
	if (!(e = check_space(job, w.fd)) && job->before_write && job->before_write(job->progress_ctx))
		e = fail(job, PW3_UNPACK_ABORTED, 0, "stopped before writing");
	if (!e) {
		report(job, PW3_UNPACK_WRITING, 0, job->output_bytes);
		e = unpack_zstd(&w, &r, work_buf);
		if (!e && r.consumed != job->input_bytes)
			e = fail(job, PW3_UNPACK_INPUT_INCOMPLETE, 0, "the input changed while it was unpacked");
		if (!e)
			e = finish(&w);
	}
	posix_fadvise(r.fd, 0, 0, POSIX_FADV_DONTNEED);
	close(r.fd);
	if (close(w.fd) && !e)
		e = fail(job, PW3_UNPACK_IO_ERROR, errno, "closing the output failed");
out:
	free(in_buf);
	free(work_buf);
	return e;
}
