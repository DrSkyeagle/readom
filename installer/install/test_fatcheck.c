// SPDX-License-Identifier: Apache-2.0
/* One call of inst_fat_exclusive(), printed. Everything that makes the call
 * mean anything -- a FAT of its own, mounted, with the images on it, and a
 * proxy filesystem over it -- is set up by test_fatcheck.py, which needs root
 * to mount and so cannot be this program.
 *
 * The answer is printed rather than returned as an exit status because what the
 * test checks is not only which of the three answers came back: a refusal names
 * the step it stopped at, and an answer that came from the wrong volume is
 * still one of the three. */
#include "fatcheck.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *name(enum inst_fat answer)
{
	switch (answer) {
	case INST_FAT_OK:
		return "OK";
	case INST_FAT_SHARED:
		return "SHARED";
	default:
		return "UNREADABLE";
	}
}

int main(int argc, char **argv)
{
	struct inst_fat_note note;
	enum inst_fat answer;
	const char *system_path, *data_path;
	uint64_t system_bytes, data_bytes;

	if (argc != 6) {
		fprintf(stderr, "usage: %s <store> <system path|-> <system bytes> <data path|-> <data bytes>\n",
			argv[0]);
		return 2;
	}
	system_path = strcmp(argv[2], "-") ? argv[2] : NULL;
	data_path = strcmp(argv[4], "-") ? argv[4] : NULL;
	system_bytes = strtoull(argv[3], NULL, 10);
	data_bytes = strtoull(argv[5], NULL, 10);

	answer = inst_fat_exclusive(argv[1], system_path, system_bytes, data_path, data_bytes, &note);
	/* Tab-separated, because `where` is a sentence: split on spaces and the
	 * step it stopped at comes back as its first word, which compares equal to
	 * nothing and fails every test that names it. */
	printf("%s\twhere=%s\terr=%d\tdetail=%d\tmount=%s\tvolume=%s\n", name(answer),
	       note.where ? note.where : "", note.err, note.detail, note.mount, note.volume);
	return 0;
}
