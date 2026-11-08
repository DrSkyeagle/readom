# Changelog

What changed in each release, and **how far it was verified on the device**. Everything here was run
on that one Kindle Paperwhite 3; anything that was not says so plainly rather than being glossed
over. This file is the repository's own record — the website's changelog and the GitHub Release
notes are written from it.

## Unreleased

**The Voyage (7th generation) device side is in the sources, and not one line of it has run on a
Voyage.** `device/amazon/voyage/` is new: it inherits the pw3 product and board configuration and
replaces only the product identity (`PRODUCT_NAME`/`PRODUCT_DEVICE`/`PRODUCT_BRAND`/`PRODUCT_MODEL`)
and the board name, because the two machines are the same part in every place this system touches.
The helper's model check no longer names the pw3 struct but takes it from the boot profile
(`-DDD_PROFILE`); the build scripts gained `--model` / `--profile` / `PW3_AOSP_PRODUCT`, and without
them everything falls back to pw3 with the build command unchanged. The firmware facts in
`boot/profiles/voyage-5.13.6.json` — the stock command line, the kernel's build string, the four
function bodies' hashes, the Wi-Fi calibration and firmware paths — **are still TODO**: they are read
off that machine, and one wrong token stops every Voyage at E101. The installer still knows only the
PW3. The reasoning, the files touched and what is outstanding are in
[voyage-port.en.md](docs/design/voyage-port.en.md).

## v0.1.0

**Status: released on 2026-11-04, and a full release since 11-05 (it was marked a pre-release until then).** What is online (the GitHub Release `v0.1.0`, `readom.app/dl/v0.1.0/`,
and the website) is the entry `Readom.sh` of build zb, 2026-11-05, 7,828,380 bytes, `20a7f7e8…`, and the
system package `readom-system-v0.1.0.img.zst` of build l, 2026-11-04, 190,831,781 bytes, `497fc919…`;
both places were read back whole and checked. The source tree is the one it was built from, and the
`v0.1.0` tag points at it. What follows reads backwards in time: how this version got here.

