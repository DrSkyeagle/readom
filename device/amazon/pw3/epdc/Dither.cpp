// SPDX-License-Identifier: Apache-2.0
#include "Dither.h"
#include "Tone.h"
#include <algorithm>
#include <stdexcept>

namespace pw3 {
Dither::Dither(int width, unsigned contrast) : width_(width) {
    if (width <= 0 || width > 4096) throw std::invalid_argument("Unsupported picture width");
    panelLuminance(contrast, levels_, luma_);
    for (int k = 0; k < 15; ++k) bounds_[k] = (int32_t(levels_[k]) + levels_[k + 1] + 1) / 2;
    here_.assign(size_t(width) + 2, 0);
    below_.assign(size_t(width) + 2, 0);
}
int Dither::nearest(int32_t value) const {
    int k = 0;
    while (k < 15 && value >= bounds_[k]) ++k;
    return k;
}
void Dither::row(const uint8_t* luma, uint8_t* out) {
    here_.swap(below_);
    std::fill(below_.begin(), below_.end(), 0);
    const int step = leftward_ ? -1 : 1;
    for (int i = 0, x = leftward_ ? width_ - 1 : 0; i < width_; ++i, x += step) {
        const int at = x + 1;  // past the margin
        const int32_t value = int32_t(luma_[luma[x]]) + here_[size_t(at)];
        const int k = nearest(value);
        out[x] = uint8_t(k * 17);
        // Weights 7, 3, 5, 1 in 16ths; the last share takes the rounding, so
        // no error is lost inside the picture.
        const int32_t error = value - levels_[k];
        const int32_t ahead = error * 7 / 16, back = error * 3 / 16, down = error * 5 / 16;
        here_[size_t(at + step)] += ahead;
        below_[size_t(at - step)] += back;
        below_[size_t(at)] += down;
        below_[size_t(at + step)] += error - ahead - back - down;
    }
    leftward_ = !leftward_;
}
}  // namespace pw3
