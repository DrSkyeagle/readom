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
// The largest picture that is decoded. The panel is 1072 x 1448 and the display
// process has 512 MB of RAM in total: a photograph larger than this is refused
// rather than attempted, and the sleep picture falls back to the PGM.
constexpr int PngMaxSide = 4096;
constexpr long long PngMaxPixels = 12LL * 1000 * 1000;  // 48 MB as RGBA once decoded
// The largest file read. A PNG of the panel's size is under 2 MB; this is well
// past any wallpaper and still small enough to read into one buffer.
constexpr long long PngMaxBytes = 32LL * 1024 * 1024;
// Decodes the picture in `size` bytes at `data`. False: not a PNG, a PNG this
// build does not carry a decoder for, or one past the limits above.
bool decodePng(const uint8_t* data, size_t size, Png* out);
// The same for a file: reads it (at most PngMaxBytes) and decodes it.
bool loadPng(const char* path, Png* out);
}  // namespace pw3
