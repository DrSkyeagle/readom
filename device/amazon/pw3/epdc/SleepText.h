// SPDX-License-Identifier: Apache-2.0
// The line written on the sleep picture: when the device went to sleep, the
// date and the battery level, "休眠于 21:47 · 10月15日 · 电量 63%". It is
// written once, as the screen goes off; the panel keeps it at no cost, and
// nothing wakes the device to change it. A clock that ticks every minute
// would cut the standby time to about a third
// (work/kindle-research/power-sleep-plan-research.md, section 5).
//
// The pieces of the line come drawn once on one strip, coverage 0..255
// (sleep/make-sleep-picture.py glyphs): the display HAL has no font engine.
#pragma once
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace pw3 {
// The pieces of the line, in order. `when`: local time, or null when the
// clock is not set; `battery`: percent, or negative when unknown. Empty: there
// is nothing to write.
std::vector<std::string> sleepLine(bool chinese, const std::tm* when, int battery);

class SleepText {
public:
    // The strip (8-bit binary PGM) and its index. False if either is unusable.
    bool load(const char* strip, const char* index);
    // Places the line centred on `center`, its top at `top`, inside a picture
    // `width` x `height`. False if a piece is not on the strip or the line
    // does not fit.
    bool place(const std::vector<std::string>& pieces, int center, int top, int width, int height);
    // Writes the placed line into row `y` of a picture (luma, `width` bytes), in `ink`.
    void row(int y, uint8_t* luma, int width, uint8_t ink) const;
    int height() const { return height_; }
private:
    struct Piece { std::string text; int x, width; };
    int stripWidth_ = 0, height_ = 0;
    std::vector<uint8_t> strip_;
    std::vector<Piece> index_;
    // The placed line: lineWidth_ x height_ coverage, its left and top in the picture.
    std::vector<uint8_t> line_;
    int lineWidth_ = 0, left_ = 0, top_ = 0;
};
}  // namespace pw3
