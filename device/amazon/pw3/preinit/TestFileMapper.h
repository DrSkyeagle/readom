/* SPDX-License-Identifier: Apache-2.0
 * Bounded mappings of fully allocated FAT test image files. FAT stays read-only.
 * FAT allocates whole clusters, whose sectors are contiguous: the first and the
 * last sector of every cluster are queried, and a cluster whose ends do not span
 * it is rejected. Until V6 every 512-byte sector was queried: 5.4 M FIBMAP calls,
 * 9.8 s per start for V37's images; per cluster 1.2 s, the same extents (Test V37,
 * measured on the device). Physical fragmentation never widens write access. */
#ifndef PW3_TEST_FILE_MAPPER_H
#define PW3_TEST_FILE_MAPPER_H
/* A failed system call and a value that does not match return the same error
 * code here, and the code names the comparison. The including program may
 * define PW3_NOTE to record which call failed; the host tests leave it a
 * no-op, so nothing about their 160 checks changes. */
#ifndef PW3_NOTE
#define PW3_NOTE(call, path) ((void)(call), (void)(path))
#endif
#include "FileMapFormat.h"
#include "TestFatMapper.h"
#include "TestFileFormat.h"
#include <linux/fs.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <limits.h>
#include <stdlib.h>

/* The name, uuid, device-mapper minor and write access of each map. dm-0 is the
 * bounded view of the FAT volume itself, so the files start at 1. */
