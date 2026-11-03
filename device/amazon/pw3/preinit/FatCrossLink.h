/* SPDX-License-Identifier: Apache-2.0
 * Whether any cluster this boot maps belongs to another file as well.
 *
 * What it maps are ordinary files on the user's FAT -- the two images, and the
 * report area when there is one -- and what is mapped is where
 * FIBMAP says their clusters are. That is this file's own chain, and it says
 * nothing about whether something else claims the same clusters -- a
 * cross-linked FAT, which an unplugged write and a repair tool that guessed
 * both produce. Writing /data through such a map writes into one of the user's
 * books, and deleting that book would free the chain Android is running on.
 *
 * Two things can claim a cluster, and both are checked here because each is
 * invisible to the other's check:
 *
 *   the table      a chain is a linked list, so a cluster two chains run
 *                  through is pointed at twice, and one a second chain begins
 *                  at is pointed at once where nothing should point at all.
 *   the directory  a second directory entry whose first cluster is one of ours
 *                  needs no table entry at all: it simply reuses our chain
 *                  from that cluster to its end. Nothing in the table changes,
 *                  so only reading the entries finds it.
 *
 * Both passes work from one copy of the first FAT, read once. This volume is
 * 3.3 GB with 8 KiB clusters, so that is 1.6 MB and about 60 ms at the eMMC's
 * 29 MB/s; the directory tree is a few hundred KB more. No heuristics are
 * involved in either: an entry either names one of our clusters or it does
 * not, and the directory entries that may name our first clusters are our own,
 * one each.
 */
#ifndef PW3_FAT_CROSS_LINK_H
#define PW3_FAT_CROSS_LINK_H
#include "StorageFormat.h"
#include "FileMapFormat.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The largest first FAT this reads into memory. A 4 GB volume with the
 * smallest clusters FAT32 is used with needs 33 MB; the start has the whole
 * device's memory to itself here, and anything past this is not a volume
 * these images can live on. */
#define PW3_FAT_TABLE_MAX (64u * 1024u * 1024u)
/* Directories nest far less than this in any path a FAT tool will produce. */
#define PW3_FAT_WALK_DEPTH 32u
/* Directory entries read at a time, 64 of them. Each level of the walk reads
 * into its own part of one buffer: a level that shared it with the level
 * below would go on reading the entries of the directory it had just
 * descended into. */
#define PW3_FAT_WALK_CHUNK (32u * 64u)

enum pw3_fat_cross {
    PW3_FAT_CROSS_OK = 0,
    PW3_FAT_CROSS_GEOMETRY,   /* not a FAT16/FAT32 volume this reads */
    PW3_FAT_CROSS_MEMORY,
    PW3_FAT_CROSS_READ,
    PW3_FAT_CROSS_EXTENT,     /* an extent is not where a cluster of this volume is */
    PW3_FAT_CROSS_DIRECTORY,  /* a directory chain loops, or is deeper than this reads */
    PW3_FAT_CROSS_SHARED      /* a cluster of a mapped file is claimed by something else too */
};

static inline const char *pw3_fat_cross_text(enum pw3_fat_cross why) {
    switch (why) {
    case PW3_FAT_CROSS_OK: return "no mapped cluster is claimed by anything else";
    case PW3_FAT_CROSS_GEOMETRY: return "the FAT is not one the cross-link check reads";
    case PW3_FAT_CROSS_MEMORY: return "no memory for the cross-link check";
    case PW3_FAT_CROSS_READ: return "the FAT table or a directory could not be read";
    case PW3_FAT_CROSS_EXTENT: return "a mapped extent is not a cluster of this FAT";
    case PW3_FAT_CROSS_DIRECTORY: return "a directory of this FAT loops or nests too deep";
    default: return "a mapped cluster belongs to another file as well";
    }
}

