// SPDX-License-Identifier: Apache-2.0
#ifndef PW3_STORAGE_FORMAT_H
#define PW3_STORAGE_FORMAT_H
#include <stdint.h>
#include <stddef.h>
/* Masks reviewed against PW3 Linux 3.0.35 fs/ext4/ext4.h. Reject an external
 * journal, MMP, and features unknown to that kernel. */
static inline uint16_t pw3_le16(const unsigned char *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static inline uint32_t pw3_le32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline int pw3_ext4_geometry(const unsigned char *s, uint64_t file_size, uint32_t incompat_allowed) {
    uint32_t log, incompat, ro_compat;
    uint64_t blocks, block_size;
    if (pw3_le16(s + 0x38) != 0xef53) return 0;
    log = pw3_le32(s + 0x18);
    incompat = pw3_le32(s + 0x60); ro_compat = pw3_le32(s + 0x64);
    if (log > 2 || (incompat & ~incompat_allowed) || (ro_compat & ~0x7fU)) return 0;
    block_size = (uint64_t)1024 << log;
    blocks = pw3_le32(s + 4);
    if (incompat & 0x80U) blocks |= (uint64_t)pw3_le32(s + 0x150) << 32;
    if (!blocks || blocks > UINT64_MAX / block_size || blocks * block_size != file_size) return 0;
    return 1;
}
/* Unmounted cleanly: valid state, no journal to replay, no orphan list. */
static inline int pw3_ext4_clean(const unsigned char *s, uint64_t file_size) {
    return pw3_le16(s + 0x3a) == 1 && !pw3_le32(s + 0xe8) && pw3_ext4_geometry(s, file_size, 0x2d2U);
}
/* A filesystem in use: as above, or left mounted by a power cut, with a
 * journal to replay (incompat RECOVER, 0x4) and orphans to release, which
 * the kernel's ext4 does when it mounts read-write. Not one on which ext4
 * recorded an error (state 0x2) or was interrupted releasing orphans (0x4). */
static inline int pw3_ext4_in_use(const unsigned char *s, uint64_t file_size) {
    return !(pw3_le16(s + 0x3a) & ~1U) && pw3_ext4_geometry(s, file_size, 0x2d6U);
}
/* The first 512-byte sector of the data area, counted from the start of the
 * FAT; 0 when the boot sector is not one pw3_fat_bounds accepts. Nothing of an
 * image may lie before it. The reserved sectors, the FAT tables and a FAT12/16
 * root directory are there, and FIBMAP answers with a sector inside them for a
 * cluster number below two: fat_get_cluster() returns the cluster it stopped
 * at without reading it again, so a chain carrying 1 on its last hop is not
 * refused by the driver (fs/fat/cache.c in the kernel this port builds). The
 * image would then be mapped over the FAT's own tables, and ext4 writing to
 * /data would destroy the filesystem holding it. */
static inline uint64_t pw3_fat_data_start(const unsigned char *s) {
    uint32_t sector = pw3_le16(s + 11), reserved = pw3_le16(s + 14);
    uint32_t fats = s[16], root_entries = pw3_le16(s + 17), fatsz = pw3_le16(s + 22);
    uint64_t root_sectors;
    if (!fatsz) fatsz = pw3_le32(s + 36);
    if (sector < 512 || sector % 512 || !reserved || !fatsz || (fats != 1 && fats != 2)) return 0;
    root_sectors = ((uint64_t)root_entries * 32 + sector - 1) / sector;
    return ((uint64_t)reserved + (uint64_t)fats * fatsz + root_sectors) * (sector / 512);
}
static inline int pw3_fat_bounds(const unsigned char *s, uint64_t available) {
    uint32_t sector, cluster, reserved, fats, total, fatsz, root_entries;
    uint64_t root_sectors, overhead, clusters;
    if (s[510] != 0x55 || s[511] != 0xaa) return 0;
    sector = pw3_le16(s + 11); cluster = s[13]; reserved = pw3_le16(s + 14);
    fats = s[16]; root_entries = pw3_le16(s + 17);
    total = pw3_le16(s + 19); if (!total) total = pw3_le32(s + 32);
    fatsz = pw3_le16(s + 22); if (!fatsz) fatsz = pw3_le32(s + 36);
    if ((sector != 512 && sector != 1024 && sector != 2048 && sector != 4096) ||
        !cluster || (cluster & (cluster - 1)) || cluster > 128 || !reserved ||
        (fats != 1 && fats != 2) || !fatsz || !total || (uint64_t)total * sector > available) return 0;
    root_sectors = ((uint64_t)root_entries * 32 + sector - 1) / sector;
    overhead = reserved + (uint64_t)fats * fatsz + root_sectors;
    if (overhead >= total) return 0;
    clusters = (total - overhead) / cluster;
    if (!clusters) return 0;
    /* FAT32 has no fixed root directory; FAT12/16 has one. */
    if (clusters >= 65525 ? (root_entries != 0 || pw3_le16(s + 22) != 0 || pw3_le32(s + 44) < 2) : root_entries == 0) return 0;
    return 1;
}
#endif
