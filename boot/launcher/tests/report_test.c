/* SPDX-License-Identifier: GPL-2.0-only
 * The report area's first line, and what it takes to believe it. */
#include "ReportArea.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { printf("PW3_REPORT_FAIL line=%u\n", __LINE__); return 1; } } while (0)

static unsigned char area[PW3_REPORT_BYTES];

/* Lays out an area the way the writer will: the first line, then the text.
 * Returns 0 when the two do not fit, which is the writer's job to avoid and
 * this test's job not to pretend otherwise. */
static int write_area(const char *text, uint64_t boot, int break_crc, int wrong_length)
{
	const size_t bytes = strlen(text);
	uint32_t crc = pw3_report_crc32((const unsigned char *)text, bytes);
	int n;
	memset(area, 0, sizeof(area));
	if (break_crc)
		crc ^= 1u;
	n = snprintf((char *)area, PW3_REPORT_HEADER_MAX, "%sbytes=%zu crc32=%08x boot=%llu\n",
		     PW3_REPORT_MAGIC, wrong_length ? bytes + 1000000 : bytes, crc,
		     (unsigned long long)boot);
	if (n <= 0 || (size_t)n >= PW3_REPORT_HEADER_MAX || bytes > sizeof(area) - (size_t)n)
		return 0;
	memcpy(area + n, text, bytes);
	return 1;
}

