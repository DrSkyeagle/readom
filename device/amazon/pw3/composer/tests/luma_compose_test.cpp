// SPDX-License-Identifier: Apache-2.0
// Composition into luma against a pixel-by-pixel reference, for whole frames
// and for the changed part of a frame.
#include "../LumaCompose.h"
#include <cassert>
#include <cstdio>
#include <utility>
#include <vector>

static uint32_t seed = 777;
static uint32_t rnd() { seed = seed * 1664525u + 1013904223u; return seed >> 8; }

namespace {
const int W = 97, H = 61;
struct Source {
    std::vector<uint8_t> pixels;
    std::vector<pw3::Rect> visible;
    pw3::Layer layer{};
};
// rects minus one rectangle, as disjoint rectangles.
std::vector<pw3::Rect> subtract(const std::vector<pw3::Rect>& rects, const pw3::Rect& cut) {
    std::vector<pw3::Rect> out;
    for (const auto& r : rects) {
        const pw3::Rect c = pw3::intersect(r, cut);
        if (pw3::isEmpty(c)) { out.push_back(r); continue; }
        if (r.top < c.top) out.push_back({r.left, r.top, r.right, c.top});
        if (c.bottom < r.bottom) out.push_back({r.left, c.bottom, r.right, r.bottom});
        if (r.left < c.left) out.push_back({r.left, c.top, c.left, c.bottom});
        if (c.right < r.right) out.push_back({c.right, c.top, r.right, c.bottom});
    }
    return out;
}
bool inside(const std::vector<pw3::Rect>& rects, int x, int y) {
    for (const auto& r : rects) if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return true;
    return false;
}
// Where each crop pixel goes, the way SurfaceFlinger's Transform::set()
// composes a transform: R * (H * V), flips first, then 90 degrees clockwise.
// Returns, for each frame pixel, the crop pixel shown there. A frame of the
// crop's own size shows each pixel once; a frame of another size takes, for
// each of its pixels, the crop pixel its centre falls in -- computed straight
// from that definition here, while the composer steps through it.
std::vector<std::pair<int, int>> forward(const pw3::Layer& l) {
    const int cw = l.crop.right - l.crop.left, ch = l.crop.bottom - l.crop.top;
    const int fw = l.frame.right - l.frame.left, fh = l.frame.bottom - l.frame.top;
    const bool turned = (l.transform & pw3::Rot90) != 0;
    const int rw = turned ? ch : cw, rh = turned ? cw : ch;
    std::vector<std::pair<int, int>> shown(size_t(fw) * fh, {-1, -1});
    for (int dy = 0; dy < fh; ++dy) for (int dx = 0; dx < fw; ++dx) {
        // The pixel of the crop, as the frame has it turned, under this centre.
        const int u = int(((long long)(2 * dx + 1) * rw) / (2 * fw));
        const int v = int(((long long)(2 * dy + 1) * rh) / (2 * fh));
        int x = turned ? v : u, y = turned ? ch - 1 - u : v;
        if (l.transform & pw3::FlipH) x = cw - 1 - x;
        if (l.transform & pw3::FlipV) y = ch - 1 - y;
        assert(x >= 0 && x < cw && y >= 0 && y < ch);
        shown[size_t(dy) * fw + dx] = {x, y};
    }
    return shown;
}
// The 32-bit pixel of an image, whatever its format holds.
std::vector<uint8_t> pixelOf(const pw3::Image& image, int x, int y) {
    const int bytes = pw3::bytesPerPixel(image.format);
    const uint8_t* p = image.pixels + (size_t(y) * image.stride + size_t(x)) * size_t(bytes);
    if (image.format == pw3::PixelFormat::Rgb565) {
        const unsigned s = unsigned(p[0]) | (unsigned(p[1]) << 8);
        const unsigned r = (s >> 11) & 31, g = (s >> 5) & 63, b = s & 31;
        // The nearest 8-bit value, which is what repeating the top bits gives.
        return {uint8_t((r * 255 + 15) / 31), uint8_t((g * 255 + 31) / 63), uint8_t((b * 255 + 15) / 31), 255};
    }
    if (image.format == pw3::PixelFormat::Rgb888) return {p[0], p[1], p[2], 255};
    return {p[0], p[1], p[2], p[3]};
}
// One pixel at a time, from the definitions in Luma.h.
std::vector<uint8_t> reference(const std::vector<Source>& sources) {
    std::vector<uint8_t> out(size_t(W) * H, pw3::Paper);
    for (const auto& s : sources) {
        const pw3::Layer& l = s.layer;
        const auto shown = l.dim ? std::vector<std::pair<int, int>>() : forward(l);
        const int fw = l.frame.right - l.frame.left;
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
            if (!inside(s.visible, x, y) || x < l.frame.left || x >= l.frame.right || y < l.frame.top || y >= l.frame.bottom) continue;
            uint8_t& v = out[size_t(y) * W + x];
            if (l.dim) {
                const unsigned t = unsigned(v) * (255u - l.alpha) + 128u;
                v = uint8_t((t + (t >> 8)) >> 8);
                continue;
            }
            const auto& c = shown[size_t(y - l.frame.top) * fw + (x - l.frame.left)];
            const std::vector<uint8_t> p = pixelOf(l.image, l.crop.left + c.first, l.crop.top + c.second);
            const pw3::LumaMode mode = pw3::modeOf(l);
            v = pw3::lumaReplaces(mode) ? pw3::lumaOf(p.data(), mode.bgra) : pw3::lumaBlendOf(p.data(), v, mode);
        }
    }
    return out;
}
}  // namespace