static inline const char *pw3_file_map_name(enum pw3_file_kind kind) {
    return kind == PW3_FILE_SYSTEM ? "pw3-test-system" : kind == PW3_FILE_DATA ? "pw3-test-data" : "pw3-test-report";
}
static inline const char *pw3_file_map_uuid(enum pw3_file_kind kind) {
    return kind == PW3_FILE_SYSTEM ? "PW3-TEST-SYSTEM-V2" : kind == PW3_FILE_DATA ? "PW3-TEST-DATA-V2" : "PW3-TEST-REPORT-V1";
}
static inline unsigned pw3_file_map_minor(enum pw3_file_kind kind) { return (unsigned)kind + 1U; }
static inline bool pw3_file_map_readonly(enum pw3_file_kind kind) { return kind == PW3_FILE_SYSTEM; }
static inline bool pw3_extents_overlap(const struct pw3_file_extent *a,
                                      const struct pw3_file_extent *b) {
    /* Call only after both physical ends have been checked for overflow. */
    return a->start < b->start + b->sectors && b->start < a->start + a->sectors;
}
static inline bool pw3_file_mapping_bounds(const struct pw3_file_mapping *m,
        uint64_t bytes, uint64_t parent_sectors) {
    if (!bytes || bytes % 512 || !m->device || !m->inode || m->reserved ||
        !m->count || m->count > PW3_FILEMAP_MAX_EXTENTS || m->sectors != bytes / 512)
        return false;
    uint64_t logical = 0;
    for (unsigned i = 0; i < m->count; ++i) {
        const struct pw3_file_extent *e = &m->extents[i];
        if (e->logical != logical || !e->sectors || logical > m->sectors ||
            e->sectors > m->sectors - logical || e->start < 16 ||
            e->start >= parent_sectors || e->sectors > parent_sectors - e->start)
            return false;
        logical += e->sectors;
        for (unsigned j = 0; j < i; ++j)
            if (pw3_extents_overlap(e, &m->extents[j])) return false;
    }
    if (logical != m->sectors) return false;
    /* A canonical fixed-size RAM receipt has no unvalidated trailing entries. */
    for (unsigned i = m->count; i < PW3_FILEMAP_MAX_EXTENTS; ++i)
        if (m->extents[i].logical || m->extents[i].start || m->extents[i].sectors)
            return false;
    return true;
}
/* The size a mapping covers, within the limits for its file (TestFileFormat.h). */
static inline bool pw3_file_mapping_sized(const struct pw3_file_mapping *m, enum pw3_file_kind kind,
        uint64_t parent_sectors) {
    return m->sectors <= UINT64_MAX / 512 && pw3_test_size(m->sectors * 512, kind) &&
        pw3_file_mapping_bounds(m, m->sectors * 512, parent_sectors);
}
static inline bool pw3_file_maps_bounds(const struct pw3_file_maps *m, uint64_t parent_sectors) {
    if (m->magic != PW3_FILEMAP_MAGIC) return false;
    for (unsigned k = 0; k < PW3_FILE_KINDS; ++k) {
        const enum pw3_file_kind kind = (enum pw3_file_kind)k;
        if (!pw3_file_maps_present(m, kind)) {
            /* Only the report area may be missing. */
            if (kind != PW3_FILE_REPORT) return false;
            continue;
        }
        if (!pw3_file_mapping_sized(&m->map[k], kind, parent_sectors)) return false;
        /* One FAT, and one file each: the same inode twice would be two maps
         * of one file, and the writable ones would then write over each other. */
        if (m->map[k].device != m->map[PW3_FILE_SYSTEM].device) return false;
        for (unsigned j = 0; j < k; ++j) {
            if (!pw3_file_maps_present(m, (enum pw3_file_kind)j)) continue;
            if (m->map[k].inode == m->map[j].inode) return false;
            for (unsigned a = 0; a < m->map[k].count; ++a)
                for (unsigned b = 0; b < m->map[j].count; ++b)
                    if (pw3_extents_overlap(&m->map[k].extents[a], &m->map[j].extents[b])) return false;
        }
    }
    return true;
}
enum pw3_file_map_error {
    PW3_FILEMAP_OK, PW3_FILEMAP_INPUT, PW3_FILEMAP_METADATA, PW3_FILEMAP_FILESYSTEM,
    PW3_FILEMAP_READONLY, PW3_FILEMAP_BLOCK_SIZE, PW3_FILEMAP_QUERY,
    PW3_FILEMAP_HOLE_OR_BOUNDS, PW3_FILEMAP_EXTENT_LIMIT, PW3_FILEMAP_OVERLAP,
    PW3_FILEMAP_CLUSTER
};
static inline const char *pw3_file_map_error_text(enum pw3_file_map_error e) {
    switch (e) {
    case PW3_FILEMAP_OK: return "file mapping verified";
    case PW3_FILEMAP_INPUT: return "file mapping input size rejected";
    case PW3_FILEMAP_METADATA: return "file mapping regular-file metadata rejected";
    case PW3_FILEMAP_FILESYSTEM: return "file mapping filesystem is not FAT";
    case PW3_FILEMAP_READONLY: return "file mapping FAT is not read-only";
    case PW3_FILEMAP_BLOCK_SIZE: return "file mapping FIGETBSZ is not 512 bytes";
    case PW3_FILEMAP_QUERY: return "file mapping FIBMAP query failed";
    case PW3_FILEMAP_HOLE_OR_BOUNDS: return "file mapping hole or physical bound rejected";
    case PW3_FILEMAP_EXTENT_LIMIT: return "file mapping exceeds the extent limit";
    case PW3_FILEMAP_OVERLAP: return "file mapping coverage or internal overlap rejected";
    case PW3_FILEMAP_CLUSTER: return "file mapping cluster size or cluster contiguity rejected";
    }
    return "file mapping unknown error";
}
/* The runs the last pw3_file_mapping_read() saw. Counting continues past the
 * stored-extent limit, so a rejected start can say how fragmented the file
 * really is (the real Voyage system image is 1,560 runs) instead of only the
 * number it managed to keep. */
