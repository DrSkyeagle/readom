/* SPDX-License-Identifier: GPL-2.0-only */
/* Renders one installer screen as screen.c does on the device, for test_screen.py:
 *   test_screen SCREENS_BIN DIR NAME PERMILLE NUMBER_MB   -> DIR/screen.png */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "screen.h"
#include "screens-data.h"

int main(int argc, char **argv)
{
	if (argc != 6)
		return 2;
	FILE *f = fopen(argv[1], "rb");
	static unsigned char assets[PW3_SCREENS_BYTES];
	if (!f || fread(assets, 1, sizeof(assets), f) != sizeof(assets))
		return 2;
	fclose(f);
	int id = -1;
	for (int i = 0; i < PW3_SCREEN_COUNT; i++)
		if (!strcmp(pw3_screen_names[i], argv[3]))
			id = i;
	if (id < 0)
		return 2;
	struct screen s;
	screen_init(&s, assets, argv[2]);
	int r = screen_render(&s, (enum pw3_screen)id, atoi(argv[4]), strtoull(argv[5], NULL, 10));
	free(s.glyphs);
	return r ? 1 : 0;
}
