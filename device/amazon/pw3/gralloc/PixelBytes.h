// SPDX-License-Identifier: Apache-2.0
// How many bytes a pixel takes, for every format this gralloc allocates.
//
// One table, because the allocator sizes a buffer with it and every reader
// walks rows with it, and the two have to be the same number. They were two
// tables once: taking RGB_565 and RGB_888 into the composer left the virtual
// display's row arithmetic at four bytes a pixel, which reads and writes past
// the end of what was allocated (2026-10-19). A second table is a bug waiting
// for the next format.
#pragma once
#include <system/graphics.h>

// 0: this gralloc does not allocate the format, so nothing may read it either.
static inline int pw3_pixel_bytes(int format) {
    switch (format) {
        case HAL_PIXEL_FORMAT_RGBA_FP16:
            return 8;
        case HAL_PIXEL_FORMAT_RGBA_8888:
        case HAL_PIXEL_FORMAT_RGBX_8888:
        case HAL_PIXEL_FORMAT_BGRA_8888:
            return 4;
        case HAL_PIXEL_FORMAT_RGB_888:
            return 3;
        case HAL_PIXEL_FORMAT_RGB_565:
        case HAL_PIXEL_FORMAT_RAW16:
            return 2;
        default:
            return 0;
    }
}