struct pw3_fat_geometry {
    uint32_t bytes_per_sector, cluster_sectors; /* cluster_sectors in 512-byte sectors */
    uint64_t table_offset, table_bytes;         /* the first FAT, in bytes from the volume's start */
    uint64_t data_start;                        /* the data area, in 512-byte sectors from the same */
    uint64_t root_offset;                       /* a fixed root directory, in bytes; 0 on FAT32 */
    uint32_t root_bytes;                        /* its length; 0 on FAT32 */
    uint32_t root_cluster;                      /* the root's first cluster on FAT32; 0 otherwise */
    uint32_t clusters;                          /* data clusters; numbers run 2..clusters+1 */
    unsigned entry_bits;                        /* 16 or 32 */
};

/* The boot sector's own numbers, as this check needs them. pw3_fat_bounds has
 * already accepted the sector; what is added here is the table's place, which
 * width its entries are, and where the root directory is. */
static inline int pw3_fat_geometry(const unsigned char *s, struct pw3_fat_geometry *g) {
    uint32_t bps = pw3_le16(s + 11), spc = s[13], reserved = pw3_le16(s + 14);
    uint32_t fats = s[16], root_entries = pw3_le16(s + 17);
    uint32_t total = pw3_le16(s + 19), fatsz = pw3_le16(s + 22);
    uint64_t root_sectors, overhead;
    memset(g, 0, sizeof(*g));
    if (!total) total = pw3_le32(s + 32);
    if (!fatsz) fatsz = pw3_le32(s + 36);
    if (bps < 512 || bps > 4096 || bps % 512 || !spc || (spc & (spc - 1)) || spc > 128 ||
        !reserved || !fatsz || (fats != 1 && fats != 2) || !total) return 0;
    root_sectors = ((uint64_t)root_entries * 32 + bps - 1) / bps;
    overhead = (uint64_t)reserved + (uint64_t)fats * fatsz + root_sectors;
    if (overhead >= total) return 0;
    g->clusters = (uint32_t)((total - overhead) / spc);
    /* FAT12 is a volume of at most a few megabytes, which cannot hold either
     * image; its packed entries are not read here. */
    if (g->clusters < 4085) return 0;
    g->entry_bits = g->clusters < 65525 ? 16 : 32;
    g->bytes_per_sector = bps;
    g->cluster_sectors = spc * (bps / 512);
    g->table_offset = (uint64_t)reserved * bps;
    g->table_bytes = (uint64_t)fatsz * bps;
    g->data_start = pw3_fat_data_start(s);
    if (!g->data_start) return 0;
    /* The table must hold an entry for every cluster, or it is not this
     * volume's table, and it must be small enough to read in. */
    if (g->table_bytes < ((uint64_t)g->clusters + 2) * (g->entry_bits / 8) ||
        g->table_bytes > PW3_FAT_TABLE_MAX) return 0;
    if (g->entry_bits == 16) {
        if (!root_entries) return 0;         /* FAT16 has a fixed root directory */
        g->root_offset = ((uint64_t)reserved + (uint64_t)fats * fatsz) * bps;
        g->root_bytes = root_entries * 32;
    } else {
        if (root_entries) return 0;          /* FAT32 has none */
        g->root_cluster = pw3_le32(s + 44);
        if (g->root_cluster < 2 || g->root_cluster > g->clusters + 1) return 0;
    }
    return 1;
}

/* One entry of the table that has been read in. */
static inline uint32_t pw3_fat_entry(const unsigned char *table, unsigned width, uint32_t cluster) {
    return width == 2 ? pw3_le16(table + (size_t)cluster * 2)
                      : pw3_le32(table + (size_t)cluster * 4) & 0x0fffffffu;
}

static inline int pw3_fat_bit(const unsigned char *set, uint32_t n) {
    return (set[n >> 3] >> (n & 7)) & 1;
}
static inline void pw3_fat_set(unsigned char *set, uint32_t n) {
    set[n >> 3] |= (unsigned char)(1u << (n & 7));
}

