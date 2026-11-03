/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * pw3-unpack INPUT INPUT_BYTES INPUT_SHA256 OUTPUT OUTPUT_BYTES OUTPUT_SHA256
 *
 * The command-line face of pw3_unpack() (unpack.h), for the installer's shell
 * steps and for tests. Prints "verify N" and "write N" (per mille) as it goes,
 * then "ok", or "error CODE ..." on standard error. The exit status is the
 * result code (unpack.h); 64 for a usage error.
 */
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unpack.h"

struct progress {
	int phase, permille;
};

static void show(void *ctx, enum pw3_unpack_phase phase, uint64_t done, uint64_t total)
{
	struct progress *p = ctx;
	int permille = total ? (int)(done * 1000 / total) : 1000;
	if ((int)phase == p->phase && permille == p->permille)
		return;
	p->phase = phase;
	p->permille = permille;
	printf("%s %d\n", phase == PW3_UNPACK_VERIFYING ? "verify" : "write", permille);
	fflush(stdout);
}

static int parse_size(const char *s, uint64_t *out)
{
	char *end;
	errno = 0;
	unsigned long long v = strtoull(s, &end, 10);
	if (errno || end == s || *end || *s == '-')
		return -1;
	*out = v;
	return 0;
}

static int parse_sha(const char *s, uint8_t out[32])
{
	if (strlen(s) != 64)
		return -1;
	for (int i = 0; i < 32; i++) {
		unsigned v;
		if (sscanf(s + 2 * i, "%2x", &v) != 1 || !strchr("0123456789abcdef", s[2 * i]) ||
		    !strchr("0123456789abcdef", s[2 * i + 1]))
			return -1;
		out[i] = (uint8_t)v;
	}
	return 0;
}

int main(int argc, char **argv)
{
	struct pw3_unpack_job job = { 0 };
	struct progress p = { 0, -1 };
	if (argc != 7 || parse_size(argv[2], &job.input_bytes) || parse_sha(argv[3], job.input_sha256) ||
	    parse_size(argv[5], &job.output_bytes) || parse_sha(argv[6], job.output_sha256)) {
		fprintf(stderr, "usage: pw3-unpack INPUT INPUT_BYTES INPUT_SHA256 OUTPUT OUTPUT_BYTES OUTPUT_SHA256 "
				"(input zstd; SHA-256 in lower-case hex)\n");
		return 64;
	}
	job.input = argv[1];
	job.output = argv[4];
	job.progress = show;
	job.progress_ctx = &p;
	enum pw3_unpack_result r = pw3_unpack(&job);
	if (r == PW3_UNPACK_OK) {
		puts("ok");
		return 0;
	}
	fprintf(stderr, "error %d %s", (int)r, job.detail ? job.detail : "");
	if (r == PW3_UNPACK_NO_SPACE && job.space_needed)
		fprintf(stderr, " (%" PRIu64 " more bytes needed)", job.space_needed);
	if (job.sys_errno)
		fprintf(stderr, ": %s", strerror(job.sys_errno));
	fputc('\n', stderr);
	return (int)r;
}
