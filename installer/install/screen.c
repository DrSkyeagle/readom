/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "screen.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "screens-data.h"
#include "zstd.h"

#define EIPS "/usr/sbin/eips"

/* ---- PNG: 8-bit grey, deflate's stored blocks (the file goes to tmpfs and eips at once) ---- */

static uint32_t crc_table[256];

static uint32_t crc32_update(uint32_t crc, const unsigned char *p, size_t n)
{
	if (!crc_table[1])
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t c = i;
			for (int k = 0; k < 8; k++)
				c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
			crc_table[i] = c;
		}
	crc = ~crc;
	while (n--)
		crc = crc_table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
	return ~crc;
}

struct out {
	FILE *f;
	uint32_t crc, a, b;	/* chunk CRC; Adler-32 halves */
};

static void be32(unsigned char *p, uint32_t v)
{
	p[0] = (unsigned char)(v >> 24);
	p[1] = (unsigned char)(v >> 16);
	p[2] = (unsigned char)(v >> 8);
	p[3] = (unsigned char)v;
}

static void put(struct out *o, const void *data, size_t n)
{
	fwrite(data, 1, n, o->f);
	o->crc = crc32_update(o->crc, data, n);
}

static void chunk_start(struct out *o, const char *type, uint32_t length)
{
	unsigned char head[4];
	be32(head, length);
	fwrite(head, 1, 4, o->f);
	o->crc = 0;
	put(o, type, 4);
}

static void chunk_end(struct out *o)
{
	unsigned char crc[4];
	be32(crc, o->crc);
	fwrite(crc, 1, 4, o->f);
}

/* Image bytes into the zlib stream: Adler-32 over them, in stored blocks of at most 65535. */
static void raw(struct out *o, const unsigned char *p, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		o->a = (o->a + p[i]) % 65521;
		o->b = (o->b + o->a) % 65521;
	}
	put(o, p, n);
}

