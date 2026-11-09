// SPDX-License-Identifier: Apache-2.0
// PNG decoding for the sleep wallpaper (Png.h): the pictures in png_fixtures.h
// decode to the pixels they were made with, and what this device cannot hold is
// refused before a picture is allocated.
#include "../Png.h"
#include "png_fixtures.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>

static int failures;
#define CHECK(x) do { if (!(x)) { std::printf("FAILED line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

namespace {
std::string directory;

void expectPixel(const pw3::Png& png, int x, int y, int r, int g, int b, int a) {
    if (x < 0 || y < 0 || x >= png.width || y >= png.height) { CHECK(false); return; }
    const uint8_t* p = &png.rgba[(size_t(y) * size_t(png.width) + size_t(x)) * 4];
    CHECK(p[0] == r && p[1] == g && p[2] == b && p[3] == a);
}
// Writes the fixture as a file and reads it back through loadPng.
bool fromFile(const char* name, const unsigned char* data, unsigned long size, pw3::Png* png) {
    const std::string path = directory + "/" + name;
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool wrote = std::fwrite(data, 1, size, f) == size;
    std::fclose(f);
    return wrote && pw3::loadPng(path.c_str(), png);
}
}  // namespace

int main() {
    char temp[] = "/tmp/pw3-png-XXXXXX";
    CHECK(mkdtemp(temp) != nullptr);
    directory = temp;

    pw3::Png png;
    // Straight alpha, in the order and with the values the fixture was made of.
    CHECK(pw3::decodePng(kRgba2x2, kRgba2x2Size, &png));
    CHECK(png.width == 2 && png.height == 2 && png.rgba.size() == 16);
    expectPixel(png, 0, 0, 255, 0, 0, 255);
    expectPixel(png, 1, 0, 0, 255, 0, 128);
    expectPixel(png, 0, 1, 0, 0, 255, 0);
    expectPixel(png, 1, 1, 255, 255, 255, 64);
    // Grey becomes RGBA of that grey, opaque.
    CHECK(pw3::decodePng(kGray4x2, kGray4x2Size, &png));
    CHECK(png.width == 4 && png.height == 2);
    expectPixel(png, 0, 0, 0, 0, 0, 255);
    expectPixel(png, 1, 0, 85, 85, 85, 255);
    expectPixel(png, 2, 0, 170, 170, 170, 255);
    expectPixel(png, 3, 0, 255, 255, 255, 255);
    expectPixel(png, 0, 1, 255, 255, 255, 255);
    expectPixel(png, 1, 1, 170, 170, 170, 255);
    // A palette entry's alpha comes from tRNS.
    CHECK(pw3::decodePng(kPalette2x2, kPalette2x2Size, &png));
    CHECK(png.width == 2 && png.height == 2);
    expectPixel(png, 0, 0, 10, 20, 30, 0);
    expectPixel(png, 1, 0, 200, 100, 50, 64);
    expectPixel(png, 0, 1, 0, 255, 0, 128);
    expectPixel(png, 1, 1, 255, 255, 255, 255);
    // 16 bits per channel: the high byte, which is what the panel can show.
    CHECK(pw3::decodePng(kRgb16x1, kRgb16x1Size, &png));
    CHECK(png.width == 1 && png.height == 1);
    expectPixel(png, 0, 0, 0x12, 0x56, 0x9a, 255);

    // The same through a file, which is what opsSleep does.
    CHECK(fromFile("wallpaper.png", kRgba2x2, kRgba2x2Size, &png));
    CHECK(png.width == 2 && png.height == 2);
    expectPixel(png, 1, 0, 0, 255, 0, 128);

    // Refused: a picture this device cannot hold, before anything is allocated.
    CHECK(!pw3::decodePng(kOversizedSide, kOversizedSideSize, &png));
    CHECK(!pw3::decodePng(kOversizedPixels, kOversizedPixelsSize, &png));
    // Refused: not a PNG at all, cut short, and nothing to read.
    unsigned char broken[sizeof(kRgba2x2)];
    std::memcpy(broken, kRgba2x2, sizeof(broken));
    broken[1] = 'X';
    CHECK(!pw3::decodePng(broken, sizeof(broken), &png));
    CHECK(!pw3::decodePng(kRgba2x2, kRgba2x2Size / 2, &png));
    CHECK(!pw3::decodePng(kRgba2x2, 0, &png));
    CHECK(!pw3::decodePng(nullptr, 0, &png));
    CHECK(!pw3::decodePng(kRgba2x2, kRgba2x2Size, nullptr));
    // The buffer is left empty on every refusal, so a caller that ignores the
    // answer cannot paint the picture that was there before.
    CHECK(png.rgba.empty() && png.width == 0 && png.height == 0);
    // A file that is not there, and one that is empty.
    pw3::Png missing;
    CHECK(!pw3::loadPng((directory + "/missing.png").c_str(), &missing));
    const std::string empty = directory + "/empty.png";
    FILE* f = std::fopen(empty.c_str(), "wb");
    CHECK(f != nullptr);
    if (f) std::fclose(f);
    CHECK(!pw3::loadPng(empty.c_str(), &missing));
    CHECK(!pw3::loadPng(nullptr, &missing));

    unlink((directory + "/wallpaper.png").c_str());
    unlink(empty.c_str());
    rmdir(directory.c_str());
    std::printf(failures ? "PNG_TEST_FAILED %d\n" : "PNG_TEST_PASS%.0d\n", failures);
    return failures != 0;
}