// What the composer relies on: one opaque layer covering the area, composed
// twice over its own result, reports no change the second time. This is the
// case of a reading screen (one full-screen window), and the one that lets a
// frame be dropped before the panel converts it.
static void opaqueTwice() {
    std::vector<uint8_t> pixels(size_t(W) * H * 4);
    for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = uint8_t(rnd());
    for (size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 255;
    const pw3::Rect whole{0, 0, W, H};
    pw3::Layer layer{};
    layer.image = pw3::Image{pixels.data(), pixels.size(), W, H, W, pw3::PixelFormat::Rgba};
    layer.crop = layer.frame = whole;
    layer.blend = pw3::Blend::Opaque;
    layer.alpha = 255;
    layer.visible = &whole;
    layer.visibleCount = 1;
    assert(pw3::replaces(layer) && pw3::supported(layer, W, H));
    std::vector<uint8_t> canvas(size_t(W) * H, 0);
    const pw3::Canvas out{canvas.data(), W, H};
    assert(pw3::compose(&layer, 1, out, whole));
    assert(!pw3::compose(&layer, 1, out, whole));
    // A pixel of the layer changes: that area, and only that area, reports it.
    pixels[(size_t(3) * W + 5) * 4 + 1] ^= 0x40;
    assert(!pw3::compose(&layer, 1, out, pw3::Rect{0, 0, W, 3}));
    assert(pw3::compose(&layer, 1, out, pw3::Rect{0, 3, W, 4}));
    assert(!pw3::compose(&layer, 1, out, pw3::Rect{0, 3, W, 4}));
}

int main() {
    opaqueTwice();
    long long frames = 0, partials = 0;
    for (int round = 0; round < 1500; ++round) {
        const int count = 1 + int(rnd() % 6);
        std::vector<Source> sources(static_cast<size_t>(count));
        for (auto& s : sources) {
            pw3::Layer& l = s.layer;
            l.dim = rnd() % 9 == 0;
            // Mostly full-screen windows and bars, sometimes anything, partly off screen.
            const int shape = int(rnd() % 4);
            int left = 0, top = 0, width = W, height = H;
            if (shape == 1) { height = 5 + int(rnd() % 6); top = rnd() % 2 ? 0 : H - height; }
            if (shape >= 2) { left = int(rnd() % (W + 20)) - 20; top = int(rnd() % (H + 20)) - 20; width = 1 + int(rnd() % 70); height = 1 + int(rnd() % 50); }
            l.frame = {left, top, left + width, top + height};
            l.alpha = uint8_t(rnd() % 3 ? 255 : rnd());
            if (!l.dim) {
                // Half of the layers flipped or turned; a turned crop is the frame on its side.
                l.transform = rnd() % 2 ? 0 : rnd() % 8;
                const bool turned = (l.transform & pw3::Rot90) != 0;
                // A fifth of the layers are shown at a size other than their
                // crop's, in either direction: a window scaled for an older
                // application, a surface with a fixed size, magnification.
                int cw = turned ? height : width, ch = turned ? width : height;
                if (rnd() % 5 == 0) {
                    cw = 1 + int(rnd() % 90);
                    ch = 1 + int(rnd() % 70);
                }
                const int extraLeft = int(rnd() % 4), extraTop = int(rnd() % 4);
                const int iw = cw + extraLeft + int(rnd() % 4), ih = ch + extraTop + int(rnd() % 4), stride = iw + int(rnd() % 5);
                static const pw3::PixelFormat formats[] = {
                    pw3::PixelFormat::Rgba, pw3::PixelFormat::Rgbx, pw3::PixelFormat::Bgra,
                    pw3::PixelFormat::Rgb888, pw3::PixelFormat::Rgb565};
                const pw3::PixelFormat format = formats[rnd() % 5];
                const int bytes = pw3::bytesPerPixel(format);
                s.pixels.resize(size_t(stride) * ih * size_t(bytes));
                for (auto& v : s.pixels) v = uint8_t(rnd());
                if (bytes == 4)
                    for (size_t i = 0; i < s.pixels.size(); i += 4) {
                        uint8_t* p = &s.pixels[i];
                        switch ((i / 44 + size_t(round)) % 4) {
                            case 0: p[0] = p[1] = p[2] = p[3] = 0; break;
                            case 1: p[3] = 255; break;
                            case 2: for (int c = 0; c < 3; ++c) p[c] = uint8_t(p[c] * p[3] / 255); break;
                            default: break;
                        }
                    }
                l.image = {s.pixels.data(), s.pixels.size(), iw, ih, stride, format};
                l.crop = {extraLeft, extraTop, extraLeft + cw, extraTop + ch};
                const int blend = int(rnd() % 3);
                l.blend = blend == 0 ? pw3::Blend::Opaque : blend == 1 ? pw3::Blend::Premultiplied : pw3::Blend::Coverage;
            }
        }
        // Visible regions as SurfaceFlinger computes them: the frame on
        // screen, minus every replacing layer above.
        for (size_t i = 0; i < sources.size(); ++i) {
            const pw3::Rect onScreen = pw3::intersect(sources[i].layer.frame, pw3::Rect{0, 0, W, H});
            if (!pw3::isEmpty(onScreen)) sources[i].visible.push_back(onScreen);
            for (size_t above = i + 1; above < sources.size(); ++above)
                if (pw3::replaces(sources[above].layer)) sources[i].visible = subtract(sources[i].visible, sources[above].layer.frame);
            sources[i].layer.visible = sources[i].visible.data();
            sources[i].layer.visibleCount = sources[i].visible.size();
            assert(pw3::supported(sources[i].layer, W, H));
        }
        std::vector<pw3::Layer> layers;
        for (const auto& s : sources) layers.push_back(s.layer);
        std::vector<uint8_t> canvas(size_t(W) * H + 2, 0xc3);
        const pw3::Canvas out{canvas.data() + 1, W, H};
        const auto blank = canvas;
        const bool firstChanged = pw3::compose(layers.data(), layers.size(), out, pw3::Rect{0, 0, W, H});
        assert(firstChanged == (canvas != blank));
        {   // Again, over its own result: the canvas may not move, and a report of
            // a change is allowed only where a pixel is written more than once (the
            // paper under a layer that does not replace it, or two layers over each
            // other), which is what the header promises.
            const auto before = canvas;
            pw3::compose(layers.data(), layers.size(), out, pw3::Rect{0, 0, W, H});
            assert(canvas == before);
        }
        const auto expected = reference(sources);
        assert(canvas.front() == 0xc3 && canvas.back() == 0xc3);
        for (size_t i = 0; i < expected.size(); ++i) assert(canvas[i + 1] == expected[i]);
        ++frames;

        // One layer draws into part of its buffer; only that area is composed
        // again and the canvas equals a whole new composition.
        for (int change = 0; change < 4; ++change) {
            Source& s = sources[rnd() % sources.size()];
            if (s.layer.dim) continue;
            const pw3::Image& image = s.layer.image;
            const int l = int(rnd() % image.width), t = int(rnd() % image.height);
            const pw3::Rect changed{l, t, std::min(image.width, l + 1 + int(rnd() % 40)), std::min(image.height, t + 1 + int(rnd() % 30))};
            const int bytes = pw3::bytesPerPixel(image.format);
            for (int y = changed.top; y < changed.bottom; ++y) for (int x = changed.left; x < changed.right; ++x) {
                uint8_t* p = &s.pixels[(size_t(y) * image.stride + x) * size_t(bytes)];
                for (int c = 0; c < bytes && c < 3; ++c) p[c] = uint8_t(rnd());
                if (bytes == 4 && rnd() % 2) p[3] = uint8_t(rnd() % 3 ? 255 : rnd());
            }
            const pw3::Rect area = pw3::onScreen(s.layer, changed);
            const auto before = canvas;
            const bool reported = pw3::compose(layers.data(), layers.size(), out, area);
            // Never false while a byte changed; a pixel written twice in one pass may
            // report a change that ended as it began, so the other direction is free.
            if (canvas != before) assert(reported);
            const auto now = reference(sources);
            for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
                const size_t at = size_t(y) * W + x;
                assert(canvas[at + 1] == now[at]);
                const bool in = x >= area.left && x < area.right && y >= area.top && y < area.bottom;
                if (!in) assert(canvas[at + 1] == before[at + 1]);
            }
            ++partials;
        }
    }
    // Rejected geometry.
    {
        std::vector<uint8_t> pixels(16 * 16 * 4);
        const pw3::Rect whole{0, 0, 16, 16};
        pw3::Layer l{false, {pixels.data(), pixels.size(), 16, 16, 16, pw3::PixelFormat::Rgba}, whole, whole, pw3::Blend::Opaque, 255, &whole, 1, 0};
        assert(pw3::supported(l, 16, 16) && pw3::unscaled(l));
        // Shown at another size: composed here, and said to be sampled.
        pw3::Layer scaled = l; scaled.frame = {0, 0, 8, 16};
        assert(pw3::supported(scaled, 16, 16) && !pw3::unscaled(scaled));
        pw3::Layer cropped = l; cropped.crop = {0, 0, 17, 16}; cropped.frame = {0, 0, 17, 16}; assert(!pw3::supported(cropped, 16, 16));
        pw3::Layer small = l; small.image.bytes -= 1; assert(!pw3::supported(small, 16, 16));
        const pw3::Rect outside{0, 0, 17, 16};
        pw3::Layer region = l; region.visible = &outside; assert(!pw3::supported(region, 16, 16));
        pw3::Layer far = l; far.frame = {9000, 0, 9016, 16}; assert(!pw3::supported(far, 16, 16));
        // Turned: the frame at its own size is the crop on its side.
        pw3::Layer turned = l; turned.crop = {0, 0, 16, 8}; turned.transform = pw3::Rot90;
        assert(pw3::supported(turned, 16, 16) && !pw3::unscaled(turned));
        turned.frame = {0, 0, 8, 16}; assert(pw3::supported(turned, 16, 16) && pw3::unscaled(turned));
        pw3::Layer unknown = l; unknown.transform = 8; assert(!pw3::supported(unknown, 16, 16));
        // A narrow format needs fewer bytes for the same image, and not fewer still.
        pw3::Layer narrow = l; narrow.image.format = pw3::PixelFormat::Rgb565;
        narrow.image.bytes = 16 * 16 * 2; assert(pw3::supported(narrow, 16, 16));
        narrow.image.bytes = 16 * 16 * 2 - 1; assert(!pw3::supported(narrow, 16, 16));
        pw3::Layer three = l; three.image.format = pw3::PixelFormat::Rgb888;
        three.image.bytes = 16 * 16 * 3; assert(pw3::supported(three, 16, 16));
        three.image.bytes = 16 * 16 * 3 - 1; assert(!pw3::supported(three, 16, 16));
        // Without an alpha channel the layer covers what is below it, whatever
        // the blending says, so the composer may start the row from it.
        pw3::Layer blended = l; blended.blend = pw3::Blend::Premultiplied;
        assert(!pw3::replaces(blended));
        blended.image.format = pw3::PixelFormat::Rgb565; assert(pw3::replaces(blended));
        blended.image.format = pw3::PixelFormat::Rgb888; assert(pw3::replaces(blended));
        blended.image.format = pw3::PixelFormat::Rgbx; assert(pw3::replaces(blended));
    }
    printf("luma compose: %lld frames and %lld partial updates checked\n", frames, partials);
    return 0;
}