int screen_write_png(const char *path, const unsigned char *grey, unsigned width, unsigned height)
{
	static const unsigned char signature[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
	const uint64_t row = (uint64_t)width + 1, total = row * height;	/* each row: filter 0, then the pixels */
	const uint64_t blocks = (total + 65534) / 65535;
	const uint64_t idat = 2 + blocks * 5 + total + 4;
	if (!width || !height || idat > 0x7fffffffu) {
		errno = EINVAL;
		return -1;
	}
	struct out o = { .f = fopen(path, "we"), .a = 1 };
	if (!o.f)
		return -1;
	fwrite(signature, 1, 8, o.f);
	unsigned char ihdr[13] = { 0 };
	be32(ihdr, width);
	be32(ihdr + 4, height);
	ihdr[8] = 8;	/* bits per sample; colour type 0 (grey), deflate, no filtering, no interlace */
	chunk_start(&o, "IHDR", 13);
	put(&o, ihdr, 13);
	chunk_end(&o);

	chunk_start(&o, "IDAT", (uint32_t)idat);
	put(&o, "\x78\x01", 2);
	uint64_t done = 0, in_block = 0;
	unsigned y = 0, x = 0;	/* x: 0 is the filter byte, 1..width the pixels */
	while (done < total) {
		if (!in_block) {
			uint64_t len = total - done < 65535 ? total - done : 65535;
			unsigned char head[5] = { (unsigned char)(done + len == total), (unsigned char)len, (unsigned char)(len >> 8),
						  (unsigned char)~len, (unsigned char)(~len >> 8) };
			put(&o, head, 5);
			in_block = len;
		}
		if (x == 0) {
			static const unsigned char filter = 0;
			raw(&o, &filter, 1);
			x = 1;
			done++;
			in_block--;
			continue;
		}
		uint64_t n = width - (x - 1);
		if (n > in_block)
			n = in_block;
		raw(&o, grey + (uint64_t)y * width + (x - 1), (size_t)n);
		x += (unsigned)n;
		done += n;
		in_block -= n;
		if (x > width) {
			x = 0;
			y++;
		}
	}
	unsigned char adler[4];
	be32(adler, (o.b << 16) | o.a);
	put(&o, adler, 4);
	chunk_end(&o);
	chunk_start(&o, "IEND", 0);
	chunk_end(&o);
	int bad = ferror(o.f);
	if (fclose(o.f) || bad)
		return -1;
	return 0;
}

/* ---- composing ---- */

static int unpack(const unsigned char *assets, const struct pw3_bitmap *b, unsigned char *out)
{
	size_t size = (size_t)b->width * b->height;
	size_t got = ZSTD_decompress(out, size, assets + b->offset, b->packed);
	return ZSTD_isError(got) || got != size ? -1 : 0;
}

static void fill(unsigned char *region, int x, int y, int w, int h, unsigned char v)
{
	for (int r = y; r < y + h; r++)
		memset(region + (size_t)r * PW3_SCREEN_REGION_W + x, v, (size_t)w);
}

static int draw_number(struct screen *s, unsigned char *region, int y, const char *text)
{
	const struct pw3_bitmap *g = &pw3_screen_glyphs;
	if (!s->glyphs) {
		s->glyphs = malloc((size_t)g->width * g->height);
		if (!s->glyphs || unpack(s->assets, g, s->glyphs)) {
			free(s->glyphs);
			s->glyphs = NULL;
			return -1;
		}
	}
	if (y + g->height > PW3_SCREEN_REGION_H)
		return -1;
	int x = 0;
	for (const char *c = text; *c; c++) {
		const char *at = strchr(pw3_screen_glyph_chars, *c);
		if (!at)
			return -1;
		int i = (int)(at - pw3_screen_glyph_chars), w = pw3_screen_glyph_w[i];
		if (x + w > PW3_SCREEN_REGION_W)
			return -1;
		for (int r = 0; r < g->height; r++)
			memcpy(region + (size_t)(y + r) * PW3_SCREEN_REGION_W + x,
			       s->glyphs + (size_t)r * g->width + pw3_screen_glyph_x[i], (size_t)w);
		x += w;
	}
	return 0;
}

static int run_eips(char *const argv[])
{
	pid_t pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		int null = open("/dev/null", O_WRONLY);
		if (null >= 0)
			dup2(null, STDOUT_FILENO);
		alarm(15);
		execv(EIPS, argv);
		_exit(127);
	}
	int status;
	pid_t got;
	do
		got = waitpid(pid, &status, 0);
	while (got < 0 && errno == EINTR);
	return got == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

void screen_init(struct screen *s, const unsigned char *assets, const char *dir)
{
	memset(s, 0, sizeof(*s));
	s->assets = assets;
	snprintf(s->png, sizeof(s->png), "%s/screen.png", dir);
}

void screen_free(struct screen *s)
{
	free(s->glyphs);
	s->glyphs = NULL;
	unlink(s->png);
}

int screen_render(struct screen *s, enum pw3_screen id, int permille, uint64_t number_mb)
{
	if ((unsigned)id >= PW3_SCREEN_COUNT)
		return -1;
	const struct pw3_bitmap *t = &pw3_screen_text[id];
	unsigned char *region = malloc((size_t)PW3_SCREEN_REGION_W * PW3_SCREEN_REGION_H);
	unsigned char *text = malloc((size_t)t->width * t->height);
	int ok = region && text && t->width == PW3_SCREEN_REGION_W && !unpack(s->assets, t, text);
	if (ok) {
		memset(region, 0xff, (size_t)PW3_SCREEN_REGION_W * PW3_SCREEN_REGION_H);
		memcpy(region, text, (size_t)t->width * t->height);
		int y = t->height;
		char number[32] = "";
		if (pw3_screen_progress[id] && permille >= 0) {
			if (permille > 1000)
				permille = 1000;
			y += PW3_SCREEN_BAR_GAP;
			const int line = PW3_SCREEN_BAR_LINE, w = PW3_SCREEN_REGION_W, h = PW3_SCREEN_BAR_H;
			fill(region, 0, y, w, h, 0);
			fill(region, line, y + line, w - 2 * line, h - 2 * line, 0xff);
			int inner = w - 2 * line, filled = inner * permille / 1000;
			if (filled)
				fill(region, line, y + line, filled, h - 2 * line, 0);
			y += h;
			snprintf(number, sizeof(number), "%d%%", permille / 10);
		} else if (pw3_screen_number[id])
			snprintf(number, sizeof(number), "%llu MB", (unsigned long long)number_mb);
		if (number[0])
			ok = !draw_number(s, region, y + PW3_SCREEN_NUMBER_GAP, number);
	}
	if (ok)
		ok = !screen_write_png(s->png, region, PW3_SCREEN_REGION_W, PW3_SCREEN_REGION_H);
	free(text);
	free(region);
	return ok ? 0 : -1;
}

int screen_show(struct screen *s, enum pw3_screen id, int permille, uint64_t number_mb)
{
	if (screen_render(s, id, permille, number_mb))
		return -1;
	if (!s->cleared) {
		char *clear[] = { EIPS, "-c", NULL };
		run_eips(clear);
		s->cleared = 1;
	}
	char x[16], y[16];
	snprintf(x, sizeof(x), "%d", PW3_SCREEN_REGION_X);
	snprintf(y, sizeof(y), "%d", PW3_SCREEN_REGION_Y);
	char *show[] = { EIPS, "-g", s->png, "-x", x, "-y", y, "-w", "gl16", NULL };
	return run_eips(show);
}
