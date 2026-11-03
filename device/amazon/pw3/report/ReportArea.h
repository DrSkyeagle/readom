/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The report area: what Android leaves behind for the stock side to find.
 *
 * Why it exists is in docs/design/diagnostics.md. In short: a release
 * that misbehaves leaves nothing anybody can send. There is no adb on a user
 * build, the kernel's ring is 32 KB and the boot is overwritten within hours,
 * and user-space state -- sys.usb.config, init.svc.adbd, the refresh counters,
 * memory -- was never in that ring. The stock side cannot read the data image
 * either: this device's stock kernel has EXT2, EXT3 and loop, and no EXT4.
 *
 * So the area is a file on the FAT user store, `android/report.bin`, made by
 * the installer at a fixed size and never resized. Android writes it by sector
 * through a block device mapped from its own clusters, so no FAT structure is
 * touched by the second kernel -- the partition holds the owner's books. The
 * launcher then reads it as an ordinary file.
 *
 * The content is text, not a struct: both sides are ARM little-endian today,
 * but a text area needs no agreement about padding or byte order, is read by
 * whoever opens the file in an editor, and is checked by the same filter as the
 * kernel log. The first line declares what follows:
 *
 *     PW3-REPORT-1 bytes=<n> crc32=<8 hex> boot=<n>\n
 *     <n bytes of text, one field per line>
 *
 * The checksum is a CRC32 and not a SHA256 because the header is copied into a
 * folder people hand on, and the log filter drops any line with twelve hex
 * digits in a row (userlog.c, which is the point of that rule).
 *
 * `boot` counts the Android starts that have written a report. The launcher
 * copies whatever valid report it finds into the folder for that tap; two taps
 * with no Android start between them therefore copy the same report twice,
 * which is accurate -- the number says which start wrote it.
 */
#ifndef PW3_REPORT_AREA_H
#define PW3_REPORT_AREA_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PW3_REPORT_NAME "report.bin"
#define PW3_REPORT_BYTES 65536u          /* about 600 lines; one write, no growth */
#define PW3_REPORT_MAGIC "PW3-REPORT-1 "
#define PW3_REPORT_HEADER_MAX 96u        /* the first line, bounded */

/* The one line of a report that the stock side acts on rather than only files
 * away. PID 1 writes it when it refuses to start (device/amazon/pw3/preinit);
 * the next tap's installer reads it (installer/install/install.c). Its name
 * lives here because it is the only text in the area that two programs have to
 * spell the same way -- everything else in the body is written by one side and
 * read by people.
 */
#define PW3_REPORT_STOP "preinit_stopped="
/* What that line says when the system image on the store is not the image its
 * recorded hash says it is. It is the one refusal a tap can do something about:
 * install the image again, which is what the owner would otherwise have to do
 * with a computer and a file manager (docs/known-issues.md, 七). PID 1's own
 * sentence, matched by its beginning, so that the sentence can gain a detail
 * without the match breaking. */
#define PW3_REPORT_STOP_SYSTEM_IMAGE "test system image SHA256 rejected"
/* The second refusal that says the same thing about the image and asks for the
 * same answer: the superblock at offset 1024 is not the clean ext4 of the
 * recorded size that this release wrote. The hash finds that too and says so
 * first whenever it runs, but it does not always run -- a start whose stamp
 * still matches skips it (testfile_stamp_matches) -- and bit rot under an
 * unchanged size and mtime is exactly the case where the stamp matches and the
 * superblock does not. So this one is acted on as well. Matched by its
 * beginning, like the line above. */
#define PW3_REPORT_STOP_SYSTEM_FORMAT "test system image format rejected"
/* The other line a program reads: the property that says the start got all the
 * way through. The sampler is deliberately started on the earliest property
 * that means the storage is up, not on this one -- a start that never completes
 * is the start whose report is worth having (pw3-boot-report.rc) -- so a report
 * existing proves nothing about completion and this line has to be looked at.
 *
 * It matters to the stock side because the applications in the data image are
 * compiled in updatePackagesIfNeeded(), which is 700 lines before
 * systemReady() in SystemServer.startOtherServices(): this property set to 1 is
 * therefore proof that compiling is done, and nothing else in the report is.
 */
#define PW3_REPORT_BOOT_COMPLETED "sys.boot_completed="

