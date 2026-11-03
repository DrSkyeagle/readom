// SPDX-License-Identifier: Apache-2.0
// The panel state against a per-pixel model of the same rules.
#include "../Panel.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

static uint32_t seed = 4242;
static uint32_t rnd() { seed = seed * 1664525u + 1013904223u; return seed >> 8; }

namespace {
const int W = 83, H = 37, S = 96;  // odd width: vector part and scalar tail; padded framebuffer rows
struct Model {
    std::vector<uint8_t> target, shown;
    pw3::Rect pending{0, 0, 0, 0};
    Model() : target(size_t(W) * H), shown(size_t(W) * H) {}
};
bool plain(uint8_t v) { return v == 0 || v == 255; }
bool page(const pw3::Rect& r) { return pw3::area(r) * 100 >= (long long)W * H * 50; }
// Expected updates for one present() call on a panel that is not idle, from
// the rules in Panel.h.
void model(Model& m, const pw3::Panel& panel, const uint8_t* canvas, const pw3::PanelPolicy& policy,
           const pw3::Rect* damage, size_t count, std::vector<pw3::PanelUpdate>* expected) {
    for (size_t r = 0; r < count; ++r) {
        const pw3::Rect area = pw3::intersect(damage[r], pw3::Rect{0, 0, W, H});
        if (pw3::isEmpty(area)) continue;
        pw3::Rect box{0, 0, 0, 0};
        bool grey = false;
        // A first pass only for a change whose pixels span movePercent of the
        // screen; a smaller one is shown as it is, all the pixels that differ
        // from what is shown, and no longer owes anything if the area holds
        // all that was owed.
        pw3::Rect span{0, 0, 0, 0};
        for (int y = area.top; y < area.bottom; ++y) for (int x = area.left; x < area.right; ++x)
            if (panel.level(canvas[size_t(y) * W + x]) != m.target[size_t(y) * W + x]) span = pw3::bounds(span, pw3::Rect{x, y, x + 1, y + 1});
        const bool first = policy.fast && pw3::area(span) * 100 >= (long long)W * H * policy.movePercent;
        for (int y = area.top; y < area.bottom; ++y) {
            // The row span to look at: level changed, or (shown at once) not shown.
            int a = -1, b = -1;
            for (int x = area.left; x < area.right; ++x) {
                const size_t at = size_t(y) * W + x;
                const uint8_t next = panel.level(canvas[at]);
                if (first ? next != m.target[at] : next != m.shown[at]) { if (a < 0) a = x; b = x + 1; }
                if (!first) m.target[at] = next;
            }
            if (a < 0) continue;
            bool changed = false, pending = false;
            for (int x = a; x < b; ++x) {
                const size_t at = size_t(y) * W + x;
                const uint8_t next = panel.level(canvas[at]);
                // First pass: a pixel whose level stays keeps what it shows.
                const uint8_t value = !first ? next : next == m.target[at] ? m.shown[at] : panel.firstPass(next, x, y);
                if (value != m.shown[at]) {
                    changed = true;
                    grey |= !plain(m.shown[at]) || (!first && !plain(value));
                }
                if (first && value != next) pending = true;
                m.target[at] = next;
                m.shown[at] = value;
            }
            if (changed) box = pw3::bounds(box, pw3::Rect{a, y, b, y + 1});
            if (pending) m.pending = pw3::bounds(m.pending, pw3::Rect{a, y, b, y + 1});
        }
        if (!first && pw3::contains(area, m.pending)) m.pending = {0, 0, 0, 0};
        if (pw3::isEmpty(box)) continue;
        // Between black and white: A2 unless it is a page. From grey to black
        // or white (first pass): DU. Grey levels at once: GL16 here, see main().
        const pw3::Waveform plainly = policy.a2 && !page(box) ? pw3::Waveform::A2 : pw3::Waveform::Du;
        expected->push_back({box, !grey ? plainly : first ? pw3::Waveform::Du : pw3::Waveform::Gl16, false});
    }
}
bool same(const pw3::Rect& a, const pw3::Rect& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}
}  // namespace

