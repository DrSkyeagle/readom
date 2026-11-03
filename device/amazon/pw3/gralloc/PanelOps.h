// SPDX-License-Identifier: Apache-2.0
// Private interface between the PW3 composer and the PW3 framebuffer device,
// both loaded into SurfaceFlinger: the composer draws luma straight into the
// panel's canvas and names the changed area, instead of handing over a
// finished RGBX frame through post(). Published in
// framebuffer_device_t::reserved_proc[0].
#pragma once
#include "../epdc/Rect.h"
#include <hardware/fb.h>
#include <cstddef>
#include <cstdint>

struct Pw3PanelOps {
    uint32_t magic;    // Pw3PanelOpsMagic
    uint32_t version;  // 3
    // width*height luma bytes, row after row. Valid for the device's lifetime.
    uint8_t* (*canvas)(framebuffer_device_t* device, int* width, int* height);
    // The next present() must follow a composition of the whole screen.
    bool (*needsFull)(framebuffer_device_t* device);
    // The canvas inside the given screen rectangles is the new picture there.
    int (*present)(framebuffer_device_t* device, const pw3::Rect* damage, size_t count);
    // Before the panel is powered down: show the sleep picture, or else the
    // grey levels of everything still in its first pass, and wait for the
    // waveform. After a picture, needsFull() is true: the next present()
    // changes what differs from the picture, without a full flash.
    void (*sleep)(framebuffer_device_t* device);
    // One line of counters for dumpsys.
    void (*describe)(framebuffer_device_t* device, char* text, int size);
    // What the next present() draws in these rectangles replaces what is there
    // rather than changing it: a window appeared or went away. The panel
    // clears them with the update that brings the new content, because GL16
    // would leave the old window showing under the new one (Panel::replaced).
    void (*replaced)(framebuffer_device_t* device, const pw3::Rect* areas, size_t count);
};
constexpr uint32_t Pw3PanelOpsMagic = 0x50573350;  // "PW3P"
inline const Pw3PanelOps* pw3PanelOps(const framebuffer_device_t* device) {
    const auto* ops = static_cast<const Pw3PanelOps*>(device->reserved_proc[0]);
    return ops && ops->magic == Pw3PanelOpsMagic && ops->version == 3 ? ops : nullptr;
}
