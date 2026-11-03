// SPDX-License-Identifier: Apache-2.0
// Host test of the data image admission rules (StorageFormat.h,
// TestFileFormat.h). Usage: data_image_test SEED_IMAGE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../StorageFormat.h"
#include "../TestFileFormat.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("FAILED line %d: %s\n", __LINE__, #x); ++failures; } } while (0)
static void put16(unsigned char *p, unsigned v) { p[0] = v & 255; p[1] = v >> 8; }
static void put32(unsigned char *p, unsigned v) { put16(p, v & 65535); put16(p + 2, v >> 16); }

int main(int argc, char **argv) {
    unsigned char seed[1024], sb[1024];
    FILE *f = argc == 2 ? fopen(argv[1], "rb") : NULL;
    if (!f || fseek(f, 1024, SEEK_SET) || fread(seed, 1, sizeof(seed), f) != sizeof(seed) || fseek(f, 0, SEEK_END)) { perror("seed"); return 2; }
    // The size of the seed given.
    const uint64_t size = (uint64_t)ftell(f);
    fclose(f);
    CHECK(pw3_test_size(size, PW3_FILE_DATA));
    // The seed as staged.
    CHECK(pw3_ext4_clean(seed, size) && pw3_ext4_in_use(seed, size));
    CHECK(!pw3_ext4_in_use(seed, size / 2) && !pw3_ext4_clean(seed, size * 2));
    // Mounted read-write and not unmounted: state cleared, journal to replay, an orphan.
    memcpy(sb, seed, sizeof(sb)); put16(sb + 0x3a, 0); put32(sb + 0x60, pw3_le32(sb + 0x60) | 4); put32(sb + 0xe8, 12345);
    CHECK(!pw3_ext4_clean(sb, size) && pw3_ext4_in_use(sb, size));
    // Unmounted cleanly after use: mount count and times differ, nothing else.
    memcpy(sb, seed, sizeof(sb)); put16(sb + 0x34, 7); put32(sb + 0x2c, 1800000000u); put32(sb + 0x30, 1800000001u);
    CHECK(pw3_ext4_clean(sb, size) && pw3_ext4_in_use(sb, size));
    // ext4 recorded an error, with and without the valid bit.
    memcpy(sb, seed, sizeof(sb)); put16(sb + 0x3a, 3); CHECK(!pw3_ext4_in_use(sb, size));
    put16(sb + 0x3a, 2); CHECK(!pw3_ext4_in_use(sb, size));
    // Interrupted while releasing orphans.
    put16(sb + 0x3a, 4); CHECK(!pw3_ext4_in_use(sb, size));
    // External journal device, MMP, and a feature Linux 3.0.35 does not know.
    for (unsigned bit = 0; bit < 32; ++bit) {
        memcpy(sb, seed, sizeof(sb)); put32(sb + 0x60, pw3_le32(sb + 0x60) | (1u << bit));
        CHECK(pw3_ext4_in_use(sb, size) == !!((1u << bit) & 0x2d6u));
        CHECK(pw3_ext4_clean(sb, size) == !!((1u << bit) & 0x2d2u));
    }
    memcpy(sb, seed, sizeof(sb)); put16(sb + 0x38, 0xef54); CHECK(!pw3_ext4_in_use(sb, size));

    struct pw3_test_data d;
    const char *uuid = "00112233445566778899aabbccddeeff";
    char text[512];
    snprintf(text, sizeof(text), "schema=2\ndata_path=pw3-test-v34/data.img\ndata_size=1073741824\ndata_uuid=%s\n", uuid);
    CHECK(pw3_test_data_parse(text, &d) && !strcmp(d.path, "pw3-test-v34/data.img") && d.uuid[0] == 0 && d.uuid[15] == 0xff);
    snprintf(text, sizeof(text), "schema=1\ndata_path=pw3-test-v34/data.img\ndata_size=1073741824\ndata_uuid=%s\n", uuid);
    CHECK(!pw3_test_data_parse(text, &d));
    snprintf(text, sizeof(text), "schema=2\ndata_path=pw3-test-v34/data.img\ndata_size=1073741824\ndata_sha256=%s%s\ndata_uuid=%s\n", uuid, uuid, uuid);
    CHECK(!pw3_test_data_parse(text, &d));
    snprintf(text, sizeof(text), "schema=2\ndata_path=pw3-test-v34/data.img\ndata_size=1073741824\n");
    CHECK(!pw3_test_data_parse(text, &d));
    snprintf(text, sizeof(text), "schema=2\ndata_path=../data.img\ndata_size=1073741824\ndata_uuid=%s\n", uuid);
    CHECK(!pw3_test_data_parse(text, &d));
    // The directory is a bounded name, not a fixed one: a release stages the
    // images under android/, a test under its own, and the entry and the
    // installer have to agree on whichever it is (build_launcher.py).
    static const struct { const char *path; int ok; } paths[] = {
        {"android/data.img", 1}, {"pw3-test-v34/data.img", 1}, {"kindle-pw3/data.img", 1},
        {"a/data.img", 1}, {"012345678901234567890123456789012345678901234567/data.img", 1},
        {"0123456789012345678901234567890123456789012345678/data.img", 0},
        {"/data.img", 0}, {"-android/data.img", 0}, {"./data.img", 0}, {"../data.img", 0},
        {"an.droid/data.img", 0}, {"Android/data.img", 0}, {"an droid/data.img", 0},
        {"android/system.img", 0}, {"android/data.img.bak", 0}, {"android/sub/data.img", 0},
        {"android/", 0}, {"data.img", 0}, {"", 0},
    };
    for (unsigned i = 0; i < sizeof(paths) / sizeof(*paths); ++i) {
        snprintf(text, sizeof(text), "schema=2\ndata_path=%s\ndata_size=1073741824\ndata_uuid=%s\n",
                 paths[i].path, uuid);
        CHECK(pw3_test_data_parse(text, &d) == paths[i].ok);
        if (paths[i].ok) CHECK(!strcmp(d.path, paths[i].path));
    }
    // The size: decimal, whole 4 KiB blocks, 512 MiB to 3 GiB.
    static const struct { const char *size; int ok; } sizes[] = {
        {"1073741824", 1}, {"2147483648", 1}, {"536870912", 1}, {"3221225472", 1},
        {"536866816", 0}, {"3221229568", 0}, {"2147483136", 0}, {"02147483648", 0}, {"+2147483648", 0},
        {"2147483648 ", 0}, {"", 0}, {"0x80000000", 0}, {"18446744073709551616", 0}, {"1e9", 0},
    };
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(*sizes); ++i) {
        snprintf(text, sizeof(text), "schema=2\ndata_path=pw3-test-v34/data.img\ndata_size=%s\ndata_uuid=%s\n", sizes[i].size, uuid);
        CHECK(pw3_test_data_parse(text, &d) == sizes[i].ok);
        if (sizes[i].ok) CHECK(d.size == strtoull(sizes[i].size, NULL, 10));
    }
    // The system image's limits.
    CHECK(pw3_test_size(576ULL << 20, PW3_FILE_SYSTEM) && pw3_test_size(1024ULL << 20, PW3_FILE_SYSTEM) && pw3_test_size(256ULL << 20, PW3_FILE_SYSTEM));
    CHECK(!pw3_test_size((1024ULL << 20) + 4096, PW3_FILE_SYSTEM) && !pw3_test_size((256ULL << 20) - 4096, PW3_FILE_SYSTEM) && !pw3_test_size((576ULL << 20) + 512, PW3_FILE_SYSTEM));
    // The report area is the one file of fixed size: 64 KiB and nothing else,
    // because Android writes it by sector through clusters that were mapped for
    // exactly that length (ReportArea.h).
    CHECK(pw3_test_size(PW3_TEST_REPORT_BYTES, PW3_FILE_REPORT));
    CHECK(!pw3_test_size(PW3_TEST_REPORT_BYTES + 4096, PW3_FILE_REPORT) &&
          !pw3_test_size(PW3_TEST_REPORT_BYTES - 4096, PW3_FILE_REPORT) && !pw3_test_size(0, PW3_FILE_REPORT));
    // And no file's limits are another's: a size each kind would take is a
    // check that says nothing about the kind.
    CHECK(!pw3_test_size(PW3_TEST_REPORT_BYTES, PW3_FILE_SYSTEM) && !pw3_test_size(PW3_TEST_REPORT_BYTES, PW3_FILE_DATA));
    CHECK(!pw3_test_size(256ULL << 20, PW3_FILE_DATA) && !pw3_test_size(3072ULL << 20, PW3_FILE_SYSTEM));
    CHECK(!pw3_test_size(1024ULL << 20, PW3_FILE_REPORT) && !pw3_test_size(576ULL << 20, PW3_FILE_REPORT));
    // PW3_FILE_KINDS is the count, not a kind.
    CHECK(!pw3_test_size(576ULL << 20, PW3_FILE_KINDS));
    printf(failures ? "%d FAILED\n" : "data image admission: all checks passed\n", failures);
    return failures != 0;
}
