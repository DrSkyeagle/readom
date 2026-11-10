// SPDX-License-Identifier: Apache-2.0
// The sleep wallpaper (SleepWallpaper.h): which picture the folder gives, how
// it is fitted to the panel, and what its transparent pixels leave of the frame
// the panel was showing.
#include "../SleepWallpaper.h"
#include "../Tone.h"
#include "png_fixtures.h"
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>
#include <sys/stat.h>

static int failures;
#define CHECK(x) do { if (!(x)) { std::printf("FAILED line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

namespace {
std::string directory;

std::string path(const char* name) { return directory + "/" + name; }

void writeFile(const std::string& to, const void* data, size_t size) {
    FILE* f = std::fopen(to.c_str(), "wb");
    if (!f) { CHECK(false); return; }
    if (size) CHECK(std::fwrite(data, 1, size, f) == size);
    std::fclose(f);
}
void writeText(const std::string& to, const char* text) { writeFile(to, text, std::strlen(text)); }

void expectPixel(const pw3::Wallpaper& wall, int x, int y, int r, int g, int b, int a) {
    const uint8_t* p = &wall.rgba[(size_t(y) * size_t(wall.width) + size_t(x)) * 4];
    CHECK(p[0] == r && p[1] == g && p[2] == b && p[3] == a);
}
// The same rounding the painter blends with (Luma.cpp).
unsigned divide255(unsigned value) {
    const unsigned t = value + 128u;
    return (t + (t >> 8)) >> 8;
}
pw3::Wallpaper flatWallpaper(int width, int height, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    pw3::Wallpaper wall;
    wall.width = width;
    wall.height = height;
    wall.rgba.resize(size_t(width) * size_t(height) * 4);
    for (size_t i = 0; i < size_t(width) * size_t(height); ++i) {
        wall.rgba[i * 4 + 0] = r;
        wall.rgba[i * 4 + 1] = g;
        wall.rgba[i * 4 + 2] = b;
        wall.rgba[i * 4 + 3] = a;
    }
    return wall;
}
// Mean luminance of the levels the painter wrote, as dither_test.cpp measures it.
double meanLuminance(unsigned contrast, const std::vector<uint8_t>& levels, int width, int x0, int y0, int x1, int y1) {
    uint16_t panel[16], luma[256];
    pw3::panelLuminance(contrast, panel, luma);
    double sum = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) sum += panel[levels[size_t(y) * size_t(width) + size_t(x)] / 17];
    return sum / double((x1 - x0) * (y1 - y0));
}
}  // namespace

