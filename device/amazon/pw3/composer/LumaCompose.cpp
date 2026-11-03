// SPDX-License-Identifier: Apache-2.0
#include "LumaCompose.h"
#include <cstddef>
#include <cstring>

namespace pw3 {
bool validImage(const Image& i) {
    if (i.format != PixelFormat::Rgba && i.format != PixelFormat::Rgbx && i.format != PixelFormat::Bgra &&
        i.format != PixelFormat::Rgb888 && i.format != PixelFormat::Rgb565)
        return false;
    if (i.width <= 0 || i.height <= 0 || i.width > 4096 || i.height > 4096 ||
        i.stride < i.width || i.stride > 4096) return false;
    return i.bytes >= size_t(i.stride) * size_t(i.height) * size_t(bytesPerPixel(i.format));
}
bool unscaled(const Layer& l) {
    const int cw = l.crop.right - l.crop.left, ch = l.crop.bottom - l.crop.top;
    const bool turned = (l.transform & Rot90) != 0;
    return l.frame.right - l.frame.left == (turned ? ch : cw) &&
           l.frame.bottom - l.frame.top == (turned ? cw : ch);
}
bool supported(const Layer& l, int width, int height) {
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096) return false;
    const Rect& f = l.frame;
    // Bound coordinates before subtracting or translating them.
    if (f.left < -4096 || f.top < -4096 || f.right > 8192 || f.bottom > 8192 ||
        f.right <= f.left || f.bottom <= f.top) return false;
    if (l.visibleCount && !l.visible) return false;
    for (size_t i = 0; i < l.visibleCount; ++i) {
        const Rect& r = l.visible[i];
        if (r.left < 0 || r.top < 0 || r.right > width || r.bottom > height || isEmpty(r)) return false;
    }
    if (l.dim) return true;
    if (l.blend != Blend::Opaque && l.blend != Blend::Premultiplied && l.blend != Blend::Coverage)
        return false;
    const Rect& c = l.crop;
    if (!validImage(l.image)) return false;
    if (c.left < 0 || c.top < 0 || c.right > l.image.width || c.bottom > l.image.height ||
        c.right <= c.left || c.bottom <= c.top) return false;
    // The frame may be any size: a crop larger or smaller than it is sampled.
    return !(l.transform & ~(FlipH | FlipV | Rot90));
}
LumaMode modeOf(const Layer& l) {
    // Only two of the formats carry an alpha channel; a layer in one of the
    // others covers what is below it whatever its blending says.
    const bool alphaInBuffer = l.image.format == PixelFormat::Rgba || l.image.format == PixelFormat::Bgra;
    return {l.blend == Blend::Opaque || !alphaInBuffer,
            l.blend == Blend::Coverage, l.image.format == PixelFormat::Bgra, l.alpha};
}
bool replaces(const Layer& l) { return !l.dim && lumaReplaces(modeOf(l)); }
Rect onScreen(const Layer& l, const Rect& changed) {
    const int cw = l.crop.right - l.crop.left, ch = l.crop.bottom - l.crop.top;
    // In crop coordinates, then flipped and turned as the crop is.
    Rect r = intersect(Rect{changed.left - l.crop.left, changed.top - l.crop.top,
                            changed.right - l.crop.left, changed.bottom - l.crop.top}, Rect{0, 0, cw, ch});
    if (isEmpty(r)) return Rect{0, 0, 0, 0};
    if (l.transform & FlipH) r = Rect{cw - r.right, r.top, cw - r.left, r.bottom};
    if (l.transform & FlipV) r = Rect{r.left, ch - r.bottom, r.right, ch - r.top};
    if (l.transform & Rot90) r = Rect{ch - r.bottom, r.left, ch - r.top, r.right};
    if (!unscaled(l)) {
        // Frame pixels whose centre can fall in r, one more on each side than
        // the division can give back: the answer may be too large, never too
        // small, or the panel would keep a pixel of the previous frame.
        const bool turned = (l.transform & Rot90) != 0;
        const int rw = turned ? ch : cw, rh = turned ? cw : ch;
        const int fw = l.frame.right - l.frame.left, fh = l.frame.bottom - l.frame.top;
        r = Rect{int((long long)r.left * fw / rw) - 1, int((long long)r.top * fh / rh) - 1,
                 int(((long long)r.right * fw + rw - 1) / rw) + 1,
                 int(((long long)r.bottom * fh + rh - 1) / rh) + 1};
    }
    return intersect(Rect{r.left + l.frame.left, r.top + l.frame.top, r.right + l.frame.left, r.bottom + l.frame.top},
                     l.frame);
}
namespace {
// The buffer pixel shown at frame offset (dx, dy), and the step in bytes to
// the pixel shown at (dx + 1, dy): the inverse of the transform. For a layer
// shown at the size of its crop, in 32-bit pixels.
const uint8_t* sourceOf(const Layer& l, int dx, int dy, ptrdiff_t* step) {
    const int cw = l.crop.right - l.crop.left, ch = l.crop.bottom - l.crop.top;
    int x = dx, y = dy, sx = 1, sy = 0;   // crop position and its change per dx
    if (l.transform & Rot90) { x = dy; y = ch - 1 - dx; sx = 0; sy = -1; }
    if (l.transform & FlipH) { x = cw - 1 - x; sx = -sx; }
    if (l.transform & FlipV) { y = ch - 1 - y; sy = -sy; }
    *step = (ptrdiff_t(sy) * l.image.stride + sx) * 4;
    return l.image.pixels + (size_t(l.crop.top + y) * l.image.stride + size_t(l.crop.left + x)) * 4;
}
// The crop pixel a frame pixel takes: the one its centre falls in, which for a
// frame the size of its crop is the pixel itself. Written as a division of
// whole numbers so that the row below can step through it without one.
inline int sampleOf(int d, int source, int frame) {
    return int(((long long)(2 * d + 1) * source) / (2 * frame));
}
// The eight-bit value nearest a five- or six-bit one. Repeating the top bits
// instead, as is usual, is wrong by one for a third of the five-bit values
// (7 gives 57 where 57.58 rounds to 58), and these are what a layer's grey
// level is computed from.
struct Widen {
    uint8_t five[32], six[64];
    constexpr Widen() : five(), six() {
        for (int v = 0; v < 32; ++v) five[v] = uint8_t((v * 255 + 15) / 31);
        for (int v = 0; v < 64; ++v) six[v] = uint8_t((v * 255 + 31) / 63);
    }
};
const Widen widen;
// One row of a layer as 32-bit pixels in the layer's own channel order, for
// the row functions of Luma.h: the path for a narrow format, a sampled layer,
// or both. `dx` and `dy` are offsets inside the frame, `count` pixels wide.
void gatherRow(const Layer& l, int dx, int dy, int count, uint8_t* out) {
    const Image& image = l.image;
    const int cw = l.crop.right - l.crop.left, ch = l.crop.bottom - l.crop.top;
    const int fw = l.frame.right - l.frame.left, fh = l.frame.bottom - l.frame.top;
    const bool turned = (l.transform & Rot90) != 0;
    const int rw = turned ? ch : cw, rh = turned ? cw : ch;   // the crop as the frame sees it
    const int bytes = bytesPerPixel(image.format);
    // The row keeps one crop coordinate and walks the other.
    const int v = sampleOf(dy, rh, fh);
    int u = sampleOf(dx, rw, fw);
    int x, y;
    ptrdiff_t unit;     // bytes from the pixel of u to the pixel of u + 1
    if (turned) {
        x = (l.transform & FlipH) ? cw - 1 - v : v;
        y = (l.transform & FlipV) ? u : ch - 1 - u;
        unit = (l.transform & FlipV) ? ptrdiff_t(image.stride) * bytes : -ptrdiff_t(image.stride) * bytes;
    } else {
        x = (l.transform & FlipH) ? cw - 1 - u : u;
        y = (l.transform & FlipV) ? ch - 1 - v : v;
        unit = (l.transform & FlipH) ? -ptrdiff_t(bytes) : ptrdiff_t(bytes);
    }
    const uint8_t* p = image.pixels + (size_t(l.crop.top + y) * image.stride + size_t(l.crop.left + x)) * size_t(bytes);
    // u advances by rw / fw per frame pixel, the remainder carried as a whole
    // number so that no pixel drifts however wide the row is.
    const int whole = rw / fw, part = 2 * (rw % fw), denominator = 2 * fw;
    int carry = int(((long long)(2 * dx + 1) * rw) % denominator);
    for (int i = 0; i < count; ++i, out += 4) {
        switch (image.format) {
            case PixelFormat::Rgb565: {
                const unsigned s = unsigned(p[0]) | (unsigned(p[1]) << 8);
                out[0] = widen.five[(s >> 11) & 31];
                out[1] = widen.six[(s >> 5) & 63];
                out[2] = widen.five[s & 31];
                out[3] = 255;
                break;
            }
            case PixelFormat::Rgb888:
                out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = 255;
                break;
            default:
                std::memcpy(out, p, 4);
                break;
        }
        int step = whole;
        carry += part;
        if (carry >= denominator) { carry -= denominator; ++step; }
        p += unit * step;
    }
}
}  // namespace
bool compose(const Layer* layers, size_t count, Canvas canvas, Rect area) {
    area = intersect(area, Rect{0, 0, canvas.width, canvas.height});
    if (isEmpty(area)) return false;
    bool changed = false;
    // Behind the bottom-most layer is the panel at rest: white. (HWC says
    // opaque black, a lit display's "off". Here that showed as a black screen
    // between two white ones whenever an application had not drawn yet -
    // Test V34, Settings starting cold - and each of the two changes is a
    // full update of the panel.) It only has to be written where no
    // replacing layer writes: the visible regions of those layers are
    // disjoint, so their areas inside `area` can be added.
    long long replaced = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!replaces(layers[i])) continue;
        for (size_t v = 0; v < layers[i].visibleCount; ++v)
            replaced += pw3::area(intersect(intersect(layers[i].visible[v], layers[i].frame), area));
    }
    if (replaced != pw3::area(area))
        for (int y = area.top; y < area.bottom; ++y)
            changed |= lumaFillRow(canvas.luma + size_t(y) * canvas.width + area.left, Paper,
                                   area.right - area.left);
    // A row of a flipped or turned layer is gathered here first: up to the
    // largest supported width.
    uint8_t gathered[4096 * 4];
    for (size_t i = 0; i < count; ++i) {
        const Layer& l = layers[i];
        if (!l.dim && !l.image.pixels) continue;
        if (l.alpha == 0) continue;
        const LumaMode mode = modeOf(l);
        // 32-bit pixels shown at their own size are read where they lie; the
        // rest are converted a row at a time.
        const bool direct = !l.dim && bytesPerPixel(l.image.format) == 4 && unscaled(l);
        for (size_t v = 0; v < l.visibleCount; ++v) {
            const Rect part = intersect(intersect(l.visible[v], l.frame), area);
            if (isEmpty(part)) continue;
            const int width = part.right - part.left;
            for (int y = part.top; y < part.bottom; ++y) {
                uint8_t* out = canvas.luma + size_t(y) * canvas.width + part.left;
                if (l.dim) { changed |= lumaDimRow(out, width, l.alpha); continue; }
                const uint8_t* source = gathered;
                if (direct) {
                    ptrdiff_t step;
                    source = sourceOf(l, part.left - l.frame.left, y - l.frame.top, &step);
                    if (step != 4) {
                        for (int x = 0; x < width; ++x, source += step) std::memcpy(gathered + size_t(x) * 4, source, 4);
                        source = gathered;
                    }
                } else {
                    gatherRow(l, part.left - l.frame.left, y - l.frame.top, width, gathered);
                }
                if (lumaReplaces(mode)) changed |= lumaRow(source, out, width, mode.bgra);
                else changed |= lumaBlendRow(source, out, width, mode);
            }
        }
    }
    return changed;
}
}  // namespace pw3
