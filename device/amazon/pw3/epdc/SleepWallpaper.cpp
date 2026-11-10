// SPDX-License-Identifier: Apache-2.0
#include "SleepWallpaper.h"
#include "Luma.h"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace pw3 {
namespace {
// round(value / 255) for a product of two bytes, as Luma.cpp: no division on
// this processor, and the same rounding the composer uses keeps the blended
// luma consistent with it.
inline unsigned divide255(unsigned value) {
    const unsigned t = value + 128u;
    return (t + (t >> 8)) >> 8;
}
// A premultiplied pixel as the scaler works on it: 0..255 per channel, alpha too.
struct Premultiplied { double r, g, b, a; };
Premultiplied sample(const uint8_t* rgba, int width, int height, double x, double y) {
    // Clamped, so that the picture's own edge is repeated rather than a border
    // of black appearing where a pixel centre falls outside.
    const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
    const double fx = x - x0, fy = y - y0;
    auto pixel = [&](int px, int py) {
        px = std::min(std::max(px, 0), width - 1);
        py = std::min(std::max(py, 0), height - 1);
        const uint8_t* p = rgba + (size_t(py) * size_t(width) + size_t(px)) * 4;
        const double a = p[3] / 255.0;
        return Premultiplied{p[0] * a, p[1] * a, p[2] * a, double(p[3])};
    };
    const Premultiplied p00 = pixel(x0, y0), p10 = pixel(x0 + 1, y0);
    const Premultiplied p01 = pixel(x0, y0 + 1), p11 = pixel(x0 + 1, y0 + 1);
    auto mix = [&](double a00, double a10, double a01, double a11) {
        const double top = a00 + (a10 - a00) * fx, bottom = a01 + (a11 - a01) * fx;
        return top + (bottom - top) * fy;
    };
    return Premultiplied{mix(p00.r, p10.r, p01.r, p11.r), mix(p00.g, p10.g, p01.g, p11.g),
                         mix(p00.b, p10.b, p01.b, p11.b), mix(p00.a, p10.a, p01.a, p11.a)};
}
inline uint8_t toByte(double value) {
    return uint8_t(std::min(255.0, std::max(0.0, std::round(value))));
}
bool endsInPng(const std::string& name) {
    if (name.size() <= 4) return false;
    std::string tail = name.substr(name.size() - 4);
    for (char& c : tail) c = char(std::tolower(static_cast<unsigned char>(c)));
    return tail == ".png";
}
}  // namespace

std::vector<std::string> wallpapersIn(const char* directory, int* error) {
    std::vector<std::string> names;
    if (error) *error = 0;
    if (!directory || !*directory) {
        if (error) *error = EINVAL;
        return names;
    }
    DIR* dir = opendir(directory);
    if (!dir) {
        if (error) *error = errno;
        return names;
    }
    while (const dirent* entry = readdir(dir)) {
        const char* name = entry->d_name;
        // Skip "." and "..", and anything hidden: a folder that the user sees
        // and the folder this reads have to be the same folder.
        if (!name || name[0] == '.') continue;
        if (!endsInPng(name)) continue;
        const std::string path = std::string(directory) + "/" + name;
        struct stat st{};
        // A directory called "x.png" is not a picture; asking for it at every
        // sleep would only be a decode that fails.
        if (stat(path.c_str(), &st) || !S_ISREG(st.st_mode)) continue;
        names.push_back(name);
    }
    closedir(dir);
    std::sort(names.begin(), names.end());
    return names;
}

int pickWallpaper(int count, int previous, uint32_t random) {
    if (count <= 0) return -1;
    if (count == 1) return 0;
    // One of the other pictures, so no sleep shows the same one twice in a row
    // while there is a choice; the whole screen changing is the point of it.
    const bool avoid = previous >= 0 && previous < count;
    int index = int(random % uint32_t(avoid ? count - 1 : count));
    if (avoid && index >= previous) ++index;
    return index;
}

