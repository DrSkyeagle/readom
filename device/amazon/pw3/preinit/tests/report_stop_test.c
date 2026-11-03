// SPDX-License-Identifier: GPL-2.0-only
/*
 * What PID 1 writes into the report area when it refuses to start
 * (device/amazon/pw3/report/ReportStop.h).
 *
 * This is the path that only runs when something has already gone wrong, so
 * without a test it would first be executed on somebody's Kindle -- and the
 * failure it is reporting would then be joined by whatever this did wrong. The
 * device gives it a 64 KiB block device mapped from the clusters of a file on
 * the owner's store; here it gets an ordinary file of the same size, which
 * exercises everything except that the clusters are the right ones (the
 * mapping is the `fat-cross-link` and `file-mapper` tests' subject).
 *
 * What matters, in order: the reason comes back out; the count of starts goes
 * up by one each time so the stock side can tell a new refusal from the one it
 * already acted on; the count does not wrap at its ceiling, because the reader
 * acts on "greater than what I have accounted for" and a wrapped count would
 * hide every refusal after it; and no failure here hangs, because PID 1 is on
 * its way to a reboot that is the owner's way back.
 */
#include "../../report/ReportStop.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

static unsigned checks;
#define CHECK(x) do { checks++; if (!(x)) { printf("PW3_REPORT_STOP_FAIL line=%d\n", __LINE__); return 1; } } while (0)

static unsigned char area[PW3_REPORT_BYTES];
static unsigned char seen[PW3_REPORT_BYTES];
static char path[256];

/* The area as it stands on disk, and what a reader makes of it. */
static int read_back(struct pw3_report *out)
{
	size_t got = 0;
	FILE *f = fopen(path, "rb");
	if (!f)
		return 0;
	got = fread(seen, 1, sizeof(seen), f);
	fclose(f);
	return pw3_report_parse(seen, got, out);
}

/* An area of zeros of the size the installer makes, which is the state of a
 * device whose Android has never written a report. */
static int make(size_t bytes)
{
	static const unsigned char zeros[4096];
	FILE *f = fopen(path, "wb");
	if (!f)
		return 0;
	for (size_t at = 0; at < bytes; at += sizeof(zeros)) {
		const size_t take = bytes - at < sizeof(zeros) ? bytes - at : sizeof(zeros);
		if (fwrite(zeros, 1, take, f) != take) {
			fclose(f);
			return 0;
		}
	}
	return fclose(f) == 0;
}

