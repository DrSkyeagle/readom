// SPDX-License-Identifier: Apache-2.0
// Layer pixels to 8-bit luma, combined with the luma already in the output.
// The panel shows grey levels only, so the composer works in luma from the
// first layer on: one byte per output pixel, instead of composing RGBX and
// converting the result. Luma is linear in the colour channels, so blending
// luma gives the luma of the blended colours up to rounding.
#pragma once
#include <cstdint>

namespace pw3 {
struct LumaMode {
    bool opaque;     // source alpha ignored: the plane alpha is the coverage
    bool coverage;   // colour not premultiplied by the source alpha
    bool bgra;       // the first byte is blue
    uint8_t plane;   // layer-wide alpha
};
// A layer whose pixels replace what is below them.
inline bool lumaReplaces(const LumaMode& m) { return m.opaque && m.plane == 255; }
// Each row function returns whether it changed any byte of `out`. The panel
// turns the canvas into levels and compares that against what it shows, which
// costs 19-46 ms for a whole screen (Test V51); a frame that composes to the
// same bytes cannot change the screen, so saying so here lets the whole of it
// be skipped. Two of the three already read `out`, and on this write-allocate
// cache the third reads the line it writes anyway, so the comparison is a
// vector operation on a line that is already there.
//
// luma = (77 R + 150 G + 29 B + 128) >> 8 for `count` four-byte pixels.
bool lumaRow(const uint8_t* source, uint8_t* out, int count, bool bgra);
// out = luma(source) * weight + out * (255 - alpha) / 255, rounded per product.
bool lumaBlendRow(const uint8_t* source, uint8_t* out, int count, const LumaMode& mode);
// A buffer-less dim surface: black with a layer-wide alpha.
bool lumaDimRow(uint8_t* out, int count, uint8_t alpha);
// out = value, for the paper the composer starts a row from.
bool lumaFillRow(uint8_t* out, uint8_t value, int count);
// The definitions the vector code has to match; used by the tests.
uint8_t lumaOf(const uint8_t* pixel, bool bgra);
uint8_t lumaBlendOf(const uint8_t* pixel, uint8_t below, const LumaMode& mode);
}  // namespace pw3