int main(void)
{
	struct pw3_report r;
	const char *text = "boot=7 uptime_ms=41230\nsys.usb.config=adb\nsys.usb.state=adb\n";

	/* What the writer wrote is what the reader gets. */
	CHECK(write_area(text, 7, 0, 0));
	CHECK(pw3_report_parse(area, sizeof(area), &r));
	CHECK(r.bytes == strlen(text) && !memcmp(r.text, text, r.bytes) && r.boot == 7);

	/* An area no Android start has written yet: zeros, which is not a report
	 * and not an error either. */
	memset(area, 0, sizeof(area));
	CHECK(!pw3_report_parse(area, sizeof(area), &r));

	/* A checksum that does not match the text, a length that does not fit in
	 * the area, a missing field, and a first line with no end to it. */
	CHECK(write_area(text, 7, 1, 0));
	CHECK(!pw3_report_parse(area, sizeof(area), &r));
	CHECK(write_area(text, 7, 0, 1));
	CHECK(!pw3_report_parse(area, sizeof(area), &r));
	CHECK(write_area(text, 7, 0, 0));
	memcpy(area, "PW3-REPORT-1 bytes=", 19);
	memset(area + 19, 'x', 40);
	CHECK(!pw3_report_parse(area, sizeof(area), &r));
	memset(area, 'A', sizeof(area));
	CHECK(!pw3_report_parse(area, sizeof(area), &r));
	/* The magic, and then no newline within the bound: the first line has no
	 * end to it, which the all-'A' area above cannot show -- that one is
	 * refused at the magic, before there is a line to look for. */
	memset(area, 'x', sizeof(area));
	memcpy(area, PW3_REPORT_MAGIC, sizeof(PW3_REPORT_MAGIC) - 1);
	area[PW3_REPORT_HEADER_MAX + 8] = '\n';
	CHECK(!pw3_report_parse(area, sizeof(area), &r));
	/* A first line whose fields are right but whose boot number is not a
	 * count of Android starts: refused, so that no such number reaches the
	 * folder people hand on. */
	CHECK(write_area(text, 4294967295u, 0, 0));
	CHECK(pw3_report_parse(area, sizeof(area), &r) && r.boot == 4294967295u);
	CHECK(write_area(text, 4294967296u, 0, 0));
	CHECK(!pw3_report_parse(area, sizeof(area), &r));
	CHECK(write_area(text, 18446744073709551615ull, 0, 0));
	CHECK(!pw3_report_parse(area, sizeof(area), &r));

	/* Another project's file, or a book, that happens to sit at that name. */
	memset(area, 0, sizeof(area));
	memcpy(area, "This is not a report\n", 21);
	CHECK(!pw3_report_parse(area, sizeof(area), &r));

	/* An empty report is not one: there would be nothing to copy. */
	CHECK(write_area("", 9, 0, 0));
	CHECK(!pw3_report_parse(area, sizeof(area), &r));

	/* The text may fill exactly what the first line leaves of the area, and
	 * one byte more may not. Started at the whole area rather than at the
	 * bound on the first line, so the largest text this finds is the largest
	 * there is: write_area refuses the ones that do not fit, and the first
	 * that parses is therefore the maximum. */
	{
		char *big = malloc(PW3_REPORT_BYTES);
		size_t room;
		CHECK(big != NULL);
		memset(big, 'z', PW3_REPORT_BYTES);
		for (room = PW3_REPORT_BYTES - 1; room > 0; room--) {
			big[room] = 0;
			if (!write_area(big, 1, 0, 0))
				continue;
			if (pw3_report_parse(area, sizeof(area), &r)) break;
		}
		CHECK(room > PW3_REPORT_BYTES - PW3_REPORT_HEADER_MAX);
		CHECK(r.bytes == room && r.boot == 1);
		/* One byte more does not fit, and the writer says so rather than
		 * laying out an area whose text runs past it. */
		big[room] = 'z';
		big[room + 1] = 0;
		CHECK(!write_area(big, 1, 0, 0));
		/* And the two halves the device actually runs say so as well. The
		 * check above is about this test's own writer, which is not the
		 * claim worth making: asked for one byte more than fits,
		 * pw3_report_build has to refuse, and an area whose first line
		 * claims one byte more than the area holds has to be refused by
		 * pw3_report_parse rather than read past its end. */
		CHECK(!pw3_report_build(area, sizeof(area), big, room + 1, 1));
		{
			const int n = snprintf((char *)area, PW3_REPORT_HEADER_MAX,
					       "%sbytes=%zu crc32=%08x boot=1\n", PW3_REPORT_MAGIC, room + 1,
					       pw3_report_crc32((const unsigned char *)big, room + 1));
			CHECK(n > 0 && (size_t)n < PW3_REPORT_HEADER_MAX);
			memcpy(area + n, big, PW3_REPORT_BYTES - (size_t)n);
			CHECK(!pw3_report_parse(area, sizeof(area), &r));
		}
		free(big);
	}

	/* pw3_report_build, which is what Android writes the area with
	 * (device/amazon/pw3/initcheck/BootReport.cpp) and the other half of what
	 * pw3_report_parse reads. Checked both as a round trip and against the
	 * bytes this test's own writer lays out for a report it would accept, so
	 * that the adversary above goes on describing the format the device
	 * really produces rather than one of its own. */
	{
		static unsigned char built[PW3_REPORT_BYTES];
		size_t i; int zeros = 1;
		CHECK(pw3_report_build(built, sizeof(built), text, strlen(text), 7));
		CHECK(write_area(text, 7, 0, 0));
		CHECK(!memcmp(built, area, sizeof(built)));
		CHECK(pw3_report_parse(built, sizeof(built), &r));
		CHECK(r.bytes == strlen(text) && !memcmp(r.text, text, r.bytes) && r.boot == 7);
		/* Zeros past the text: the area carries this report and no part of
		 * any report before it. */
		memset(area, 'x', sizeof(area));
		CHECK(pw3_report_build(area, sizeof(area), text, strlen(text), 1));
		for (i = PW3_REPORT_HEADER_MAX + strlen(text); i < sizeof(area); i++)
			if (area[i])
				zeros = 0;
		CHECK(zeros);
		/* The count of starts at both ends of what it can be. */
		CHECK(pw3_report_build(built, sizeof(built), text, strlen(text), 0xffffffffu));
		CHECK(pw3_report_parse(built, sizeof(built), &r) && r.boot == 0xffffffffu);
		CHECK(pw3_report_build(built, sizeof(built), text, strlen(text), 0));
		CHECK(pw3_report_parse(built, sizeof(built), &r) && r.boot == 0);
		/* No text is no report; an area smaller than the format is not one
		 * either, and neither is a text that does not fit in it. */
		CHECK(!pw3_report_build(built, sizeof(built), text, 0, 1));
		CHECK(!pw3_report_build(built, PW3_REPORT_BYTES - 1, text, strlen(text), 1));
		CHECK(!pw3_report_build(built, sizeof(built), text, PW3_REPORT_BYTES, 1));
	}

	/* Whose file is at the area's name. The installer may write over one of
	 * ours and nothing else, and PID 1 maps only one of ours, so both sides ask
	 * this one question of the bytes. */
	{
		struct pw3_report_claim claim;
		static unsigned char made[PW3_REPORT_BYTES], other[PW3_REPORT_BYTES];
		size_t i;
		/* A file cut off inside the magic is one of ours cut off: the installer
		 * writes the area from the front, so a write that stopped early leaves
		 * exactly that. Asking for all thirteen bytes left such a file
		 * unrecognised for ever -- kept by the installer, refused by PID 1 for
		 * its size, the channel dead with no way back. */
		CHECK(pw3_report_build_made(made, sizeof(made), 1800000000u));
		for (i = 1; i <= PW3_REPORT_MAGIC_LEN; i++) {
			pw3_report_claim_begin(&claim);
			pw3_report_claim_feed(&claim, made, i);
			CHECK(pw3_report_claim_ours(&claim));
		}
		/* A whole area of ours, and an area of zeros, which is what installers
		 * before this made. */
		pw3_report_claim_begin(&claim);
		pw3_report_claim_feed(&claim, made, sizeof(made));
		CHECK(pw3_report_claim_ours(&claim));
		{
			static const unsigned char zeros[PW3_REPORT_BYTES];
			pw3_report_claim_begin(&claim);
			pw3_report_claim_feed(&claim, zeros, sizeof(zeros));
			CHECK(pw3_report_claim_ours(&claim));
			pw3_report_claim_begin(&claim);
			pw3_report_claim_feed(&claim, zeros, 0);
			CHECK(pw3_report_claim_ours(&claim));
		}
		/* Somebody's file is not, however short, and neither is one whose
		 * first bytes only resemble the magic. */
		pw3_report_claim_begin(&claim);
		pw3_report_claim_feed(&claim, (const unsigned char *)"PW3-REPORX", 10);
		CHECK(!pw3_report_claim_ours(&claim));
		pw3_report_claim_begin(&claim);
		pw3_report_claim_feed(&claim, (const unsigned char *)"q", 1);
		CHECK(!pw3_report_claim_ours(&claim));
		/* The area the installer makes is a report, told from a start's by its
		 * body and not by its count: the count is in the first line, which the
		 * body's checksum does not cover, so a bit turned over in a real
		 * report's `boot=1` used to make it a placeholder to every reader
		 * (install.c, read_report). The count is the installer's to choose,
		 * because the next start writes one more than it. */
		CHECK(pw3_report_parse(made, sizeof(made), &r) && r.boot == 1800000000u);
		CHECK(pw3_report_is_made(&r));
		CHECK(r.bytes == sizeof(PW3_REPORT_MADE) - 1 &&
		      !memcmp(r.text, PW3_REPORT_MADE, r.bytes));
		CHECK(pw3_report_boot_after(1800000000u) == 1800000001u);
		/* A start's report is not a placeholder, whatever its count says. */
		CHECK(pw3_report_build(other, sizeof(other), "sys.boot_completed=1\n",
				       sizeof("sys.boot_completed=1\n") - 1, 0));
		CHECK(pw3_report_parse(other, sizeof(other), &r) && !pw3_report_is_made(&r));
	}

	/* The one field of a body that a program rather than a person reads: the
	 * reason PID 1 refused to start, which the next tap acts on
	 * (installer/install/install.c). The body is a slice of a 64 KiB buffer and
	 * is not terminated, so what matters here is that nothing is read past the
	 * length -- an older, longer report's tail lies right behind it. */
	{
		static char body[PW3_REPORT_BYTES];
		char out[32];
		const char *one = "a=1\npreinit_stopped=refused\nb=2\n";
		CHECK(pw3_report_field(one, strlen(one), "preinit_stopped=", out, sizeof(out)) &&
		      !strcmp(out, "refused"));
		/* The first line, and the last line without its newline. */
		CHECK(pw3_report_field(one, strlen(one), "a=", out, sizeof(out)) && !strcmp(out, "1"));
		CHECK(pw3_report_field("x=9", 3, "x=", out, sizeof(out)) && !strcmp(out, "9"));
		/* Absent, and absent leaves the answer empty rather than whatever the
		 * caller's buffer held. */
		memcpy(out, "stale", 6);
		CHECK(!pw3_report_field(one, strlen(one), "c=", out, sizeof(out)) && !out[0]);
		/* Only at the start of a line: a field cannot be made out of the middle
		 * of a longer one, which is how a report's own text could otherwise
		 * claim a refusal. */
		CHECK(!pw3_report_field("see preinit_stopped=no\n", 23, "preinit_stopped=", out, sizeof(out)));
		/* Not past the length: the name is there, in the zeros behind the
		 * report, and must not be found. */
		memset(body, 0, sizeof(body));
		memcpy(body, "a=1\n", 4);
		memcpy(body + 4, "preinit_stopped=behind\n", 23);
		CHECK(!pw3_report_field(body, 4, "preinit_stopped=", out, sizeof(out)));
		CHECK(pw3_report_field(body, sizeof(body), "preinit_stopped=", out, sizeof(out)) &&
		      !strcmp(out, "behind"));
		/* A value longer than the caller's buffer is cut, not refused: the
		 * callers compare a beginning. */
		CHECK(pw3_report_field("preinit_stopped=0123456789abcdefghij\n", 37, "preinit_stopped=", out, 5) &&
		      !strcmp(out, "0123"));
		/* A line that is the name and nothing else is the field, with an empty
		 * value: both callers read an empty value as "not this", and "last"
		 * has to mean last. A reader that skipped empty values would answer
		 * with a reading two rounds old because the newest came out blank. */
		CHECK(pw3_report_field("preinit_stopped=\n", 17, "preinit_stopped=", out, sizeof(out)) && !out[0]);
		CHECK(pw3_report_field("sys.boot_completed=1\nsys.boot_completed=\n", 40,
				       "sys.boot_completed=", out, sizeof(out)) && !out[0]);
		CHECK(!pw3_report_field("", 0, "a=", out, sizeof(out)));
		/* The last line with that name, not the first. A report is four
		 * snapshots of one start, so a property appears once per round and the
		 * newest reading is the one that says how the start turned out. Taking
		 * the first would read sys.boot_completed as it was at uptime zero,
		 * which is unset on every start there has ever been -- and the stock
		 * side decides whether the applications finished compiling on exactly
		 * that line (install.c). */
		{
			const char *rounds = "snapshot=0\nsys.boot_completed=unset\n"
					     "snapshot=1\nsys.boot_completed=unset\n"
					     "snapshot=2\nsys.boot_completed=1\n";
			CHECK(pw3_report_field(rounds, strlen(rounds), "sys.boot_completed=", out, sizeof(out)) &&
			      !strcmp(out, "1"));
			CHECK(pw3_report_field(rounds, strlen(rounds), "snapshot=", out, sizeof(out)) &&
			      !strcmp(out, "2"));
			/* And the last one found is what is left in the buffer even when
			 * a later line of the same name is cut short by the length. */
			CHECK(pw3_report_field(rounds, 36, "sys.boot_completed=", out, sizeof(out)) &&
			      !strcmp(out, "unset"));
		}
	}

	/* A report of ours whose magic alone is wrong. PID 1 refuses to map such a
	 * file and the installer used to keep it, which left the report channel
	 * dead for good -- so it has to be told from somebody else's file of the
	 * same size, and from the file itself rather than from a record that one
	 * side made it (installer/install/install.c, ensure_report_area). */
	{
		static unsigned char built[PW3_REPORT_BYTES];
		size_t i;
		CHECK(pw3_report_build(built, sizeof(built), text, strlen(text), 12));
		/* Intact: a report, not a damaged one. */
		CHECK(!pw3_report_damaged_magic(built, sizeof(built)));
		CHECK(pw3_report_parse(built, sizeof(built), &r));
		/* Any one byte of the thirteen of the magic, and the rest still agrees. */
		for (i = 0; i < PW3_REPORT_MAGIC_LEN; i++) {
			built[i] ^= 0xff;
			CHECK(!pw3_report_parse(built, sizeof(built), &r));
			CHECK(pw3_report_damaged_magic(built, sizeof(built)));
			built[i] ^= 0xff;
		}
		/* A byte of the body is not this: the magic is intact, PID 1 maps the
		 * area, and the next start writes over it. */
		built[PW3_REPORT_HEADER_MAX] ^= 0xff;
		CHECK(!pw3_report_parse(built, sizeof(built), &r));
		CHECK(!pw3_report_damaged_magic(built, sizeof(built)));
		built[PW3_REPORT_HEADER_MAX] ^= 0xff;
		/* Somebody's file, and an area of zeros: neither is a damaged report.
		 * Imitating one means carrying a first line that parses and a CRC32
		 * over exactly the length that line names. */
		memset(built, 'q', sizeof(built));
		CHECK(!pw3_report_damaged_magic(built, sizeof(built)));
		memset(built, 0, sizeof(built));
		CHECK(!pw3_report_damaged_magic(built, sizeof(built)));
		for (i = 0; i < sizeof(built); i++)
			built[i] = (unsigned char)i;
		CHECK(!pw3_report_damaged_magic(built, sizeof(built)));
		/* A file that only imitates the first line is not one either: the
		 * text's checksum can be made to match in one byte, and everything
		 * behind it left as it was. Our own writer zeroes the whole area
		 * before writing, so a report of ours has nothing after its text --
		 * which is what refuses this. The header below is the real shape, with
		 * the real CRC32 of the one-byte body, under somebody else's
		 * thirteen-byte magic. */
		{
			static unsigned char forged[PW3_REPORT_BYTES];
			char line[64];
			const int n = snprintf(line, sizeof(line),
					       "OTHER-FORMAT bytes=1 crc32=%08x boot=1\nx", 0x8cdc1683u);
			CHECK(n > 0 && (size_t)n < sizeof(line));
			memset(forged, 'q', sizeof(forged));
			memcpy(forged, line, (size_t)n);
			CHECK(!pw3_report_damaged_magic(forged, sizeof(forged)));
			/* With the tail zeroed it has the shape of one of ours and is
			 * taken for one: the rule's work is done by the tail, not by the
			 * thirteen bytes, which are what was lost in the first place. */
			memset(forged + n, 0, sizeof(forged) - (size_t)n);
			CHECK(pw3_report_damaged_magic(forged, sizeof(forged)));
		}
		/* And a first line that parses with a checksum that does not match is
		 * not one either: that is the half-written area, which every reader
		 * already treats as no report. */
		CHECK(pw3_report_build(built, sizeof(built), text, strlen(text), 12));
		built[0] ^= 0xff;
		built[PW3_REPORT_HEADER_MAX + 1] ^= 0xff;
		CHECK(!pw3_report_damaged_magic(built, sizeof(built)));
	}

	/* The checksum is the one zlib and PNG compute: a known answer, so that a
	 * writer on the other side can be checked against something. */
	CHECK(pw3_report_crc32((const unsigned char *)"123456789", 9) == 0xcbf43926u);
	CHECK(pw3_report_crc32((const unsigned char *)"", 0) == 0u);

	printf("PW3_REPORT_OK checks=%u\n", checks);
	return 0;
}
