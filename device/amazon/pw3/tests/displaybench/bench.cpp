// SPDX-License-Identifier: Apache-2.0
// Times the CPU work the composer and the framebuffer device do for one frame
// of the PW3 panel, using their own code on synthetic layers. Prints
// milliseconds (best and median of nine runs); it touches no display hardware.
#include "bench.h"
#include "LumaCompose.h"
#include "Panel.h"
#include <cstring>

int main() {
    using namespace bench;
    printf("PW3 display CPU cost, %dx%d (layer stride %d)\n", W, H, S);
    const size_t bytes = size_t(S) * H * 4;
    // An opaque page with text-like rows, a window that is transparent except
    // for opaque blocks, and one whose every pixel is translucent.
    std::vector<uint8_t> page(bytes), window(bytes), glass(bytes), other(bytes);
    for (int y = 0; y < H; ++y) for (int x = 0; x < S; ++x) {
        uint8_t* p = &page[(size_t(y) * S + x) * 4];
        const uint8_t v = (y % 40 < 22 && ((x * 7 + y * 13) % 11) < 4) ? uint8_t(rnd() % 3 == 0 ? 90 : 0) : 255;
        p[0] = p[1] = p[2] = v; p[3] = 255;
        uint8_t* q = &other[(size_t(y) * S + x) * 4];
        const uint8_t u = (y % 36 < 20 && ((x * 5 + y * 3) % 13) < 5) ? 0 : 255;
        q[0] = q[1] = q[2] = u; q[3] = 255;
        uint8_t* w = &window[(size_t(y) * S + x) * 4];
        const bool block = (x / 180 + y / 180) % 3 == 0;
        w[0] = w[1] = w[2] = block ? 40 : 0; w[3] = block ? 255 : 0;
        uint8_t* g = &glass[(size_t(y) * S + x) * 4];
        g[0] = g[1] = g[2] = 60; g[3] = 128;
    }
    const pw3::Rect all{0, 0, W, H};
    const auto layer = [&](const std::vector<uint8_t>& pixels, pw3::PixelFormat format, pw3::Blend blend) {
        return pw3::Layer{false, {pixels.data(), bytes, W, H, S, format}, all, all, blend, 255, &all, 1};
    };
    pw3::Panel panel(W, H);
    const pw3::Canvas canvas{panel.canvas(), W, H};
    std::vector<uint8_t> fb(size_t(S) * H);
    std::vector<pw3::PanelUpdate> updates;
    const auto nothing = [] {};

    pw3::Layer one[1] = {layer(page, pw3::PixelFormat::Rgbx, pw3::Blend::Opaque)};
    measure("compose: one opaque full-screen layer", nothing, [&] { pw3::compose(one, 1, canvas, all); });
    pw3::Layer two[2] = {one[0], layer(window, pw3::PixelFormat::Rgba, pw3::Blend::Premultiplied)};
    measure("compose: opaque + window with transparent areas", nothing, [&] { pw3::compose(two, 2, canvas, all); });
    pw3::Layer three[2] = {one[0], layer(glass, pw3::PixelFormat::Rgba, pw3::Blend::Premultiplied)};
    measure("compose: opaque + fully translucent layer", nothing, [&] { pw3::compose(three, 2, canvas, all); });
    const pw3::Rect small{300, 600, 700, 760};
    measure("compose: opaque layer, 400x160 changed", nothing, [&] { pw3::compose(one, 1, canvas, small); });

    for (int fast = 1; fast >= 0; --fast) {
        pw3::PanelPolicy policy;
        policy.fast = fast;
        panel.setPolicy(policy);
        panel.invalidate();
        pw3::compose(one, 1, canvas, all);
        updates.clear(); panel.present(&all, 1, fb.data(), S, &updates);
        printf("-- panel, %s\n", fast ? "black/white first pass" : "grey levels at once");
        measure("present: whole screen, nothing changed", nothing, [&] { updates.clear(); panel.present(&all, 1, fb.data(), S, &updates); });
        measure("present: 400x160 damaged, nothing changed", nothing, [&] { updates.clear(); panel.present(&small, 1, fb.data(), S, &updates); });
        bool flip = false;
        measure("compose + present: whole screen, new page",
                [&] { flip = !flip; },
                [&] {
                    pw3::Layer next[1] = {layer(flip ? other : page, pw3::PixelFormat::Rgbx, pw3::Blend::Opaque)};
                    pw3::compose(next, 1, canvas, all);
                    updates.clear(); panel.present(&all, 1, fb.data(), S, &updates);
                });
        measure("second pass over the whole screen",
                [&] {
                    flip = !flip;
                    pw3::Layer next[1] = {layer(flip ? other : page, pw3::PixelFormat::Rgbx, pw3::Blend::Opaque)};
                    pw3::compose(next, 1, canvas, all);
                    updates.clear(); panel.present(&all, 1, fb.data(), S, &updates);
                },
                [&] { pw3::PanelUpdate u{}; panel.settle(fb.data(), S, &u); });
    }
    return 0;
}