/* Marks the clusters of one mapping. `data_start` is in the extents' own
 * frame: see pw3_fat_cross_check, which is the only thing that works it out.
 * A file whose length is not a whole number of clusters ends in a partial
 * one, and the cluster it is in is as much ours as any other. */
static inline enum pw3_fat_cross pw3_fat_cross_mark(const struct pw3_file_mapping *m,
        const struct pw3_fat_geometry *g, uint64_t data_start, unsigned char *ours,
        uint32_t *head) {
    *head = 0;
    for (unsigned i = 0; i < m->count; ++i) {
        uint64_t start = m->extents[i].start, sectors = m->extents[i].sectors;
        uint64_t first, count, c;
        if (start < data_start || !sectors) return PW3_FAT_CROSS_EXTENT;
        if ((start - data_start) % g->cluster_sectors) return PW3_FAT_CROSS_EXTENT;
        first = (start - data_start) / g->cluster_sectors + 2;
        count = (sectors + g->cluster_sectors - 1) / g->cluster_sectors;
        if (first < 2 || first + count > (uint64_t)g->clusters + 2) return PW3_FAT_CROSS_EXTENT;
        if (!i) *head = (uint32_t)first;
        for (c = first; c < first + count; ++c) pw3_fat_set(ours, (uint32_t)c);
    }
    return *head ? PW3_FAT_CROSS_OK : PW3_FAT_CROSS_EXTENT;
}

struct pw3_fat_walk {
    int fd;
    const struct pw3_fat_geometry *g;
    const unsigned char *table;
    const unsigned char *ours;
    unsigned char *visited;          /* directory clusters already read, against a loop */
    unsigned char *entries;          /* PW3_FAT_WALK_CHUNK for each level of the walk */
    /* The first cluster of each mapping this boot has, and how many directory
     * entries name it. By kind rather than by name (TestFileFormat.h): the
     * report area made these three instead of two, and a check written once
     * per file is a check that gets one of them wrong. A head of 0 is a file
     * this boot did not map. */
    uint32_t head[PW3_FILE_KINDS];
    unsigned named[PW3_FILE_KINDS];
};

/* Every entry of one directory, and then of every directory below it. The
 * root is `cluster == 0` on FAT16, where it is a fixed area instead of a
 * chain. */
