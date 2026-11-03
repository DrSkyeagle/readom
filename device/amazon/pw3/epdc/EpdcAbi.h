// SPDX-License-Identifier: Apache-2.0
// It restates, for interoperability, the ioctl interface of Lab126's
// include/linux/mxcfb.h, part of the GPL-2.0 kernel, whose COPYING leaves
// programs that use the kernel through normal system calls outside it.
// PW3 Lab126 Linux 3.0.35 userspace ioctl layout, pinned to the Amazon source.
// Keep the two Lab126 histogram fields: vanilla i.MX layouts are incompatible.
#pragma once
#include <cstdint>
#include <sys/ioctl.h>
namespace pw3 {
struct EpdcRect { uint32_t top, left, width, height; };
struct AltBuffer { uint32_t address, width, height; EpdcRect region; };
struct EpdcUpdate {
    EpdcRect region;
    uint32_t waveform, mode, marker, bwWaveform, grayWaveform;
    int32_t temperature;
    uint32_t flags;
    AltBuffer alt;
};
struct Marker { uint32_t value, collision; };
static_assert(sizeof(EpdcUpdate) == 72, "Wrong Lab126 update ABI");
static_assert(sizeof(Marker) == 8, "Wrong Lab126 marker ABI");
constexpr unsigned long SendUpdate = _IOW('F', 0x2E, EpdcUpdate);
constexpr unsigned long WaitUpdate = _IOWR('F', 0x2F, Marker);
constexpr unsigned long SetAutoUpdate = _IOW('F', 0x2D, uint32_t);
constexpr unsigned long SetUpdateScheme = _IOW('F', 0x32, uint32_t);
// MXCFB_SET_AUTO_UPDATE_MODE: updates happen only for explicitly sent regions.
constexpr uint32_t AutoUpdateRegion = 0;
// MXCFB_SET_UPDATE_SCHEME values are SNAPSHOT 0, QUEUE 1, QUEUE_AND_MERGE 2.
// Only the queue paths power the panel and its clocks up around an update;
// the snapshot path submits to a controller that may be powered down.
constexpr uint32_t UpdateSchemeQueueAndMerge = 2;
// TEMP_USE_AMBIENT in this Lab126 tree uses a cached index (initially 20 C).
// AUTO uses the display thermistor maintained by the PMIC ADC driver.
constexpr int32_t TempUseAuto = 0x1001;
// MXCFB_SEND_UPDATE accepts Lab126 logical selectors, then the driver maps
// them into the loaded file's mode slots. This is not an arbitrary file index.
constexpr uint32_t WaveformGc16 = 2;
struct EpdcAndroidInfo {
    uint32_t version, size, flags, modeVersion, gc16Mode, rotate, xres, yres;
    uint32_t temperatureAuto, modeCount, reserved[6];
};
static_assert(sizeof(EpdcAndroidInfo) == 64, "Wrong Android EPDC info ABI");
constexpr unsigned long GetAndroidInfo = _IOR('F', 0x80, EpdcAndroidInfo);
}
