// SPDX-License-Identifier: Apache-2.0
// What the e-paper panel shows and what it should show.
//
// Measured on the PW3 panel (pw3-epdc-probe, 2026-10-13): A2 134 ms, DU 216 ms,
// every 16-level waveform (GC16, GL16, REAGL) 463 ms.
//
// A change that comes on its own - the panel is idle - is shown once and
// exactly: with a black/white waveform if nothing grey is involved, else with
// a 16-level one. The stock system shows everything it draws this way.
//
// Android also draws movement: a list being scrolled, frames arriving sooner
// than a 16-level waveform ends. Such a frame, if it changes a large part of
// the screen, is shown at once in black and white (first pass), and exactly
// once the picture has stopped changing (second pass). A smaller change is
// shown at once and exactly even while the panel is busy: typing, a cursor, a
// list of candidates. Given a first pass, the keyboard's grey text went
// through DU and was repainted with GC16 - 22 of 23 second passes while typing
// flashed ("why does the keyboard keep going black?", Test V36).
//
// The fast waveforms are not exact. DU takes a grey pixel to black or white
// only roughly: what was there stays visible, the panel driver, told the pixel
// is white now, will not touch it again on its own, and of the waveforms only
// GC16, which flashes, removes it (Test V34, user's photograph: the bookshelf's
// covers on the white home screen). The second pass can repaint with GC16 what
// a first pass moved away from grey (`clean`); it does not by default, since
// that flashes: what DU leaves is removed by the next full repaint.
//
// The 16-level waveforms: GL16 drives only the pixels whose level changes.
// REAGL, the stock system's waveform for a page, is sent to drive its whole
// area and depends on the device's REAGL algorithm in the driver: a driver
// built without it shows GC16 instead (Test V35, kernel log), a flash with
// every page ("every time the screen goes black" - not acceptable).
#pragma once
#include "Rect.h"
#include "Tone.h"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace pw3 {
// Lab126 logical waveform selectors (include/linux/mxcfb.h).
enum class Waveform : uint32_t { Du = 1, Gc16 = 2, A2 = 4, Gl16 = 5 };
struct PanelUpdate { Rect rect; Waveform waveform; bool full; };
struct PanelPolicy {
    bool fast = true;        // movement gets a black/white first pass; false: every change at once, exactly
    bool a2 = true;          // A2 for a small change between black and white, else DU
    bool dither = true;      // first pass: ordered dither for the middle greys, else a plain threshold
    // A change arriving while the panel is idle is shown once and exactly if
    // its pixels span at least this share of the screen; a smaller one gets
    // the first pass. 0: every such change; above 100: none.
    unsigned directPercent = 0;
    // A change on a busy panel gets the first pass only if its pixels span at
    // least this share of the screen; a smaller one is shown at once, exactly.
    // 0: every such change.
    unsigned movePercent = 50;
    // An update whose pixels span at least this share of the screen is a page.
    unsigned pagePercent = 50;
    // The 16-level waveform for a page: GC16 is sent to drive the whole
    // area, GL16 only the pixels whose level changes. Anything smaller than
    // a page is shown with GL16. (REAGL was a third choice until
    // 2026-10-29; it needs the stock driver's proprietary module.)
    Waveform exact = Waveform::Gl16;
    // The second pass repaints with GC16 what a first pass moved away from grey.
    bool clean = false;
    // How many updates that do not clear a part of the screen (GL16, DU, A2)
    // it takes before that part is repainted with GC16, which does; 0: never.
    // With GL16, after 30 to 40 pages the text of earlier ones shows, lighter
    // than the page; every 20 pages was too often for the user, who asked for
    // 30 (Test V35).
    //
    // Counted per region, not per page. Reading is unchanged by that -- a page
    // covers every region, so the 30th page repaints the screen, as before --
    // but a screen that is not a reader now clears as well: dragging an icon,
    // a list, a keyboard are changes far smaller than a page, and while this
    // was a count of pages they never reached it. The home screen measured 99
    // updates with GL16 against 2 with GC16 in 46 minutes of use, which is
    // what the ghosting was ("重影比较多", user, 2026-10-19).
    //
    // 20, chosen by the reader in front of the screen on 2026-10-22, from the
    // three it was shown at (tools/ghosting-ab.sh, the same twenty page turns
    // each time): 30 cleared once in those twenty pages, 20 twice, 12 five
    // times. Twenty page turns between clears is still less often than the
    // stock firmware, which clears every six; the eye is the instrument for
    // this one, and it picked the middle.
    unsigned fullEvery = 20;
    // Luminance of the panel's white over that of its black: decides which
    // level shows a luma value (Tone.h). E Ink gives 15 for the bare Carta
    // film; behind the PW3's touch layer and light guide it is lower. Not
    // measured on this device.
    unsigned contrast = 12;
};
class Panel {
public:
    Panel(int width, int height);
    int width() const { return width_; }
    int height() const { return height_; }
    // Another contrast changes the level of every pixel: the next present()
    // repaints the screen.
    void setPolicy(const PanelPolicy& policy);
    const PanelPolicy& policy() const { return policy_; }
    // width*height luma bytes, row after row. The composer writes damaged areas here.
    uint8_t* canvas() { return canvas_.data(); }
    // The next present() takes the whole canvas, whatever damage it is given:
    // after invalidate() it repaints the screen with GC16, after assumeShown()
    // it changes what differs from the picture shown.
    bool needsFull() const { return invalid_ || stale_; }
    // Nothing is known about what the screen holds, which is the half of
    // needsFull() that has to be repaired whether or not another frame ever
    // arrives. The other half, after assumeShown(), is a picture that is known
    // and merely is not the canvas -- the sleep picture, which is meant to stay.
    bool unknown() const { return invalid_; }
    // Nothing is known about the panel.
    // The screen now holds something this object does not know. A second pass
    // that was owed goes with it: it would be worked out against the picture
    // that is no longer there, and settle() refuses while invalid, so a loop
    // driven by pending() would turn without end.
    // The next frame repaints the whole screen with GC16, which owes nothing
    // afterwards.
    void invalidate() {
        invalid_ = true;
        pending_ = touched_ = Rect{0, 0, 0, 0};
        debt_.assign(debt_.size(), 0);
    }
    // The panel shows `fb` (levels, as written to the framebuffer), drawn
    // outside present(), e.g. the sleep picture. The canvas is not what the
    // screen shows any more.
    void assumeShown(const uint8_t* fb, size_t fbStride);
    // What is about to be drawn in this area is not a change to what is there
    // but a different thing in its place: a window appeared or went away.
    //
    // It matters because of what GL16 is. It drives the pixels whose level
    // changes and leaves the rest, so a picture drawn over another one keeps a
    // little of the one underneath, and only GC16 takes that off (Test V34,
    // the reader's photograph). Inside one window that is the right trade --
    // the page before shows faintly and the screen does not flash -- and
    // `fullEvery` pays the debt off every so often. Between windows it is not:
    // the Wi-Fi password dialog went away and stayed on the screen under the
    // list ("输入 WI-FI 密码后变脏", the reader, 2026-10-23), and the squares
    // it covered were nowhere near their twentieth update, because a dialog
    // opens once.
    //
    // So the area is charged to the limit and the update that draws the new
    // content pays it: one GC16, exactly where the thing used to be. Typing is
    // untouched (the keys are one window and the reader says it is clean), and
    // so is reading -- a page turn changes no window, and whether a page
    // flashes is still `fullEvery`'s question and the reader's answer to it.
    void replaced(const Rect& area);
    // Makes the canvas inside `damage` the picture. Writes the pixels the
    // panel has to change now into `fb` and appends the updates to send.
    // `idle`: every update sent so far has finished, i.e. this is not one
    // frame among many.
    void present(const Rect* damage, size_t count, uint8_t* fb, size_t fbStride, std::vector<PanelUpdate>* updates,
                 bool idle = false);
    // True while a second pass is due: pixels show the first pass instead of
    // their grey level, or were moved away from grey by one.
    bool pending() const { return !isEmpty(pending_) || !isEmpty(touched_); }
    // Second pass. False when there is nothing to send.
    bool settle(uint8_t* fb, size_t fbStride, PanelUpdate* update);
    // The picture this panel was last told to show, sent again: the framebuffer
    // and `shown_` are set to it and `update` is one GC16 of the whole screen.
    //
    // For the one caller that cannot repaint from the canvas: the gralloc device
    // repaints the screen by itself when the panel holds something unknown
    // (framebuffer.cpp, recoverLocked), and it runs on the settle thread, while
    // the canvas is written by the composer's thread without that lock. Half of
    // a frame being composed is not a picture to put on a panel. `target_` is
    // the levels the last frame asked for, it is only ever written under that
    // same lock, and it is what the screen ought to be showing -- which is the
    // whole of what a repaint has to say.
    void repaint(uint8_t* fb, size_t fbStride, PanelUpdate* update);
    const uint8_t* target() const { return target_.data(); }
    const uint8_t* shown() const { return shown_.data(); }
    // The one of the panel's 16 grey levels that shows a luma value, as 0, 17, ... 255.
    uint8_t level(uint8_t luma) const { return levels_[luma]; }
    // What the first pass shows for a grey level at a screen position.
    uint8_t firstPass(uint8_t level, int x, int y) const { return level > threshold(x, y) ? 255 : 0; }
private:
    struct RowResult { bool changed, grey, pending; };
    uint8_t threshold(int x, int y) const;
    void levelRow(const uint8_t* luma, uint8_t* out, int count) const;
    RowResult firstPassRow(const uint8_t* next, const uint8_t* target, const uint8_t* shown, uint8_t* out, int count, int x, int y) const;
    RowResult exactRow(const uint8_t* next, const uint8_t* shown, int count) const;
    int width_, height_;
    PanelPolicy policy_;
    bool invalid_ = true, stale_ = false;
    // How many updates that did not clear it have reached each square of the
    // screen since it was last cleared, saturating. A square is kTile pixels:
    // small enough that a repaint stays near what was ghosted, large enough
    // that the whole grid is a few hundred bytes.
    static const int kTile = 128;
    int tilesX_ = 0, tilesY_ = 0;
    std::vector<uint8_t> debt_;
    // pending_: pixels not at their level. touched_: pixels a first pass took
    // from grey to black or white, at their level as far as the driver knows.
    Rect pending_{0, 0, 0, 0}, touched_{0, 0, 0, 0};
    bool isPage(const Rect& box) const;
    bool changeSpans(const Rect& area, unsigned percent);
    PanelUpdate plainUpdate(const Rect& box) const;
    PanelUpdate exactUpdate(const Rect& box) const;
    PanelUpdate charge(PanelUpdate update);
    // canvas_: luma from the composer. target_: the picture in panel levels.
    // shown_: what has been written to the framebuffer, i.e. what the panel
    // shows once its queued updates have finished.
    std::vector<uint8_t> canvas_, target_, shown_, next_, out_;
    // levels_: Tone.h for policy_.contrast. steps_[k]: the lowest luma shown
    // as level k + 1 or above, i.e. the level is the number of steps reached.
    uint8_t levels_[256], steps_[15];
};
}  // namespace pw3