__attribute__((unused)) static unsigned long pw3_file_map_observed_runs;
static inline const char *pw3_file_map_error_message(enum pw3_file_map_error e,
        enum pw3_file_kind kind, char *out, size_t cap) {
    if (e == PW3_FILEMAP_EXTENT_LIMIT)
        snprintf(out, cap, "file mapping exceeds the %u-extent limit (%s=%lu limit=%u)",
                 PW3_FILEMAP_MAX_EXTENTS,
                 kind == PW3_FILE_SYSTEM ? "system" : kind == PW3_FILE_DATA ? "data" : "report",
                 pw3_file_map_observed_runs, PW3_FILEMAP_MAX_EXTENTS);
    else
        snprintf(out, cap, "%s", pw3_file_map_error_text(e));
    return out;
}
static inline enum pw3_file_map_error pw3_file_mapping_read(int fd, uint64_t bytes,
        uint64_t fat_sectors, uint64_t data_start, struct pw3_file_mapping *result) {
    struct stat file; struct statfs fs; struct statvfs flags; int block_bytes = 0;
    int limit_reached = 0;
    memset(result, 0, sizeof(*result));
    pw3_file_map_observed_runs = 0;
    if (!bytes || bytes % 512 || bytes / 512 > INT_MAX || fat_sectors > UINT64_MAX - 16)
        return PW3_FILEMAP_INPUT;
    if (fstat(fd, &file)) { PW3_NOTE("fstat", NULL); return PW3_FILEMAP_METADATA; }
    if (!S_ISREG(file.st_mode) || file.st_nlink != 1 ||
        (uint64_t)file.st_size != bytes || !file.st_dev || !file.st_ino)
        return PW3_FILEMAP_METADATA;
    if (fstatfs(fd, &fs)) { PW3_NOTE("fstatfs", NULL); return PW3_FILEMAP_FILESYSTEM; }
    if ((uint32_t)fs.f_type != 0x4d44) return PW3_FILEMAP_FILESYSTEM;
    if (fstatvfs(fd, &flags)) { PW3_NOTE("fstatvfs", NULL); return PW3_FILEMAP_READONLY; }
    if (!(flags.f_flag & ST_RDONLY)) return PW3_FILEMAP_READONLY;
    if (ioctl(fd, FIGETBSZ, &block_bytes)) { PW3_NOTE("ioctl FIGETBSZ", NULL); return PW3_FILEMAP_BLOCK_SIZE; }
    if (block_bytes != 512) return PW3_FILEMAP_BLOCK_SIZE;
    /* FAT reports its cluster size as the block size of statfs: 512 B to 64 KiB. */
    const int64_t cluster_bytes = (int64_t)fs.f_bsize;
    if (cluster_bytes < 512 || cluster_bytes > 65536 || cluster_bytes & (cluster_bytes - 1))
        return PW3_FILEMAP_CLUSTER;
    const int cluster = (int)(cluster_bytes / 512), total = (int)(bytes / 512);
    result->sectors = bytes / 512; result->device = file.st_dev; result->inode = file.st_ino;
    for (int logical = 0; logical < total; logical += cluster) {
        /* The file's last cluster may be partly beyond its end. */
        const int span = total - logical < cluster ? total - logical : cluster;
        int first = logical, last_sector = logical + span - 1;
        if (ioctl(fd, FIBMAP, &first) || ioctl(fd, FIBMAP, &last_sector)) return PW3_FILEMAP_QUERY;
        if (first <= 0 || last_sector <= 0 || (uint64_t)first >= fat_sectors ||
            (uint64_t)last_sector >= fat_sectors ||
            (uint64_t)first < data_start) return PW3_FILEMAP_HOLE_OR_BOUNDS;
        if (last_sector != first + span - 1) return PW3_FILEMAP_CLUSTER;
        uint64_t physical = 16 + (uint64_t)first;
        struct pw3_file_extent *last = result->count ? &result->extents[result->count - 1] : NULL;
        if (last && last->start + last->sectors == physical) last->sectors += (uint64_t)span;
        else {
            ++pw3_file_map_observed_runs;
            if (result->count == PW3_FILEMAP_MAX_EXTENTS) limit_reached = 1;
            else {
                struct pw3_file_extent *next = &result->extents[result->count++];
                next->logical = (unsigned)logical; next->start = physical; next->sectors = (uint64_t)span;
            }
        }
    }
    /* The whole file is still walked past the limit: the count is what the
     * failure message needs, and the FIBMAP reads dominate either way. */
    if (limit_reached) return PW3_FILEMAP_EXTENT_LIMIT;
    return pw3_file_mapping_bounds(result, bytes, fat_sectors + 16)
        ? PW3_FILEMAP_OK : PW3_FILEMAP_OVERLAP;
}
#define PW3_FILE_DM_BYTES (sizeof(struct dm_ioctl) + PW3_FILEMAP_MAX_EXTENTS * (sizeof(struct dm_target_spec) + 64U + 8U))
union pw3_file_dm_buffer { uint64_t align; unsigned char bytes[PW3_FILE_DM_BYTES]; };
static inline struct dm_ioctl *pw3_file_map_request(union pw3_file_dm_buffer *b, enum pw3_file_kind kind) {
    memset(b, 0, sizeof(*b)); struct dm_ioctl *d = (struct dm_ioctl *)b->bytes;
    d->version[0] = 4; d->data_size = sizeof(*b); d->data_start = sizeof(*d);
    strcpy(d->name, pw3_file_map_name(kind)); return d;
}
static inline bool pw3_file_map_status(const struct dm_ioctl *d, enum pw3_file_kind kind,
        unsigned mapped_major, unsigned mapped_minor, unsigned count) {
    return !strncmp(d->name, pw3_file_map_name(kind), sizeof(d->name)) &&
        !strncmp(d->uuid, pw3_file_map_uuid(kind), sizeof(d->uuid)) &&
        d->dev == pw3_dm_encode(mapped_major, mapped_minor) && d->target_count == count &&
        (d->flags & DM_ACTIVE_PRESENT_FLAG) &&
        !!(d->flags & DM_READONLY_FLAG) == pw3_file_map_readonly(kind) &&
        !(d->flags & (DM_SUSPEND_FLAG | DM_INACTIVE_PRESENT_FLAG | DM_BUFFER_FULL_FLAG));
}
static inline bool pw3_file_map_verify_buffer(union pw3_file_dm_buffer *b, int ctl, enum pw3_file_kind kind,
        unsigned mapped_major, unsigned mapped_minor, unsigned parent_major,
        unsigned parent_minor, uint64_t parent_sectors, const struct pw3_file_mapping *map) {
    if (!pw3_file_mapping_sized(map, kind, parent_sectors)) return false;
    struct dm_ioctl *d = pw3_file_map_request(b, kind);
    if (ioctl(ctl, DM_DEV_STATUS, d) ||
        !pw3_file_map_status(d, kind, mapped_major, mapped_minor, map->count)) return false;
    d = pw3_file_map_request(b, kind); d->flags = DM_STATUS_TABLE_FLAG;
    if (ioctl(ctl, DM_TABLE_STATUS, d) ||
        !pw3_file_map_status(d, kind, mapped_major, mapped_minor, map->count) ||
        d->data_start < sizeof(*d) || d->data_start % 8 ||
        d->data_start > d->data_size || d->data_size > sizeof(*b)) return false;
    uint32_t cursor = 0; size_t available = d->data_size - d->data_start;
    for (unsigned i = 0; i < map->count; ++i) {
        if (cursor > available || available - cursor < sizeof(struct dm_target_spec) + 1) return false;
        const struct dm_target_spec *t = (const struct dm_target_spec *)(b->bytes + d->data_start + cursor);
        const char *params = (const char *)(t + 1); size_t remaining = available - cursor - sizeof(*t);
        if (remaining > 64) remaining = 64;
        const char *end = (const char *)memchr(params, 0, remaining); if (!end) return false;
        char expected[64]; int n = snprintf(expected, sizeof(expected), "%u:%u %llu", parent_major,
                    parent_minor, (unsigned long long)map->extents[i].start);
        if (n <= 0 || n >= (int)sizeof(expected) || strcmp(params, expected) ||
            t->sector_start != map->extents[i].logical || t->length != map->extents[i].sectors ||
            t->status || strncmp(t->target_type, "linear", sizeof(t->target_type))) return false;
        size_t used = cursor + sizeof(*t) + (size_t)(end - params) + 1;
        size_t next = (used + 7) & ~(size_t)7;
        /* Linux 3.0 status replies use offsets from the first target, unlike
         * TABLE_LOAD requests whose next offsets are relative to each target. */
        if (t->next != next || next > sizeof(*b) - d->data_start) return false;
        if (i + 1 == map->count) { if (used != available) return false; }
        else if (next >= available) return false;
        cursor = (uint32_t)next;
    }
    return true;
}
static inline bool pw3_file_map_verify(int ctl, enum pw3_file_kind kind, unsigned mapped_major,
        unsigned mapped_minor, unsigned parent_major, unsigned parent_minor,
        uint64_t parent_sectors, const struct pw3_file_mapping *map) {
    union pw3_file_dm_buffer *b = (union pw3_file_dm_buffer *)malloc(sizeof(*b));
    if (!b) return false;
    bool ok = pw3_file_map_verify_buffer(b, ctl, kind, mapped_major, mapped_minor,
                                       parent_major, parent_minor, parent_sectors, map);
    free(b); return ok;
}
static inline bool pw3_file_map_create_buffer(union pw3_file_dm_buffer *b, int ctl, enum pw3_file_kind kind,
        unsigned parent_major, unsigned parent_minor, uint64_t parent_sectors,
        const struct pw3_file_mapping *map, unsigned *mapped_major, unsigned *mapped_minor) {
    if (!pw3_file_mapping_sized(map, kind, parent_sectors)) return false;
    struct dm_ioctl *d = pw3_file_map_request(b, kind); strcpy(d->uuid, pw3_file_map_uuid(kind));
    if (ioctl(ctl, DM_DEV_CREATE, d) || !d->dev) return false;
    *mapped_major = (unsigned)((d->dev >> 8) & 4095U);
    *mapped_minor = (unsigned)((d->dev & 255U) | ((d->dev >> 12) & 0xfff00U));
    if (*mapped_minor != pw3_file_map_minor(kind)) return false;
    d = pw3_file_map_request(b, kind); d->target_count = map->count;
    d->flags = pw3_file_map_readonly(kind) ? DM_READONLY_FLAG : 0; size_t cursor = d->data_start;
    for (unsigned i = 0; i < map->count; ++i) {
        if (cursor > sizeof(*b) - sizeof(struct dm_target_spec) - 64) return false;
        struct dm_target_spec *t = (struct dm_target_spec *)(b->bytes + cursor);
        t->sector_start = map->extents[i].logical; t->length = map->extents[i].sectors;
        strcpy(t->target_type, "linear"); char *params = (char *)(t + 1);
        int n = snprintf(params, 64, "%u:%u %llu", parent_major, parent_minor,
                         (unsigned long long)map->extents[i].start);
        if (n <= 0 || n >= 64) return false;
        t->next = (sizeof(*t) + (unsigned)n + 1 + 7) & ~7U; cursor += t->next;
    }
    d->data_size = cursor;
    if (ioctl(ctl, DM_TABLE_LOAD, d)) return false;
    d = pw3_file_map_request(b, kind);
    if (ioctl(ctl, DM_DEV_SUSPEND, d)) return false;
    return pw3_file_map_verify_buffer(b, ctl, kind, *mapped_major, *mapped_minor,
                                     parent_major, parent_minor, parent_sectors, map);
}
static inline bool pw3_file_map_create(int ctl, enum pw3_file_kind kind, unsigned parent_major,
        unsigned parent_minor, uint64_t parent_sectors, const struct pw3_file_mapping *map,
        unsigned *mapped_major, unsigned *mapped_minor) {
    union pw3_file_dm_buffer *b = (union pw3_file_dm_buffer *)malloc(sizeof(*b));
    if (!b) return false;
    bool ok = pw3_file_map_create_buffer(b, ctl, kind, parent_major, parent_minor,
                                       parent_sectors, map, mapped_major, mapped_minor);
    free(b); return ok;
}
#endif