static inline enum pw3_fat_cross pw3_fat_walk_directory(struct pw3_fat_walk *w,
        uint32_t cluster, unsigned depth) {
    const struct pw3_fat_geometry *g = w->g;
    const uint32_t cluster_bytes = g->cluster_sectors * 512u;
    unsigned char *const buffer = w->entries + (size_t)depth * PW3_FAT_WALK_CHUNK;
    uint64_t at, left;
    if (depth > PW3_FAT_WALK_DEPTH) return PW3_FAT_CROSS_DIRECTORY;
    for (;;) {
        if (cluster) {
            if (cluster < 2 || cluster > g->clusters + 1) return PW3_FAT_CROSS_DIRECTORY;
            if (pw3_fat_bit(w->visited, cluster)) return PW3_FAT_CROSS_DIRECTORY;
            pw3_fat_set(w->visited, cluster);
            /* A directory that runs through one of the images is the same
             * fault as a file that does. */
            if (pw3_fat_bit(w->ours, cluster)) return PW3_FAT_CROSS_SHARED;
            at = (uint64_t)g->data_start * 512u + (uint64_t)(cluster - 2) * cluster_bytes;
            left = cluster_bytes;
        } else {
            at = g->root_offset;
            left = g->root_bytes;
        }
        while (left >= 32) {
            const size_t want = left < PW3_FAT_WALK_CHUNK ? (size_t)left : PW3_FAT_WALK_CHUNK;
            const ssize_t got = pread(w->fd, buffer, want, (off_t)at);
            size_t offset;
            if (got < 32 || (size_t)got % 32) return PW3_FAT_CROSS_READ;
            for (offset = 0; offset + 32 <= (size_t)got; offset += 32) {
                const unsigned char *e = buffer + offset;
                const unsigned char attr = e[11];
                uint32_t first;
                /* A free slot, not the end of anything this has to believe.
                 * MS-DOS wrote a zero slot to mean "stop reading here", and a
                 * scan that stops there sees less than the kernel does: Linux
                 * skips free slots and keeps reading to the end of the
                 * directory's own clusters (fs/fat/dir.c, fat_search_long:
                 * `IS_FREE(de->name)` continues, and IS_FREE is a zero or a
                 * 0xe5). A file whose entry sits after a zero slot is opened by
                 * the kernel and was invisible here, so a chain of it running
                 * through one of the images passed as "no cluster claimed". */
                if (!e[0] || e[0] == 0xe5) continue;        /* free slot: deleted, or never used */
                if ((attr & 0x0f) == 0x0f) continue;       /* a piece of a long name */
                if (attr & 0x08) continue;                 /* the volume's label */
                /* The high half of the first cluster is FAT32's. On FAT16 those
                 * two bytes are reserved and the kernel ignores them
                 * (fs/fat/inode.c: `if (sbi->fat_bits == 32)`), so reading them
                 * here turned an entry the kernel starts at cluster N into one
                 * this check saw at cluster N + 65536*x -- out of range, and so
                 * skipped, while the kernel went on sharing cluster N with an
                 * image. */
                first = g->entry_bits == 32
                        ? (((uint32_t)pw3_le16(e + 20) << 16) | pw3_le16(e + 26))
                        : pw3_le16(e + 26);
                if (first >= 2 && first <= g->clusters + 1) {
                    unsigned k = 0;
                    while (k < PW3_FILE_KINDS && first != w->head[k]) ++k;
                    if (k < PW3_FILE_KINDS) ++w->named[k];
                    else if (pw3_fat_bit(w->ours, first)) return PW3_FAT_CROSS_SHARED;
                }
                /* `.` and `..` are the two entries a directory holds for itself
                 * and its parent, and following them is how a walk loops. They
                 * are the only short names a file system writes beginning with
                 * a dot, and the test used to be that first byte alone -- so a
                 * directory whose short name merely starts with one (".BOOKS",
                 * which the kernel reaches by its long name) was never entered,
                 * and a file inside it could share an image's clusters
                 * unnoticed. Now only the two real ones are skipped: 8.3 names
                 * are space-padded, so they are exactly "." and ".." there. */
                const int self_or_parent = e[0] == '.' &&
                        (e[1] == ' ' || (e[1] == '.' && e[2] == ' '));
                if ((attr & 0x10) && !self_or_parent && first >= 2 && first <= g->clusters + 1) {
                    const enum pw3_fat_cross why = pw3_fat_walk_directory(w, first, depth + 1);
                    if (why != PW3_FAT_CROSS_OK) return why;
                }
            }
            at += (uint64_t)got;
            left -= (uint64_t)got;
        }
        if (!cluster) return PW3_FAT_CROSS_OK;             /* the fixed root ends here */
        cluster = pw3_fat_entry(w->table, g->entry_bits / 8, cluster);
        if (cluster < 2 || cluster > g->clusters + 1) return PW3_FAT_CROSS_OK;  /* end of chain */
    }
}

/* Two frames meet in this check and they are not the same one. Everything
 * read out of the boot sector counts from the volume's first sector, and
 * `volume_fd` reads the volume, so the table is where the boot sector says.
 * The extents of `maps`, though, are sectors of the whole partition:
 * pw3_file_mapping_read adds the FAT's own offset to every FIBMAP answer,
 * because the device mapper targets are built on the partition. So the one
 * thing the caller passes is `volume_first_sector`, where the volume begins
 * in the extents' frame, and the data area is worked out here rather than
 * handed in -- a data area in the wrong frame would have this reading the
 * wrong clusters and saying nothing about it. */