int main() {
    // Levels, for an even spread and for panels of several contrasts: 16 of
    // them, rising with luma, black and white kept.
    {
        uint8_t levels[256], weaker[256];
        for (unsigned contrast : {0u, 1u, 2u, 8u, 12u, 15u, 40u, 1000u}) {
            pw3::panelLevels(contrast, levels);
            assert(levels[0] == 0 && levels[255] == 255);
            for (int v = 1; v < 256; ++v) assert(levels[v] >= levels[v - 1] && levels[v] % 17 == 0);
            if (contrast < 2) for (int v = 0; v < 256; ++v) assert(levels[v] == (v + 8) / 17 * 17);
            // The darker a panel's black, the lighter the level that shows a grey.
            if (contrast > 2) for (int v = 0; v < 256; ++v) assert(levels[v] >= weaker[v]);
            std::memcpy(weaker, levels, sizeof(weaker));
        }
    }
    {
        // The product's panel. Android's greys for text on white: primary and
        // secondary text black or next to it, hints in the middle, the window
        // background white.
        pw3::Panel panel(W, H);
        assert(pw3::PanelPolicy().contrast == 12);
        assert(panel.level(0x21) == 0 && panel.level(0x42) == 0 && panel.level(0x75) == 3 * 17 && panel.level(0x9e) == 7 * 17);
        assert(panel.level(0xe0) == 13 * 17 && panel.level(0xf5) == 14 * 17 && panel.level(0xfa) == 255);
        // Kept: how much darker than white a grey is. Level k of 15 has
        // lightness black + k/15 * (100 - black), white being 100.
        const double black = 116.0 * std::cbrt(1.0 / 12) - 16.0;
        for (int v = 0; v < 256; ++v) {
            const double c = v / 255.0, y = c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
            const double wanted = std::max(black, y > 216.0 / 24389 ? 116.0 * std::cbrt(y) - 16.0 : y * 24389.0 / 27);
            const double shown = black + panel.level(uint8_t(v)) / 255.0 * (100.0 - black);
            assert(std::fabs(shown - wanted) <= (100.0 - black) / 30 + 1e-9);  // half a level
        }
        // Changing the contrast repaints the screen.
        std::vector<uint8_t> fb(size_t(S) * H);
        std::vector<pw3::PanelUpdate> updates;
        const pw3::Rect all{0, 0, W, H};
        panel.present(&all, 1, fb.data(), S, &updates);
        pw3::PanelPolicy other; other.contrast = 15;
        panel.setPolicy(other);
        assert(panel.needsFull());
        updates.clear(); panel.present(&all, 1, fb.data(), S, &updates);
        panel.setPolicy(other);
        assert(!panel.needsFull());
    }

    long long presents = 0, updatesSeen = 0, settles = 0;
    for (int variant = 0; variant < 32; ++variant) {
        pw3::PanelPolicy policy;
        policy.fast = variant & 1; policy.a2 = variant & 2; policy.dither = variant & 4;
        if (variant & 8) policy.contrast = 0;
        if (variant & 16) policy.movePercent = 0;
        // The model above is the two-pass rule alone, or no first pass at all:
        // the panel is never idle, GL16 is the only 16-level waveform, and the
        // second pass covers the pixels whose level is still wrong; no page
        // repaints the whole screen.
        policy.clean = false; policy.fullEvery = 0;
        pw3::Panel panel(W, H);
        panel.setPolicy(policy);
        // First pass values: black below the dither range, white above it.
        for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) {
            assert(panel.firstPass(0, x, y) == 0 && panel.firstPass(85, x, y) == 0);
            assert(panel.firstPass(187, x, y) == 255 && panel.firstPass(255, x, y) == 255);
        }
        std::vector<uint8_t> fb(size_t(S) * H, 0x5a), before;
        std::vector<pw3::PanelUpdate> updates, expected;
        Model m;
        uint8_t* canvas = panel.canvas();
        for (size_t i = 0; i < size_t(W) * H; ++i) canvas[i] = uint8_t(rnd());
        // Unknown panel: whatever the damage, the whole screen is repainted with GC16.
        assert(panel.needsFull());
        const pw3::Rect tiny{3, 3, 5, 5};
        panel.present(&tiny, 1, fb.data(), S, &updates);
        assert(updates.size() == 1 && updates[0].full && updates[0].waveform == pw3::Waveform::Gc16 &&
               same(updates[0].rect, pw3::Rect{0, 0, W, H}) && !panel.needsFull() && !panel.pending());
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
            const uint8_t v = panel.level(canvas[size_t(y) * W + x]);
            m.target[size_t(y) * W + x] = m.shown[size_t(y) * W + x] = v;
            assert(fb[size_t(y) * S + x] == v);
        }
        for (int y = 0; y < H; ++y) for (int x = W; x < S; ++x) assert(fb[size_t(y) * S + x] == 0x5a);
        // The same picture again: nothing to send.
        updates.clear();
        const pw3::Rect all{0, 0, W, H};
        panel.present(&all, 1, fb.data(), S, &updates);
        assert(updates.empty());

        for (int round = 0; round < 3000; ++round) {
            // Change some pixels: blocks of black, white, greys, noise or a single pixel.
            pw3::Rect damage[3];
            const size_t count = 1 + rnd() % 3;
            for (size_t r = 0; r < count; ++r) {
                const int l = int(rnd() % W), t = int(rnd() % H);
                const int w = 1 + int(rnd() % (rnd() % 4 ? 20 : W)), h = 1 + int(rnd() % (rnd() % 4 ? 8 : H));
                damage[r] = {l, t, std::min(W, l + w), std::min(H, t + h)};
                const int kind = int(rnd() % 6);
                // Sometimes only part of the damaged rectangle really changes.
                const pw3::Rect paint = rnd() % 3 ? damage[r]
                    : pw3::Rect{damage[r].left, damage[r].top, damage[r].left + (damage[r].right - damage[r].left + 1) / 2, damage[r].bottom};
                for (int y = paint.top; y < paint.bottom; ++y) for (int x = paint.left; x < paint.right; ++x) {
                    uint8_t& v = canvas[size_t(y) * W + x];
                    switch (kind) {
                        case 0: v = 0; break;
                        case 1: v = 255; break;
                        case 2: v = uint8_t(rnd()); break;
                        case 3: v = uint8_t((x / 3 + y / 2) & 1 ? 0 : 255); break;
                        case 4: v = uint8_t(120 + rnd() % 30); break;
                        default: if (rnd() % 7 == 0) v = uint8_t(255 - v); break;
                    }
                }
            }
            // Damage reaching outside the screen is clipped.
            if (rnd() % 50 == 0) { damage[0].left -= 5; damage[0].bottom += 9; }
            before = fb;
            updates.clear(); expected.clear();
            model(m, panel, canvas, policy, damage, count, &expected);
            panel.present(damage, count, fb.data(), S, &updates);
            ++presents;
            assert(updates.size() == expected.size());
            for (size_t i = 0; i < updates.size(); ++i, ++updatesSeen)
                assert(same(updates[i].rect, expected[i].rect) && updates[i].waveform == expected[i].waveform && !updates[i].full);
            assert(!std::memcmp(panel.target(), m.target.data(), m.target.size()));
            // The framebuffer is what is shown; outside the updates nothing moved.
            for (int y = 0; y < H; ++y) for (int x = 0; x < S; ++x) {
                const uint8_t v = fb[size_t(y) * S + x];
                if (x >= W) { assert(v == 0x5a); continue; }
                assert(v == panel.shown()[size_t(y) * W + x]);
                bool inside = false;
                for (const auto& u : updates) inside |= x >= u.rect.left && x < u.rect.right && y >= u.rect.top && y < u.rect.bottom;
                if (!inside) assert(v == before[size_t(y) * S + x]);
                // Every pixel shows its level or, in the first pass, black or white.
                const uint8_t t = panel.target()[size_t(y) * W + x];
                assert(v == t || (policy.fast && plain(v)));
                if (v != t) assert(x >= m.pending.left && x < m.pending.right && y >= m.pending.top && y < m.pending.bottom);
            }
            assert(!std::memcmp(panel.shown(), m.shown.data(), m.shown.size()));
            assert(panel.pending() == !pw3::isEmpty(m.pending));
            if (rnd() % 9 == 0) {
                before = fb;
                pw3::PanelUpdate u{};
                const bool sent = panel.settle(fb.data(), S, &u);
                ++settles;
                assert(!panel.pending());
                assert(!std::memcmp(panel.shown(), panel.target(), m.target.size()));
                bool differs = false;
                for (int y = 0; y < H; ++y) for (int x = 0; x < S; ++x) {
                    const uint8_t v = fb[size_t(y) * S + x];
                    if (x >= W) { assert(v == 0x5a); continue; }
                    assert(v == panel.target()[size_t(y) * W + x]);
                    if (v != before[size_t(y) * S + x]) {
                        differs = true;
                        assert(sent && x >= u.rect.left && x < u.rect.right && y >= u.rect.top && y < u.rect.bottom);
                    }
                }
                assert(sent == differs);
                if (sent) assert(u.waveform == pw3::Waveform::Gl16 && !u.full);
                m.shown = m.target;
                m.pending = {0, 0, 0, 0};
                pw3::PanelUpdate again{};
                assert(!panel.settle(fb.data(), S, &again));
            }
        }
        // After invalidate() the next frame is a full repaint again.
        panel.invalidate();
        assert(panel.needsFull() && panel.unknown());
        updates.clear();
        panel.present(&tiny, 1, fb.data(), S, &updates);
        assert(updates.size() == 1 && updates[0].full && !panel.pending());
        assert(!panel.needsFull() && !panel.unknown());
        // The two halves of needsFull(), which must not be read as one. Only
        // after invalidate() is the picture on the screen unknown; after
        // assumeShown() it is known and merely is not the canvas -- the sleep
        // picture, which is meant to stay. The gralloc device repaints the
        // screen by itself when the picture is unknown and no frame may be
        // coming (framebuffer.cpp, recoverLocked); keyed on needsFull() instead,
        // the same code would paint the canvas over the sleep picture of a
        // device that is going to sleep.
        std::vector<uint8_t> picture(size_t(S) * H, 0x80);
        panel.assumeShown(picture.data(), S);
        assert(panel.needsFull() && !panel.unknown());
        // The repaint the gralloc device makes by itself when the panel holds an
        // unknown picture: what the last frame asked for, one GC16 of the whole
        // screen, nothing owed afterwards. It takes target_ and never the
        // canvas, because it runs on the settle thread while the composer writes
        // the canvas without that lock -- so a canvas scribbled over here must
        // not reach the panel.
        panel.invalidate();
        std::memset(panel.canvas(), 0x3c, size_t(W) * H);
        pw3::PanelUpdate repainted{};
        panel.repaint(fb.data(), S, &repainted);
        assert(same(repainted.rect, pw3::Rect{0, 0, W, H}) && repainted.full &&
               repainted.waveform == pw3::Waveform::Gc16);
        assert(!panel.unknown() && !panel.pending());
        // stale_ is left standing: the canvas is still not what the screen
        // shows, and the next frame has to take all of it.
        assert(panel.needsFull());
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
            const uint8_t want = panel.target()[size_t(y) * W + x];
            assert(fb[size_t(y) * S + x] == want && panel.shown()[size_t(y) * W + x] == want);
        }
    }
    // Every Nth large second pass becomes one GC16 repaint of the whole screen.
    {
        pw3::PanelPolicy policy;
        policy.fullEvery = 3;
        policy.clean = false;
        pw3::Panel panel(W, H);
        panel.setPolicy(policy);
        std::vector<uint8_t> fb(size_t(S) * H);
        std::vector<pw3::PanelUpdate> updates;
        const pw3::Rect all{0, 0, W, H};
        std::memset(panel.canvas(), 255, size_t(W) * H);
        panel.present(&all, 1, fb.data(), S, &updates);
        int full = 0, partial = 0;
        for (int i = 0; i < 9; ++i) {
            std::memset(panel.canvas(), i % 2 ? 200 : 100, size_t(W) * H);
            updates.clear();
            panel.present(&all, 1, fb.data(), S, &updates);
            assert(panel.pending());
            pw3::PanelUpdate u{};
            assert(panel.settle(fb.data(), S, &u));
            if (u.full) { ++full; assert(u.waveform == pw3::Waveform::Gc16 && same(u.rect, all)); } else ++partial;
        }
        assert(full == 3 && partial == 6);
    }
    // The same count, on a part of the screen: what a list, a keyboard or a
    // dragged icon does. Each square of the screen owes for the updates that
    // reached it, so a corner being changed over and over is repainted while
    // the rest of the screen, which nothing has touched, is left alone. On
    // this device's own geometry: the squares are 128 px, and the panel the
    // tests above use is smaller than one of them.
    {
        const int PW = 1072, PH = 1448;
        pw3::PanelPolicy policy;
        policy.fullEvery = 4;
        policy.clean = false;
        policy.fast = false;         // every change at once, exactly: one update each
        pw3::Panel panel(PW, PH);
        panel.setPolicy(policy);
        std::vector<uint8_t> fb(size_t(PW) * PH);
        std::vector<pw3::PanelUpdate> updates;
        const pw3::Rect all{0, 0, PW, PH};
        std::memset(panel.canvas(), 255, size_t(PW) * PH);
        panel.present(&all, 1, fb.data(), PW, &updates);   // the first frame clears everything

        // A box well inside one corner, changed again and again.
        const pw3::Rect corner{16, 16, 80, 80};
        int cleared = 0;
        for (int i = 0; i < 8; ++i) {
            for (int y = corner.top; y < corner.bottom; ++y)
                std::memset(panel.canvas() + size_t(y) * PW + corner.left, i % 2 ? 160 : 96,
                            size_t(corner.right - corner.left));
            updates.clear();
            panel.present(&corner, 1, fb.data(), PW, &updates);
            assert(updates.size() == 1);
            if (updates[0].waveform == pw3::Waveform::Gc16) {
                ++cleared;
                // It covers the square that was owing, not the whole screen.
                assert(contains(updates[0].rect, corner) && area(updates[0].rect) < area(all));
            }
        }
        assert(cleared == 2);        // of eight updates, the 4th and the 8th

        // A different corner owes nothing yet: the first change there is not
        // a repaint, however much the other corner has been changing.
        const pw3::Rect far{PW - 80, PH - 80, PW - 16, PH - 16};
        for (int y = far.top; y < far.bottom; ++y)
            std::memset(panel.canvas() + size_t(y) * PW + far.left, 160, size_t(far.right - far.left));
        updates.clear();
        panel.present(&far, 1, fb.data(), PW, &updates);
        assert(updates.size() == 1 && updates[0].waveform != pw3::Waveform::Gc16);
    }
    // The policy the product runs with, with GC16 as the page waveform: the
    // one setting in which a page drives its whole area.
    {
        pw3::PanelPolicy policy;
        // 20, not 30: the reader chose it in front of the screen on
        // 2026-10-22 (Panel.h). This line said 30 until 2026-10-23 and nothing
        // noticed, because this file was built only as a device module and the
        // suite never ran it.
        assert(policy.fast && policy.a2 && !policy.clean && policy.directPercent == 0 && policy.movePercent == 50 && policy.pagePercent == 50 &&
               policy.exact == pw3::Waveform::Gl16 && policy.fullEvery == 20);
        policy.exact = pw3::Waveform::Gc16; policy.fullEvery = 0;
        pw3::Panel panel(W, H);
        panel.setPolicy(policy);
        std::vector<uint8_t> fb(size_t(S) * H);
        std::vector<pw3::PanelUpdate> updates;
        const pw3::Rect all{0, 0, W, H};
        auto fill = [&](const pw3::Rect& r, uint8_t v) {
            for (int y = r.top; y < r.bottom; ++y) std::memset(panel.canvas() + size_t(y) * W + r.left, v, size_t(r.right - r.left));
        };
        auto shows = [&](bool exactly) {
            for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
                const uint8_t v = fb[size_t(y) * S + x], t = panel.target()[size_t(y) * W + x];
                assert(v == panel.shown()[size_t(y) * W + x] && (v == t || (!exactly && plain(v))));
            }
        };
        auto present = [&](const pw3::Rect& damage, bool idle) {
            updates.clear();
            panel.present(&damage, 1, fb.data(), S, &updates, idle);
        };
        auto one = [&](const pw3::Rect& rect, pw3::Waveform waveform, bool full) {
            return updates.size() == 1 && same(updates[0].rect, rect) && updates[0].waveform == waveform && updates[0].full == full;
        };
        pw3::PanelUpdate u{};
        fill(all, 255);
        present(all, true);
        const pw3::Rect block{10, 5, 30, 15};

        // The panel is idle: every change is shown once, exactly, and nothing follows.
        // Grey, smaller than a page: GL16, which drives the pixels that change.
        fill(block, 102); present(all, true);
        assert(one(block, pw3::Waveform::Gl16, false) && !panel.pending()); shows(true);
        fill(block, 255); present(block, true);
        assert(one(block, pw3::Waveform::Gl16, false) && !panel.pending()); shows(true);
        // Between black and white: A2 for a small one, DU for a page.
        fill(block, 0); present(block, true);
        assert(one(block, pw3::Waveform::A2, false) && !panel.pending());
        fill(block, 255); present(block, true);
        fill(all, 0); present(all, true);
        assert(one(all, pw3::Waveform::Du, false) && !panel.pending());
        fill(all, 255); present(all, true);
        // A page with grey in it: the page waveform, sent for the whole area.
        fill(all, 187); present(all, true);
        assert(one(all, pw3::Waveform::Gc16, true) && !panel.pending()); shows(true);
        // A large damaged area in which little changes is not a page.
        fill(pw3::Rect{0, 0, 4, 4}, 102); present(all, true);
        assert(one(pw3::Rect{0, 0, 4, 4}, pw3::Waveform::Gl16, false) && !panel.pending());
        fill(all, 255); present(all, true);
        assert(one(all, pw3::Waveform::Gc16, true));

        // Movement - the panel is not idle. A change smaller than a page is
        // shown at once and exactly all the same: nothing is owed, nothing flashes.
        fill(block, 102); present(all, false);
        assert(one(block, pw3::Waveform::Gl16, false) && !panel.pending()); shows(true);
        fill(block, 0); present(block, false);
        assert(one(block, pw3::Waveform::Gl16, false) && !panel.pending());
        fill(block, 255); present(block, false);
        assert(one(block, pw3::Waveform::A2, false) && !panel.pending()); shows(true);
        // A large one: black and white at once (DU, being a page), then the levels.
        const pw3::Rect most{0, 0, W, H * 2 / 3};
        fill(most, 102); present(all, false);
        assert(updates.size() == 1 && updates[0].waveform == pw3::Waveform::Du && !updates[0].full && panel.pending()); shows(false);
        // While that is owed, a small change elsewhere is still shown at once.
        const pw3::Rect other{40, 30, 60, 35};
        fill(other, 153); present(other, true);
        assert(one(other, pw3::Waveform::Gl16, false) && panel.pending()); shows(false);
        assert(panel.settle(fb.data(), S, &u) && u.waveform == pw3::Waveform::Gc16 && u.full && contains(most, u.rect));
        assert(!panel.pending() && !panel.settle(fb.data(), S, &u)); shows(true);
        // From grey back to white: DU, the levels right at once; nothing is owed.
        fill(most, 255); present(most, false);
        assert(updates.size() == 1 && updates[0].waveform == pw3::Waveform::Du && !panel.pending()); shows(true);
        fill(other, 255); present(other, true);
        {
            // With cleaning, the second pass repaints with GC16 what DU took away from grey.
            pw3::PanelPolicy cleaning = policy; cleaning.clean = true;
            panel.setPolicy(cleaning);
            fill(most, 102); present(most, true);
            assert(one(most, pw3::Waveform::Gc16, true) && !panel.pending());
            fill(most, 255); present(most, false);
            assert(updates.size() == 1 && updates[0].waveform == pw3::Waveform::Du && panel.pending()); shows(true);
            const std::vector<uint8_t> unchanged = fb;
            assert(panel.settle(fb.data(), S, &u) && contains(most, u.rect) && u.waveform == pw3::Waveform::Gc16 && u.full);
            assert(fb == unchanged && !panel.pending() && !panel.settle(fb.data(), S, &u));
            // A small change is shown at once, so it never needs cleaning.
            fill(block, 102); present(block, true);
            fill(block, 255); present(block, false);
            assert(one(block, pw3::Waveform::Gl16, false) && !panel.pending());
            panel.setPolicy(policy);
        }
        // A page in the middle of movement keeps its first pass and is made
        // exact, all of it, by the page waveform.
        fill(all, 102); present(all, false);
        assert(one(all, pw3::Waveform::Du, false) && panel.pending()); shows(false);
        assert(panel.settle(fb.data(), S, &u) && same(u.rect, all) && u.waveform == pw3::Waveform::Gc16 && u.full);
        shows(true);
        fill(all, 255); present(all, true);

        {
            // With GL16 as the page waveform, a page as well: not sent
            // for its whole area, and every fullEvery-th page repaints the
            // screen. Read from the policy rather than written out, because
            // the number is the reader's and has moved once already.
            const unsigned every = pw3::PanelPolicy().fullEvery;
            panel.setPolicy(pw3::PanelPolicy());
            for (unsigned i = 1; i <= 3 * every; ++i) {
                fill(all, i % 2 ? 187 : 255); present(all, true);
                assert(i % every ? one(all, pw3::Waveform::Gl16, false) : one(all, pw3::Waveform::Gc16, true));
            }
            // Only a page shown at once on an idle panel, and every change
            // may have a first pass: a small one gets it as well.
            pw3::PanelPolicy pages = policy; pages.directPercent = 50; pages.movePercent = 0;
            panel.setPolicy(pages);
            fill(block, 102); present(all, true);
            assert(one(block, pw3::Waveform::A2, false) && panel.pending());
            assert(panel.settle(fb.data(), S, &u) && u.waveform == pw3::Waveform::Gl16);
            fill(all, 187); present(all, true);
            assert(one(all, pw3::Waveform::Gc16, true) && !panel.pending());
            // No change at once.
            pages.directPercent = 101; panel.setPolicy(pages);
            fill(all, 255); present(all, true);
            assert(one(all, pw3::Waveform::Du, false) && !panel.pending());
            // No first pass at all.
            pw3::PanelPolicy slow = policy; slow.fast = false;
            panel.setPolicy(slow);
            fill(all, 102); present(all, false);
            assert(one(all, pw3::Waveform::Gc16, true) && !panel.pending()); shows(true);
            fill(all, 255); present(all, false);
        }

        // Every third page shown with the 16-level waveform repaints the whole
        // screen. The pages between it are GL16, which is what the product
        // ships; this counted against REAGL until 2026-10-29, when that went,
        // and GC16 is now the only waveform that drives the whole area, so the
        // two roles have to be told apart by which waveform arrives.
        pw3::PanelPolicy every = policy; every.fullEvery = 3; every.exact = pw3::Waveform::Gl16;
        panel.setPolicy(every);
        int full = 0, exact = 0;
        for (int i = 0; i < 9; ++i) {
            fill(all, i % 2 ? 204 : 153); present(all, true);
            assert(updates.size() == 1 && same(updates[0].rect, all) && !panel.pending());
            if (updates[0].waveform == pw3::Waveform::Gc16) { assert(updates[0].full); ++full; }
            else { assert(updates[0].waveform == pw3::Waveform::Gl16); ++exact; }
            shows(true);
        }
        assert(full == 3 && exact == 6);

        // Random changes: the framebuffer is what is shown; a pixel is at its
        // level or, between the passes, black or white; on an idle panel with
        // nothing owed, and after the second pass, every pixel is at its level.
        panel.setPolicy(policy);
        for (int round = 0; round < 3000; ++round) {
            const int l = int(rnd() % W), t = int(rnd() % H);
            const pw3::Rect r{l, t, std::min(W, l + 1 + int(rnd() % (rnd() % 3 ? 20 : W))), std::min(H, t + 1 + int(rnd() % (rnd() % 3 ? 8 : H)))};
            const int kind = int(rnd() % 4);
            for (int y = r.top; y < r.bottom; ++y) for (int x = r.left; x < r.right; ++x)
                panel.canvas()[size_t(y) * W + x] = kind == 0 ? 0 : kind == 1 ? 255 : kind == 2 ? uint8_t(rnd()) : uint8_t(120 + rnd() % 30);
            const bool idle = rnd() % 2, owed = panel.pending();
            present(rnd() % 4 ? r : all, idle);
            ++presents; updatesSeen += (long long)updates.size();
            shows(idle && !owed);
            if (idle && !owed) assert(!panel.pending());
            if (rnd() % 5 == 0) {
                const bool was = panel.pending();
                const bool sent = panel.settle(fb.data(), S, &u);
                ++settles;
                assert((sent || !was) && !panel.pending());
                if (sent) assert(u.waveform == pw3::Waveform::Gc16 ? u.full : page(u.rect) ? u.full && u.waveform == pw3::Waveform::Gc16
                                                                                           : !u.full && u.waveform == pw3::Waveform::Gl16);
                shows(true);
            }
        }
    }
    {
        // A picture drawn outside present() (the sleep picture): the next
        // present() takes all of the canvas, whatever damage it is given, and
        // changes only the pixels that differ from the picture, without GC16.
        pw3::PanelPolicy policy; policy.fullEvery = 0;
        pw3::Panel panel(W, H);
        panel.setPolicy(policy);
        std::vector<uint8_t> fb(size_t(S) * H, 0);
        std::vector<pw3::PanelUpdate> updates;
        const pw3::Rect all{0, 0, W, H};
        std::memset(panel.canvas(), 255, size_t(W) * H);
        panel.present(&all, 1, fb.data(), S, &updates, true);
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) fb[size_t(y) * S + x] = uint8_t((x + y) % 4 ? 255 : 17 * (x % 16));
        panel.assumeShown(fb.data(), S);
        assert(panel.needsFull() && !panel.pending());
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) assert(panel.shown()[size_t(y) * W + x] == fb[size_t(y) * S + x]);
        // The page the device wakes to: white, with one line of text.
        std::memset(panel.canvas(), 255, size_t(W) * H);
        for (int x = 5; x < 60; ++x) panel.canvas()[size_t(10) * W + x] = 0;
        const pw3::Rect line{5, 10, 60, 11};
        updates.clear();
        panel.present(&line, 1, fb.data(), S, &updates, true);
        assert(!panel.needsFull() && !updates.empty());
        for (const auto& u : updates) assert(u.waveform != pw3::Waveform::Gc16);
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
            const uint8_t want = y == 10 && x >= 5 && x < 60 ? 0 : 255;
            assert(panel.shown()[size_t(y) * W + x] == want && fb[size_t(y) * S + x] == want);
        }
        // Not stale any more: damage is taken as given.
        updates.clear();
        panel.present(&line, 1, fb.data(), S, &updates, true);
        assert(updates.empty());
        // After invalidate(), a picture still leaves the screen known.
        panel.invalidate();
        panel.assumeShown(fb.data(), S);
        updates.clear();
        panel.present(&all, 1, fb.data(), S, &updates, true);
        for (const auto& u : updates) assert(u.waveform != pw3::Waveform::Gc16);
    }
    {
        // A second pass that is owed does not outlive invalidate(). After it the
        // screen holds something unknown, so a second pass worked out against the
        // old picture would be wrong; and a loop that asks pending() whether there
        // is work, then settle() for the work, would turn for ever between a
        // pending() that says yes and a settle() that refuses while invalid.
        pw3::PanelPolicy policy; policy.fullEvery = 0;
        pw3::Panel panel(W, H);
        panel.setPolicy(policy);
        std::vector<uint8_t> fb(size_t(S) * H, 0);
        std::vector<pw3::PanelUpdate> updates;
        const pw3::Rect all{0, 0, W, H};
        std::memset(panel.canvas(), 255, size_t(W) * H);
        panel.present(&all, 1, fb.data(), S, &updates, true);
        // Grey over the whole screen while the panel is busy: a black-and-white
        // first pass now and a grey second pass owed.
        std::memset(panel.canvas(), 119, size_t(W) * H);
        updates.clear();
        panel.present(&all, 1, fb.data(), S, &updates, false);
        assert(!updates.empty() && panel.pending());
        panel.invalidate();
        pw3::PanelUpdate owed{};
        assert(!panel.pending() && !panel.settle(fb.data(), S, &owed));
    }
    // replaced(): a window appeared or went away here, so the update that
    // brings the new content clears instead of drawing over what is there.
    // On a panel of several squares, so that "clears where it was told to"
    // can be told from "clears the screen".
    {
        const int PW2 = 300, PH2 = 300, PS2 = 320;   // 3 x 3 squares of 128
        pw3::Panel panel(PW2, PH2);
        std::vector<uint8_t> fb(size_t(PS2) * PH2);
        std::vector<pw3::PanelUpdate> updates;
        const pw3::Rect all{0, 0, PW2, PH2};
        const auto paint = [&](const pw3::Rect& r, uint8_t v) {
            for (int y = r.top; y < r.bottom; ++y)
                std::memset(panel.canvas() + size_t(y) * PW2 + r.left, v, size_t(r.right - r.left));
        };
        const auto show = [&](const pw3::Rect& r) {
            updates.clear();
            panel.present(&r, 1, fb.data(), size_t(PS2), &updates, true);
        };
        // The first present repaints the screen and owes nothing.
        paint(all, 255); show(all);
        assert(updates.size() == 1 && updates[0].waveform == pw3::Waveform::Gc16);

        // A grey change in the top left square, with nothing replaced: GL16.
        const pw3::Rect topLeft{10, 10, 100, 100};
        paint(topLeft, 119); show(topLeft);
        assert(updates.size() == 1 && updates[0].waveform == pw3::Waveform::Gl16 && !updates[0].full);

        // The same change where a window went away: GC16, and over at least
        // the area that was replaced.
        const pw3::Rect window{140, 140, 260, 260};
        panel.replaced(window);
        paint(window, 68); show(window);
        assert(updates.size() == 1 && updates[0].waveform == pw3::Waveform::Gc16 && updates[0].full);
        assert(updates[0].rect.left <= window.left && updates[0].rect.top <= window.top &&
               updates[0].rect.right >= window.right && updates[0].rect.bottom >= window.bottom);

        // It was paid off: the next change there is GL16 again.
        paint(window, 170); show(window);
        assert(updates.size() == 1 && updates[0].waveform == pw3::Waveform::Gl16);

        // And a square that was never replaced is untouched by all of this.
        const pw3::Rect elsewhere{10, 200, 100, 290};
        paint(elsewhere, 119); show(elsewhere);
        assert(updates.size() == 1 && updates[0].waveform == pw3::Waveform::Gl16);

        // fullEvery 0 means the panel is never cleared on a count, and the
        // same switch turns this off: a replaced area is not cleared either.
        pw3::PanelPolicy never;
        never.fullEvery = 0;
        panel.setPolicy(never);
        panel.replaced(window);
        paint(window, 119); show(window);
        assert(updates.size() == 1 && updates[0].waveform != pw3::Waveform::Gc16);

        // An area outside the panel is ignored rather than written past.
        panel.setPolicy(pw3::PanelPolicy());
        panel.replaced(pw3::Rect{-50, -50, 0, 0});
        panel.replaced(pw3::Rect{PW2, PH2, PW2 + 500, PH2 + 500});
        paint(topLeft, 68); show(topLeft);
        assert(updates.size() == 1 && updates[0].waveform == pw3::Waveform::Gl16);
    }
    bool rejected = false;
    try { pw3::Panel bad(0, 10); } catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
    printf("panel: %lld presents, %lld updates, %lld second passes checked\n", presents, updatesSeen, settles);
    return 0;
}
