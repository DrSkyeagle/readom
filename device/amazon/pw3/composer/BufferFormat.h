// SPDX-License-Identifier: Apache-2.0
// The gralloc formats the composer reads, and how to say so once.
#pragma once
#include "LumaCompose.h"
#include "../gralloc/PixelBytes.h"

namespace pw3 {
// True when a buffer of this HAL format can be composed here, with the
// composer's name for it. RGBA_FP16 and RAW16 are allocated by this gralloc
// but reach no layer: one needs a wide-gamut EGL configuration this display
// does not offer, the other a camera. A format whose two tables disagree on
// the size of a pixel is refused rather than read with the wrong stride --
// the composer and the allocator have to mean the same buffer.
inline bool bufferFormat(int hal, PixelFormat* out) {
    switch (hal) {
        case HAL_PIXEL_FORMAT_RGBA_8888: *out = PixelFormat::Rgba; break;
        case HAL_PIXEL_FORMAT_RGBX_8888: *out = PixelFormat::Rgbx; break;
        case HAL_PIXEL_FORMAT_BGRA_8888: *out = PixelFormat::Bgra; break;
        case HAL_PIXEL_FORMAT_RGB_888: *out = PixelFormat::Rgb888; break;
        case HAL_PIXEL_FORMAT_RGB_565: *out = PixelFormat::Rgb565; break;
        default: return false;
    }
    return pw3_pixel_bytes(hal) == bytesPerPixel(*out);
}
}  // namespace pw3
