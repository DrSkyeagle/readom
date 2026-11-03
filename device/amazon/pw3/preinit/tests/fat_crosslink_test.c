// SPDX-License-Identifier: Apache-2.0
// The cross-link check (FatCrossLink.h) against FAT volumes built here. Both
// things that can claim a cluster are checked, because each is invisible to
// the other's pass: a second chain in the table, and a second directory entry
// that needs no table change at all. Both entry widths, because the user
// store is FAT32 and the code reads FAT16 as well.
// Usage: fat_crosslink_test   (no arguments; it writes its volumes to a
// temporary file and removes it)
#include "../FatCrossLink.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { printf("PW3_FAT_CROSSLINK_FAIL line=%u\n", __LINE__); return 1; } } while (0)

#define BPS 512u
#define SPC 8u
#define RESERVED 32u
// The extents of a mapping are sectors of the whole partition, and the volume
// starts 16 of them in (pw3_file_mapping_read adds that offset itself). The
// test puts them in that frame too, so that a check which confused the two
// would look at the wrong clusters here and fail.
#define PARTITION_OFFSET 16u
#define PATH "/tmp/pw3-fat-crosslink-test.img"

// Where each thing lives in these volumes.
#define ROOT_CLUSTER 3u        // FAT32 only; FAT16 has a fixed root
#define SUBDIR_CLUSTER 4u      // the directory the images are in
#define SYSTEM_FIRST 10u
#define SYSTEM_CLUSTERS 100u
#define DATA_FIRST 200u
#define DATA_CLUSTERS 200u
// The report area, the third file a boot may map. 64 KiB is 16 of these
// clusters. It is in every volume built here, and only some of the cases map
// it: an installation from before it existed maps two files, and the check has
// to accept that as well as refuse a second claim on the third.
#define REPORT_FIRST 600u
#define REPORT_CLUSTERS 16u
#define OTHER_CLUSTER 5000u    // a third file, out of the way
// One cluster of directory entries, which is how big each directory here is.
#define DIR_BYTES (SPC * BPS)

struct volume {
    uint32_t clusters, width, table_sectors, total, root_entries;
    unsigned char boot[BPS];
    unsigned char *table;
    uint64_t data_start;        // in the extents' frame
    uint64_t volume_data_start; // in the volume's own frame, in bytes
    uint32_t cluster_bytes;
};

