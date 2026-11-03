/* SPDX-License-Identifier: Apache-2.0
 * The shape of a mapping receipt: which files a start maps, and where their
 * clusters lie. Only the shape. Nothing here opens a file, asks the kernel
 * where it is, or knows that a device mapper exists.
 *
 * It is a header of its own because two programs need the shape and only one
 * of them has any business with the device mapper. preinit builds dm targets
 * out of these mappings (TestFileMapper.h, which is where the FIBMAP reading
 * and the ioctls live). The installer runs in the stock system, before any
 * handover, and needs the shape for one question only: whether a cluster it is
 * about to write belongs to another file as well (FatCrossLink.h). Including
 * TestFileMapper.h to get a struct would pull <linux/dm-ioctl.h> into a
 * program that must never touch the device mapper, and copying the definitions
 * across instead would be worse than either -- a receipt written by one
 * program and read by the other would drift with nothing to notice.
 */
#ifndef PW3_FILE_MAP_FORMAT_H
#define PW3_FILE_MAP_FORMAT_H
#include "TestFileFormat.h"
#include <stdint.h>

#define PW3_FILEMAP_PATH "/pw3-testmap.bin"
#define PW3_FILEMAP_MAGIC UINT64_C(0x3250414d335750)
#define PW3_FILEMAP_MAX_EXTENTS 1024U
struct pw3_file_extent { uint64_t logical, start, sectors; };
struct pw3_file_mapping {
    uint64_t sectors, device, inode;
    uint32_t count, reserved;
    struct pw3_file_extent extents[PW3_FILEMAP_MAX_EXTENTS];
};
/* The mappings of this boot, in kind order: the first two are the images and
 * are required, the third is the report area and is optional -- an installation
 * from before it existed has none, and the installer makes none when the store
 * has no room to spare or when a file that is not ours sits at that name. An
 * absent mapping is all zeros, which is what memset leaves and what no present
 * mapping can be (every present one has a count, a size and an inode).
 *
 * The two images keep the offsets they had when this was a pair of named
 * members, so a receipt is the same bytes plus one mapping at the end. */
struct pw3_file_maps { uint64_t magic; struct pw3_file_mapping map[PW3_FILE_KINDS]; };
static inline bool pw3_file_mapping_absent(const struct pw3_file_mapping *m) {
    if (m->sectors || m->device || m->inode || m->count || m->reserved) return false;
    for (unsigned i = 0; i < PW3_FILEMAP_MAX_EXTENTS; ++i)
        if (m->extents[i].logical || m->extents[i].start || m->extents[i].sectors) return false;
    return true;
}
/* Whether this boot mapped that file. Required of the images, so an absent one
 * is a rejected receipt rather than an answer. */
static inline bool pw3_file_maps_present(const struct pw3_file_maps *m, enum pw3_file_kind kind) {
    return !pw3_file_mapping_absent(&m->map[kind]);
}
#endif
