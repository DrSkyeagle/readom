// SPDX-License-Identifier: Apache-2.0
// Host test of the system image record (SystemStamp.h): a record matches only
// the file and the configuration it was made for.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include "../SystemStamp.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("FAILED line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

int main(void) {
    static struct pw3_file_mapping map, other;
    static struct pw3_system_stamp stamp, changed;
    unsigned char sha[32], sha2[32];
    memset(sha, 0x5a, sizeof(sha)); memcpy(sha2, sha, 32); sha2[31] ^= 1;
    map.sectors = (576u << 20) / 512; map.device = 64768; map.inode = 1234; map.count = 3;
    map.extents[0] = (struct pw3_file_extent){0, 100000, 1000};
    map.extents[1] = (struct pw3_file_extent){1000, 300000, 2000};
    map.extents[2] = (struct pw3_file_extent){3000, 900000, map.sectors - 3000};
    const uint64_t size = 576u << 20; const int64_t mtime = 1792000000, nsec = 0;

    pw3_system_stamp_make(&stamp, size, mtime, nsec, sha, &map);
    CHECK(pw3_system_stamp_matches(&stamp, size, mtime, nsec, sha, &map));
    // The same file on another start: device and inode numbers of FAT files differ.
    other = map; other.device = 1; other.inode = 99;
    CHECK(pw3_system_stamp_matches(&stamp, size, mtime, nsec, sha, &other));
    // Another configuration (a new system image is expected).
    CHECK(!pw3_system_stamp_matches(&stamp, size, mtime, nsec, sha2, &map));
    // The file changed: size, modification time.
    CHECK(!pw3_system_stamp_matches(&stamp, size + 4096, mtime, nsec, sha, &map));
    CHECK(!pw3_system_stamp_matches(&stamp, size, mtime + 2, nsec, sha, &map));
    CHECK(!pw3_system_stamp_matches(&stamp, size, mtime, nsec + 1, sha, &map));
    // The file moved on the FAT: an extent's place, length or the number of extents.
    other = map; other.extents[1].start++; CHECK(!pw3_system_stamp_matches(&stamp, size, mtime, nsec, sha, &other));
    other = map; other.extents[1].sectors--; other.extents[2].logical--; other.extents[2].sectors++;
    CHECK(!pw3_system_stamp_matches(&stamp, size, mtime, nsec, sha, &other));
    other = map; other.count = 2; other.extents[1].sectors += other.extents[2].sectors;
    memset(&other.extents[2], 0, sizeof(other.extents[2]));
    CHECK(!pw3_system_stamp_matches(&stamp, size, mtime, nsec, sha, &other));
    // A damaged or foreign record: every byte matters.
    for (size_t i = 0; i < sizeof(stamp); i += 97) {
        changed = stamp; ((unsigned char *)&changed)[i] ^= 0x10;
        CHECK(!pw3_system_stamp_matches(&changed, size, mtime, nsec, sha, &map));
    }
    changed = stamp; changed.version++; CHECK(!pw3_system_stamp_matches(&changed, size, mtime, nsec, sha, &map));
    changed = stamp; changed.magic = 0; CHECK(!pw3_system_stamp_matches(&changed, size, mtime, nsec, sha, &map));
    printf(failures ? "SYSTEM_STAMP_TEST_FAILED %d\n" : "SYSTEM_STAMP_TEST_PASS%.0d\n", failures);
    return failures != 0;
}