static inline enum pw3_fat_cross pw3_fat_cross_check(int volume_fd, const unsigned char *boot,
        uint64_t volume_first_sector, const struct pw3_file_maps *maps) {
    struct pw3_fat_geometry g;
    struct pw3_fat_walk walk;
    enum pw3_fat_cross why = PW3_FAT_CROSS_OK;
    unsigned char *ours = NULL, *seen = NULL, *visited = NULL, *table = NULL, *entries = NULL;
    uint64_t data_start, at;
    size_t bits;
    uint32_t head[PW3_FILE_KINDS] = {0}, cluster;
    unsigned width;

    if (!pw3_fat_geometry(boot, &g)) return PW3_FAT_CROSS_GEOMETRY;
    if (volume_first_sector > UINT64_MAX - g.data_start) return PW3_FAT_CROSS_GEOMETRY;
    data_start = volume_first_sector + g.data_start;
    width = g.entry_bits / 8;
    bits = ((size_t)g.clusters + 2 + 7) / 8;
    ours = (unsigned char *)calloc(1, bits);
    seen = (unsigned char *)calloc(1, bits);
    visited = (unsigned char *)calloc(1, bits);
    table = (unsigned char *)calloc(1, (size_t)g.table_bytes);
    entries = (unsigned char *)malloc((size_t)(PW3_FAT_WALK_DEPTH + 1) * PW3_FAT_WALK_CHUNK);
    if (!ours || !seen || !visited || !table || !entries) { why = PW3_FAT_CROSS_MEMORY; goto done; }

    for (unsigned k = 0; k < PW3_FILE_KINDS; ++k) {
        if (!pw3_file_maps_present(maps, (enum pw3_file_kind)k)) continue;
        why = pw3_fat_cross_mark(&maps->map[k], &g, data_start, ours, &head[k]);
        if (why != PW3_FAT_CROSS_OK) goto done;
    }

    for (at = 0; at < g.table_bytes;) {
        const size_t chunk = 64 * 1024;
        size_t want = g.table_bytes - at < chunk ? (size_t)(g.table_bytes - at) : chunk;
        const ssize_t got = pread(volume_fd, table + at, want, (off_t)(g.table_offset + at));
        if (got <= 0) { why = PW3_FAT_CROSS_READ; goto done; }
        at += (uint64_t)got;
    }

    /* Every entry of the table that names a cluster, from the third (the
     * first two hold the media marks) to the last this volume has. */
    for (cluster = 2; cluster <= g.clusters + 1; ++cluster) {
        const uint32_t value = pw3_fat_entry(table, width, cluster);
        if (value < 2 || value > g.clusters + 1) continue;
        if (!pw3_fat_bit(ours, value)) continue;
        /* A head is reached from its directory entry and never from the
         * table; anything else of ours is reached from exactly one place,
         * the cluster before it in its own chain. */
        {
            unsigned k = 0;
            while (k < PW3_FILE_KINDS && value != head[k]) ++k;
            if (k < PW3_FILE_KINDS || pw3_fat_bit(seen, value)) {
                why = PW3_FAT_CROSS_SHARED; goto done;
            }
        }
        pw3_fat_set(seen, value);
    }

    /* And every directory entry, which is where a second claim on a head, or
     * on any cluster of ours, leaves no trace in the table at all. */
    memset(&walk, 0, sizeof(walk));
    walk.fd = volume_fd; walk.g = &g; walk.table = table; walk.ours = ours;
    walk.visited = visited; walk.entries = entries;
    memcpy(walk.head, head, sizeof(walk.head));
    why = pw3_fat_walk_directory(&walk, g.root_cluster, 0);
    if (why != PW3_FAT_CROSS_OK) goto done;
    /* One entry each: every file this boot mapped is named once, by its own. */
    for (unsigned k = 0; k < PW3_FILE_KINDS; ++k)
        if (head[k] && walk.named[k] != 1) why = PW3_FAT_CROSS_SHARED;
done:
    free(ours); free(seen); free(visited); free(table); free(entries);
    return why;
}
#endif
