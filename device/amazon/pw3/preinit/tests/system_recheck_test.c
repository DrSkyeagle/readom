// SPDX-License-Identifier: Apache-2.0
// The hand-off between the start and pw3-system-verify (SystemRecheck.h): one
// line of text, written by a C program built against this old kernel's libc
// and read by a C++ one built by AOSP, so what it accepts is worth pinning.
// Usage: system_recheck_test   (no arguments; it makes its own cases)
#include "../SystemRecheck.h"
#include <stdio.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { printf("PW3_SYSTEM_RECHECK_FAIL line=%u\n", __LINE__); return 1; } } while (0)

static void fill(struct pw3_system_recheck *r, const char *device, uint64_t size,
                 unsigned char first) {
    memset(r, 0, sizeof(*r));
    snprintf(r->device, sizeof(r->device), "%s", device);
    r->size = size;
    for (unsigned i = 0; i < 32; ++i) r->sha[i] = (unsigned char)(first + i);
}

int main(void) {
    struct pw3_system_recheck out, in;
    char line[PW3_SYSTEM_RECHECK_MAX], longer[PW3_SYSTEM_RECHECK_MAX * 2];
    size_t n;

    // What the start writes comes back as what the start meant.
    fill(&out, "/dev/block/dm-1", 536870912u, 0x10);
    n = pw3_system_recheck_print(line, sizeof(line), &out);
    CHECK(n && n < sizeof(line) && line[n - 1] == '\n');
    CHECK(pw3_system_recheck_parse(line, &in));
    CHECK(!strcmp(in.device, out.device) && in.size == out.size &&
          !memcmp(in.sha, out.sha, 32));
    // The exact bytes, so that a change to either side is a change here.
    CHECK(!strcmp(line, "pw3-system-recheck 1 /dev/block/dm-1 536870912 "
                        "101112131415161718191a1b1c1d1e1f"
                        "202122232425262728292a2b2c2d2e2f\n"));
    // A reader that was handed the line without its newline takes it too.
    line[n - 1] = 0;
    CHECK(pw3_system_recheck_parse(line, &in));

    // Nothing to write about.
    fill(&out, "", 536870912u, 0);
    CHECK(!pw3_system_recheck_print(line, sizeof(line), &out));
    fill(&out, "/dev/block/dm-1", 0, 0);
    CHECK(!pw3_system_recheck_print(line, sizeof(line), &out));
    // A device name that does not fit is refused rather than cut in half.
    fill(&out, "/dev/block/dm-1", 536870912u, 0);
    memset(out.device, 'a', sizeof(out.device));
    CHECK(!pw3_system_recheck_print(line, sizeof(line), &out));
    // Nor does a buffer that cannot hold the line get a shorter one.
    fill(&out, "/dev/block/dm-1", 536870912u, 0);
    CHECK(!pw3_system_recheck_print(line, 64, &out));

    // What a reader must refuse.
    CHECK(!pw3_system_recheck_parse("", &in));
    CHECK(!pw3_system_recheck_parse("pw3-system-recheck 2 /dev/block/dm-1 4096 "
        "0000000000000000000000000000000000000000000000000000000000000000\n", &in));
    CHECK(!pw3_system_recheck_parse("pw3-system-recheck 1 dev/block/dm-1 4096 "
        "0000000000000000000000000000000000000000000000000000000000000000\n", &in));
    CHECK(!pw3_system_recheck_parse("pw3-system-recheck 1 /dev/../etc/x 4096 "
        "0000000000000000000000000000000000000000000000000000000000000000\n", &in));
    CHECK(!pw3_system_recheck_parse("pw3-system-recheck 1 /dev/block/dm-1 4095 "
        "0000000000000000000000000000000000000000000000000000000000000000\n", &in));
    CHECK(!pw3_system_recheck_parse("pw3-system-recheck 1 /dev/block/dm-1 0 "
        "0000000000000000000000000000000000000000000000000000000000000000\n", &in));
    CHECK(!pw3_system_recheck_parse("pw3-system-recheck 1 /dev/block/dm-1 04096 "
        "0000000000000000000000000000000000000000000000000000000000000000\n", &in));
    // 64 hex digits, no more, no fewer, and nothing after the line.
    CHECK(!pw3_system_recheck_parse("pw3-system-recheck 1 /dev/block/dm-1 4096 "
        "000000000000000000000000000000000000000000000000000000000000000\n", &in));
    CHECK(!pw3_system_recheck_parse("pw3-system-recheck 1 /dev/block/dm-1 4096 "
        "00000000000000000000000000000000000000000000000000000000000000000\n", &in));
    CHECK(!pw3_system_recheck_parse("pw3-system-recheck 1 /dev/block/dm-1 4096 "
        "00000000000000000000000000000000000000000000000000000000000000g0\n", &in));
    CHECK(!pw3_system_recheck_parse("pw3-system-recheck 1 /dev/block/dm-1 4096 "
        "0000000000000000000000000000000000000000000000000000000000000000 x\n", &in));
    CHECK(!pw3_system_recheck_parse("pw3-system-recheck 1 /dev/block/dm-1 4096 "
        "0000000000000000000000000000000000000000000000000000000000000000\n\n", &in));
    // A size that would wrap the accumulator is a refusal, not a small number.
    CHECK(!pw3_system_recheck_parse("pw3-system-recheck 1 /dev/block/dm-1 "
        "184467440737095516160 "
        "0000000000000000000000000000000000000000000000000000000000000000\n", &in));
    // A line longer than anything this writes is refused before it is read.
    memset(longer, 'a', sizeof(longer));
    longer[sizeof(longer) - 1] = 0;
    memcpy(longer, PW3_SYSTEM_RECHECK_PREFIX, strlen(PW3_SYSTEM_RECHECK_PREFIX));
    CHECK(!pw3_system_recheck_parse(longer, &in));
    // Upper-case hex is not what the start writes, so it is not read either.
    CHECK(!pw3_system_recheck_parse("pw3-system-recheck 1 /dev/block/dm-1 4096 "
        "000000000000000000000000000000000000000000000000000000000000000A\n", &in));

    // A refused line leaves nothing of itself in the caller's record.
    fill(&in, "/dev/block/dm-9", 4096, 0x55);
    CHECK(!pw3_system_recheck_parse("no", &in));
    CHECK(!in.device[0] && !in.size && !in.sha[0]);

    printf("PW3_SYSTEM_RECHECK_PASS checks=%u\n", checks);
    return 0;
}
