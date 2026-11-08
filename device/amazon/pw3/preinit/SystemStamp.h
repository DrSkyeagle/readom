/* SPDX-License-Identifier: Apache-2.0
 * The record of the system image's last complete verification.
 *
 * Reading all of the system image for its SHA256 took about 20 s of each start
 * (576 MiB then, 512 MiB since 2026-11-02; 30 MB/s from the FAT and 33 MB/s of
 * SHA256 on the Cortex-A9, Test
 * V37, measured on the device). The image is verified completely when it has no
 * record, or when the file is not the one recorded: its size, modification time
 * and every extent on the FAT, and the expected hash of this start's
 * configuration. A file that was replaced, rewritten or moved, or a FAT whose
 * allocation changed under it, therefore gets the complete check; the record is
 * written only after a check passed. The installer verifies what it writes (read
 * back past the host's cache); eMMC corrects or reports bit errors itself.
 *
 * What is left is content that changed with none of those changing: another
 * directory entry whose chain runs through this file's clusters, which a FAT
 * repaired by guesswork produces. A start that trusts the record leaves
 * PW3_SYSTEM_RECHECK_PATH behind, and pw3-system-verify reads the image in the
 * background once Android is up; a mismatch there deletes this record, so the
 * next start reads the image and refuses it (SystemRecheck.h). Said plainly:
 * **this** start runs the image it trusted, and what the background re-check
 * protects is the one after it. Closing that would cost the 20 s on every
 * start, which is what the record exists to avoid; the cross-link check
 * (FatCrossLink.h) is what keeps the case that produces it from arising.
 *
 * Verification on read (dm-verity) was not chosen: it hashes every block read
 * from the image, again after each eviction from the page cache, which on this
 * device's 511 MB is often, and slows every cold start of an application. */
#ifndef PW3_SYSTEM_STAMP_H
#define PW3_SYSTEM_STAMP_H
#include "TestFileMapper.h"
/* Where this record lives is named there, with the hand-off that tells
 * pw3-system-verify which starts left the image unread. */
#include "SystemRecheck.h"

#define PW3_SYSTEM_STAMP_MAGIC UINT64_C(0x504d545333575031) /* "1PW3STMP" */
#define PW3_SYSTEM_STAMP_VERSION 1U

struct pw3_system_stamp {
    uint64_t magic;
    uint32_t version, reserved;
    uint64_t size;
    int64_t mtime_sec, mtime_nsec;
    unsigned char sha[32];
    /* Sectors and extents only: device and inode numbers of a FAT file are not
     * the same from one start to the next. */
    uint64_t sectors;
    uint32_t count, reserved2;
    struct pw3_file_extent extents[PW3_FILEMAP_MAX_EXTENTS];
};

static inline void pw3_system_stamp_make(struct pw3_system_stamp *s, uint64_t size,
        int64_t mtime_sec, int64_t mtime_nsec, const unsigned char sha[32],
        const struct pw3_file_mapping *map) {
    memset(s, 0, sizeof(*s));
    s->magic = PW3_SYSTEM_STAMP_MAGIC; s->version = PW3_SYSTEM_STAMP_VERSION;
    s->size = size; s->mtime_sec = mtime_sec; s->mtime_nsec = mtime_nsec;
    memcpy(s->sha, sha, 32);
    s->sectors = map->sectors; s->count = map->count;
    memcpy(s->extents, map->extents, sizeof(s->extents));
}

/* Whether a record read from the data image is that of this very file, verified
 * against this configuration's hash. Anything else is no record. */
static inline bool pw3_system_stamp_matches(const struct pw3_system_stamp *recorded,
        uint64_t size, int64_t mtime_sec, int64_t mtime_nsec, const unsigned char sha[32],
        const struct pw3_file_mapping *map) {
    /* ~196 KiB at PW3_FILEMAP_MAX_EXTENTS = 8192: a function-static keeps PID 1's
     * stack out of it. Single-threaded PID 1 (and the single-threaded host
     * tests), so no reentrancy to lose. */
    static struct pw3_system_stamp expected;
    pw3_system_stamp_make(&expected, size, mtime_sec, mtime_nsec, sha, map);
    return map->count <= PW3_FILEMAP_MAX_EXTENTS && !memcmp(recorded, &expected, sizeof(expected));
}
#endif
