// SPDX-License-Identifier: Apache-2.0
// The sleep wallpaper: a folder of pictures, one shown when the screen goes
// off, and the frame before it showing through the picture's transparent pixels.
//
// The panel keeps what it shows after the power goes, which is why there is a
// sleep picture at all (gralloc/framebuffer.cpp, opsSleep). It also means that a
// picture with transparent pixels can do something a PGM cannot: the frame that
// was on the screen before can stay there, exactly as it was drawn, and the
// picture is laid over it. Two things make that "exactly": the panel object
// holds the level of every pixel it believes the panel shows (Panel::shown), and
// the pixel values are written back unchanged for the fully transparent parts
// instead of being dithered with the rest. Partly transparent pixels are blended
// into the luma of the frame before (Panel::canvas) and dithered with the picture,
// which is the closest the 16 levels of the panel can come to a real alpha.
#pragma once
#include "Dither.h"
#include "Png.h"
#include <cstdint>
#include <string>
#include <vector>

namespace pw3 {
// The wallpapers in `directory`: regular files whose name ends in ".png" in any
// case, in name order so that two starts with the same folder choose alike.
// Empty: no such folder, nothing readable in it, or only other files.
std::vector<std::string> wallpapersIn(const char* directory);
// Which of `count` to show, given the one last shown: any but that one, when
// there is a choice. `random` is the caller's randomness, kept out of here so
// that the rule can be tested. -1: nothing to show.
int pickWallpaper(int count, int previous, uint32_t random);
// Randomness for that choice, from the clock and the process. Not from rand():
// the display HAL is a library in a process that never seeds a generator, and
// the same picture at every sleep would be the point of the folder missed.
uint32_t wallpaperRandom();

// A picture fitted to the screen: `width` x `height` premultiplied RGBA.
// Premultiplied because it is scaled: interpolating straight alpha pulls the
// colour of transparent pixels (usually black) into every soft edge.
struct Wallpaper {
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;  // premultiplied RGBA, row after row
};
// Reads the picture at `path` and fits it to `width` x `height`: scaled to
// cover the screen, the overflow cropped equally at both sides, bilinear.
// False if it is not a picture this build can read (Png.h).
bool loadWallpaper(const char* path, int width, int height, Wallpaper* out);

// Paints the wallpaper into the panel's level bytes, row by row, over what the
// panel was showing. The caller owns the row loops: this holds only the picture,
// one row of luma and the error diffusion (Dither.h), so that a sleep costs one
// row of work and not a second screen-sized buffer beside the picture.
class WallpaperPainter {
public:
    WallpaperPainter(int width, int height, unsigned contrast);
    // The picture to paint, fitted to the painter's size; must outlive it.
    void setWallpaper(const Wallpaper* wallpaper) { wallpaper_ = wallpaper; }
    // Row `y` of the screen -> `width` level bytes. `background` is the luma of
    // the frame the panel was showing and `shown` the level bytes of its picture
    // (Panel::canvas, Panel::shown); both null: there is nothing behind, and
    // transparent pixels are painted as paper. With both, a pixel the picture
    // leaves fully transparent is written as the level `shown` already has, and
    // the panel needs to change nothing there.
    void row(int y, const uint8_t* background, const uint8_t* shown, uint8_t* out);

private:
    int width_, height_;
    const Wallpaper* wallpaper_ = nullptr;
    Dither dither_;
    std::vector<uint8_t> luma_;
};
}  // namespace pw3