static void put16(unsigned char *p, uint32_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void put32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

static int volume_make(struct volume *v, uint32_t clusters) {
    uint32_t width = clusters < 65525 ? 2 : 4;
    uint32_t root_entries = width == 2 ? DIR_BYTES / 32u : 0;   // one cluster's worth
    uint32_t root_sectors = (root_entries * 32 + BPS - 1) / BPS;
    uint32_t bytes = (clusters + 2) * width;
    memset(v, 0, sizeof(*v));
    v->clusters = clusters;
    v->width = width;
    v->root_entries = root_entries;
    v->table_sectors = (bytes + BPS - 1) / BPS;
    v->total = RESERVED + v->table_sectors + root_sectors + clusters * SPC;
    v->cluster_bytes = SPC * BPS;
    v->table = (unsigned char *)calloc(1, (size_t)v->table_sectors * BPS);
    if (!v->table) return 0;
    put16(v->boot + 11, BPS);
    v->boot[13] = SPC;
    put16(v->boot + 14, RESERVED);
    v->boot[16] = 1;
    put16(v->boot + 17, root_entries);
    put16(v->boot + 19, 0);
    put32(v->boot + 32, v->total);
    if (width == 2) put16(v->boot + 22, v->table_sectors);
    else { put32(v->boot + 36, v->table_sectors); put32(v->boot + 44, ROOT_CLUSTER); }
    v->boot[510] = 0x55; v->boot[511] = 0xaa;
    v->volume_data_start = (uint64_t)(RESERVED + v->table_sectors + root_sectors) * BPS;
    v->data_start = v->volume_data_start / 512u + PARTITION_OFFSET;
    return 1;
}

static void entry(struct volume *v, uint32_t cluster, uint32_t value) {
    if (v->width == 2) put16(v->table + cluster * 2u, value);
    else put32(v->table + cluster * 4u, value);
}
static uint32_t chain_end(const struct volume *v) { return v->width == 2 ? 0xfff8u : 0x0ffffff8u; }

static void build_chain(struct volume *v, uint32_t first, uint32_t count) {
    for (uint32_t i = 0; i + 1 < count; ++i) entry(v, first + i, first + i + 1);
    entry(v, first + count - 1, chain_end(v));
}

// The directories of these volumes, as bytes to be written.
static unsigned char root_dir[DIR_BYTES], sub_dir[DIR_BYTES];

static void dir_entry(unsigned char *dir, unsigned slot, const char *name, unsigned char attr,
                      uint32_t first, uint32_t size) {
    unsigned char *e = dir + slot * 32u;
    memset(e, ' ', 11);
    for (unsigned i = 0; name[i] && i < 11; ++i) e[i] = (unsigned char)name[i];
    e[11] = attr;
    put16(e + 20, first >> 16);
    put16(e + 26, first & 0xffffu);
    put32(e + 28, size);
}

static void build_directories(const struct volume *v) {
    memset(root_dir, 0, sizeof(root_dir));
    memset(sub_dir, 0, sizeof(sub_dir));
    dir_entry(root_dir, 0, "ANDROID", 0x10, SUBDIR_CLUSTER, 0);
    dir_entry(root_dir, 1, "NOTES   TXT", 0x20, OTHER_CLUSTER, 100);
    dir_entry(sub_dir, 0, ".", 0x10, SUBDIR_CLUSTER, 0);
    dir_entry(sub_dir, 1, "..", 0x10, v->width == 2 ? 0 : ROOT_CLUSTER, 0);
    dir_entry(sub_dir, 2, "SYSTEM  IMG", 0x20, SYSTEM_FIRST, SYSTEM_CLUSTERS * v->cluster_bytes);
    dir_entry(sub_dir, 3, "DATA    IMG", 0x20, DATA_FIRST, DATA_CLUSTERS * v->cluster_bytes);
    dir_entry(sub_dir, 4, "REPORT  BIN", 0x20, REPORT_FIRST, REPORT_CLUSTERS * v->cluster_bytes);
}

static void extent(struct pw3_file_mapping *m, uint32_t first_cluster, uint32_t clusters,
                   uint64_t data_start, uint32_t short_by) {
    const uint32_t cluster_sectors = SPC * (BPS / 512u);
    m->extents[m->count].logical = 0;
    m->extents[m->count].start = data_start + (uint64_t)(first_cluster - 2) * cluster_sectors;
    m->extents[m->count].sectors = (uint64_t)clusters * cluster_sectors - short_by;
    ++m->count;
    m->sectors += m->extents[m->count - 1].sectors;
    m->device = 1; m->inode = 1;
}

static uint64_t cluster_at(const struct volume *v, uint32_t cluster) {
    return v->volume_data_start + (uint64_t)(cluster - 2) * v->cluster_bytes;
}

static int volume_fd(const struct volume *v) {
    int fd = open(PATH, O_RDWR | O_CREAT | O_TRUNC, 0600);
    uint64_t root_at = v->width == 2
        ? (uint64_t)(RESERVED + v->table_sectors) * BPS : cluster_at(v, ROOT_CLUSTER);
    size_t root_size = v->width == 2 ? v->root_entries * 32u : DIR_BYTES;
    if (fd < 0) return -1;
    if (pwrite(fd, v->boot, BPS, 0) != (ssize_t)BPS ||
        pwrite(fd, v->table, (size_t)v->table_sectors * BPS, RESERVED * BPS) !=
            (ssize_t)((size_t)v->table_sectors * BPS) ||
        pwrite(fd, root_dir, root_size, (off_t)root_at) != (ssize_t)root_size ||
        pwrite(fd, sub_dir, DIR_BYTES, (off_t)cluster_at(v, SUBDIR_CLUSTER)) != (ssize_t)DIR_BYTES) {
        close(fd); return -1;
    }
    return fd;
}

// The volume every case starts from: the two images and the report area, a
// directory holding all three, and one unrelated file.
static void build_clean(struct volume *v) {
    memset(v->table, 0, (size_t)v->table_sectors * BPS);
    entry(v, 0, chain_end(v)); entry(v, 1, v->width == 2 ? 0xffffu : 0x0fffffffu);
    if (v->width == 4) entry(v, ROOT_CLUSTER, chain_end(v));
    entry(v, SUBDIR_CLUSTER, chain_end(v));
    entry(v, OTHER_CLUSTER, chain_end(v));
    build_chain(v, SYSTEM_FIRST, SYSTEM_CLUSTERS);
    build_chain(v, DATA_FIRST, DATA_CLUSTERS);
    build_chain(v, REPORT_FIRST, REPORT_CLUSTERS);
    build_directories(v);
}

static int one_width(uint32_t clusters, unsigned expect_bits) {
    struct volume v;
    struct pw3_fat_geometry g;
    struct pw3_file_maps maps;
    int fd;

    CHECK(volume_make(&v, clusters));
    CHECK(pw3_fat_geometry(v.boot, &g));
    CHECK(g.clusters == clusters && g.entry_bits == expect_bits);
    CHECK(g.cluster_sectors == SPC && g.table_offset == RESERVED * BPS);
    CHECK(g.table_bytes == (uint64_t)v.table_sectors * BPS);
    CHECK(g.data_start == v.data_start - PARTITION_OFFSET);
    if (expect_bits == 32) {
        CHECK(g.root_cluster == ROOT_CLUSTER && !g.root_bytes && !g.root_offset);
    } else {
        CHECK(!g.root_cluster && g.root_bytes == v.root_entries * 32u);
        CHECK(g.root_offset == (uint64_t)(RESERVED + v.table_sectors) * BPS);
    }

    memset(&maps, 0, sizeof(maps));
    extent(&maps.map[PW3_FILE_SYSTEM], SYSTEM_FIRST, SYSTEM_CLUSTERS, v.data_start, 0);
    extent(&maps.map[PW3_FILE_DATA], DATA_FIRST, DATA_CLUSTERS, v.data_start, 0);

    build_clean(&v);
    fd = volume_fd(&v);
    CHECK(fd >= 0);
    CHECK(pw3_fat_cross_check(fd, v.boot, PARTITION_OFFSET, &maps) == PW3_FAT_CROSS_OK);
    close(fd);

    // A file whose last cluster is only partly its own is an ordinary file.
    {
        struct pw3_file_maps partial;
        memset(&partial, 0, sizeof(partial));
        extent(&partial.map[PW3_FILE_SYSTEM], SYSTEM_FIRST, SYSTEM_CLUSTERS, v.data_start, SPC - 1);
        extent(&partial.map[PW3_FILE_DATA], DATA_FIRST, DATA_CLUSTERS, v.data_start, 0);
        fd = volume_fd(&v);
        CHECK(fd >= 0);
        CHECK(pw3_fat_cross_check(fd, v.boot, PARTITION_OFFSET, &partial) == PW3_FAT_CROSS_OK);
        close(fd);
    }

    // The report area, which a boot either maps or does not. Every case above
    // maps the two images only, which is the installation from before there was
    // a third file: the report's own chain and entry are in the volume and
    // belong to nobody this boot mapped, so nothing about them is checked. The
    // cases here map all three, and each of the claims the two passes exist to
    // catch is made on the third file rather than on an image -- the point of
    // marking and naming by kind is that the third is not a weaker case.
    {
        struct pw3_file_maps three;
        memset(&three, 0, sizeof(three));
        extent(&three.map[PW3_FILE_SYSTEM], SYSTEM_FIRST, SYSTEM_CLUSTERS, v.data_start, 0);
        extent(&three.map[PW3_FILE_DATA], DATA_FIRST, DATA_CLUSTERS, v.data_start, 0);
        extent(&three.map[PW3_FILE_REPORT], REPORT_FIRST, REPORT_CLUSTERS, v.data_start, 0);

        build_clean(&v);
        fd = volume_fd(&v);
        CHECK(fd >= 0);
        CHECK(pw3_fat_cross_check(fd, v.boot, PARTITION_OFFSET, &three) == PW3_FAT_CROSS_OK);
        close(fd);

        // A third chain into the middle of it.
        build_clean(&v);
        entry(&v, OTHER_CLUSTER, REPORT_FIRST + 5);
        fd = volume_fd(&v);
        CHECK(fd >= 0);
        CHECK(pw3_fat_cross_check(fd, v.boot, PARTITION_OFFSET, &three) == PW3_FAT_CROSS_SHARED);
        close(fd);

        // And at its head, which nothing in the table may point at.
        build_clean(&v);
        entry(&v, OTHER_CLUSTER, REPORT_FIRST);
        fd = volume_fd(&v);
        CHECK(fd >= 0);
        CHECK(pw3_fat_cross_check(fd, v.boot, PARTITION_OFFSET, &three) == PW3_FAT_CROSS_SHARED);
        close(fd);

        // A second directory entry naming it, which changes no table entry.
        build_clean(&v);
        dir_entry(root_dir, 2, "BOOK    BIN", 0x20, REPORT_FIRST, 1000);
        fd = volume_fd(&v);
        CHECK(fd >= 0);
        CHECK(pw3_fat_cross_check(fd, v.boot, PARTITION_OFFSET, &three) == PW3_FAT_CROSS_SHARED);
        close(fd);

        // A directory inside its clusters.
        build_clean(&v);
        dir_entry(root_dir, 2, "ODD        ", 0x10, REPORT_FIRST + 2, 0);
        fd = volume_fd(&v);
        CHECK(fd >= 0);
        CHECK(pw3_fat_cross_check(fd, v.boot, PARTITION_OFFSET, &three) == PW3_FAT_CROSS_SHARED);
        close(fd);

        // Mapped and named by nothing: not the file that was mapped.
        build_clean(&v);
        sub_dir[4 * 32] = 0xe5;
        fd = volume_fd(&v);
        CHECK(fd >= 0);
        CHECK(pw3_fat_cross_check(fd, v.boot, PARTITION_OFFSET, &three) == PW3_FAT_CROSS_SHARED);
        close(fd);

        // The same volume with the two-image receipt: unmapped, so its own
        // chain and entry are somebody else's file and no concern of ours.
        fd = volume_fd(&v);
        CHECK(fd >= 0);
        CHECK(pw3_fat_cross_check(fd, v.boot, PARTITION_OFFSET, &maps) == PW3_FAT_CROSS_OK);
        close(fd);

        // An extent of the report area that is not a cluster of this volume is
        // refused like any other, rather than passed over as optional.
        build_clean(&v);
        fd = volume_fd(&v);
        CHECK(fd >= 0);
        {
            struct pw3_file_maps bad = three;
            bad.map[PW3_FILE_REPORT].extents[0].start += 1;
            CHECK(pw3_fat_cross_check(fd, v.boot, PARTITION_OFFSET, &bad) == PW3_FAT_CROSS_EXTENT);
        }
        close(fd);
    }

    // A third chain that runs into the middle of the system image's.
    build_clean(&v);
    entry(&v, OTHER_CLUSTER, SYSTEM_FIRST + 40);
    fd = volume_fd(&v);
    CHECK(fd >= 0);
    CHECK(pw3_fat_cross_check(fd, v.boot, PARTITION_OFFSET, &maps) == PW3_FAT_CROSS_SHARED);
    close(fd);

    // A third chain that ends at a head: nothing in the table points at one.
    build_clean(&v);
    entry(&v, OTHER_CLUSTER, DATA_FIRST);
    fd = volume_fd(&v);
    CHECK(fd >= 0);
    CHECK(pw3_fat_cross_check(fd, v.boot, PARTITION_OFFSET, &maps) == PW3_FAT_CROSS_SHARED);
    close(fd);

    // The same, from an entry in the last part of the table: the table is
    // read in 64 KiB pieces and this one is not in the first of them.
    build_clean(&v);
    entry(&v, clusters - 1, SYSTEM_FIRST + 7);
    fd = volume_fd(&v);
    CHECK(fd >= 0);
    CHECK(pw3_fat_cross_check(fd, v.boot, PARTITION_OFFSET, &maps) == PW3_FAT_CROSS_SHARED);
    close(fd);

    // FAT32 keeps the top four bits of an entry for itself; a cluster number
    // read without masking them is not a cluster number.
    if (expect_bits == 32) {
        build_clean(&v);
        entry(&v, OTHER_CLUSTER, 0xf0000000u | (SYSTEM_FIRST + 11));
        fd = volume_fd(&v);
        CHECK(fd >= 0);
        CHECK(pw3_fat_cross_check(fd, v.boot, PARTITION_OFFSET, &maps) == PW3_FAT_CROSS_SHARED);
        close(fd);
    }

    // A second directory entry whose first cluster is the data image's. The
    // table is untouched -- this is the case the table pass cannot see.
    build_clean(&v);
    dir_entry(root_dir, 2, "BOOK    BIN", 0x20, DATA_FIRST, DATA_CLUSTERS * v.cluster_bytes);
    fd = volume_fd(&v);
    CHECK(fd >= 0);
    CHECK(pw3_fat_cross_check(fd, v.boot, PARTITION_OFFSET, &maps) == PW3_FAT_CROSS_SHARED);
    close(fd);

    // The same, beginning in the middle of the chain: it reuses the rest.
    build_clean(&v);
    dir_entry(root_dir, 2, "BOOK    BIN", 0x20, DATA_FIRST + 50, 1000);
    fd = volume_fd(&v);
    CHECK(fd >= 0);
    CHECK(pw3_fat_cross_check(fd, v.boot, PARTITION_OFFSET, &maps) == PW3_FAT_CROSS_SHARED);
    close(fd);

    // A directory that lives in the image's clusters.
    build_clean(&v);
    dir_entry(root_dir, 2, "ODD        ", 0x10, SYSTEM_FIRST + 3, 0);
    fd = volume_fd(&v);
    CHECK(fd >= 0);
    CHECK(pw3_fat_cross_check(fd, v.boot, PARTITION_OFFSET, &maps) == PW3_FAT_CROSS_SHARED);
    close(fd);

    // An entry after a zero slot. MS-DOS stopped reading a directory there;
    // Linux skips free slots and reads to the end of the directory's clusters
    // (fs/fat/dir.c, IS_FREE continues), so a file whose entry sits behind one
    // is opened by the kernel and has to be seen here.
    build_clean(&v);
    dir_entry(root_dir, 3, "BOOK    BIN", 0x20, DATA_FIRST, 1000);
    memset(root_dir + 2 * 32u, 0, 32);               // slot 2: never used
    fd = volume_fd(&v);
    CHECK(fd >= 0);
    CHECK(pw3_fat_cross_check(fd, v.boot, PARTITION_OFFSET, &maps) == PW3_FAT_CROSS_SHARED);
    close(fd);

    // The two bytes at offset 20 are the first cluster's high half on FAT32 and
    // reserved on FAT16, where the kernel ignores them (fs/fat/inode.c reads
    // them only when fat_bits == 32). So the same entry shares the data image's
    // first cluster on FAT16, and names a cluster far outside the volume -- no
    // cluster of ours -- on FAT32.
    build_clean(&v);
    dir_entry(root_dir, 2, "BOOK    BIN", 0x20, DATA_FIRST, 1000);
    put16(root_dir + 2 * 32u + 20, 1);
    fd = volume_fd(&v);
    CHECK(fd >= 0);
    CHECK(pw3_fat_cross_check(fd, v.boot, PARTITION_OFFSET, &maps) ==
          (v.width == 2 ? PW3_FAT_CROSS_SHARED : PW3_FAT_CROSS_OK));
    close(fd);

    // A directory whose short name begins with a dot without being "." or
    // "..": the kernel reaches it by its long name, so what is inside it has to
    // be walked. The file in it is in the image's clusters.
    build_clean(&v);
    entry(&v, OTHER_CLUSTER + 1, chain_end(&v));     // the dotted directory's own cluster
    dir_entry(root_dir, 2, ".BOOKS     ", 0x10, OTHER_CLUSTER + 1, 0);
    {
        unsigned char dotted[DIR_BYTES];
        memset(dotted, 0, sizeof(dotted));
        dir_entry(dotted, 0, ".", 0x10, OTHER_CLUSTER + 1, 0);
        dir_entry(dotted, 1, "..", 0x10, v.width == 2 ? 0 : ROOT_CLUSTER, 0);
        dir_entry(dotted, 2, "BOOK    BIN", 0x20, SYSTEM_FIRST + 9, 1000);
        fd = volume_fd(&v);
        CHECK(fd >= 0);
        CHECK(pwrite(fd, dotted, sizeof(dotted), (off_t)cluster_at(&v, OTHER_CLUSTER + 1)) ==
              (ssize_t)sizeof(dotted));
        CHECK(pw3_fat_cross_check(fd, v.boot, PARTITION_OFFSET, &maps) == PW3_FAT_CROSS_SHARED);
        close(fd);
    }

    // An image with no directory entry at all is not the file that was mapped.
    build_clean(&v);
    sub_dir[3 * 32] = 0xe5;
    fd = volume_fd(&v);
    CHECK(fd >= 0);
    CHECK(pw3_fat_cross_check(fd, v.boot, PARTITION_OFFSET, &maps) == PW3_FAT_CROSS_SHARED);
    close(fd);

    // A directory chain that loops: the walk must stop, not spin.
    build_clean(&v);
    for (unsigned slot = 4; slot < DIR_BYTES / 32u; ++slot) sub_dir[slot * 32u] = 0xe5;
    entry(&v, SUBDIR_CLUSTER, SUBDIR_CLUSTER);
    fd = volume_fd(&v);
    CHECK(fd >= 0);
    CHECK(pw3_fat_cross_check(fd, v.boot, PARTITION_OFFSET, &maps) == PW3_FAT_CROSS_DIRECTORY);
    close(fd);

    // Clean again, which shows the cases above were the cause and not the
    // shape of the volume.
    build_clean(&v);
    fd = volume_fd(&v);
    CHECK(fd >= 0);
    CHECK(pw3_fat_cross_check(fd, v.boot, PARTITION_OFFSET, &maps) == PW3_FAT_CROSS_OK);

    // An extent that does not begin at a cluster, lies before the data area,
    // runs past the last cluster, or is missing, is refused rather than
    // wrapped round.
    {
        struct pw3_file_maps bad = maps;
        bad.map[PW3_FILE_SYSTEM].extents[0].start += 1;
        CHECK(pw3_fat_cross_check(fd, v.boot, PARTITION_OFFSET, &bad) == PW3_FAT_CROSS_EXTENT);
        bad = maps; bad.map[PW3_FILE_SYSTEM].extents[0].start = v.data_start - 8;
        CHECK(pw3_fat_cross_check(fd, v.boot, PARTITION_OFFSET, &bad) == PW3_FAT_CROSS_EXTENT);
        bad = maps; bad.map[PW3_FILE_DATA].extents[0].sectors = (uint64_t)clusters * SPC;
        CHECK(pw3_fat_cross_check(fd, v.boot, PARTITION_OFFSET, &bad) == PW3_FAT_CROSS_EXTENT);
        bad = maps; bad.map[PW3_FILE_SYSTEM].count = 0;
        CHECK(pw3_fat_cross_check(fd, v.boot, PARTITION_OFFSET, &bad) == PW3_FAT_CROSS_EXTENT);
    }
    close(fd);
    free(v.table);
    return 0;
}

int main(void) {
    struct volume v;
    int rc = one_width(70000, 32);
    if (rc) return rc;
    rc = one_width(8192, 16);
    if (rc) return rc;

    CHECK(volume_make(&v, 70000));
    build_clean(&v);
    // A boot sector this check will not read rather than misread.
    {
        unsigned char bad[BPS];
        struct pw3_fat_geometry ignored;
        memcpy(bad, v.boot, sizeof(bad));            // FAT12 by cluster count
        put32(bad + 32, RESERVED + v.table_sectors + 100 * SPC);
        CHECK(!pw3_fat_geometry(bad, &ignored));
        memcpy(bad, v.boot, sizeof(bad));            // no table
        put32(bad + 36, 0);
        CHECK(!pw3_fat_geometry(bad, &ignored));
        memcpy(bad, v.boot, sizeof(bad));            // clusters not a power of two
        bad[13] = 6;
        CHECK(!pw3_fat_geometry(bad, &ignored));
        memcpy(bad, v.boot, sizeof(bad));            // a table too short for them
        put32(bad + 36, 1);
        CHECK(!pw3_fat_geometry(bad, &ignored));
        memcpy(bad, v.boot, sizeof(bad));            // three tables
        bad[16] = 3;
        CHECK(!pw3_fat_geometry(bad, &ignored));
        memcpy(bad, v.boot, sizeof(bad));            // reserved sectors: at least one
        put16(bad + 14, 0);
        CHECK(!pw3_fat_geometry(bad, &ignored));
        memcpy(bad, v.boot, sizeof(bad));            // FAT32 with a fixed root directory
        put16(bad + 17, 512);
        CHECK(!pw3_fat_geometry(bad, &ignored));
        memcpy(bad, v.boot, sizeof(bad));            // a root cluster outside the volume
        put32(bad + 44, 70000 + 2);
        CHECK(!pw3_fat_geometry(bad, &ignored));
        // Two tables is ordinary, and the first one is the one to read.
        memcpy(bad, v.boot, sizeof(bad));
        bad[16] = 2;
        put32(bad + 32, RESERVED + 2 * v.table_sectors + 70000 * SPC);
        CHECK(pw3_fat_geometry(bad, &ignored));
        CHECK(ignored.clusters == 70000 && ignored.table_offset == RESERVED * BPS);
        CHECK(ignored.data_start == RESERVED + 2 * v.table_sectors);
        memcpy(bad, v.boot, sizeof(bad));            // a table larger than this reads in
        put32(bad + 36, PW3_FAT_TABLE_MAX / BPS + 1);
        put32(bad + 32, RESERVED + PW3_FAT_TABLE_MAX / BPS + 1 + 70000 * SPC);
        CHECK(!pw3_fat_geometry(bad, &ignored));
    }
    // A volume whose table cannot be read at all.
    {
        struct pw3_file_maps maps;
        int empty = open("/dev/null", O_RDONLY);
        memset(&maps, 0, sizeof(maps));
        extent(&maps.map[PW3_FILE_SYSTEM], SYSTEM_FIRST, SYSTEM_CLUSTERS, v.data_start, 0);
        extent(&maps.map[PW3_FILE_DATA], DATA_FIRST, DATA_CLUSTERS, v.data_start, 0);
        CHECK(empty >= 0);
        CHECK(pw3_fat_cross_check(empty, v.boot, PARTITION_OFFSET, &maps) == PW3_FAT_CROSS_READ);
        close(empty);
    }
    free(v.table);
    unlink(PATH);
    printf("PW3_FAT_CROSSLINK_PASS checks=%u\n", checks);
    return 0;
}