int main() {
    char temp[] = "/tmp/pw3-wallpaper-XXXXXX";
    CHECK(mkdtemp(temp) != nullptr);
    directory = temp;

    // ---- the folder -------------------------------------------------------
    writeFile(path("a.PNG"), kSolid1x1, kSolid1x1Size);
    writeFile(path("b.png"), kRgba2x2, kRgba2x2Size);
    writeText(path("c.txt"), "not a picture");
    writeText(path(".hidden.png"), "hidden");       // a dot file is not the user's
    CHECK(mkdir(path("d.png").c_str(), 0755) == 0);  // a folder called .png is not a picture
    CHECK(mkdir(path("empty").c_str(), 0755) == 0);
    int error = -1;
    const std::vector<std::string> names = pw3::wallpapersIn(directory.c_str(), &error);
    CHECK(error == 0);
    CHECK(names.size() == 2);
    if (names.size() == 2) CHECK(names[0] == "a.PNG" && names[1] == "b.png");
    // An empty folder is opened, and says so: the caller has to tell it from a
    // folder that is not there, because one is a setting and the other a fault.
    error = -1;
    CHECK(pw3::wallpapersIn(path("empty").c_str(), &error).empty());
    CHECK(error == 0);
    error = 0;
    CHECK(pw3::wallpapersIn(path("nothing-here").c_str(), &error).empty());
    CHECK(error == ENOENT);
    error = 0;
    CHECK(pw3::wallpapersIn("", &error).empty() && error != 0);
    error = 0;
    CHECK(pw3::wallpapersIn(nullptr, &error).empty() && error != 0);
    error = -1;
    CHECK(pw3::wallpapersIn(directory.c_str(), nullptr).size() == 2);  // the error out is optional
    rmdir(path("empty").c_str());
    rmdir(path("d.png").c_str());

    // ---- which picture ----------------------------------------------------
    CHECK(pw3::pickWallpaper(0, -1, 0) == -1);
    CHECK(pw3::pickWallpaper(1, -1, 12345) == 0);
    CHECK(pw3::pickWallpaper(1, 0, 12345) == 0);  // only one: it repeats
    for (int count = 2; count <= 6; ++count) {
        for (int previous = -1; previous < count; ++previous) {
            std::vector<bool> seen(size_t(count), false);
            for (uint32_t random = 0; random < 997; ++random) {
                const int index = pw3::pickWallpaper(count, previous, random);
                CHECK(index >= 0 && index < count);
                if (previous >= 0) CHECK(index != previous);
                if (index >= 0 && index < count) seen[size_t(index)] = true;
            }
            // Every picture is reachable; the one shown last is the only one not.
            for (int i = 0; i < count; ++i) CHECK(seen[size_t(i)] == (i != previous));
        }
    }

    // ---- fitting ----------------------------------------------------------
    pw3::Wallpaper wall;
    // The screen's own size: the pixels are the picture's, premultiplied.
    CHECK(pw3::loadWallpaper(path("b.png").c_str(), 2, 2, &wall));
    CHECK(wall.width == 2 && wall.height == 2 && wall.rgba.size() == 16);
    expectPixel(wall, 0, 0, 255, 0, 0, 255);
    expectPixel(wall, 1, 0, 0, 128, 0, 128);
    expectPixel(wall, 0, 1, 0, 0, 0, 0);
    expectPixel(wall, 1, 1, 64, 64, 64, 64);
    for (size_t i = 0; i < wall.rgba.size(); i += 4)
        CHECK(wall.rgba[i] <= wall.rgba[i + 3] && wall.rgba[i + 1] <= wall.rgba[i + 3] && wall.rgba[i + 2] <= wall.rgba[i + 3]);
    // Wider than the screen: cover crops the two sides equally.
    writeFile(path("gray.png"), kGray4x2, kGray4x2Size);
    CHECK(pw3::loadWallpaper(path("gray.png").c_str(), 2, 2, &wall));
    expectPixel(wall, 0, 0, 85, 85, 85, 255);
    expectPixel(wall, 1, 0, 170, 170, 170, 255);
    expectPixel(wall, 0, 1, 170, 170, 170, 255);
    expectPixel(wall, 1, 1, 85, 85, 85, 255);
    // One pixel for the whole screen.
    CHECK(pw3::loadWallpaper(path("a.PNG").c_str(), 3, 5, &wall));
    CHECK(wall.width == 3 && wall.height == 5);
    for (int y = 0; y < 5; ++y) for (int x = 0; x < 3; ++x) expectPixel(wall, x, y, 100, 150, 200, 255);
    // Not a picture, or not there.
    CHECK(!pw3::loadWallpaper(path("c.txt").c_str(), 2, 2, &wall));
    CHECK(!pw3::loadWallpaper(path("missing.png").c_str(), 2, 2, &wall));
    CHECK(!pw3::loadWallpaper(nullptr, 2, 2, &wall));
    CHECK(!pw3::loadWallpaper(path("b.png").c_str(), 0, 2, &wall));

    // ---- painting over the frame before -----------------------------------
    const int W = 64, H = 32;
    std::vector<uint8_t> background(size_t(W) * H), shown(size_t(W) * H), out(size_t(W) * H);
    for (size_t i = 0; i < background.size(); ++i) { background[i] = 200; shown[i] = 187; }  // level 11
    // Fully transparent: the frame before stays, pixel for pixel.
    {
        const pw3::Wallpaper clear = flatWallpaper(W, H, 0, 0, 0, 0);
        pw3::WallpaperPainter painter(W, H, 12);
        painter.setWallpaper(&clear);
        for (int y = 0; y < H; ++y) {
            painter.row(y, background.data() + size_t(y) * W, shown.data() + size_t(y) * W, out.data());
            for (int x = 0; x < W; ++x) CHECK(out[size_t(x)] == 187);
        }
        // Nothing behind: transparent is paper white.
        for (int y = 0; y < H; ++y) {
            painter.row(y, nullptr, nullptr, out.data());
            for (int x = 0; x < W; ++x) CHECK(out[size_t(x)] == 255);
        }
    }
    // Opaque grey: the dithered picture, its brightness kept.
    {
        const pw3::Wallpaper grey = flatWallpaper(W, H, 128, 128, 128, 255);
        pw3::WallpaperPainter painter(W, H, 12);
        painter.setWallpaper(&grey);
        for (int y = 0; y < H; ++y) painter.row(y, nullptr, nullptr, out.data() + size_t(y) * W);
        uint16_t panel[16], luma[256];
        pw3::panelLuminance(12, panel, luma);
        CHECK(std::fabs(meanLuminance(12, out, W, 2, 2, W - 2, H - 2) - luma[128]) <= 0.01 * 65535);
        for (uint8_t level : out) CHECK(level % 17 == 0);
    }
    // Half transparent: the picture blended into the frame before, then dithered.
    {
        const pw3::Wallpaper half = flatWallpaper(W, H, 64, 64, 64, 128);  // premultiplied 50% of 128
        pw3::WallpaperPainter painter(W, H, 12);
        painter.setWallpaper(&half);
        for (int y = 0; y < H; ++y)
            painter.row(y, background.data() + size_t(y) * W, shown.data() + size_t(y) * W, out.data() + size_t(y) * W);
        uint16_t panel[16], luma[256];
        pw3::panelLuminance(12, panel, luma);
        const int expected = int(luma[64 + divide255(200u * 127u)]);
        CHECK(std::fabs(meanLuminance(12, out, W, 2, 2, W - 2, H - 2) - expected) <= 0.02 * 65535);
    }
    // Without a picture set, a row is painted as paper rather than left as
    // whatever was in the buffer: the caller has nothing to catch.
    {
        pw3::WallpaperPainter none(4, 1, 12);
        uint8_t bare[4] = {1, 2, 3, 4};
        none.row(0, nullptr, nullptr, bare);
        for (uint8_t level : bare) CHECK(level == 255);
    }

    unlink(path("a.PNG").c_str());
    unlink(path("b.png").c_str());
    unlink(path("c.txt").c_str());
    unlink(path(".hidden.png").c_str());
    unlink(path("gray.png").c_str());
    rmdir(directory.c_str());
    std::printf(failures ? "SLEEP_WALLPAPER_TEST_FAILED %d\n" : "SLEEP_WALLPAPER_TEST_PASS%.0d\n", failures);
    return failures != 0;
}
