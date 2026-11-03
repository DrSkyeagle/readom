/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The installer's screens on the stock system (installer/README.md, "安装画面"): the text
 * of each screen was laid out in Chinese and English at build time
 * (installer/screens/render_screens.py: screens.h and the bitmaps in pw3_screens);
 * here a screen is put together with its progress bar or number, written as a PNG
 * and shown with the stock eips. The first screen clears what the library showed;
 * after that only changed pixels are updated, with GL16 (no flashing).
 */
#ifndef PW3_SCREEN_H
#define PW3_SCREEN_H

#include <limits.h>
#include <stdint.h>

#include "screens.h"

struct screen {
	const unsigned char *assets;	/* pw3_screens */
	char png[PATH_MAX];		/* where the picture is written */
	int cleared;
	unsigned char *glyphs;		/* the number glyphs, unpacked on first use */
};

/* dir: a private directory for the picture. */
void screen_init(struct screen *s, const unsigned char *assets, const char *dir);
/* permille: the progress bar's fill, or -1 for none; number_mb: shown for screens
 * with a number. 0 when shown. */
int screen_show(struct screen *s, enum pw3_screen id, int permille, uint64_t number_mb);
void screen_free(struct screen *s);
/* What screen_show() does before eips: the picture, at s->png. */
int screen_render(struct screen *s, enum pw3_screen id, int permille, uint64_t number_mb);

/* The PNG of a grey picture (no compression): for screen.c and its tests. */
int screen_write_png(const char *path, const unsigned char *grey, unsigned width, unsigned height);

#endif