#define PW3_REPORT_MAGIC_LEN (sizeof(PW3_REPORT_MAGIC) - 1)

/* Whether a file is a report area rather than somebody's file that happens to
 * be called that. Two things are and nothing else is: it begins with the magic
 * (Android wrote a report), or every byte of it is zero, which includes a file
 * of no bytes (made and never written). Every byte, not the first few: a file
 * whose first bytes are zeros and whose rest is somebody's data is somebody's
 * data.
 *
 * Fed a chunk at a time because the two callers meet the file differently --
 * the installer walks it to bail out of a large one early (install.c), and PID
 * 1 has all 64 KiB of it in a buffer (TestFileInit.h) -- and the rule they
 * apply has to be one rule. Neither caller lets a file longer than an area
 * reach this: a file that long cannot be one that was never written.
 */
struct pw3_report_claim {
	size_t seen;
	int blank;
	unsigned char head[PW3_REPORT_MAGIC_LEN];
};
static inline void pw3_report_claim_begin(struct pw3_report_claim *c)
{
	c->seen = 0;
	c->blank = 1;
	memset(c->head, 0, sizeof(c->head));
}
static inline void pw3_report_claim_feed(struct pw3_report_claim *c, const unsigned char *data, size_t bytes)
{
	for (size_t i = 0; i < bytes; i++) {
		if (c->seen < sizeof(c->head))
			c->head[c->seen] = data[i];
		if (data[i])
			c->blank = 0;
		c->seen++;
	}
}
static inline int pw3_report_claim_ours(const struct pw3_report_claim *c)
{
	/* The magic, or as much of it as a shorter file can hold. A file cut off
	 * inside the magic is one of ours cut off: the installer writes the area
	 * from the front, so a write that stopped early leaves exactly the first
	 * bytes of the magic and nothing else, and so does a file somebody
	 * truncated from a computer. Asking for all thirteen left such a file
	 * unrecognised for ever -- kept by the installer, refused by PID 1 for its
	 * size, and so the report channel dead with no way back. Zeros covered that
	 * case while a new area was zeros; one that says what it is has a magic to
	 * lose.
	 *
	 * Nothing is lost by it: a file this short holds nothing anybody wants, and
	 * PID 1 reaches this only for a file of exactly the area's size, where the
	 * comparison is the whole magic as before. */
	const size_t have = c->seen < sizeof(c->head) ? c->seen : sizeof(c->head);
	if (have && !memcmp(c->head, PW3_REPORT_MAGIC, have))
		return 1;
	return c->blank;
}

/* CRC-32 (the one zlib and PNG use), table-free: 32 bytes of state instead of
 * 1 KB, and this runs once per tap on at most 64 KB. */
static inline uint32_t pw3_report_crc32(const unsigned char *data, size_t bytes)
{
	uint32_t crc = 0xffffffffu;
	for (size_t i = 0; i < bytes; i++) {
		crc ^= data[i];
		for (unsigned bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)(-(int32_t)(crc & 1u)));
	}
	return ~crc;
}

/* A decimal field of the first line: `name=` followed by digits, worth at most
 * `max`. Every field here has a meaning that bounds it -- a length cannot
 * exceed the area, a count of Android starts cannot exceed what a device can
 * do -- so the bound is the caller's and is checked while the digits are read:
 * nothing accumulates past it, which is why there is no overflow arithmetic
 * here to get wrong. Returns 0 when the field is absent, is not a number, or
 * is worth more than `max`. */
static inline int pw3_report_number(const char *line, size_t len, const char *name, uint64_t max, uint64_t *value)
{
	const size_t n = strlen(name);
	for (size_t i = 0; i + n < len; i++) {
		if (memcmp(line + i, name, n) || (i && line[i - 1] != ' '))
			continue;
		size_t at = i + n;
		uint64_t got = 0;
		unsigned digits = 0;
		while (at < len && line[at] >= '0' && line[at] <= '9') {
			got = got * 10 + (uint64_t)(line[at++] - '0');
			if (got > max)
				return 0;
			digits++;
		}
		if (!digits)
			return 0;
		*value = got;
		return 1;
	}
	return 0;
}