**Three more replacements, the evening of 11-04 and 11-05 (the h, l and p entries are in the release
record's `superseded`).** Build l fixes three things found on h that a reader can see: the screen went black for a
moment before the home screen at every start (85 seconds during an upgrade), because the display composer
put frames with no layers on the panel; with USB debugging on, the computer saw no drive after a start,
because the switch to MTP waited one second and lost it to the start-up jobs; and with the screen on,
Wi‑Fi sometimes would not turn on, because the screen-off timer's alarm arrived in the instant the screen
came on and tore the state machine apart. All three were checked on the device. Build p changes only the
entry: the screen an install stops on used to stay about a second before the library covered it (the
jailbreak's launcher that runs library scripts repaints the whole screen when a script exits); it now
stays until you tap the screen, ten minutes at most, and every error screen ends "Tap the screen to go
back to the library". The E202 and E303 screens were checked on the device. Build zb again changes only
the entry: an install does not respond to touch from its first screen on (with a finger on the screen
at that moment, it takes over at the next screen it draws after the finger lifts; once, a swipe down
from the top at 0% to look at Wi‑Fi opened the stock menu over the progress and left a white screen when
it closed), and an install progress screen that has not changed for 30 seconds is drawn again; whether the Kindle slept is now read
from `/proc/uptime` -- the stock kernel's boot clock has an overflow that, once the Kindle has slept,
reads 4.29 seconds short every so often, so p now and then let an error screen go as if the Kindle had
slept; and "a tap" is told by the order of the touch events, not by the time on them, so the Kindle
setting its clock does not affect it. E202 and E303 staying, a swipe down opening no menu during an
install or on an error screen, and a clean install into Android were all checked on the device.

**Three things closed on 2026-11-04.** One, **the patch chain 0067-0073 is settled**: the "killed" branch of
the 11-03 candidate was staged on userdebug and was broken (a frozen application force-stopped left the
screen stuck on the wallpaper and the two bars); patch 0073 makes the release after the deadline take the
activity manager's word. Then, on the userdebug twin of the release build, every state that could be staged
was run -- Home, opening Settings, Back, force-stop, force-stop while frozen, sleep and wake, a cold start
with the starting window off, a link cold start frozen past 45 s -- and each one let go correctly. One
thing is wrong and is not this build's doing; it is section 16 of `known-issues`. Two, **a clean install
after a full reset, downloading everything** (release build f): fetched from the network, switched to the
second origin when the first was made unreachable and finished 190,824,793 bytes from it (the first change
of origin on the device), a fresh `data.img`, first initialisation, `verify-device.sh` 31 answered and 0
failed; 33 after WeRead was installed. Three, **one installer defect**, hit by the owner in that clean
install: waiting on the "Install WeRead" screen until the install finished went back to the home screen
instead of into WeRead. Build h fixes it (when the install succeeds and that screen has focus, WeRead is
started directly); the device was upgraded in place from f to g to h with its data kept, and on h an
uninstall and reinstall confirmed "straight into WeRead when done". h's boot image is byte for byte f's;
the difference is `build.prop` and the installer.

**The builds from 10-31 to 11-03 (all replaced by h, each recorded under `superseded` in the release
manifest).** Built and accepted on the device on 2026-10-31. The entry has been
rebuilt twice since that acceptance: once on the evening of 2026-10-31 (to carry the fix to the
installer's cross-link check — an image whose length is not whole sectors used to be waved through
in full), and once on 2026-11-01 (the download route, below). What was online then was 7,759,418
bytes, `a05afb58…`; the system image had not changed since 10-31. Both origins have been uploaded
to and read back.

**A further candidate was built on 2026-11-03 and installed the same day (it went online on 11-04 with 0073 and the installer fix, above).** Three of its changes are ones a reader sees: **tapping an application now brings up that application's own starting screen at once** (the system draws the window background from its theme, instead of the screen turning into the two system bars over nothing five seconds in -- WeRead's is a white page with its own black emblem); **an application transition waits for the incoming application to draw its first frame**, so a slow start no longer has the previous screen taken away; and `/system` goes from 576 MiB to **512 MiB**, which **hands 64 MiB back to the Kindle's own storage**. `/data` stays at 1.75 GiB and the data format does not move, so **an upgrade keeps the applications, the sign-ins and the books** -- which the upgrade itself confirmed that day: the shelf, the sign-in and the compiled code all survived it. The starting screen was measured reaching the panel **0.7 s** after the icon is tapped, and it stays until the application draws its own first frame, 11-13 s while signed in; the cost is two or three more full-screen clearings per cold start. There is also a net under all this: **even if a transition does run all the way to its deadline, the previous screen is no longer taken away and replaced with nothing** -- ordinary use never reaches it (once a starting screen appears the deadline is never armed), and what it covers is an application that turns its own starting screen off. That was checked on the device the same day with both arms: turned off, the window is gone from the screen four seconds in; turned on, the home screen stays until the application really draws.

**A real book was read on the device for the first time on 2026-11-02**: 28 pages turned in a row,
every one of them answered, giving **GL16 +33 and GC16 +1** on the panel counters -- a page turn is
one non-flashing GL16, with a single clearing pass in 28 pages, the cadence the design asks for.
The page number and the chapter both moved, and nothing crashed or stopped answering. On 11-01 it
was briefly reported as "a flash on every page"; that was a tap in the wrong place, which opened
the reader's menu instead of turning the page, so what was measured was the cost of that menu.

### What it is

- Android 8.1 (AOSP) built for the Kindle Paperwhite 3 (7th generation, 2015, firmware 5.16.2.1.1),
  with an installer that runs on the Kindle itself: copy `Readom.sh` into `documents` and tap it in
  the library. About 15 minutes the first time (measured on 2026-11-01, download and decompression
  included), about 5 for an upgrade that replaces only the system.
- **The Kindle system and your books are left alone.** To go back to the stock system, hold the
  power button in Android and choose "Restart to Kindle" in the menu (the menu is there only once
  Android's interface is up). An install that fails never left the stock system; when a start-up check
  does not pass, or the system services keep crashing, it restarts back by itself. Not every failure to
  start comes back by itself: a hang that does not crash, a crash loop that is slow, zygote dying as
  soon as it starts, or the checker itself hanging all need the power button held until the screen
  changes (up to about 40 seconds), which goes back to the Kindle the same way, books and data kept.
- **No flash on a page turn**: about one clear every twenty pages, picked from 30 / 20 / 12 in an
  A/B in front of the screen (the stock firmware clears every six).
- A built-in browser (EinkBro with the 128 web view) and a pinyin IME, its keyboard redrawn for
  the Kindle.
- WeRead for e-ink installs in one tap: the bundled installer downloads the official build from
  Tencent (over 100 MB) and checks that it really is signed by Tencent before installing it.
- The sleep screen is built in and mostly white, and the wake does not flash; the page you were
  reading is not left on the screen.
- **The boot entry carries no device data**: waveform, Wi-Fi calibration and storage layout are
  produced on the spot at every start. This is what makes open-sourcing possible.

### Three defects fixed on 2026-10-31

- **After a suspend, some reads of `CLOCK_BOOTTIME` came back 2^32 nanoseconds (4.295 s) short.** The
  kernel's `get_monotonic_boottime()` sums three `long` nanosecond fields in 32-bit arithmetic and
  overflows; Android's `elapsedRealtime` reads it, `UsageStatsService` compares the wall clock against it, so a wake reported a time change. **The fault is on the read side and does not accumulate**: each read is either right or short by that one constant, depending on the phase of the three nanosecond fields at that instant. The patch is the 3.4.y backport's edit: the
  first operand is cast to `s64`.
- **The installer did not know whose clusters it was about to overwrite.** The unpack opens
  `system.img` / `data.img` and writes from byte 0, while a FAT cross-link (one cluster belonging
  to another file as well) was only checked by the new kernel after the handover -- the right
  answer, arriving after the book had already been overwritten. The installer now asks before it
  writes, from the filesystem the bytes actually land on, and stops with E404 only on a confirmed
  share.
- **A finished frame occasionally never reached the panel.** Once the panel does not know what is
  on the screen, the only repair was "repaint the whole screen on the next frame", and an
  application that has finished drawing draws nothing more, so the reader was left in front of the
  previous picture. The repaint is now done by this process itself.

### 2026-11-01: the download route became a list, not an address

The failed download of 10-31 was first put down to "the Wi-Fi is slow". It was not: same device,
same router, and on 10-30 the 302 to the GitHub release delivered all 191 MB in one attempt, while
10-31 went straight to R2 and got 515 KB in 26 minutes. The only variable was the origin host.

- **The entry now carries an ordered list of addresses** (four at most), currently the GitHub
  release first and `readom.app/dl/` second. An origin answering "this file is not here" takes only
  itself out of the running; the release is missing only when every origin says so. The part is not
  dropped when the origin changes, because the payload is admitted by its SHA-256 once the whole of
  it has arrived.
- **Under the speed floor counts against the origin too.** The floor curl is given is under 1 KB/s
  for a whole minute. "Any bytes are progress" kept the same origin — which is how the 26 minutes
  of 10-31 went: four attempts, a few hundred KB each, every one of them "more than before". The
  next origin is now asked, and it counts toward the idle stop, so a single origin stops at E201
  after two such attempts instead of six. The part is kept and tapping again continues it.
- The one definition of the list lives in the release description; the upload script checks it
  against the built entry's receipt, item for item and in order, and refuses to upload on a
  mismatch or when the description records no list at all.

### How far it was verified on the device

| Item | How | Result |
|---|---|---|
| Clean install | Images and `state` removed, one tap | The whole chain succeeded first time, no E code; the first boot of an empty `/data` reached `boot_completed` in 92 s |
| Upgrade | The old `state` put back, one more tap | `data.img` kept, the boot image unchanged, no recompile; the cross-link check ran before any image content was written and named the right volume |
| `CLOCK_BOOTTIME` | Cable out, the magnetic cover closed and opened 3 times | The wall clock and `CLOCK_BOOTTIME` advanced within 1 ms of each other, `Time changed` 0 times; the suspends were real (the CPU was stopped for 325.6 s) |
| A real download | 2026-11-01: images removed, one tap, left to fetch it itself | **Finished in one attempt**: the first origin (the GitHub release), `curl 0`, HTTP 206, 188,326,526 bytes appended to the 2,490,368-byte part left by 2026-10-31 — which came from the *other* origin, so **resuming across origins holds**, and the whole of it passed `payload.sha256`. Downloading and unpacking 576 MiB took 5 min 56 s together, the data image 9 min 18 s, the installer about 15 min 14 s in all; no part was left behind |

### What is not verified

- **The dropped-frame recovery has never executed on the hardware**: it needs a real refresh
  failure to trigger, and a release image has no way to inject one. Its first execution will be on
  a reader's device.
- **The FAT cross-link refusal has only been seen against real FAT fixtures in the build VM**
  (17 cases); on the device only the normal path ran.
- ~~**Changing origin has still never run on the device.**~~ **It ran on 2026-11-04**: with the first
  origin made unreachable (the GitHub asset renamed), the stock curl got an empty reply (curl 52) and
  the installer moved to `readom.app`, finished 190,824,793 bytes there and passed `payload.sha256`.
  Two branches are still unexercised and stay listed as such: **"under the speed floor, move on"**
  (it needs a third origin that is slow but not absent) and **a 404 counting as E303** (GitHub gave
  an empty reply, not a 404).
- ~~Signing in to WeRead and reading a book~~ (read on 11-02; signed in and read again on the release
  build on 11-04). **The battery and deep-sleep measurements** are not done -- the owner put them after
  the rest.
- **A freshly installed WeRead is slow on its first day**: just after installation it runs as
  `quicken`, interpreted, and a book not yet downloaded takes about a minute to open; after the device
  has been asleep and idle for about 71 minutes the background job compiles it to `speed` by itself.
  The numbers are in section 12 of `known-issues`.
