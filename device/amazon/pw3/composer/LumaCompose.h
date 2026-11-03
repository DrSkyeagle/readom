// SPDX-License-Identifier: Apache-2.0
// Composition of 32-bit layers into the panel's luma canvas, limited to the
// part of the screen that changed.
#pragma once
#include "../epdc/Luma.h"
#include "../epdc/Rect.h"
#include <cstddef>
#include <cstdint>

namespace pw3 {
// What gralloc hands out that an application can draw into (gralloc.cpp). The
// two narrow ones are read a pixel at a time into 32-bit form; 8888 is the
// common case and keeps its own path. RGBA_FP16 and RAW16 are allocated too
// but reach no layer here: one needs a wide-gamut EGL configuration this
// display does not offer, the other a camera.
enum class PixelFormat { Rgba, Rgbx, Bgra, Rgb888, Rgb565 };
inline int bytesPerPixel(PixelFormat format) {
    return format == PixelFormat::Rgb565 ? 2 : format == PixelFormat::Rgb888 ? 3 : 4;
}
enum class Blend { Opaque, Premultiplied, Coverage };
struct Image {
    const uint8_t* pixels;
    size_t bytes;
    int width, height, stride;
    PixelFormat format;
};
struct Layer {
    bool dim;            // no buffer: black, weighted by the plane alpha
    Image image;
    Rect crop, frame;
    Blend blend;
    uint8_t alpha;       // plane alpha
    // Screen rectangles of the layer that no opaque layer above it hides.
    // SurfaceFlinger supplies them disjoint.
    const Rect* visible;
    size_t visibleCount;
    // HAL_TRANSFORM_* of the crop: flip horizontally (1), flip vertically
    // (2), then rotate 90 degrees clockwise (4), as SurfaceFlinger's
    // Transform::set() composes them. A display turned to landscape gives
    // every layer a rotation.
    uint32_t transform;
};
// The transform bits.
const uint32_t FlipH = 1, FlipV = 2, Rot90 = 4;
struct Canvas { uint8_t* luma; int width, height; };  // one byte per pixel, stride == width
// What the canvas holds where no layer is: the panel at rest.
const uint8_t Paper = 255;

// Integer crops, flipped or rotated by multiples of 90 degrees at most. The
// crop may be any size: a layer whose frame is larger or smaller is sampled,
// taking for each frame pixel the crop pixel its centre falls in. Nearest
// rather than interpolated, because the panel shows sixteen grey levels and an
// interpolated edge lands on the same level as a nearest one nearly always,
// while costing four reads per pixel on a CPU that has to finish a frame.
//
// Everything the layer list can hold is composed here. What this refuses goes
// through SwiftShader instead, which is 10 to 23 seconds a frame on this CPU
// (Test N35) -- a frozen screen, so the answer to a layer that is merely
// awkward is to read it a pixel at a time, not to refuse it.
bool validImage(const Image& image);
// Whether the frame shows the crop at its own size.
bool unscaled(const Layer& layer);
bool supported(const Layer& layer, int width, int height);
LumaMode modeOf(const Layer& layer);
// True when the layer fully defines every pixel of its visible region.
bool replaces(const Layer& layer);
// Screen area that a change inside `changed` (buffer coordinates) can touch.
Rect onScreen(const Layer& layer, const Rect& changed);
// Recomposes `area` of the canvas from all layers, bottom-most first. Only
// pixels inside `area` are written; the result there is what composing the
// whole screen would give. Returns whether any byte of the canvas changed:
// false means this frame cannot change the screen, so the panel need not be
// given the area at all (Luma.h). A pixel written twice in a row, by a layer
// and then by one above it, counts as changed even if it ends as it began;
// the answer is never wrongly false.
bool compose(const Layer* layers, size_t count, Canvas canvas, Rect area);
}  // namespace pw3