/* The same, for the 8 hex digits of the checksum. */
static inline int pw3_report_hex32(const char *line, size_t len, const char *name, uint32_t *value)
{
	const size_t n = strlen(name);
	for (size_t i = 0; i + n < len; i++) {
		if (memcmp(line + i, name, n) || (i && line[i - 1] != ' '))
			continue;
		size_t at = i + n;
		uint32_t got = 0;
		unsigned digits = 0;
		while (at < len && digits < 8) {
			const char c = line[at];
			unsigned v;
			if (c >= '0' && c <= '9') v = (unsigned)(c - '0');
			else if (c >= 'a' && c <= 'f') v = (unsigned)(c - 'a') + 10u;
			else break;
			got = (got << 4) | v;
			++digits; ++at;
		}
		if (digits != 8 || (at < len && line[at] != ' ' && line[at] != '\n'))
			return 0;
		*value = got;
		return 1;
	}
	return 0;
}

/* What a report says, once its first line has been believed. `boot` is 32 bits
 * because it counts Android starts on one Kindle: 4,294,967,295 of them is
 * already past anything the hardware can do, and the bound is what keeps this
 * number short. That matters because it is the one value out of the area that
 * the launcher writes into the folder people hand on without the line filter
 * having anything to say about it -- ten digits cannot be the twelve-digit run
 * of hex the filter exists to catch (userlog.c, identifier). */
struct pw3_report {
	const char *text;	/* inside the caller's buffer; not terminated */
	size_t bytes;
	uint32_t boot;
};

/* The count the next report carries, given the count in the one before it.
 *
 * It wraps to 1 rather than sticking at the ceiling. A reader acts on "this
 * number is not the one I accounted for", so a count that stops changing is a
 * device that is never heard from again: a damaged system image would stay
 * damaged and the compiling notice would never clear. Four billion starts is
 * past anything this hardware can do, so the ceiling is not reached by
 * counting -- it is reached by a writer that put a sentinel in the field, which
 * is what BootReport.cpp did with "could not read the previous count" until
 * 2026-10-21. Wrapping costs one report misread as already-seen, once, in
 * exchange for the device going on speaking.
 */
static inline uint32_t pw3_report_boot_after(uint32_t previous)
{
	return previous == 0xffffffffu ? 1u : previous + 1u;
}

/* Reads the area a tap found. `area` is PW3_REPORT_BYTES of the file as it
 * stands; an area of zeros (no Android start has written one yet) is not a
 * report and not an error. Returns 1 only when the first line parses, the
 * length fits what is left, and the checksum of that much text matches. */
/* Everything a report has to be except the magic: a first line that parses, a
 * length that fits what is left, and a body whose checksum is the one that line
 * declares. Separate from the magic so that a file whose *only* fault is the
 * magic can be recognised -- see pw3_report_damaged_magic. */
static inline int pw3_report_body(const unsigned char *area, size_t bytes, struct pw3_report *out)
{
	const char *line = (const char *)area;
	size_t end = 0;
	uint64_t declared = 0, boot = 0;
	uint32_t crc = 0;

	if (bytes < sizeof(PW3_REPORT_MAGIC))
		return 0;
	while (end < bytes && end < PW3_REPORT_HEADER_MAX && line[end] != '\n')
		end++;
	if (end >= bytes || end >= PW3_REPORT_HEADER_MAX || line[end] != '\n')
		return 0;
	if (!pw3_report_number(line, end, "bytes=", PW3_REPORT_BYTES, &declared) ||
	    !pw3_report_hex32(line, end, "crc32=", &crc) ||
	    !pw3_report_number(line, end, "boot=", 0xffffffffu, &boot))
		return 0;
	const size_t start = end + 1;
	if (declared == 0 || declared > bytes - start)
		return 0;
	if (pw3_report_crc32(area + start, (size_t)declared) != crc)
		return 0;
	out->text = (const char *)area + start;
	out->bytes = (size_t)declared;
	out->boot = (uint32_t)boot;
	return 1;
}

static inline int pw3_report_parse(const unsigned char *area, size_t bytes, struct pw3_report *out)
{
	if (bytes < sizeof(PW3_REPORT_MAGIC) || memcmp(area, PW3_REPORT_MAGIC, PW3_REPORT_MAGIC_LEN))
		return 0;
	return pw3_report_body(area, bytes, out);
}

