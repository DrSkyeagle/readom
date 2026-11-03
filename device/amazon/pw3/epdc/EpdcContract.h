// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "EpdcAbi.h"
#include <linux/fb.h>
namespace pw3 {
// The captured PW3 portrait framebuffer is 1072 x 1448, FB_ROTATE_CCW.
// A known file version is required; the kernel supplies the actual GC16 index.
inline bool validEpdcContract(const EpdcAndroidInfo& info, const fb_var_screeninfo& screen) {
    if (info.version != 1 || info.size != sizeof(info) || info.flags != 3 ||
        info.xres != screen.xres || info.yres != screen.yres || info.rotate != screen.rotate ||
        info.xres != 1072 || info.yres != 1448 || info.rotate != FB_ROTATE_CCW ||
        info.temperatureAuto != uint32_t(TempUseAuto) || info.modeCount < 3 || info.modeCount > 256 ||
        info.gc16Mode != 2 || info.gc16Mode >= info.modeCount) return false;
    switch (info.modeVersion) {
        case 7: case 18: case 19: case 24: case 25: break;
        default: return false;
    }
    for (const auto field : info.reserved) if (field) return false;
    return true;
}
} // namespace pw3