uint32_t wallpaperRandom() {
    struct timespec now {};
    clock_gettime(CLOCK_MONOTONIC, &now);
    // A splitmix64 finaliser: the two clock halves on their own are too regular
    // to pick between two pictures well.
    uint64_t value = uint64_t(now.tv_sec) * 1000000007ULL + uint64_t(now.tv_nsec) + uint64_t(getpid()) * 0x9E3779B97F4A7C15ULL;
    value ^= value >> 30;
    value *= 0xBF58476D1CE4E5B9ULL;
    value ^= value >> 27;
    value *= 0x94D049BB133111EBULL;
    value ^= value >> 31;
    return uint32_t(value);
}

bool loadWallpaper(const char* path, int width, int height, Wallpaper* out) {
    if (!out || !path || width <= 0 || height <= 0) return false;
    out->width = out->height = 0;
    out->rgba.clear();
    Png png;
    if (!loadPng(path, &png)) return false;
    // Cover: the screen is filled, and what does not fit is cropped from the two
    // sides equally. A picture of the screen's own shape is only scaled.
    const double scale = std::max(double(width) / double(png.width), double(height) / double(png.height));
    const double offsetX = (double(png.width) * scale - double(width)) / 2.0;
    const double offsetY = (double(png.height) * scale - double(height)) / 2.0;
    std::vector<uint8_t> fitted;
    try {
        fitted.resize(size_t(width) * size_t(height) * 4);
    } catch (...) {
        return false;
    }
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            // Where the centre of this screen pixel falls in the picture, in
            // picture pixels.
            const double sx = (double(x) + offsetX + 0.5) / scale - 0.5;
            const double sy = (double(y) + offsetY + 0.5) / scale - 0.5;
            const Premultiplied p = sample(png.rgba.data(), png.width, png.height, sx, sy);
            uint8_t* target = fitted.data() + (size_t(y) * size_t(width) + size_t(x)) * 4;
            const uint8_t alpha = toByte(p.a);
            // Premultiplied to a byte: a channel can never exceed its alpha, and
            // rounding both keeps that (fitted[r] <= fitted[a]).
            target[3] = alpha;
            target[0] = std::min(alpha, toByte(p.r));
            target[1] = std::min(alpha, toByte(p.g));
            target[2] = std::min(alpha, toByte(p.b));
        }
    }
    out->rgba.swap(fitted);
    out->width = width;
    out->height = height;
    return true;
}

WallpaperPainter::WallpaperPainter(int width, int height, unsigned contrast)
    : width_(width), height_(height), dither_(width, contrast), luma_(size_t(width)) {}

void WallpaperPainter::row(int y, const uint8_t* background, const uint8_t* shown, uint8_t* out) {
    if (!out) return;
    uint8_t* const luma = luma_.data();
    if (!wallpaper_ || wallpaper_->width != width_ || wallpaper_->height != height_ || y < 0 || y >= height_) {
        // No picture to paint: the frame before, or paper. Never reachable from
        // opsSleep, which only constructs a painter around a loaded picture.
        for (int x = 0; x < width_; ++x) luma[x] = background ? background[x] : 255;
        dither_.row(luma, out);
        return;
    }
    const uint8_t* const source = wallpaper_->rgba.data() + size_t(y) * size_t(width_) * 4;
    for (int x = 0; x < width_; ++x) {
        const uint8_t* const pixel = source + size_t(x) * 4;
        const unsigned alpha = pixel[3];
        // Luma of the premultiplied colour, which is the colour times its alpha:
        // adding what is behind it is then one add, exactly the blend wanted.
        const unsigned over = lumaOf(pixel, false);
        if (!background) {
            luma[x] = uint8_t(std::min(255u, over + (255u - alpha)));
        } else {
            luma[x] = uint8_t(std::min(255u, over + divide255(unsigned(background[x]) * (255u - alpha))));
        }
    }
    dither_.row(luma, out);
    // The picture asked for nothing here: leave the pixel at the level the panel
    // already shows, so the frame before it stays exactly as it was drawn rather
    // than a dither of it. Error diffusion ran through the pixel anyway, so the
    // picture's own edges keep their brightness.
    if (!background || !shown) return;
    for (int x = 0; x < width_; ++x) {
        if (source[size_t(x) * 4 + 3] != 0) continue;
        out[x] = shown[x];
    }
}
}  // namespace pw3
