// SPDX-License-Identifier: Apache-2.0
// The repaint the EPDC device owes when the panel holds a picture this process
// does not know (framebuffer.cpp, recoverLocked and settleLoop).
//
// Nothing on the device has ever run that path. It is reached only when the
// driver refuses an update or never ends one, and neither can be arranged on a
// release build: there is no root and the panel has no failure to inject. So
// the driver is replaced here -- ioctl() is defined in this translation unit,
// which is what send() and waitFor() call -- and framebuffer.cpp is included
// rather than linked, so that its Device, which is file-local as it should be,
// can be built by hand.
//
// What is checked is what framebuffer.cpp promises: one GC16 of the whole
// screen per failure, nothing while the panel is asleep or already known, one
// attempt rather than a flash per settle period while a driver stays broken,
// and the attempt back as soon as an update ends normally.
//
// Build (in the build VM, for the host; the AOSP tree has the HAL and log
// headers):
//   g++ -std=c++14 -O1 -g -Wall -Wextra -Werror -fexceptions
//       -fsanitize=address,undefined -fno-sanitize-recover=all
//       -Idevice/amazon/pw3/gralloc/tests/include
//       -I/home/builder/android-src/hardware/libhardware/include
//       -I/home/builder/android-src/system/core/include
//       -Idevice/amazon/pw3/sleep/third_party
//       device/amazon/pw3/gralloc/tests/recover_test.cpp
//       device/amazon/pw3/epdc/{Panel,Tone,Luma,Dither,Picture,Png,SleepWallpaper}.cpp
//       -o /tmp/recover_test
// which is what tools/run-tests.py gives it ('gralloc-recover'), run from the
// root of this tree. Nothing but includes: no -D and no -lpthread, so the
// suite needs no flags of its own for it.
// The device's own module defines this on the command line (gralloc/Android.mk);
// here it is, so that the test needs nothing on the command line but includes.
#define LOG_TAG "PW3GrallocTest"
#include "../../epdc/EpdcAbi.h"
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>

static int failures;
#define CHECK(x) do { if (!(x)) { printf("FAILED line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

namespace {
// The driver the device talks to. What was sent is counted here and not read
// out of the device's own counters, so that a counter it forgets to raise
// cannot make this pass.
struct Fake {
    bool takeUpdates = true;   // false: every send is refused
    // The markers whose update never ends, as the driver's -ETIMEDOUT leaves
    // them. Named one at a time rather than switched on and off, so that the
    // thread under test cannot race the switch: which update fails is decided
    // before it is sent.
    uint32_t neverEnds = 0;
    unsigned attempts = 0;     // sends offered, refused ones included
    std::vector<pw3::EpdcUpdate> sent;
    void reset() { takeUpdates = true; neverEnds = 0; attempts = 0; sent.clear(); }
};
Fake fake;
}  // namespace

// Interposed: a definition here is what this executable's own calls resolve to.
extern "C" int ioctl(int, unsigned long request, ...) noexcept {
    va_list args;
    va_start(args, request);
    void* argument = va_arg(args, void*);
    va_end(args);
    if (request == pw3::SendUpdate) {
        ++fake.attempts;
        if (!fake.takeUpdates) { errno = EIO; return -1; }
        fake.sent.push_back(*static_cast<pw3::EpdcUpdate*>(argument));
        return 0;
    }
    if (request == pw3::WaitUpdate) {
        if (static_cast<pw3::Marker*>(argument)->value != fake.neverEnds) return 0;
        errno = ETIMEDOUT;   // not EINTR: waitFor retries only on that one
        return -1;
    }
    return 0;
}
// liblog and libcutils, which this device links against and this test does not.
extern "C" int __android_log_print(int, const char*, const char*, ...) { return 0; }
extern "C" int property_get(const char*, char* value, const char* fallback) {
    const size_t length = fallback ? strlen(fallback) : 0;
    if (length) memcpy(value, fallback, length);
    value[length] = '\0';
    return int(length);
}
// Every property this device reads is a policy with a default, and the default
// is what it runs with on a device nobody has set one on: that is the policy
// this test wants, so each of these answers what the caller asked for.
extern "C" int8_t property_get_bool(const char*, int8_t fallback) { return fallback; }
extern "C" int32_t property_get_int32(const char*, int32_t fallback) { return fallback; }

