// SPDX-License-Identifier: Apache-2.0
#include "Panel.h"
#include <cstring>
#include <stdexcept>
#if (defined(__ARM_NEON) || defined(__ARM_NEON__)) && !defined(PW3_PANEL_SCALAR)
#include <arm_neon.h>
#define PW3_PANEL_NEON 1
#endif

namespace pw3 {
namespace {
// 4x4 ordered-dither thresholds between level 5 (85, always black) and
// level 11 (187, always white); level 8 comes out half black, half white.
const uint8_t Thresholds[4][4] = {
    { 93, 138, 104, 149},
    {161, 115, 172, 127},
    {110, 155,  98, 144},
    {178, 132, 166, 121},
};
#ifdef PW3_PANEL_NEON
inline bool any(uint8x16_t v) {
    const uint64x2_t w = vreinterpretq_u64_u8(v);
    return (vgetq_lane_u64(w, 0) | vgetq_lane_u64(w, 1)) != 0;
}
#endif
inline bool blackOrWhite(uint8_t v) { return v == 0 || v == 255; }
// Shrinks [first, last) of a row known to differ somewhere to its differing ends.
inline void differingSpan(const uint8_t* a, const uint8_t* b, int* first, int* last) {
    while (a[*first] == b[*first]) ++*first;
    while (a[*last - 1] == b[*last - 1]) --*last;
}
inline void include(Rect* box, int left, int top, int right, int bottom) {
    *box = bounds(*box, Rect{left, top, right, bottom});
}
}  // namespace

Panel::Panel(int width, int height) : width_(width), height_(height) {
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096)
        throw std::invalid_argument("Unsupported panel geometry");
    const size_t pixels = size_t(width) * size_t(height);
    canvas_.resize(pixels);
    target_.resize(pixels);
    shown_.resize(pixels);
    next_.resize(size_t(width));
    out_.resize(size_t(width));
    tilesX_ = (width + kTile - 1) / kTile;
    tilesY_ = (height + kTile - 1) / kTile;
    debt_.assign(size_t(tilesX_) * size_t(tilesY_), 0);
    setPolicy(policy_);
}
void Panel::setPolicy(const PanelPolicy& policy) {
    if (policy.contrast != policy_.contrast) invalid_ = true;
    policy_ = policy;
    panelLevels(policy_.contrast, levels_);
    int luma = 0;
    for (int k = 0; k < 15; ++k) {
        while (levels_[luma] <= k * 17) ++luma;  // levels_[255] is 255: ends for every k
        steps_[k] = uint8_t(luma);
    }
}
void Panel::levelRow(const uint8_t* luma, uint8_t* out, int count) const {
    int x = 0;
#ifdef PW3_PANEL_NEON
    // A comparison yields 255 where it holds: subtracting it counts one step.
    uint8x16_t step[15];
    for (int k = 0; k < 15; ++k) step[k] = vdupq_n_u8(steps_[k]);
    const uint8x16_t seventeen = vdupq_n_u8(17);
    for (; x + 16 <= count; x += 16) {
        const uint8x16_t v = vld1q_u8(luma + x);
        uint8x16_t reached = vdupq_n_u8(0);
        for (int k = 0; k < 15; ++k) reached = vsubq_u8(reached, vcgeq_u8(v, step[k]));
        vst1q_u8(out + x, vmulq_u8(reached, seventeen));
    }
#endif
    for (; x < count; ++x) out[x] = levels_[luma[x]];
}
uint8_t Panel::threshold(int x, int y) const {
    return policy_.dither ? Thresholds[y & 3][x & 3] : 127;
}
// First pass for one row. A pixel whose level did not change keeps what it
// shows; a changed one becomes black or white.
Panel::RowResult Panel::firstPassRow(const uint8_t* next, const uint8_t* target, const uint8_t* shown,
                                     uint8_t* out, int count, int x, int y) const {
    uint8_t pattern[16];
    for (int k = 0; k < 16; ++k) pattern[k] = threshold(x + k, y);
    bool changed = false, grey = false, pending = false;
    int i = 0;
#ifdef PW3_PANEL_NEON
    const uint8x16_t limit = vld1q_u8(pattern), zero = vdupq_n_u8(0), full = vdupq_n_u8(255);
    uint8x16_t anyChanged = zero, anyGrey = zero, anyPending = zero;
    for (; i + 16 <= count; i += 16) {
        const uint8x16_t n = vld1q_u8(next + i), s = vld1q_u8(shown + i);
        const uint8x16_t value = vbslq_u8(vceqq_u8(n, vld1q_u8(target + i)), s, vcgtq_u8(n, limit));
        const uint8x16_t differs = veorq_u8(value, s);
        anyChanged = vorrq_u8(anyChanged, differs);
        anyGrey = vorrq_u8(anyGrey, vbicq_u8(differs, vorrq_u8(vceqq_u8(s, zero), vceqq_u8(s, full))));
        anyPending = vorrq_u8(anyPending, veorq_u8(value, n));
        vst1q_u8(out + i, value);
    }
    changed = any(anyChanged); grey = any(anyGrey); pending = any(anyPending);
#endif
    for (; i < count; ++i) {
        const uint8_t value = next[i] == target[i] ? shown[i] : uint8_t(next[i] > pattern[i & 15] ? 255 : 0);
        if (value != shown[i]) { changed = true; grey |= !blackOrWhite(shown[i]); }
        pending |= value != next[i];
        out[i] = value;
    }
    return {changed, grey, pending};
}
// Without a first pass the row is shown as it is. `grey` tells whether a
// pixel that changes is, or becomes, something other than black or white.
Panel::RowResult Panel::exactRow(const uint8_t* next, const uint8_t* shown, int count) const {
    bool changed = false, grey = false;
    int i = 0;
#ifdef PW3_PANEL_NEON
    const uint8x16_t zero = vdupq_n_u8(0), full = vdupq_n_u8(255);
    uint8x16_t anyChanged = zero, anyGrey = zero;
    for (; i + 16 <= count; i += 16) {
        const uint8x16_t n = vld1q_u8(next + i), s = vld1q_u8(shown + i);
        const uint8x16_t differs = veorq_u8(n, s);
        const uint8x16_t bothPlain = vandq_u8(vorrq_u8(vceqq_u8(s, zero), vceqq_u8(s, full)),
                                              vorrq_u8(vceqq_u8(n, zero), vceqq_u8(n, full)));
        anyChanged = vorrq_u8(anyChanged, differs);
        anyGrey = vorrq_u8(anyGrey, vbicq_u8(differs, bothPlain));
    }
    changed = any(anyChanged); grey = any(anyGrey);
#endif
    for (; i < count; ++i) {
        if (next[i] == shown[i]) continue;
        changed = true;
        grey |= !blackOrWhite(next[i]) || !blackOrWhite(shown[i]);
    }
    return {changed, grey, false};
}
void Panel::assumeShown(const uint8_t* fb, size_t fbStride) {
    for (int y = 0; y < height_; ++y) {
        std::memcpy(shown_.data() + size_t(y) * width_, fb + size_t(y) * fbStride, size_t(width_));
        std::memcpy(target_.data() + size_t(y) * width_, fb + size_t(y) * fbStride, size_t(width_));
    }
    invalid_ = false;
    stale_ = true;
    pending_ = touched_ = {0, 0, 0, 0};
}
// Charged to the limit rather than cleared here and now: the screen still
// shows the old content at this moment, and clearing it would be a flash of
// its own before the new content arrives. charge() turns the update that
// brings the new content into the GC16 instead, which is one flash, in the
// right place, showing the right thing afterwards.
//
// Nothing happens with fullEvery at 0. That setting says the panel is never to
// be cleared on a count, and this is the same question asked by a window
// rather than by a counter; one switch turns both off.
void Panel::replaced(const Rect& area) {
    if (debt_.empty() || !policy_.fullEvery) return;
    const Rect r = intersect(area, Rect{0, 0, width_, height_});
    if (isEmpty(r)) return;
    for (int ty = r.top / kTile; ty <= (r.bottom - 1) / kTile; ++ty)
        for (int tx = r.left / kTile; tx <= (r.right - 1) / kTile; ++tx)
            debt_[size_t(ty) * tilesX_ + tx] = uint8_t(policy_.fullEvery > 255 ? 255 : policy_.fullEvery);
}
void Panel::present(const Rect* damage, size_t count, uint8_t* fb, size_t fbStride, std::vector<PanelUpdate>* updates,
                    bool idle) {
    const Rect screen{0, 0, width_, height_};
    if (stale_ && !invalid_) {
        // All of the canvas is new against the picture shown.
        stale_ = false;
        damage = &screen;
        count = 1;
    }
    if (invalid_) {
        stale_ = false;
        for (int y = 0; y < height_; ++y) {
            uint8_t* t = target_.data() + size_t(y) * width_;
            levelRow(canvas_.data() + size_t(y) * width_, t, width_);
            std::memcpy(shown_.data() + size_t(y) * width_, t, size_t(width_));
            std::memcpy(fb + size_t(y) * fbStride, t, size_t(width_));
        }
        invalid_ = false;
        pending_ = touched_ = {0, 0, 0, 0};
        debt_.assign(debt_.size(), 0);   // this GC16 covers the screen
        updates->push_back({screen, Waveform::Gc16, true});
        return;
    }
    // Shown once only if nothing is owed: a second pass that is due means the
    // picture was moving a moment ago.
    const bool alone = idle && !pending();
    for (size_t r = 0; r < count; ++r) {
        const Rect area = intersect(damage[r], screen);
        if (isEmpty(area)) continue;
        const int width = area.right - area.left;
        Rect box{0, 0, 0, 0};
        bool grey = false;
        // changeSpans() converts rows to levels to answer, and the branch it
        // chooses converts them again. Left as it is, measured: with
        // directPercent 0 an idle panel answers without looking at a pixel, so
        // a page turn pays nothing, and the most a busy one can pay is the
        // half screen changeSpans stops at, 10 to 23 ms of the 19 to 46 a
        // whole screen costs (Test V51) -- while the panel is already busy.
        // Keeping the levels instead would need a fourth screen-sized buffer,
        // 1.55 MB held for the life of the system to save that.
        if (!policy_.fast || (alone && changeSpans(area, policy_.directPercent)) || !changeSpans(area, policy_.movePercent)) {
            for (int y = area.top; y < area.bottom; ++y) {
                const size_t at = size_t(y) * width_ + area.left;
                uint8_t* next = next_.data();
                uint8_t* s = shown_.data() + at;
                levelRow(canvas_.data() + at, next, width);
                std::memcpy(target_.data() + at, next, size_t(width));
                if (!std::memcmp(next, s, size_t(width))) continue;
                int a = 0, b = width;
                differingSpan(next, s, &a, &b);
                grey |= exactRow(next + a, s + a, b - a).grey;
                std::memcpy(s + a, next + a, size_t(b - a));
                std::memcpy(fb + size_t(y) * fbStride + area.left + a, next + a, size_t(b - a));
                include(&box, area.left + a, y, area.left + b, y + 1);
            }
            // Only without a first pass at all can something be owed here: the
            // policy changed while it was.
            if (contains(area, pending_)) pending_ = {0, 0, 0, 0};
            if (!isEmpty(box)) updates->push_back(charge(!grey ? plainUpdate(box) : isPage(box) ? exactUpdate(box) : PanelUpdate{box, Waveform::Gl16, false}));
            continue;
        }
        for (int y = area.top; y < area.bottom; ++y) {
            const size_t at = size_t(y) * width_ + area.left;
            uint8_t* next = next_.data();
            uint8_t* t = target_.data() + at;
            uint8_t* s = shown_.data() + at;
            levelRow(canvas_.data() + at, next, width);
            if (!std::memcmp(next, t, size_t(width))) continue;
            int a = 0, b = width;
            differingSpan(next, t, &a, &b);
            const int n = b - a;
            const RowResult row = firstPassRow(next + a, t + a, s + a, out_.data(), n, area.left + a, y);
            std::memcpy(t + a, next + a, size_t(n));
            if (row.changed) {
                std::memcpy(s + a, out_.data(), size_t(n));
                std::memcpy(fb + size_t(y) * fbStride + area.left + a, out_.data(), size_t(n));
                include(&box, area.left + a, y, area.left + b, y + 1);
                grey |= row.grey;
            }
            if (row.pending) include(&pending_, area.left + a, y, area.left + b, y + 1);
        }
        if (isEmpty(box)) continue;
        // Every changed pixel ends black or white; DU if one of them was grey.
        if (grey) {
            if (policy_.clean) touched_ = bounds(touched_, box);
            updates->push_back({box, Waveform::Du, false});
        } else {
            updates->push_back(plainUpdate(box));
        }
    }
}
bool Panel::isPage(const Rect& box) const {
    return area(box) * 100 >= (long long)width_ * height_ * policy_.pagePercent;
}
// Whether the pixels that change inside `area` span at least `percent` of the
// screen. Only an area that large is looked at.
bool Panel::changeSpans(const Rect& area, unsigned percent) {
    if (!percent) return true;
    const long long needed = (long long)width_ * height_ * percent;
    if (pw3::area(area) * 100 < needed) return false;
    const int width = area.right - area.left;
    Rect box{0, 0, 0, 0};
    for (int y = area.top; y < area.bottom; ++y) {
        const size_t at = size_t(y) * width_ + area.left;
        levelRow(canvas_.data() + at, next_.data(), width);
        if (!std::memcmp(next_.data(), target_.data() + at, size_t(width))) continue;
        int a = 0, b = width;
        differingSpan(next_.data(), target_.data() + at, &a, &b);
        include(&box, area.left + a, y, area.left + b, y + 1);
        if (pw3::area(box) * 100 >= needed) return true;
    }
    return false;
}
// An update between black and white: exact with the fast waveforms. The stock
// system keeps A2 to its keyboard's keys - small and soon changed back.
PanelUpdate Panel::plainUpdate(const Rect& box) const {
    return {box, policy_.a2 && !isPage(box) ? Waveform::A2 : Waveform::Du, false};
}
// A page shown with the 16-level waveform. What it leaves behind is charged
// to the squares it covered (charge), and the screen is cleared there once
// enough has built up.
PanelUpdate Panel::exactUpdate(const Rect& box) const {
    return {box, policy_.exact, policy_.exact != Waveform::Gl16};
}
// Every update passes through here on its way out.
//
// A waveform that does not clear (GL16, DU, A2) leaves a little of what was
// there before: after enough of them the earlier pictures show through, and
// only GC16 takes them off. So each one is charged to the squares of the
// screen it covered, and when a square has taken `fullEvery` of them the
// update that would have been sent becomes a GC16 over everything owing --
// the squares at the limit, together with the update's own area, as one
// rectangle. GC16 pays off what it covers.
//
// Charging squares rather than counting pages is what makes this work away
// from a book: a page covers every square, so reading still clears the screen
// every `fullEvery` pages, while a list or a dragged icon -- far smaller than
// a page, and never counted before -- now clears the part of the screen it
// has been smearing.
PanelUpdate Panel::charge(PanelUpdate update) {
    if (debt_.empty() || !policy_.fullEvery) return update;
    const Rect& r = update.rect;
    if (isEmpty(r)) return update;
    const int x0 = r.left / kTile, x1 = (r.right - 1) / kTile;
    const int y0 = r.top / kTile, y1 = (r.bottom - 1) / kTile;
    if (update.waveform == Waveform::Gc16) {
        for (int ty = y0; ty <= y1; ++ty)
            for (int tx = x0; tx <= x1; ++tx) debt_[size_t(ty) * tilesX_ + tx] = 0;
        return update;
    }
    Rect owed{0, 0, 0, 0};
    for (int ty = y0; ty <= y1; ++ty) {
        for (int tx = x0; tx <= x1; ++tx) {
            uint8_t& n = debt_[size_t(ty) * tilesX_ + tx];
            if (n < 255) ++n;
            if (n >= policy_.fullEvery)
                include(&owed, tx * kTile, ty * kTile, (tx + 1) * kTile, (ty + 1) * kTile);
        }
    }
    if (isEmpty(owed)) return update;
    if (owed.right > width_) owed.right = width_;
    if (owed.bottom > height_) owed.bottom = height_;
    const Rect clear = bounds(owed, r);
    for (int ty = clear.top / kTile; ty <= (clear.bottom - 1) / kTile; ++ty)
        for (int tx = clear.left / kTile; tx <= (clear.right - 1) / kTile; ++tx)
            debt_[size_t(ty) * tilesX_ + tx] = 0;
    return {clear, Waveform::Gc16, true};
}
void Panel::repaint(uint8_t* fb, size_t fbStride, PanelUpdate* update) {
    for (int y = 0; y < height_; ++y) {
        const uint8_t* t = target_.data() + size_t(y) * width_;
        std::memcpy(shown_.data() + size_t(y) * width_, t, size_t(width_));
        std::memcpy(fb + size_t(y) * fbStride, t, size_t(width_));
    }
    // What a GC16 of the whole screen settles: the picture is known again, no
    // second pass is owed against a picture that is no longer there, and every
    // square has just been driven, so nothing is owed on a count either.
    //
    // stale_ is left as it is. It means the canvas is not what the screen shows
    // -- the sleep picture -- and that is still true of a repaint which does not
    // read the canvas at all; the next frame has to take the whole canvas.
    invalid_ = false;
    pending_ = touched_ = {0, 0, 0, 0};
    debt_.assign(debt_.size(), 0);
    *update = {Rect{0, 0, width_, height_}, Waveform::Gc16, true};
}
bool Panel::settle(uint8_t* fb, size_t fbStride, PanelUpdate* update) {
    if (!pending() || invalid_) return false;
    const Rect area = pending_, touched = touched_;
    pending_ = touched_ = {0, 0, 0, 0};
    const int width = area.right - area.left;
    Rect box{0, 0, 0, 0};
    for (int y = area.top; y < area.bottom; ++y) {
        const size_t at = size_t(y) * width_ + area.left;
        const uint8_t* t = target_.data() + at;
        uint8_t* s = shown_.data() + at;
        if (!std::memcmp(s, t, size_t(width))) continue;
        int a = 0, b = width;
        differingSpan(s, t, &a, &b);
        std::memcpy(s + a, t + a, size_t(b - a));
        std::memcpy(fb + size_t(y) * fbStride + area.left + a, t + a, size_t(b - a));
        include(&box, area.left + a, y, area.left + b, y + 1);
    }
    // Everything outside the box already shows its level, so a larger area of
    // the framebuffer, or all of it, is the picture as well.
    if (!isEmpty(touched)) {
        *update = charge({bounds(box, touched), Waveform::Gc16, true});
        return true;
    }
    if (isEmpty(box)) return false;
    *update = charge(isPage(box) ? exactUpdate(box) : PanelUpdate{box, Waveform::Gl16, false});
    return true;
}
}  // namespace pw3
