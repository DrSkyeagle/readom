// SPDX-License-Identifier: Apache-2.0
// PNG pictures for the sleep wallpaper (SleepWallpaper.h), as 8-bit RGBA.
//
// The sleep picture used to be a PGM this project made itself (Picture.h), so
// the display HAL had no decoder at all. A wallpaper is a file the user put in
// a folder on the store, which means the display HAL reads a picture it did not
// make, in a format it cannot check row by row. The decoder is therefore a
// vendored single-header library -- stb_image, public domain (sleep/third_party,
// with its licence), PNG only and without stdio -- chosen over linking the
// system's libpng because a HAL that pulls in another library to read one
// picture is a dependency to carry for the life of the port, and this one
// compiles into the module with no ABI to match.
//
// What comes out is straight (not premultiplied) alpha; SleepWallpaper.cpp
// premultiplies while it fits the picture to the screen, which is what the
// blend over the frame before the sleep picture wants.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace pw3 {
struct Png {
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;  // width * height * 4 bytes, straight alpha, row after row
};
// The largest picture decoded, and the largest file read. A file past either is
// refused before stb_image allocates anything, and the sleep picture falls back
// to the PGM it always had -- a wallpaper is worth a fallback, not a risk to the
// display process.
//
// The numbers are a memory budget, not a picture-quality one: the panel is
// 1072 x 1448 (1.6 MP), so anything above that is scaled down anyway, and the
// display process has 512 MB in total. A 4 MP RGBA picture costs 16 MB decoded;
// with the file in memory, stb_image's own buffer and the copy this decoder
// makes, the worst case (an interlaced PNG) is about 64 MB, and about 32 MB in
// the ordinary case. 12 MP and a 32 MB file were what this was first written
// with, and that is roughly 128 MB at a first sleep: T14 measured it as a risk
// of taking SurfaceFlinger down on the device, so the limits came down with it.
constexpr int PngMaxSide = 4096;
constexpr long long PngMaxPixels = 4000000;             // 4 megapixels; 16 MB as RGBA
constexpr long long PngMaxBytes = 16LL * 1024 * 1024;   // a 4 MP RGBA PNG cannot be larger
// Decodes the picture in `size` bytes at `data`. False: not a PNG, a PNG this
// build does not carry a decoder for, or one past the limits above.
bool decodePng(const uint8_t* data, size_t size, Png* out);
// The same for a file: reads it (at most PngMaxBytes) and decodes it.
bool loadPng(const char* path, Png* out);
}  // namespace pw3