#include "../framebuffer.cpp"

namespace {
// Small enough to be quick, and the logic does not read the size: a repaint is
// the whole screen whatever that is.
constexpr int kWidth = 64, kHeight = 32;

// A device as fb_device_open leaves one, without a framebuffer or a driver.
struct Fixture {
    Device d;
    explicit Fixture(bool withThread = false) {
        d.fd = open("/dev/null", O_RDWR | O_CLOEXEC);
        d.screen.xres = kWidth;
        d.screen.yres = kHeight;
        d.fixed.line_length = kWidth;
        d.mapped = size_t(kWidth) * kHeight;
        d.pixels = static_cast<uint8_t*>(mmap(nullptr, d.mapped, PROT_READ | PROT_WRITE,
                                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        d.panel.reset(new pw3::Panel(kWidth, kHeight));
        // No settle period: every wait in the loop is already due, so the test
        // does not sleep and does not depend on a clock.
        d.settleNs = 0;
        if (!withThread) return;
        pthread_condattr_t options;
        pthread_condattr_init(&options);
        pthread_condattr_setclock(&options, CLOCK_MONOTONIC);
        d.wakeReady = !pthread_cond_init(&d.wake, &options);
        pthread_condattr_destroy(&options);
        d.settleStarted = !pthread_create(&d.settleThread, nullptr, settleLoop, &d);
    }
};

bool wholeScreenGc16(const pw3::EpdcUpdate& u) {
    return u.region.top == 0 && u.region.left == 0 && u.region.width == uint32_t(kWidth) &&
           u.region.height == uint32_t(kHeight) && u.waveform == uint32_t(pw3::Waveform::Gc16) &&
           u.mode == 1;
}

// The panel holds something unknown, the device is awake, and one attempt is
// owed: one GC16 of the whole screen, and the picture is known again.
void aRepaintPerFailure() {
    fake.reset();
    Fixture f;
    Device& d = f.d;
    d.panel->invalidate();
    d.recover = true;
    Hold held(&d.lock);
    recoverLocked(&d);
    CHECK(fake.sent.size() == 1);
    if (fake.sent.size() == 1) CHECK(wholeScreenGc16(fake.sent[0]));
    CHECK(!d.panel->unknown());
    CHECK(d.recoveries == 1);
    CHECK(!d.recover);
    CHECK(d.recoveriesLeft == 0);
    CHECK(d.failures == 0);
}

// And a second failure before any update has ended does not repaint again: one
// flash per settle period for as long as a driver stays broken is worse than a
// stale screen. The picture is left unknown, so the next frame still repairs it.
void aSecondFailureWaitsForTheNextFrame() {
    fake.reset();
    Fixture f;
    Device& d = f.d;
    Hold held(&d.lock);
    d.panel->invalidate();
    d.recover = true;
    recoverLocked(&d);
    d.panel->invalidate();
    d.recover = true;
    recoverLocked(&d);
    CHECK(fake.sent.size() == 1);
    CHECK(d.recoveries == 1);
    CHECK(d.panel->unknown());
}

// Asleep: the sleep picture is meant to stay until the panel wakes, and an
// update must not be sent at all.
void asleepSendsNothing() {
    fake.reset();
    Fixture f;
    Device& d = f.d;
    Hold held(&d.lock);
    d.panel->invalidate();
    d.asleep = true;
    d.recover = true;
    recoverLocked(&d);
    CHECK(fake.attempts == 0);
    CHECK(d.recoveries == 0);
    CHECK(d.panel->unknown());
    CHECK(d.recoveriesLeft == 1);
}

// A frame that arrived in the meantime has repainted it already.
void aKnownPictureSendsNothing() {
    fake.reset();
    Fixture f;
    Device& d = f.d;
    Hold held(&d.lock);
    // A new Panel has not seen the screen, so it starts out holding something
    // unknown: one repaint makes it known, which is the state under test here.
    pw3::PanelUpdate known{};
    d.panel->repaint(visiblePixels(&d), d.fixed.line_length, &known);
    CHECK(!d.panel->unknown());
    fake.reset();
    d.recover = true;
    recoverLocked(&d);
    CHECK(fake.attempts == 0);
    CHECK(d.recoveries == 0);
    CHECK(!d.recover);
}

// A repaint the driver refuses leaves the picture unknown again and the attempt
// spent: what repairs it then is the next frame, as before.
void arefusedRepaintSpendsTheAttempt() {
    fake.reset();
    fake.takeUpdates = false;
    Fixture f;
    Device& d = f.d;
    Hold held(&d.lock);
    d.panel->invalidate();
    d.recover = true;
    recoverLocked(&d);
    CHECK(fake.sent.empty());
    CHECK(fake.attempts == 3);   // send() offers it three times
    CHECK(d.failures == 1);
    CHECK(d.panel->unknown());
    CHECK(d.recoveriesLeft == 0);
    CHECK(d.recoveries == 1);    // the repaint was made, the driver would not take it
}

// Through the thread that follows the panel, which is the only caller: an
// update that never ends is counted as over, the picture is unknown, and the
// repaint follows. Then an update that ends normally gives the attempt back, so
// the next failure gets a repaint of its own rather than a stale screen.
void theAttemptComesBackWhenAnUpdateEnds() {
    fake.reset();
    Fixture f(true);
    Device& d = f.d;
    CHECK(d.settleStarted);
    if (!d.settleStarted) return;

    // Wait for a change under the lock, with a ceiling: the thread is real.
    auto until = [&d](unsigned recoveries, uint32_t finished) {
        for (int tries = 0; tries < 3000; ++tries) {   // 3 s
            pthread_mutex_lock(&d.lock);
            const bool there = d.recoveries >= recoveries && d.finished >= finished;
            pthread_mutex_unlock(&d.lock);
            if (there) return true;
            usleep(1000);
        }
        return false;
    };

    // Update 1 never ends; the repaint it causes is update 2, which does. The
    // thread then has an update that ended, so the attempt comes back.
    pthread_mutex_lock(&d.lock);
    fake.neverEnds = 1;
    d.marker = 1;
    pthread_cond_broadcast(&d.wake);
    pthread_mutex_unlock(&d.lock);
    CHECK(until(1, 2));
    pthread_mutex_lock(&d.lock);
    CHECK(d.failures == 1);
    CHECK(d.recoveries == 1);
    CHECK(d.recoveriesLeft == 1);   // update 2 ended: the attempt is back
    CHECK(d.marker == 2);

    // So update 3, which never ends either, gets a repaint of its own rather
    // than leaving a stale screen. Without the attempt coming back there would
    // be one repaint in this whole test, not two.
    fake.neverEnds = 3;
    d.marker = 3;
    pthread_cond_broadcast(&d.wake);
    pthread_mutex_unlock(&d.lock);
    CHECK(until(2, 4));
    pthread_mutex_lock(&d.lock);
    CHECK(d.recoveries == 2);
    CHECK(d.failures == 2);
    CHECK(fake.sent.size() == 2);   // two repaints, and nothing else was sent
    if (fake.sent.size() == 2) CHECK(wholeScreenGc16(fake.sent[1]));
    pthread_mutex_unlock(&d.lock);
}
}  // namespace

int main() {
    aRepaintPerFailure();
    aSecondFailureWaitsForTheNextFrame();
    asleepSendsNothing();
    aKnownPictureSendsNothing();
    arefusedRepaintSpendsTheAttempt();
    theAttemptComesBackWhenAnUpdateEnds();
    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("PW3 recover test: every check passed\n");
    return 0;
}