/* A report of ours whose magic alone is wrong: the first line still parses and
 * the body's checksum is still the one that line declares. Nothing but a report
 * this project wrote looks like that -- somebody else's file would have to
 * carry a parseable header and a CRC32 over exactly the length it names -- so
 * this is what tells "our area, damaged" from "somebody's file at that name",
 * and it does so from the file itself rather than from a record that one of the
 * two sides made it.
 *
 * One byte lost out of the thirteen of the magic is the whole reason this
 * exists: PID 1 then refuses to map the file (pw3_report_claim_ours in
 * TestFileInit.h) and the installer used to keep it because its size was right,
 * which left the report channel dead for good -- no report ever written again,
 * so the compiling notice could never clear and a damaged system image could
 * never be found. The installer empties such a file (install.c).
 *
 * A byte lost anywhere else needs nothing: the magic is then intact, PID 1 maps
 * the area, and the next start writes over it.
 *
 * Asked the direct way: the magic is put back, what is left is asked whether it
 * is a report, and the caller's bytes are then as they were. That is why `area`
 * is not const. Reading the rest of the first line past a damaged magic would
 * not do -- the magic's last byte is the space that separates it from `bytes=`,
 * and a reader that lost that space finds no fields at all, so one of the
 * thirteen bytes would go unrecognised. The buffer belongs to the caller, which
 * read the file into it and is about to empty or keep that file.
 */
static inline int pw3_report_damaged_magic(unsigned char *area, size_t bytes)
{
	unsigned char saved[PW3_REPORT_MAGIC_LEN];
	struct pw3_report found;
	size_t at;
	int restored;
	if (bytes < sizeof(PW3_REPORT_MAGIC))
		return 0;
	if (!memcmp(area, PW3_REPORT_MAGIC, PW3_REPORT_MAGIC_LEN))
		return 0;	/* the magic is there, so this is a report and not a damaged one */
	memcpy(saved, area, sizeof(saved));
	memcpy(area, PW3_REPORT_MAGIC, PW3_REPORT_MAGIC_LEN);
	restored = pw3_report_body(area, bytes, &found);
	memcpy(area, saved, sizeof(saved));
	if (!restored)
		return 0;
	/* And nothing at all after the text. pw3_report_build zeroes the whole
	 * area before it writes into it, so a report of ours always has zeros from
	 * the end of its text to the end of the file; a file that does not is not
	 * one of ours however well its first line reads.
	 *
	 * Without this the test asks too little: thirteen bytes of anything, then
	 * a line of our header's shape declaring one byte of body, then a byte
	 * whose CRC32 matches -- and the remaining 65 kilobytes could be somebody's
	 * data, which this would have had the installer empty. */
	at = (size_t)(found.text - (const char *)area) + found.bytes;
	for (; at < bytes; at++)
		if (area[at])
			return 0;
	return 1;
}

/* The value of a one-line field of a report body: `name` at the start of a
 * line, the rest of that line copied into `out` and terminated there. Returns 0
 * when no line begins with the name, and leaves `out` empty.
 *
 * The **last** such line, not the first. A report is a series of snapshots of
 * one start -- the sampler takes four over two minutes and writes them all --
 * so a property appears once per round and the newest reading is the one that
 * describes how the start turned out. Taking the first would read
 * `sys.boot_completed` as it was at uptime zero, which is unset on every start
 * there has ever been.
 *
 * A line that is the name and nothing else counts as the field, with an empty
 * value, and so replaces an earlier one. "Last" has to mean last: a reader
 * that skipped empty values would answer with a reading two rounds old because
 * the newest one came out blank, and both callers treat an empty value as "not
 * this" (a stop reason nobody wrote, a completion nobody reached).
 *
 * Walked a character at a time rather than with strstr or strchr because the
 * body is a slice of the caller's area buffer and is not terminated: the area
 * is 64 KiB of which the report is the first `bytes`, and the rest is zeros
 * from an older, longer report that this must not read into. A name is only
 * matched at the start of a line so that a field cannot be forged by a longer
 * line that happens to contain it. A value longer than `out` is cut, which is
 * the caller's bound and not an error -- the callers compare a beginning.
 */
