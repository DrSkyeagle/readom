// SPDX-License-Identifier: Apache-2.0
// The EPDC framebuffer device. It owns the panel state (epdc/Panel.h), the
// mapped 8-bit framebuffer and the waveform updates. The PW3 composer draws
// luma into the panel canvas and calls present() through PanelOps; post()
// remains for frames SurfaceFlinger composed itself into an RGBX buffer.
#include "gralloc_priv.h"
#include "PanelOps.h"
#include "../epdc/Panel.h"
#include "../epdc/Luma.h"
#include "../epdc/EpdcAbi.h"
#include "../epdc/EpdcContract.h"
#include "../epdc/Dither.h"
#include "../epdc/Picture.h"
#include "../epdc/SleepWallpaper.h"
#include <hardware/fb.h>
#include <log/log.h>
#include <cutils/properties.h>
#include <atomic>
#include <algorithm>
#include <cerrno>
#include <climits>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <new>
#include <pthread.h>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <vector>

namespace {
struct Device {
    framebuffer_device_t api{};
    int fd = -1;
    uint8_t* pixels = nullptr;
    size_t mapped = 0;
    fb_var_screeninfo screen{};
    fb_fix_screeninfo fixed{};
    pw3::EpdcAndroidInfo epdc{};
    std::unique_ptr<pw3::Panel> panel;
    // Guards the panel, the framebuffer pixels and everything below.
    pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t wake{};
    pthread_t settleThread{};
    bool wakeReady = false, settleStarted = false, stopping = false;
    // marker: the last update sent. finished: every update up to this one has
    // left the panel; the settle thread follows the driver. Equal: the panel is idle.
    uint32_t marker = 0, finished = 0;
    // The second pass runs once the panel is idle and nothing has been shown
    // for settleNs.
    int64_t lastUpdateNs = 0, settleNs = 350000000;
    std::vector<pw3::PanelUpdate> updates;
    // Set when the panel stops holding something this process knows -- an update
    // the driver refused, or one it never ended. The design's repair was "the
    // next frame repaints everything", and a frame may never come: an
    // application that has finished drawing draws nothing more, and the reader
    // is then left looking at the picture before the one that was composed. So
    // the repaint is owed by this process, and the settle thread does it.
    //
    // One attempt per failure. If the repaint's own update fails as well, a
    // second would flash the screen again every settle period for as long as
    // the driver stays broken, which is worse than a stale screen; the budget
    // comes back as soon as an update ends normally.
    bool recover = false;
    unsigned recoveriesLeft = 1;
    // Between the sleep picture and the first frame after waking the panel is
    // losing or has lost power, and an update must not be sent at all -- the
    // picture is meant to stay, and the screen is repainted on the way back up.
    bool asleep = false;
    uint64_t presents = 0, sent[4] = {}, settles = 0, failures = 0, prepareUs = 0, sendUs = 0, changedPixels = 0,
             replacedAreas = 0, recoveries = 0;
    std::atomic<unsigned> reportedErrors{0};
    bool reportedFirst = false;
    ~Device() {
        if (settleStarted) {
            pthread_mutex_lock(&lock); stopping = true; pthread_cond_broadcast(&wake); pthread_mutex_unlock(&lock);
            pthread_join(settleThread, nullptr);
        }
        if (wakeReady) pthread_cond_destroy(&wake);
        pthread_mutex_destroy(&lock);
        if (pixels) munmap(pixels, mapped);
        if (fd >= 0) close(fd);
    }
};
struct Hold {
    pthread_mutex_t* m;
    explicit Hold(pthread_mutex_t* mutex) : m(mutex) { pthread_mutex_lock(m); }
    ~Hold() { pthread_mutex_unlock(m); }
};

int64_t nanoseconds() {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return int64_t(t.tv_sec) * 1000000000 + t.tv_nsec;
}
Device* device(framebuffer_device_t* api) { return reinterpret_cast<Device*>(api); }
uint8_t* visiblePixels(Device* d) { return d->pixels + size_t(d->screen.yoffset) * d->fixed.line_length + d->screen.xoffset; }

int postFailure(Device* d, const char* stage, int error) {
    // The first eight, so that a driver failing on every frame cannot fill
    // the log. There is no switch on this any more: it used to need
    // androidboot.pw3.diag, which went with the test entry on 2026-10-29,
    // and a release that cannot say an update failed says nothing at all.
    if (d->reportedErrors.fetch_add(1, std::memory_order_relaxed) < 8)
        ALOGE("EPDC stage=%s error=%d", stage, error);
    return error;
}
// debug.pw3.epdc.* can be changed while the system runs.
void readPolicy(Device* d) {
    pw3::PanelPolicy policy;
    policy.fast = property_get_bool("debug.pw3.epdc.fast", true);
    policy.a2 = property_get_bool("debug.pw3.epdc.a2", policy.a2);
    policy.dither = property_get_bool("debug.pw3.epdc.dither", true);
    const int32_t direct = property_get_int32("debug.pw3.epdc.direct_percent", int32_t(policy.directPercent));
    policy.directPercent = direct < 0 ? 0 : direct > 101 ? 101 : unsigned(direct);
    const int32_t page = property_get_int32("debug.pw3.epdc.page_percent", int32_t(policy.pagePercent));
    policy.pagePercent = page < 1 ? 1 : page > 100 ? 100 : unsigned(page);
    const int32_t move = property_get_int32("debug.pw3.epdc.move_percent", int32_t(policy.movePercent));
    policy.movePercent = move < 0 ? 0 : move > 101 ? 101 : unsigned(move);
    policy.clean = property_get_bool("debug.pw3.epdc.clean", policy.clean);
    // The panel's white over its black; below 2: luma spread evenly over the levels.
    const int32_t contrast = property_get_int32("debug.pw3.epdc.contrast", int32_t(policy.contrast));
    policy.contrast = contrast < 0 ? 0 : contrast > 1000 ? 1000 : unsigned(contrast);
    // The 16-level waveform a page is shown with. GL16 unless something on
    // the device asks for GC16 to look at the difference; REAGL went on
    // 2026-10-29, because it needs the stock driver's proprietary module and
    // an open-source release has no way to carry one.
    char exact[PROPERTY_VALUE_MAX];
    property_get("debug.pw3.epdc.exact", exact, "gl16");
    policy.exact = !strcmp(exact, "gc16") ? pw3::Waveform::Gc16 : pw3::Waveform::Gl16;
    // How many updates a part of the screen takes before it is repainted with
    // GC16 (Panel.h). With GC16 as the page waveform every page already
    // repaints, so the count is none. The property is
    // what the number is tried with on the device: every value is a trade
    // between what the screen holds of earlier pictures and how often part of
    // it flashes, and only looking at it settles that.
    const int32_t every = property_get_int32("debug.pw3.epdc.full_every", policy.exact == pw3::Waveform::Gl16 ? int32_t(policy.fullEvery) : 0);
    policy.fullEvery = every > 0 ? unsigned(every) : 0;
    d->panel->setPolicy(policy);
    const int32_t ms = property_get_int32("debug.pw3.epdc.settle_ms", 350);
    d->settleNs = int64_t(ms < 50 ? 50 : ms > 5000 ? 5000 : ms) * 1000000;
}
size_t counter(pw3::Waveform w) {
    switch (w) {
        case pw3::Waveform::A2: return 0;
        case pw3::Waveform::Du: return 1;
        case pw3::Waveform::Gl16: return 2;
        default: return 3;
    }
}
// Queues one update; the driver's queue-and-merge path orders overlapping
// updates itself, so nothing here waits for the waveform. The two waveforms
// the driver may choose between when it sends again what collided with a
// running update are both the one asked for.
int send(Device* d, const pw3::PanelUpdate& plan) {
    pw3::EpdcUpdate update{};
    update.region = {uint32_t(plan.rect.top), uint32_t(plan.rect.left),
                     uint32_t(plan.rect.right - plan.rect.left), uint32_t(plan.rect.bottom - plan.rect.top)};
    update.waveform = update.bwWaveform = update.grayWaveform = uint32_t(plan.waveform);
    update.mode = plan.full ? 1 : 0;
    update.temperature = pw3::TempUseAuto;
    // The marker counts updates the driver took. Were it moved before the call,
    // a send that fails would leave finished != marker for an update the driver
    // never saw: the settle thread would wait for a marker that cannot end, and
    // a caller of waitIdle would wait for the settle thread.
    uint32_t marker = d->marker + 1;
    if (marker == 0) marker = 1;
    update.marker = marker;
    int rc = -1;
    for (unsigned attempt = 0; attempt < 3 && rc < 0; ++attempt) {
        if (attempt) usleep(30000);
        rc = ioctl(d->fd, pw3::SendUpdate, &update);
    }
    if (rc < 0) return -errno;
    d->marker = marker;
    ++d->sent[counter(plan.waveform)];
    d->changedPixels += uint64_t(pw3::area(plan.rect));
    return 0;
}
// Caller holds the lock.
int presentLocked(Device* d, const pw3::Rect* damage, size_t count) {
    if ((d->presents++ & 15) == 0) readPolicy(d);
    // A frame is being drawn, so the panel has power: whatever the sleep path
    // left behind is over, and a repaint owed from before this frame is this
    // frame's own business -- Panel::present() repaints everything while its
    // content is unknown.
    d->asleep = false;
    d->recover = false;
    const int64_t began = nanoseconds();
    d->updates.clear();
    d->panel->present(damage, count, visiblePixels(d), d->fixed.line_length, &d->updates, d->finished == d->marker);
    const int64_t prepared = nanoseconds();
    int result = 0;
    for (const auto& update : d->updates) {
        const int rc = send(d, update);
        if (rc && !result) result = postFailure(d, "send-update", rc);
    }
    if (result) {
        // What the panel shows is unknown now: repaint everything, and do not
        // leave that to a frame that may never arrive.
        ++d->failures;
        d->panel->invalidate();
        d->recover = true;
    }
    if (!d->updates.empty()) d->lastUpdateNs = nanoseconds();
    // Woken whenever this frame left work behind, which is not the same thing as
    // having sent something. A first pass can owe a second one while sending
    // nothing at all: a row whose pixels are already on the side of the dither
    // threshold they belong on changes nothing now and still has to reach its
    // grey level later (Panel::firstPassRow). With the default policy that
    // cannot happen on an idle panel -- directPercent 0 sends an idle change
    // exactly, in one pass -- but debug.pw3.epdc.direct_percent can be set, and
    // then the only thread that would finish the frame is asleep on this
    // condition with no timer to bring it back.
    if (!d->updates.empty() || d->panel->pending() || d->recover) pthread_cond_broadcast(&d->wake);
    d->prepareUs += uint64_t((prepared - began) / 1000);
    d->sendUs += uint64_t((nanoseconds() - prepared) / 1000);
    if (!result && !d->reportedFirst) {
        d->reportedFirst = true;
        ALOGI("EPDC first update queued: size=%ux%u", d->screen.xres, d->screen.yres);
    }
    return result;
}
// For the settle thread alone: the driver frees an update's record under the
// first thread whose wait for it ends, while a second one would still sleep on it.
// False when the wait itself failed: the driver answers -ETIMEDOUT after five
// seconds, and then what is on the panel is not what the panel object thinks.
bool waitFor(Device* d, uint32_t marker) {
    pw3::Marker m{marker, 0};
    int rc;
    do { rc = ioctl(d->fd, pw3::WaitUpdate, &m); } while (rc < 0 && errno == EINTR);
    return rc >= 0;
}
// Caller holds the lock.
void settleLocked(Device* d) {
    pw3::PanelUpdate update{};
    if (!d->panel->settle(visiblePixels(d), d->fixed.line_length, &update)) return;
    ++d->settles;
    if (send(d, update)) { ++d->failures; d->panel->invalidate(); d->recover = !d->asleep; }
    pthread_cond_broadcast(&d->wake);
}
// The panel holds something this process does not know, and the repaint is this
// process's to make rather than the next frame's -- there may be no next frame.
// One GC16 of the whole screen, from the picture the last frame asked for
// (Panel::repaint, which is also where it says why not from the canvas).
//
// Caller holds the lock.
void recoverLocked(Device* d) {
    pw3::PanelUpdate update{};

    d->recover = false;
    // A frame that arrived in the meantime has repainted it already, and a
    // panel that is losing power keeps the sleep picture until it wakes.
    if (!d->panel->unknown() || d->asleep || !d->recoveriesLeft) return;
    --d->recoveriesLeft;
    ++d->recoveries;
    ALOGE("EPDC: the panel held an unknown picture, repainting it (%llu)",
          (unsigned long long)d->recoveries);
    d->panel->repaint(visiblePixels(d), d->fixed.line_length, &update);
    // A repaint that cannot be sent leaves the picture unknown again, and the
    // budget is spent: what repairs it then is the next frame, as before. One
    // flash per settle period for as long as a driver stays broken is worse
    // than a stale screen, and it would take the log with it.
    if (send(d, update)) {
        ++d->failures;
        d->panel->invalidate();
    }
    d->lastUpdateNs = nanoseconds();
    pthread_cond_broadcast(&d->wake);
}
void* settleLoop(void* pointer) {
    auto* d = static_cast<Device*>(pointer);
    pthread_mutex_lock(&d->lock);
    while (!d->stopping) {
        if (d->finished != d->marker) {
            // Follow the panel, update after update: a short one sent later
            // ends before a long one sent earlier. Waited for unlocked; an
            // update that has ended already returns at once.
            uint32_t next = d->finished + 1;
            if (!next) ++next;
            pthread_mutex_unlock(&d->lock);
            const bool ended = waitFor(d, next);
            pthread_mutex_lock(&d->lock);
            // The update is counted as over either way, so that one the driver
            // never ends does not stop the thread that follows the panel. What
            // it leaves on the screen is unknown, though, and a later partial
            // update would be worked out against the wrong picture.
            if (!ended) {
                ++d->failures;
                d->panel->invalidate();
                d->recover = !d->asleep;
            } else {
                // An update ended normally, so the driver is answering: a later
                // failure gets a repaint of its own.
                d->recoveriesLeft = 1;
            }
            d->finished = next;
            if (d->finished == d->marker) pthread_cond_broadcast(&d->wake);  // opsSleep may be waiting
            continue;
        }
        if (!d->panel->pending() && !d->recover) { pthread_cond_wait(&d->wake, &d->lock); continue; }
        const int64_t due = d->lastUpdateNs + d->settleNs;
        if (nanoseconds() < due) {
            const timespec until{time_t(due / 1000000000), long(due % 1000000000)};
            pthread_cond_timedwait(&d->wake, &d->lock, &until);
            continue;
        }
        // The panel is idle: grey levels must not reach the framebuffer while
        // a black/white update is still queued for the same pixels, the driver
        // reads the pixels when it starts an update. The same wait serves the
        // repaint, which gives a frame that is on its way the chance to do it
        // instead, in its own waveform and without a second flash.
        if (d->recover) recoverLocked(d); else settleLocked(d);
    }
    pthread_mutex_unlock(&d->lock);
    return nullptr;
}

uint8_t* opsCanvas(framebuffer_device_t* api, int* width, int* height) {
    Device* d = device(api);
    *width = d->panel->width(); *height = d->panel->height();
    return d->panel->canvas();
}
bool opsNeedsFull(framebuffer_device_t* api) {
    Device* d = device(api);
    Hold hold(&d->lock);
    return d->panel->needsFull();
}
int opsPresent(framebuffer_device_t* api, const pw3::Rect* damage, size_t count) {
    Device* d = device(api);
    Hold hold(&d->lock);
    return presentLocked(d, damage, count);
}
// Counted as well as acted on: how often a window changes is the cost of this
// rule, and it is the number to look at if the screen turns out to clear more
// often than the reader wants (dumpsys SurfaceFlinger).
void opsReplaced(framebuffer_device_t* api, const pw3::Rect* areas, size_t count) {
    Device* d = device(api);
    Hold hold(&d->lock);
    for (size_t i = 0; i < count; ++i) {
        d->panel->replaced(areas[i]);
        d->replacedAreas++;
    }
}
// Caller holds the lock.
void waitIdle(Device* d) {
    while (d->finished != d->marker && !d->stopping) pthread_cond_wait(&d->wake, &d->lock);
}
bool chineseSystem() {
    char locale[PROPERTY_VALUE_MAX];
    if (property_get("persist.sys.locale", locale, "") <= 0) property_get("ro.product.locale", locale, "");
    return !strncmp(locale, "zh", 2);
}
// The folder of sleep wallpapers (README.md, sleep/): on the store the user
// sees, the same FAT the documents bind comes from (preinit, /kindle-us), so
// pictures are added the way the installer's files are added -- over USB, from
// a PC. The second folder is the copy that ships in the image (the three
// default pictures, sleep/wallpapers): it shows when the user's folder is
// empty or unreadable, and it is also what a start without the policy and
// kernel change for the store has (sepolicy/pw3_sleepwall.te).
constexpr const char* kSleepWallpaperDir = "/kindle-us/sleepwall";
// The picture shown last, by name so that it is looked up again after the
// folder changed, and not shown twice in a row while there is a choice. Kept for
// the life of the process: a reboot starts over, and the first sleep after it
// may repeat the last picture of the start before.
std::string lastWallpaper;

// The end of both sleep pictures: everything, with GC16 (GL16 would leave the
// page faintly inside it), then the waveform is waited for so that the caller
// can power the panel down without cutting an update in half (opsSleep).
bool sleepUpdateLocked(Device* d, int w, int h) {
    const int rc = send(d, {pw3::Rect{0, 0, w, h}, pw3::Waveform::Gc16, true});
    if (rc) { ++d->failures; d->panel->invalidate(); ALOGE("Sleep picture update failed: %d", rc); return false; }
    d->panel->assumeShown(visiblePixels(d), d->fixed.line_length);
    d->lastUpdateNs = nanoseconds();
    pthread_cond_broadcast(&d->wake);
    waitIdle(d);
    return true;
}

// A wallpaper over the frame the panel was showing, instead of the picture that
// ships in the image (SleepWallpaper.h). True: the panel holds it. False: nothing
// was drawn and the caller shows the picture instead -- the folders have no
// picture, none of their pictures can be read, or the panel's contents are
// unknown, and then there is no frame to show through and no wallpaper to draw.
//
// The user's folder is tried first and the pictures that ship in the image
// second: an empty folder, an unreadable one, or one whose pictures all fail to
// decode falls through to the next, and the picture that ships in the image is
// the last resort. Every sleep says which of the three it showed: "the default
// pictures are on the screen" and "the folder was empty" look the same to a
// person and are not the same thing to whoever is checking a build.
bool wallpaperSleepLocked(Device* d, int w, int h) {
    if (!property_get_bool("debug.pw3.sleepwall.enabled", true)) return false;
    if (d->panel->needsFull()) return false;
    const std::string folders[] = {kSleepWallpaperDir};
    const size_t folderCount = sizeof(folders) / sizeof(folders[0]);
    // Why a folder showed nothing -- and why a picture in one could not be read
    // -- is said once per process: this runs at every sleep, and a folder that
    // is not there is a setting, not a fault.
    static std::string complaints[1];
    static std::vector<std::string> unreadable;
    for (size_t f = 0; f < folderCount; ++f) {
        const std::string& folder = folders[f];
        if (folder.empty()) continue;
        int error = 0;
        const std::vector<std::string> names = pw3::wallpapersIn(folder.c_str(), &error);
        if (names.empty()) {
            const std::string why = error ? std::string(strerror(error)) : std::string("no .png");
            if (complaints[f] != why) {
                complaints[f] = why;
                ALOGW("sleep wallpaper: %s: %s", folder.c_str(), why.c_str());
            }
            continue;
        }
        // The one shown last, by name: the folder may have gained or lost pictures.
        int previous = -1;
        for (size_t i = 0; i < names.size(); ++i) {
            if (names[i] == lastWallpaper) { previous = int(i); break; }
        }
        const int index = pw3::pickWallpaper(int(names.size()), previous, pw3::wallpaperRandom());
        // A picture that cannot be read -- half copied from a PC, or a format this
        // build carries no decoder for -- must not lose the sleep picture: the
        // next one in the folder is tried, then the next folder.
        pw3::Wallpaper wallpaper;
        int chosen = -1;
        for (size_t attempt = 0; attempt < names.size(); ++attempt) {
            const int candidate = (index + int(attempt)) % int(names.size());
            const std::string path = folder + "/" + names[size_t(candidate)];
            if (pw3::loadWallpaper(path.c_str(), w, h, &wallpaper)) { chosen = candidate; break; }
            if (std::find(unreadable.begin(), unreadable.end(), path) == unreadable.end() && unreadable.size() < 16) {
                unreadable.push_back(path);
                ALOGW("sleep wallpaper: %s is not a picture this build can read", path.c_str());
            }
        }
        if (chosen < 0) {
            if (complaints[f] != "no readable picture") {
                complaints[f] = "no readable picture";
                ALOGW("sleep wallpaper: nothing in %s could be read", folder.c_str());
            }
            continue;
        }
        // Only now is the framebuffer written: the driver reads it when an update
        // starts, and nothing may be queued that has not ended.
        waitIdle(d);
        try {
            pw3::WallpaperPainter painter(w, h, d->panel->policy().contrast);
            painter.setWallpaper(&wallpaper);
            // What the panel shows now, in luma for the blend and in level bytes for
            // the pixels the picture leaves alone. Neither changes while this runs:
            // the composer's thread writes the canvas under this same lock.
            const uint8_t* const canvas = d->panel->canvas();
            const uint8_t* const shown = d->panel->shown();
            uint8_t* const fb = visiblePixels(d);
            for (int y = 0; y < h; ++y) {
                painter.row(y, canvas + size_t(y) * size_t(w), shown + size_t(y) * size_t(w),
                            fb + size_t(y) * d->fixed.line_length);
            }
        } catch (const std::exception&) {
            // Nothing that can throw runs after the first row (the painter's own
            // construction does, before it); the picture path repaints every row.
            ALOGE("Sleep wallpaper could not be painted");
            return false;
        }
        if (!sleepUpdateLocked(d, w, h)) return false;
        lastWallpaper = names[size_t(chosen)];
        // Said every sleep: this is the line a test looks for to know which
        // picture was chosen and whether it came from the user or the image.
        ALOGI("Sleep wallpaper: %s/%s", folder.c_str(), names[size_t(chosen)].c_str());
        return true;
    }
    return false;
}

// Before the panel is powered down: the sleep picture, all of the screen with
// GC16 (GL16 would leave the page faintly inside it), then the waveform ends.
// An e-ink panel keeps what it shows: without it, the page would stay on the
// screen. The picture is read and dithered row by row straight into the
// framebuffer (about 10 KB of memory, not the 1.5 MB of a whole picture). The
// panel state then holds the picture, so the first frame after waking changes
// only what differs from it. Without a picture, only the grey levels of what
// is still in its first pass are shown.
void pictureSleepLocked(Device* d, bool chinese, int w, int h) {
    FILE* picture = pw3::openPicture(chinese ? "/system/usr/share/pw3/sleep-zh.pgm" : "/system/usr/share/pw3/sleep-en.pgm",
                                     w, h);
    if (!picture) {
        ALOGE("Sleep picture could not be opened");
        settleLocked(d);
        waitIdle(d);
        return;
    }
    // The driver reads the framebuffer when an update starts: nothing may still be queued.
    waitIdle(d);
    bool complete = false;
    try {
        pw3::Dither dither(w, d->panel->policy().contrast);
        std::vector<uint8_t> row(static_cast<size_t>(w));
        complete = true;
        for (int y = 0; y < h && complete; ++y) {
            complete = fread(row.data(), 1, row.size(), picture) == row.size();
            if (!complete) break;
            dither.row(row.data(), visiblePixels(d) + size_t(y) * d->fixed.line_length);
        }
    } catch (const std::exception&) {
        complete = false;
    }
    fclose(picture);
    if (!complete) {
        // Part of a picture may be in the framebuffer, not on the panel: repaint all of it next time.
        ALOGE("Sleep picture could not be read");
        d->panel->invalidate();
        return;
    }
    sleepUpdateLocked(d, w, h);
}

// Before the panel is powered down. An e-ink panel keeps what it shows, so
// without a picture here the page would stay on the screen; a picture drawn on
// the way out also means the first frame after waking changes only what differs
// from it. A wallpaper from the user's folder is preferred, with the frame
// before it showing through its transparent pixels; the picture that ships in
// the image is the fallback, and the grey levels of what is still in its first
// pass are the fallback below that (Panel::settle).
void opsSleep(framebuffer_device_t* api) {
    Device* d = device(api);
    Hold hold(&d->lock);
    // From here until the first frame after waking, nothing else may update the
    // panel: the caller powers it down as soon as this returns, and a picture
    // whose update is cut off by that is the one thing a repaint cannot help --
    // the screen is repainted on the way back up, by SurfaceFlinger's own
    // complete frame.
    d->asleep = true;
    d->recover = false;
    const bool chinese = chineseSystem();
    const int w = d->panel->width(), h = d->panel->height();
    if (wallpaperSleepLocked(d, w, h)) return;
    // Said every sleep as well: the line a test looks for when it wants to know
    // that the wallpaper was tried and which of the three sources showed.
    ALOGI("Sleep wallpaper: none; the sleep picture that ships in the image is shown");
    pictureSleepLocked(d, chinese, w, h);
}

void opsDescribe(framebuffer_device_t* api, char* text, int size) {
    Device* d = device(api);
    Hold hold(&d->lock);
    // recoveries is here because the fault it answers is one a reader reports
    // and nobody can see: a page that was composed and never shown. Its count
    // is the difference between "the panel was repainted because an update
    // failed" and "something else is wrong", and this line is the only place
    // either can be read from a release (open-items, 2026-10-28).
    snprintf(text, size_t(size), "PW3 panel: presents=%llu a2=%llu du=%llu gl16=%llu gc16=%llu settles=%llu failures=%llu"
             " recoveries=%llu prepare_us_total=%llu send_us_total=%llu update_pixels_total=%llu replaced_areas=%llu"
             " pending=%d idle=%d unknown=%d\n",
             (unsigned long long)d->presents, (unsigned long long)d->sent[0], (unsigned long long)d->sent[1],
             (unsigned long long)d->sent[2], (unsigned long long)d->sent[3],
             (unsigned long long)d->settles,
             (unsigned long long)d->failures, (unsigned long long)d->recoveries,
             (unsigned long long)d->prepareUs, (unsigned long long)d->sendUs,
             (unsigned long long)d->changedPixels, (unsigned long long)d->replacedAreas,
             d->panel->pending() ? 1 : 0, d->finished == d->marker ? 1 : 0, d->panel->unknown() ? 1 : 0);
}
const Pw3PanelOps PanelOps = {Pw3PanelOpsMagic, 3, opsCanvas, opsNeedsFull, opsPresent, opsSleep, opsDescribe,
                              opsReplaced};

// A frame SurfaceFlinger composed itself (GLES fallback): convert all of it.
int post(framebuffer_device_t* api, buffer_handle_t buffer) {
    Device* d = device(api);
    if (private_handle_t::validate(buffer)) return postFailure(d, "handle", -EINVAL);
    const auto* h = static_cast<const private_handle_t*>(buffer);
    if ((h->format != HAL_PIXEL_FORMAT_RGBX_8888 && h->format != HAL_PIXEL_FORMAT_RGBA_8888) ||
        h->width != int(d->screen.xres) || h->height != int(d->screen.yres) || h->size <= 0 ||
        h->stride < h->width || size_t(h->size) < size_t(h->stride) * size_t(h->height) * 4)
        return postFailure(d, "buffer-shape", -EINVAL);
    Hold hold(&d->lock);
    auto* module = reinterpret_cast<const gralloc_module_t*>(api->common.module);
    void* source = nullptr;
    const int rc = module->lock(module, buffer, GRALLOC_USAGE_SW_READ_OFTEN, 0, 0, h->width, h->height, &source);
    if (rc) return postFailure(d, "source-lock", rc);
    for (int y = 0; y < h->height; ++y)
        pw3::lumaRow(static_cast<const uint8_t*>(source) + size_t(y) * h->stride * 4,
                     d->panel->canvas() + size_t(y) * h->width, h->width, false);
    module->unlock(module, buffer);
    const pw3::Rect all{0, 0, h->width, h->height};
    return presentLocked(d, &all, 1);
}
int closeDevice(hw_device_t* api) { delete reinterpret_cast<Device*>(api); return 0; }
int setSwap(framebuffer_device_t*, int value) { return value == 1 ? 0 : -EINVAL; }
}

