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
// Empty: no such folder, nothing readable in it, or only other files. `error`,
// when given, is why: the errno of a folder that could not be opened, or 0 when
// it was opened and simply held no picture. The display HAL logs it, because
// "the default pictures are showing" and "the folder is there but empty" look
// the same on the screen and are not the same thing to a person debugging it.
std::vector<std::string> wallpapersIn(const char* directory, int* error = nullptr);
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

// The compact form of a fitted picture: the two bytes a grey panel can use, the
// luma of the premultiplied colour and the alpha, instead of 4 bytes of RGBA.
// 2 bytes a pixel is 3.1 MB a screen, and the blend and the dither need nothing
// else (WallpaperPainter). What is kept decoded (WallpaperSet) is kept this way.
struct WallpaperLevels {
    int width = 0, height = 0;
    std::vector<uint8_t> luma;   // premultiplied colour luma, one byte a pixel
    std::vector<uint8_t> alpha;  // one byte a pixel
};
WallpaperLevels levelsOf(const Wallpaper& wallpaper);

// The pictures of a folder, decoded and fitted once. The folder that ships in
// the image cannot change while the process runs, so the display HAL reads it
// when it opens and every sleep after that only picks one: the decode that used
// to sit in the sleep path (100-300 ms per picture on this CPU) happens at
// start-up instead (README.md, t29-sleep-wallpaper-perf.md). Memory: 3.1 MB a
// picture in the compact form, 9.3 MB for the three that ship.
class WallpaperSet {
public:
    // Reads every picture in `directory` (wallpapersIn order) and fits each to
    // `width` x `height`. Returns how many were loaded; a picture that cannot be
    // read is skipped, because a folder in the image that cannot be read is a
    // build fault the caller says once, not a reason to leave the screen as it is.
    int load(const char* directory, int width, int height);
    int size() const { return int(names_.size()); }
    // Which picture to show, given the name shown last: any but that one when
    // there is a choice (pickWallpaper). -1: none.
    int pick(const std::string& previous, uint32_t random) const;
    const WallpaperLevels* picture(int index) const;
    // Empty for an index that is not a picture.
    const std::string& name(int index) const;
    void clear();

private:
    std::vector<std::string> names_;
    std::vector<WallpaperLevels> pictures_;
};

// Paints the wallpaper into the panel's level bytes, row by row, over what the
// panel was showing. The caller owns the row loops: this holds only the picture,
// one row of luma and the error diffusion (Dither.h), so that a sleep costs one
// row of work and not a second screen-sized buffer beside the picture.
class WallpaperPainter {
public:
    WallpaperPainter(int width, int height, unsigned contrast);
    // The picture to paint, fitted to the painter's size; must outlive it. One
    // of the two forms, not both: RGBA (setWallpaper) or the compact levels the
    // prefetched pictures are kept in (setLevels).
    void setWallpaper(const Wallpaper* wallpaper) { wallpaper_ = wallpaper; }
    void setLevels(const WallpaperLevels* levels) { levels_ = levels; }
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
    const WallpaperLevels* levels_ = nullptr;
    Dither dither_;
    std::vector<uint8_t> luma_;
};
}  // namespace pw3
