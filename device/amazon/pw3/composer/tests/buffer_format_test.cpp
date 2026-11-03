// SPDX-License-Identifier: Apache-2.0
// The allocator and the composer have to mean the same buffer.
//
// A format the composer takes but sizes differently from the allocator is a
// read and a write past the end of the buffer, in every path that walks rows:
// that is how taking RGB_565 into the composer broke the virtual display
// (2026-10-19). The two cannot disagree now -- one table, and a mapping that
// refuses a format where the two do not match -- and this says so for every
// format either of them names, so that the next one added is checked too.
//
// Build (in the build VM, for the host; the AOSP tree has the HAL header):
//   g++ -std=c++14 -O1 -Wall -Wextra -Werror
//       -I/home/builder/android-src/system/core/include
//       composer/tests/buffer_format_test.cpp -o /tmp/buffer_format_test
#include "../BufferFormat.h"
#include <cstdio>
#include <set>

static int failures;
#define CHECK(x) do { if (!(x)) { printf("FAILED line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

namespace {
struct Known { int hal; int bytes; const char* name; bool composed; };
// Every format this gralloc allocates (gralloc.cpp), the size it allocates it
// at, and whether a layer in it is composed here. Written out rather than
// derived, so that a change to either table has to be made here as well.
const Known known[] = {
    {HAL_PIXEL_FORMAT_RGBA_8888, 4, "RGBA_8888", true},
    {HAL_PIXEL_FORMAT_RGBX_8888, 4, "RGBX_8888", true},
    {HAL_PIXEL_FORMAT_BGRA_8888, 4, "BGRA_8888", true},
    {HAL_PIXEL_FORMAT_RGB_888, 3, "RGB_888", true},
    {HAL_PIXEL_FORMAT_RGB_565, 2, "RGB_565", true},
    {HAL_PIXEL_FORMAT_RGBA_FP16, 8, "RGBA_FP16", false},
    {HAL_PIXEL_FORMAT_RAW16, 2, "RAW16", false},
};
}  // namespace

int main() {
    std::set<int> allocated;
    for (const Known& k : known) {
        allocated.insert(k.hal);
        CHECK(pw3_pixel_bytes(k.hal) == k.bytes);
        pw3::PixelFormat format{};
        const bool composed = pw3::bufferFormat(k.hal, &format);
        CHECK(composed == k.composed);
        if (!composed) continue;
        // The size the composer walks rows with is the size the buffer has.
        CHECK(pw3::bytesPerPixel(format) == k.bytes);
        // And a valid image of that format needs exactly that many bytes.
        const pw3::Image image{nullptr, size_t(16 * 16 * k.bytes), 16, 16, 16, format};
        CHECK(pw3::validImage(image));
        pw3::Image shorter = image;
        shorter.bytes -= 1;
        CHECK(!pw3::validImage(shorter));
    }
    // Nothing else is allocated, so nothing else may be composed.
    for (int hal = -4096; hal < 4096; ++hal) {
        if (allocated.count(hal)) continue;
        pw3::PixelFormat format{};
        CHECK(pw3_pixel_bytes(hal) == 0);
        CHECK(!pw3::bufferFormat(hal, &format));
    }
    printf(failures ? "%d FAILED\n" : "buffer formats: the allocator and the composer agree\n", failures);
    return failures != 0;
}