static inline int pw3_report_field(const char *text, size_t bytes, const char *name,
				   char *out, size_t cap)
{
	const size_t n = strlen(name);
	size_t at = 0;
	int found = 0;
	if (!cap)
		return 0;
	out[0] = 0;
	while (at < bytes) {
		size_t end = at;
		while (end < bytes && text[end] != '\n')
			end++;
		if (end - at >= n && !memcmp(text + at, name, n)) {
			size_t take = end - at - n;
			if (take > cap - 1)
				take = cap - 1;
			memcpy(out, text + at + n, take);
			out[take] = 0;
			found = 1;
		}
		at = end + 1;
	}
	return found;
}

/* The body the installer puts in an area it has just made.
 *
 * An area has to be recognisable as ours from its own bytes: that is what lets
 * a damaged one be repaired rather than left for dead
 * (pw3_report_damaged_magic), and 64 KiB of zeros is recognisable as nothing at
 * all -- one byte turned over in it leaves a file neither side will touch and
 * PID 1 will not map, which is the report channel dead for good before Android
 * has written to it even once. So a new area is a report that says what it is.
 *
 * A placeholder is told from a start's report by this line and by nothing else.
 * It used to be told by its count, which was fixed at 0 -- but the count is in
 * the first line, which the body's checksum does not cover, so one bit turned
 * over in a real report's `boot=1` made it a placeholder to every reader, and
 * the installer gave up the count it had acted on because of it. The body is
 * what the checksum protects, so the body is what says whose report this is.
 */
#define PW3_REPORT_MADE "installer_made=1\n"
#define PW3_REPORT_MADE_FIELD "installer_made="

/* Whether a parsed report is one the installer made rather than one a start
 * wrote. Takes the parsed body, so it is asked of text whose checksum has
 * already been verified. */
static inline int pw3_report_is_made(const struct pw3_report *r)
{
	char value[8];
	return pw3_report_field(r->text, r->bytes, PW3_REPORT_MADE_FIELD, value, sizeof(value)) &&
	       !strcmp(value, "1");
}

/* Makes the area Android leaves behind: the first line, the text, and zeros to
 * the end. Here rather than in the writer so that the format has one place
 * that writes it and one that reads it, in the same file, and the round trip
 * is a test (boot/launcher/tests/report_test.c) instead of two pieces of code
 * agreeing by inspection.
 *
 * The whole area is written every time, zeros included: a shorter write would
 * leave the tail of an older report behind the new one's length, and a partial
 * area is the one thing a checksum cannot describe.
 *
 * The text has to fit under PW3_REPORT_HEADER_MAX rather than under the first
 * line's actual length, which is the bound the caller collects text with
 * anyway. The length is checked before the text is read: a caller that got its
 * own bound wrong is refused rather than walked past the end of (the test
 * found this by asking for it). */
static inline int pw3_report_build(unsigned char *area, size_t cap, const char *text,
				   size_t bytes, uint32_t boot)
{
	char head[PW3_REPORT_HEADER_MAX];
	uint32_t crc;
	int n;
	if (cap < PW3_REPORT_BYTES || !bytes || bytes > cap - PW3_REPORT_HEADER_MAX)
		return 0;
	crc = pw3_report_crc32((const unsigned char *)text, bytes);
	n = snprintf(head, sizeof(head), "%sbytes=%zu crc32=%08x boot=%u\n",
		     PW3_REPORT_MAGIC, bytes, crc, boot);
	if (n <= 0 || (size_t)n >= sizeof(head))
		return 0;
	memset(area, 0, cap);
	memcpy(area, head, (size_t)n);
	memcpy(area + n, text, bytes);
	return 1;
}

/* An area as the installer makes it: a report of no start, saying so, carrying
 * the count the installer chooses.
 *
 * The count is an input because the next start's report is this count plus one
 * (pw3_report_boot_after, read by BootReport.cpp through PreviousBoot), so
 * whoever makes the area decides where the sequence that follows it begins.
 * The installer plants a number there that no earlier report can hold, which is
 * how a start of this install is told from a report of the same system kept on
 * a computer since the last time it was installed: ordering alone cannot say
 * that, because a count restarts whenever an area is made again.
 *
 * A placeholder's own count is never read as a start's: pw3_report_is_made
 * answers that from the body. */
static inline int pw3_report_build_made(unsigned char *area, size_t cap, uint32_t boot)
{
	return pw3_report_build(area, cap, PW3_REPORT_MADE, sizeof(PW3_REPORT_MADE) - 1, boot);
}

#endif
