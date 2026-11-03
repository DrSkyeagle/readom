/* SPDX-License-Identifier: Apache-2.0
 * What the start leaves behind when it did not read the system image.
 *
 * The start skips the complete SHA256 when its record says this very file was
 * verified before (SystemStamp.h): the same size, modification time and FAT
 * extents, and the same expected hash. That record is about the file, not about
 * its content, and there is one way for the content to change under it: another
 * directory entry on the FAT whose chain runs through the clusters this file
 * lists -- a cross-linked FAT, which a host that was unplugged mid-write and a
 * repair tool that guessed wrong both produce. Writing to that other file then
 * writes into the system image, with its size, its modification time and its
 * own chain all unchanged.
 *
 * Reading the image at every start would close that, at the 21 s per start the
 * record exists to save (Test V55: 8.4 s to 30.5 s of a 181 s start). So the
 * reading moves to where it costs nothing: this file names what was not read,
 * and pw3-system-verify reads it in the background once Android is up, at the
 * lowest scheduling and I/O priority, dropping each block from the page cache
 * as it goes. A mismatch deletes the record, so that the next start reads the
 * image, finds the same mismatch and refuses to boot it -- the behaviour the
 * installer's "damaged system image" screen describes.
 *
 * One line of text, because the two sides are built by different build systems
 * and a struct would have to agree about padding as well as content:
 *
 *   pw3-system-recheck 1 <device> <size> <sha256 as 64 hex digits>\n
 */
#ifndef PW3_SYSTEM_RECHECK_H
#define PW3_SYSTEM_RECHECK_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The record of the last complete check, which a mismatch found here has to
 * delete: named here because both sides of the hand-off need it, and only the
 * start needs what is in it (SystemStamp.h). */
#define PW3_SYSTEM_STAMP_DIR "/data/pw3"
#define PW3_SYSTEM_STAMP_PATH "/data/pw3/system-verified"

#define PW3_SYSTEM_RECHECK_PATH "/pw3-system-recheck"
#define PW3_SYSTEM_RECHECK_PREFIX "pw3-system-recheck 1 "
/* The longest line this writes: the prefix, a device path, a size, 64 hex
 * digits and the separators. A reader that needs more has not been given one
 * of these. */
#define PW3_SYSTEM_RECHECK_MAX 256

struct pw3_system_recheck {
    char device[128];          /* the read-only block device the image is mapped to */
    uint64_t size;             /* its length in bytes, which is the image's */
    unsigned char sha[32];     /* what reading all of it must come to */
};

/* The line, into `out`. Returns its length, or 0 when anything about the
 * record would not survive being written and read back. */
static inline size_t pw3_system_recheck_print(char *out, size_t cap,
        const struct pw3_system_recheck *r) {
    static const char digits[] = "0123456789abcdef";
    char hex[65];
    int n;
    if (!r->device[0] || !memchr(r->device, 0, sizeof(r->device)) || !r->size) return 0;
    for (unsigned i = 0; i < 32; ++i) {
        hex[i * 2] = digits[r->sha[i] >> 4];
        hex[i * 2 + 1] = digits[r->sha[i] & 15];
    }
    hex[64] = 0;
    n = snprintf(out, cap, "%s%s %llu %s\n", PW3_SYSTEM_RECHECK_PREFIX, r->device,
                 (unsigned long long)r->size, hex);
    if (n <= 0 || (size_t)n >= cap) return 0;
    return (size_t)n;
}

/* The record a line holds, if it holds one. Everything about it is checked
 * here, so that the reader has nothing left to validate: the version, a device
 * path that is absolute and has no directory tricks in it, a size that is a
 * whole number of 4 KiB blocks, and exactly 64 hex digits. */
static inline bool pw3_system_recheck_parse(const char *text, struct pw3_system_recheck *out) {
    const char *p = text, *space;
    size_t length;
    uint64_t size = 0;
    memset(out, 0, sizeof(*out));
    if (strlen(text) >= PW3_SYSTEM_RECHECK_MAX) return false;
    if (strncmp(p, PW3_SYSTEM_RECHECK_PREFIX, strlen(PW3_SYSTEM_RECHECK_PREFIX))) return false;
    p += strlen(PW3_SYSTEM_RECHECK_PREFIX);

    space = strchr(p, ' ');
    if (!space) return false;
    length = (size_t)(space - p);
    if (!length || length >= sizeof(out->device) || p[0] != '/') return false;
    for (size_t i = 0; i < length; ++i)
        if (!((p[i] >= 'a' && p[i] <= 'z') || (p[i] >= 'A' && p[i] <= 'Z') ||
              (p[i] >= '0' && p[i] <= '9') || p[i] == '/' || p[i] == '-' || p[i] == '_'))
            return false;
    memcpy(out->device, p, length);
    p = space + 1;

    if (*p < '1' || *p > '9') return false;
    for (; *p >= '0' && *p <= '9'; ++p) {
        if (size > (UINT64_MAX - 9) / 10) return false;
        size = size * 10 + (uint64_t)(*p - '0');
    }
    if (*p != ' ' || size % 4096) return false;
    out->size = size;
    ++p;

    for (unsigned i = 0; i < 32; ++i) {
        unsigned byte = 0;
        for (unsigned j = 0; j < 2; ++j) {
            char c = p[i * 2 + j];
            unsigned value = c >= '0' && c <= '9' ? (unsigned)(c - '0')
                           : c >= 'a' && c <= 'f' ? (unsigned)(c - 'a') + 10 : 16;
            if (value == 16) return false;
            byte = byte * 16 + value;
        }
        out->sha[i] = (unsigned char)byte;
    }
    p += 64;
    return !strcmp(p, "\n") || !*p;
}
#endif
