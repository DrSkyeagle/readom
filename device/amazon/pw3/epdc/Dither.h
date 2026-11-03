// SPDX-License-Identifier: Apache-2.0
// A picture spread over the panel's 16 levels by error diffusion, for what is
// shown once and looked at, such as the sleep picture.
//
// Floyd-Steinberg in luminance, row by row in alternating directions: every
// area keeps its average brightness, so a gradient or a photograph shows no
// bands. A pixel whose luma the panel shows exactly as a level stays that
// level (black and white always: Tone.h), unless error from a neighbour
// reaches it.
//
// The composed screen is not dithered: what changes from frame to frame
// would move its pattern, and text would lose its edges.
#pragma once
#include <cstdint>
#include <vector>

namespace pw3 {
class Dither {
public:
    // contrast: as PanelPolicy::contrast.
    Dither(int width, unsigned contrast);
    // The next row of the picture: luma in, level bytes (0, 17, ... 255) out.
    void row(const uint8_t* luma, uint8_t* out);
private:
    int nearest(int32_t value) const;
    int width_;
    bool leftward_ = false;
    uint16_t levels_[16], luma_[256];
    // bounds_[k]: the lowest luminance nearer to level k + 1 than to level k.
    int32_t bounds_[15];
    // Error carried into this row and into the next, one pixel of margin at either end.
    std::vector<int32_t> here_, below_;
};
}  // namespace pw3
