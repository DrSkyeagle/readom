/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Unpacking a release file onto the Kindle's user store (installer/README.md, "解包工具"):
 * the zstd-compressed system image and empty data image become plain image files
 * of a known size and SHA-256 (build_payload.py writes both into the manifest).
 */
#ifndef PW3_UNPACK_H
#define PW3_UNPACK_H

#include <stdint.h>

enum pw3_unpack_result {
	PW3_UNPACK_OK = 0,
	PW3_UNPACK_INPUT_INCOMPLETE = 1, /* missing, or not its full size: copy or download again */
	PW3_UNPACK_INPUT_CORRUPT = 2,	 /* full size, other SHA-256: copy or download again */
	PW3_UNPACK_PAYLOAD_INVALID = 3,	 /* the right file does not unpack to the manifest: a release defect */
	PW3_UNPACK_NO_SPACE = 4,	 /* space_needed more bytes are needed on the output's file system */
	PW3_UNPACK_IO_ERROR = 5,	 /* reading or writing failed (sys_errno) */
	PW3_UNPACK_NO_MEMORY = 6,
	PW3_UNPACK_ABORTED = 7,		 /* before_write returned nonzero */
};

enum pw3_unpack_phase {
	PW3_UNPACK_VERIFYING = 1,	/* done/total: bytes of the input read */
	PW3_UNPACK_WRITING = 2,		/* done/total: bytes of the output on the device (written back) */
};

/* The input: one or more zstd frames, window at most 8 MiB. */
struct pw3_unpack_job {
	const char *input;
	uint64_t input_bytes;
	uint8_t input_sha256[32];
	const char *output;
	uint64_t output_bytes;
	uint8_t output_sha256[32];
	void (*progress)(void *ctx, enum pw3_unpack_phase phase, uint64_t done, uint64_t total);
	void *progress_ctx;
	/* Called once the input is verified and the space is there, before the output's
	 * first byte changes (the caller records that the output is no longer usable);
	 * nonzero stops the job with PW3_UNPACK_ABORTED. Optional. */
	int (*before_write)(void *ctx);
	/* Set on return. */
	uint64_t space_needed;	/* PW3_UNPACK_NO_SPACE */
	int sys_errno;		/* PW3_UNPACK_IO_ERROR, and INPUT_INCOMPLETE when the input could not be opened */
	const char *detail;	/* a short English reason for the log, or NULL */
};

/*
 * Checks the input's size and SHA-256 first (nothing is written if it is not the
 * expected file), then writes the output in place: an existing output file is
 * overwritten from its start and cut to output_bytes, so no second copy's space
 * is needed. Until this returns PW3_UNPACK_OK the output is not usable; running
 * the job again starts over. The output's SHA-256 is taken from the bytes as
 * they are written; the storage entry checks the system image again at boot.
 */
enum pw3_unpack_result pw3_unpack(struct pw3_unpack_job *job);

#endif
