# Changelog

What changed in each release, and **how far it was verified on the device**. Everything here was run
on that one Kindle Paperwhite 3; anything that was not says so plainly rather than being glossed
over. This file is the repository's own record — the website's changelog and the GitHub Release
notes are written from it.

## v0.1.0

**Status: built and accepted on the device on 2026-10-31, not published yet.** The entry has been
rebuilt twice since that acceptance: once on the evening of 2026-10-31 (to carry the fix to the
installer's cross-link check — an image whose length is not whole sectors used to be waved through
in full), and once on 2026-11-01 (the download route, below). What is online now is 7,759,418
bytes, `a05afb58…`; the system image has not changed since 10-31. Both origins have been uploaded
to and read back, but the source is not public, so this one does not count as released.

**A further candidate was built on 2026-11-03 and installed the same day; it is not online yet.** Three of its changes are ones a reader sees: **tapping an application now brings up that application's own starting screen at once** (the system draws the window background from its theme, instead of the screen turning into the two system bars over nothing five seconds in -- WeRead's is a white page with its own black emblem); **an application transition waits for the incoming application to draw its first frame**, so a slow start no longer has the previous screen taken away; and `/system` goes from 576 MiB to **512 MiB**, which **hands 64 MiB back to the Kindle's own storage**. `/data` stays at 1.75 GiB and the data format does not move, so **an upgrade keeps the applications, the sign-ins and the books** -- which the upgrade itself confirmed that day: the shelf, the sign-in and the compiled code all survived it. The starting screen was measured reaching the panel **0.7 s** after the icon is tapped, and it stays until the application draws its own first frame, 11-13 s while signed in; the cost is two or three more full-screen clearings per cold start. There is also a net under all this: **even if a transition does run all the way to its deadline, the previous screen is no longer taken away and replaced with nothing** -- ordinary use never reaches it (once a starting screen appears the deadline is never armed), and what it covers is an application that turns its own starting screen off. That was checked on the device the same day with both arms: turned off, the window is gone from the screen four seconds in; turned on, the home screen stays until the application really draws.

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
- **The Kindle system and your books are left alone.** Hold the power button to go back to the
  stock system at any time; if it cannot install or cannot start, it returns by itself.
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
- **Changing origin has still never run on the device.** The clean install of 2026-11-01 fetched
  all 188,326,526 bytes from the first origin (the GitHub release) in one attempt, so only the
  first entry of the list was used and neither `asking the next one` nor the "under the speed
  floor, move on" rule executed. Reaching them needs a first origin that really cannot be reached
  or really is slow — their first execution will be on a reader's device.
- Signing in to WeRead and reading a book, and the battery and deep-sleep measurements.
