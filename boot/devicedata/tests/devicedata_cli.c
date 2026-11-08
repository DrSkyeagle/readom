/* SPDX-License-Identifier: GPL-2.0-only */
/* Host front end for checking devicedata.c against captured device files:
 *   devicedata_cli atags ROOT CMDLINE INITRD_START INITRD_SIZE OUT [PROFILE]
 *   devicedata_cli archive ROOT PREINIT_CONF_FILE DATA_PATH|- DATA_SIZE OUT [PROFILE]
 * ROOT is a directory laid out like the device ("/" on the device). PROFILE is
 * the model whose captured files these are -- "pw3" (the default) or "voyage";
 * the profiles are the same structs the shipped helpers are built with, so a
 * check done here is a check of one firmware's facts, not of whichever model
 * this tool was compiled for. */
#include "../devicedata.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int save(const char *path, const struct dd_buf *b) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int ok = fwrite(b->data, 1, b->size, f) == b->size;
    return fclose(f) == 0 && ok ? 0 : -1;
}

static const struct dd_profile *profile(const char *name) {
    if (!name || !strcmp(name, "pw3")) return &dd_pw3_5_16_2_1_1;
    if (!strcmp(name, "voyage")) return &dd_voyage_5_13_6;
    return NULL;
}

int main(int argc, char **argv) {
    struct dd_buf out = {0};
    struct dd_why why = {{0}, 0};
    int rc;
    const struct dd_profile *chosen = profile(argc == 8 ? argv[7] : NULL);
    if (!chosen) { fprintf(stderr, "unknown profile: %s (pw3 or voyage)\n", argv[7]); return 2; }
    if (argc == 7 || argc == 8) {
        if (!strcmp(argv[1], "atags")) {
            rc = dd_atags(argv[2], chosen, argv[3], (uint32_t)strtoul(argv[4], NULL, 0),
                          (uint32_t)strtoul(argv[5], NULL, 0), &out, &why);
        } else if (!strcmp(argv[1], "archive")) {
            static char conf[8192];
            FILE *f = fopen(argv[3], "rb");
            size_t n = f ? fread(conf, 1, sizeof(conf) - 1, f) : 0;
            if (f) fclose(f);
            conf[n] = 0;
            struct dd_build build = {NULL, conf, strcmp(argv[4], "-") ? argv[4] : NULL, (uint32_t)strtoul(argv[5], NULL, 0)};
            rc = dd_archive(argv[2], chosen, &build, &out, &why);
        } else {
            fprintf(stderr, "usage: see the head of this file\n");
            return 2;
        }
        if (rc) { fprintf(stderr, "refused: %s\n", why.text); return 1; }
        if (save(argv[6], &out)) { perror(argv[6]); return 1; }
        printf("%zu bytes\n", out.size);
        dd_free(&out);
        return 0;
    }
    fprintf(stderr, "usage: see the head of this file\n");
    return 2;
}
