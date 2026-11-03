// SPDX-License-Identifier: Apache-2.0
// Which of the panel's 16 grey levels shows a luma value.
//
// Applications draw for a screen whose white is a thousand times as bright as
// its black. The panel's white is about twelve times as bright as its black.
// Spreading luma 0..255 evenly over the 16 levels keeps the order of the greys
// and loses what makes them readable, their contrast against the white they
// stand on: the grey Android uses for every second line of text (#757575,
// 5.6 times darker than white) came out 2.9 times darker than the panel's
// white (Test V34: "I cannot make out the interface").
//
// So a grey is shown by the level whose luminance relative to the panel's
// white is nearest, in lightness, to the grey's luminance relative to the
// white it was drawn for. Contrast against white is kept; what would have to
// be darker than the panel's black is black.
//
// Taken as given, not measured: luma is an sRGB grey; the 16 levels are evenly
// spaced in lightness (CIE L*) between the panel's black and its white, which
// is what its waveforms are made for.
#pragma once
#include <cstdint>

namespace pw3 {
// levels[luma]: the level as its framebuffer byte, 0, 17, ... 255.
// contrast: the luminance of the panel's white over that of its black.
// Below 2: luma spread evenly over the levels.
void panelLevels(unsigned contrast, uint8_t levels[256]);
// For pictures spread over the levels by error diffusion (Dither.h): the
// luminance, relative to the panel's white and in units of 1/65535, of each
// level and of each luma value as the panel is to show it. The same model as
// panelLevels(): with a contrast, lightness relative to white is kept and
// what would be darker than the panel's black is black (luma[0] == levels[0]);
// below 2, level k is luma 17 k. White is levels[15] == luma[255] == 65535.
void panelLuminance(unsigned contrast, uint16_t levels[16], uint16_t luma[256]);
}  // namespace pw3
