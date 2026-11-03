/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Writing a refusal into the report area, from PID 1.
 *
 * A start that refuses to go on is the one its owner has no other way to hear
 * about: the launcher's screen says the Kindle may restart itself back to its
 * own system within a few minutes, and that is all it says, while the reason is
 * in the kernel log, which goes with the kernel. So the reason also goes into
 * the area, where the next tap reads it (installer/install/install.c): that tap
 * can install the system image again when that is what was wrong, instead of
 * handing over to the same refusal for ever (docs/known-issues.md, 七).
 *
 * In its own header rather than in PreInit.c because this is the path that only
 * runs when something has already gone wrong, so it is the path that would
 * otherwise first be executed on somebody's Kindle. Here it is a function of a
 * path and a sentence, and the test gives it an ordinary file
 * (preinit/tests/report_stop_test.c).
 */
#ifndef PW3_REPORT_STOP_H
#define PW3_REPORT_STOP_H

#include "ReportArea.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Which Android start this is, as the area itself records it: the count in the
 * report already there, carried on by pw3_report_boot_after. Read with the same
 * function every other reader of the area uses, so an area of zeros, or one cut
 * off mid-write, counts as no report and makes this start 1 -- and so does an
 * area that could not be opened, because a writer that put "unknown" in the
 * count field would stop the count changing, and a count that stops changing
 * is a device the stock side never acts on again.
 */
static inline uint32_t pw3_report_next_boot(const char *node, unsigned char *area, size_t bytes)
{
	struct pw3_report previous;
	size_t got = 0;
	int fd = open(node, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return 1;
	while (got < bytes) {
		const ssize_t count = read(fd, area + got, bytes - got);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			break;
		got += (size_t)count;
	}
	close(fd);
	if (!pw3_report_parse(area, got, &previous))
		return 1;
	return pw3_report_boot_after(previous.boot);
}

/* The whole area, written once, carrying one field: why PID 1 stopped. Returns
 * 1 only when all of it reached the device.
 *
 * `area` is the caller's buffer of PW3_REPORT_BYTES -- PID 1 keeps one in .bss
 * rather than putting 64 KiB on the stack of a path that is already failing.
 * Nothing here retries, waits or loops without a bound: this runs in PID 1 just
 * before a 120-second wait and a reboot, and a diagnostic that hangs would cost
 * the restart that is the way back to the owner's own system.
 */
static inline int pw3_report_write_stop(const char *node, const char *message,
					unsigned char *area, size_t bytes)
{
	char text[512];
	size_t written = 0;
	int n, fd;
	if (!node || !*node || bytes < PW3_REPORT_BYTES)
		return 0;
	const uint32_t boot = pw3_report_next_boot(node, area, bytes);
	n = snprintf(text, sizeof(text), "%s%s\n", PW3_REPORT_STOP, message);
	if (n <= 0 || (size_t)n >= sizeof(text))
		return 0;
	if (!pw3_report_build(area, bytes, text, (size_t)n, boot))
		return 0;
	fd = open(node, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return 0;
	while (written < PW3_REPORT_BYTES) {
		const ssize_t count = write(fd, area + written, PW3_REPORT_BYTES - written);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			break;
		written += (size_t)count;
	}
	/* A short write leaves an area whose checksum will not match, which every
	 * reader treats as no report -- so the failure is safe, and saying so is
	 * all that is left to do about it. */
	if (written == PW3_REPORT_BYTES && fsync(fd))
		written = 0;
	close(fd);
	return written == PW3_REPORT_BYTES;
}

#endif
