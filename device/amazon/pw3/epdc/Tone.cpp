// SPDX-License-Identifier: Apache-2.0
#include "Tone.h"
#include <algorithm>
#include <cmath>

namespace pw3 {
namespace {
// Luminance (white = 1) of an sRGB grey, and CIE lightness (white = 100) of a luminance.
double luminance(int luma) {
    const double v = luma / 255.0;
    return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}
double lightness(double y) { return y > 216.0 / 24389.0 ? 116.0 * std::cbrt(y) - 16.0 : y * 24389.0 / 27.0; }
double luminanceOf(double l) { return l > 8.0 ? std::pow((l + 16.0) / 116.0, 3.0) : l * 27.0 / 24389.0; }
uint16_t scaled(double y) { return uint16_t(std::lround(65535.0 * std::min(1.0, std::max(0.0, y)))); }
}  // namespace

void panelLevels(unsigned contrast, uint8_t levels[256]) {
    if (contrast < 2) {
        for (unsigned v = 0; v < 256; ++v) levels[v] = uint8_t((v + 8u) / 17u * 17u);
        return;
    }
    // Lightness of the panel's black with the panel's white as 100.
    const double black = lightness(1.0 / contrast);
    for (int v = 0; v < 256; ++v) {
        const double share = (lightness(luminance(v)) - black) / (100.0 - black);
        levels[v] = uint8_t(std::lround(15.0 * std::min(1.0, std::max(0.0, share))) * 17);
    }
}
void panelLuminance(unsigned contrast, uint16_t levels[16], uint16_t luma[256]) {
    if (contrast < 2) {
        for (int k = 0; k < 16; ++k) levels[k] = scaled(luminance(17 * k));
        for (int v = 0; v < 256; ++v) luma[v] = scaled(luminance(v));
        return;
    }
    const double black = lightness(1.0 / contrast);
    for (int k = 0; k < 16; ++k) levels[k] = scaled(luminanceOf(black + k * (100.0 - black) / 15.0));
    for (int v = 0; v < 256; ++v) luma[v] = std::max(levels[0], scaled(luminance(v)));
}
}  // namespace pw3