int main(void)
{
	struct pw3_report r;
	char value[256];

	snprintf(path, sizeof(path), "/tmp/pw3-report-stop-%ld.bin", (long)getpid());
	CHECK(make(PW3_REPORT_BYTES));

	/* An area of zeros is the state of a device whose Android has never
	 * written a report, and it is where the first refusal lands. */
	CHECK(pw3_report_write_stop(path, "test system image SHA256 rejected", area, sizeof(area)));
	CHECK(read_back(&r) && r.boot == 1);
	CHECK(pw3_report_field(r.text, r.bytes, PW3_REPORT_STOP, value, sizeof(value)) &&
	      !strcmp(value, "test system image SHA256 rejected"));
	/* The installer matches the beginning of that sentence, so the beginning
	 * is what has to survive the round trip, sentence and name together. */
	CHECK(!strncmp(value, PW3_REPORT_STOP_SYSTEM_IMAGE, strlen(PW3_REPORT_STOP_SYSTEM_IMAGE)));

	/* A second refusal is a different start, and the stock side tells them
	 * apart by the count alone. */
	CHECK(pw3_report_write_stop(path, "writable multi-extent test data map rejected", area, sizeof(area)));
	CHECK(read_back(&r) && r.boot == 2);
	CHECK(pw3_report_field(r.text, r.bytes, PW3_REPORT_STOP, value, sizeof(value)) &&
	      !strcmp(value, "writable multi-extent test data map rejected"));
	/* And nothing of the longer report before it is left behind the shorter
	 * one: the whole area is written every time. */
	CHECK(r.bytes == strlen(PW3_REPORT_STOP) + strlen("writable multi-extent test data map rejected") + 1);

	/* A report Android wrote: the refusal after it is the next start, not the
	 * first one. This is the ordinary case on a device that has been working. */
	CHECK(pw3_report_build(area, sizeof(area), "sys.boot_completed=1\n", 21, 40));
	{
		FILE *f = fopen(path, "wb");
		CHECK(f && fwrite(area, 1, sizeof(area), f) == sizeof(area) && fclose(f) == 0);
	}
	CHECK(pw3_report_write_stop(path, "a reason", area, sizeof(area)));
	CHECK(read_back(&r) && r.boot == 41);

	/* The ceiling wraps to 1 instead of staying put. The reader acts on "this
	 * number is not the one I accounted for", so a count that stops changing
	 * is a device that is never heard from again -- a damaged system image
	 * would stay damaged for the rest of its life. Four billion starts is past
	 * anything this hardware can do, so the only way the ceiling is reached is
	 * a writer that put a sentinel in the field, which is what the Android-side
	 * writer did for "could not read the previous count" until 2026-10-21
	 * (BootReport.cpp). Wrapping unsticks such an area on the next refusal. */
	CHECK(pw3_report_build(area, sizeof(area), "x\n", 2, 0xffffffffu));
	{
		FILE *f = fopen(path, "wb");
		CHECK(f && fwrite(area, 1, sizeof(area), f) == sizeof(area) && fclose(f) == 0);
	}
	CHECK(pw3_report_write_stop(path, "a reason", area, sizeof(area)));
	CHECK(read_back(&r) && r.boot == 1);
	/* And the count carried on from anywhere else is simply the next one. */
	CHECK(pw3_report_boot_after(0) == 1 && pw3_report_boot_after(1) == 2 &&
	      pw3_report_boot_after(0xfffffffeu) == 0xffffffffu && pw3_report_boot_after(0xffffffffu) == 1);

	/* An area that is not a report at all -- half a header, somebody's file --
	 * is not read as one, so the refusal is start 1 and not a number taken out
	 * of whatever was there. */
	{
		FILE *f = fopen(path, "wb");
		int ok = f != NULL;
		memset(area, 'q', sizeof(area));
		memcpy(area, "PW3-REPORT-1 bytes=nonsense", 27);
		if (ok)
			ok = fwrite(area, 1, sizeof(area), f) == sizeof(area) && fclose(f) == 0;
		CHECK(ok);
	}
	CHECK(pw3_report_write_stop(path, "a reason", area, sizeof(area)));
	CHECK(read_back(&r) && r.boot == 1);

	/* Every way this can fail returns instead of hanging or writing something
	 * a reader would believe. */
	CHECK(!pw3_report_write_stop("/pw3-no-such-path/report.bin", "a reason", area, sizeof(area)));
	CHECK(!pw3_report_write_stop("", "a reason", area, sizeof(area)));
	CHECK(!pw3_report_write_stop(NULL, "a reason", area, sizeof(area)));
	/* A buffer smaller than the area: refused rather than written short. */
	CHECK(!pw3_report_write_stop(path, "a reason", area, PW3_REPORT_BYTES - 1));
	/* A sentence longer than the line this builds: refused, and the area is
	 * left holding whatever it held, which a reader can still use. */
	{
		char huge[700];
		memset(huge, 'r', sizeof(huge) - 1);
		huge[sizeof(huge) - 1] = 0;
		CHECK(make(PW3_REPORT_BYTES));
		CHECK(pw3_report_write_stop(path, "a reason", area, sizeof(area)));
		CHECK(!pw3_report_write_stop(path, huge, area, sizeof(area)));
		CHECK(read_back(&r) && r.boot == 1 &&
		      pw3_report_field(r.text, r.bytes, PW3_REPORT_STOP, value, sizeof(value)) &&
		      !strcmp(value, "a reason"));
	}
	/* A file shorter than the area is the shape a truncated one has: the write
	 * cannot complete, so it reports that it did not. On the device the node is
	 * a block device of exactly this size and cannot be short, which is what
	 * the installer's ensure_report_area is for -- this is the belt. */
	CHECK(make(PW3_REPORT_BYTES));
	CHECK(pw3_report_write_stop(path, "a reason", area, sizeof(area)));
	CHECK(truncate(path, PW3_REPORT_BYTES / 2) == 0);
	{
		/* An ordinary file simply grows, so being short is not itself an
		 * error here; what must hold is that the area is a whole report
		 * afterwards either way. */
		CHECK(pw3_report_write_stop(path, "another reason", area, sizeof(area)));
		CHECK(read_back(&r) &&
		      pw3_report_field(r.text, r.bytes, PW3_REPORT_STOP, value, sizeof(value)) &&
		      !strcmp(value, "another reason"));
	}
	remove(path);

	printf("PW3_REPORT_STOP_OK checks=%u\n", checks);
	return 0;
}