int fb_device_open(const hw_module_t* module, const char* name, hw_device_t** output) {
    if (!output || strcmp(name, GRALLOC_HARDWARE_FB0)) return -EINVAL;
    std::unique_ptr<Device> d(new (std::nothrow) Device);
    if (!d) return -ENOMEM;
    const auto failure = [](const char* stage, int error) {
        ALOGE("EPDC open stage=%s error=%d", stage, error);
        return error;
    };
    d->fd = open("/dev/graphics/fb0", O_RDWR | O_CLOEXEC);
    if (d->fd < 0) return failure("device", -errno);
    if (ioctl(d->fd, FBIOGET_VSCREENINFO, &d->screen)) return failure("variable-info", -errno);
    if (ioctl(d->fd, FBIOGET_FSCREENINFO, &d->fixed)) return failure("fixed-info", -errno);
    if (strncmp(d->fixed.id, "mxc_epdc_fb", sizeof(d->fixed.id)) || d->screen.bits_per_pixel != 8 ||
        d->screen.grayscale != 1 || d->screen.xres == 0 || d->screen.yres == 0 ||
        d->screen.xres > 4096 || d->screen.yres > 4096 ||
        uint64_t(d->screen.xoffset) + d->screen.xres > d->fixed.line_length ||
        (uint64_t(d->screen.yoffset) + d->screen.yres) * d->fixed.line_length > d->fixed.smem_len)
        return failure("geometry-contract", -ENOTSUP);
    // fb0 is registered before waveform loading finishes; presence alone is insufficient.
    int infoRc;
    do { infoRc = ioctl(d->fd, pw3::GetAndroidInfo, &d->epdc); } while (infoRc < 0 && errno == EINTR);
    if (infoRc < 0) return failure("startup-query", -errno);
    if (!pw3::validEpdcContract(d->epdc, d->screen)) return failure("startup-contract", -ENOTSUP);
    d->mapped = d->fixed.smem_len;
    void* mapped = mmap(nullptr, d->mapped, PROT_READ|PROT_WRITE, MAP_SHARED, d->fd, 0);
    if (mapped == MAP_FAILED) return failure("mmap", -errno);
    d->pixels = static_cast<uint8_t*>(mapped);
    // Explicit region updates on the driver's queue path; no continuous display
    // writes. The driver reads the mapped framebuffer when it processes a
    // queued update, so an update may show pixels of a later frame; that
    // frame's own update covers them again, and unchanged pixels are skipped.
    uint32_t autoUpdate = pw3::AutoUpdateRegion;
    uint32_t scheme = pw3::UpdateSchemeQueueAndMerge;
    if (ioctl(d->fd, pw3::SetAutoUpdate, &autoUpdate)) return failure("auto-update", -errno);
    if (ioctl(d->fd, pw3::SetUpdateScheme, &scheme)) return failure("update-scheme", -errno);
    try { d->panel.reset(new pw3::Panel(int(d->screen.xres), int(d->screen.yres))); }
    catch (const std::exception&) { return failure("panel-state", -ENOMEM); }
    readPolicy(d.get());
    pthread_condattr_t options;
    if (pthread_condattr_init(&options)) return failure("condition", -ENOMEM);
    int rc = pthread_condattr_setclock(&options, CLOCK_MONOTONIC);
    if (!rc) { rc = pthread_cond_init(&d->wake, &options); d->wakeReady = !rc; }
    pthread_condattr_destroy(&options);
    if (!rc) { rc = pthread_create(&d->settleThread, nullptr, settleLoop, d.get()); d->settleStarted = !rc; }
    if (rc) return failure("settle-thread", -rc);
    d->api.common.tag = HARDWARE_DEVICE_TAG;
    d->api.common.version = 0;
    d->api.common.module = const_cast<hw_module_t*>(module);
    d->api.common.close = closeDevice;
    d->api.setSwapInterval = setSwap;
    d->api.post = post;
    const_cast<uint32_t&>(d->api.width) = d->screen.xres;
    const_cast<uint32_t&>(d->api.height) = d->screen.yres;
    const_cast<int&>(d->api.stride) = d->screen.xres;
    const_cast<int&>(d->api.format) = HAL_PIXEL_FORMAT_RGBX_8888;
    const_cast<float&>(d->api.xdpi) = 300;
    const_cast<float&>(d->api.ydpi) = 300;
    // Framework scheduling rate, not the physical e-paper refresh rate.
    const_cast<float&>(d->api.fps) = 30;
    const_cast<int&>(d->api.minSwapInterval) = 1;
    const_cast<int&>(d->api.maxSwapInterval) = 1;
    d->api.reserved_proc[0] = const_cast<Pw3PanelOps*>(&PanelOps);
    ALOGI("EPDC framebuffer open ready: size=%ux%u stride=%u bpp=%u rotate=%u bytes=%zu settle_ms=%lld",
          d->screen.xres, d->screen.yres, d->fixed.line_length,
          d->screen.bits_per_pixel, d->screen.rotate, d->mapped, (long long)(d->settleNs / 1000000));
    *output = &d.release()->api.common;
    return 0;
}
