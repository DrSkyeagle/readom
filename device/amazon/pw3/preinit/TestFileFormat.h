// SPDX-License-Identifier: Apache-2.0
#ifndef PW3_TEST_FILE_FORMAT_H
#define PW3_TEST_FILE_FORMAT_H
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// The data image of the file-backed boot. It is the seed when it is staged
// and the user's from its first start on (accounts, books, Wi-Fi): its
// content has no expected hash on the device. Until Test V33 the complete
// image had to equal the seed, so nothing survived a restart and every start
// needed the seed copied again; stage.py still compares the seed it stages.
// 1 GiB since Test V31: with 512 MiB the WeRead package and its own downloads
// (455 MiB on Test V30) filled /data and the application refused to start.
//
// The sizes of both images come from the configuration (system: system_size
// of pw3-preinit.conf; data: data_size below) within these limits; each is a
// whole number of 4 KiB blocks. Until Test V36 both were fixed at 1 GiB: the
// system image held 453 MB, and as each start reads all of it for its SHA256
// at about 28 MB/s, the rest cost some 20 s per start; /data had 299 MB left.
#define PW3_TEST_SYSTEM_MIN_BYTES (256ULL << 20)
#define PW3_TEST_SYSTEM_MAX_BYTES (1024ULL << 20)
#define PW3_TEST_DATA_MIN_BYTES (512ULL << 20)
/* 3.5 GiB on this private branch (2026-11-10), from the 1.75 GiB the release
 * before it used. The ceiling is 4 GiB because a file on this FAT cannot be
 * larger; the image a release ships may sit anywhere under it, so the store
 * this entry is built for states its own number in the configuration.
 *
 * A data image may also be a file *larger* than the filesystem in it: the
 * installer grows a kept 1.75 GiB image in place and cannot change what is
 * inside it (it runs on the stock system, whose kernel has no ext4). The
 * entry's job is then to grow that filesystem to the file it is in, before
 * Android mounts it (TestFileInit.h, testfile_data_grow in PreInit.c). The
 * other direction -- a filesystem larger than its file -- is still refused,
 * because there is no cluster of the file to hold it. */
#define PW3_TEST_DATA_MAX_BYTES (4096ULL << 20)
// The report area (device/amazon/pw3/report/ReportArea.h) is the third file of
// the user store this boot maps, and the only one of fixed size: it is made at
// 64 KiB and never resized, because Android writes it by sector and a file that
// changed length would be mapped by clusters that are no longer its own. The
// number is repeated here rather than included, so that this header stays
// dependent on nothing; TestFileInit.h, where both headers are visible, holds
// the static assertion that the two agree.
#define PW3_TEST_REPORT_BYTES 65536ULL
// Which of the files of the user store this boot maps. What used to be a
// boolean "is it the system image" is a name, because there are three of them
// and because every place that asked the question -- the size limits, the
// device-mapper name, uuid and minor, whether the map is read-only -- answers
// it differently for each.
enum pw3_file_kind { PW3_FILE_SYSTEM = 0, PW3_FILE_DATA = 1, PW3_FILE_REPORT = 2, PW3_FILE_KINDS = 3 };
static inline bool pw3_test_size(uint64_t bytes, enum pw3_file_kind kind) {
    if (bytes % 4096) return false;
    switch (kind) {
    case PW3_FILE_SYSTEM: return bytes >= PW3_TEST_SYSTEM_MIN_BYTES && bytes <= PW3_TEST_SYSTEM_MAX_BYTES;
    case PW3_FILE_DATA:   return bytes >= PW3_TEST_DATA_MIN_BYTES && bytes <= PW3_TEST_DATA_MAX_BYTES;
    case PW3_FILE_REPORT: return bytes == PW3_TEST_REPORT_BYTES;
    default: return false;
    }
}
// Decimal digits only, as written by the assembler; at most 12 of them.
static inline bool pw3_test_decimal(const char *s, uint64_t *out) {
    uint64_t v = 0; size_t n = strlen(s);
    if (!n || n > 12 || (s[0] == '0' && n > 1)) return false;
    for (size_t i = 0; i < n; ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + (uint64_t)(s[i] - '0');
    }
    *out = v; return true;
}
struct pw3_test_data {
    char path[128];
    unsigned char uuid[16];
    uint64_t size;
};
static inline bool pw3_test_hex(const char *s, unsigned char *out, unsigned n) {
    if (strlen(s) != n * 2) return false;
    for (unsigned i = 0; i < n; ++i) {
        unsigned byte = 0;
        for (unsigned j = 0; j < 2; ++j) {
            char c = s[i * 2 + j];
            unsigned v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : 16;
            if (v == 16) return false;
            byte = byte * 16 + v;
        }
        out[i] = (unsigned char)byte;
    }
    return true;
}
// Both images sit in one directory of the user store: "android" for a release,
// "pw3-test-<version>" while a version is being tried, and a model name once
// there is more than one. The name is bounded instead of fixed -- lowercase
// letters, digits and dashes, no dot, so it can be neither a traversal nor a
// name FAT reserves -- and the file is always data.img, with system.img beside
// it (TestFileInit.h). The longest this accepts is 57 bytes, which fits the
// path of struct pw3_test_data with room to spare.
#define PW3_TEST_DIRECTORY_MAX 48
static inline bool pw3_test_data_path(const char *s) {
    const char *slash = strchr(s, '/');
    if (!slash || slash == s || slash - s > PW3_TEST_DIRECTORY_MAX || *s == '-') return false;
    if (strcmp(slash, "/data.img")) return false;
    for (const char *p = s; p < slash; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-')) return false;
    return true;
}
static inline bool pw3_test_data_parse(const char *text, struct pw3_test_data *out) {
    char buffer[1024], *save = NULL;
    unsigned seen = 0;
    if (strlen(text) >= sizeof(buffer)) return false;
    memset(out, 0, sizeof(*out));
    strcpy(buffer, text);
    for (char *line = strtok_r(buffer, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *value = strchr(line, '='); unsigned bit;
        if (!value) return false;
        *value++ = 0;
        if (!strcmp(line, "schema")) { bit = 1; if (strcmp(value, "2")) return false; }
        else if (!strcmp(line, "data_path")) {
            bit = 2; if (!pw3_test_data_path(value)) return false; strcpy(out->path, value);
        } else if (!strcmp(line, "data_size")) {
            bit = 4; if (!pw3_test_decimal(value, &out->size) || !pw3_test_size(out->size, PW3_FILE_DATA)) return false;
        } else if (!strcmp(line, "data_uuid")) {
            bit = 8; if (!pw3_test_hex(value, out->uuid, 16)) return false;
        } else return false;
        if (seen & bit) return false;
        seen |= bit;
    }
    return seen == 15;
}
#endif
