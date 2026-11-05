# Where this machine's time goes

Every number here was measured on a PW3 running a development build of this system, not estimated -- the few that are extrapolated from a measurement say so where they stand, and say that they have not been measured again; each section gives the date it was measured, and a section without one was measured on 2026-10-20. It is written down so that nobody has to measure it again, and so that the several things that **looked worth doing and turned out not to be** are on the record.

The hardware: one 1 GHz Cortex-A9 (i.MX6SoloLite, **no 3D GPU**), 511 MB of usable memory, 256 MB of zram, and a 1072x1448 16-level grey e-paper panel.

## 1. Boot: 52 seconds, 21 of them once only

The stage timestamps PID 1 recorded itself during development (that record was removed before v0.1.0), plus `boot_progress_*` from `logcat -b events`:

| To this point | Seconds | How long that stretch took |
|---|---|---|
| PID 1 starts | 8.4 | The kernel plus the initramfs |
| The FAT is mounted | 8.6 | |
| The cross-link check is done | 9.8 | 1.3 |
| The mappings are made and /data is mounted | 9.8 | |
| **The system image is verified** | **30.8** | **20.9** (576 MiB / 27.6 MiB/s; with the image cut to 512 MiB on 2026-11-02 this should be about 18.6 s, **not measured again yet**) |
| system is mounted | 30.8 | |
| zygote starts | 33.4 | 2.6 (init starting services, 0.9 of it loading the touch module) |
| zygote begins preloading | 40.2 | **6.8** (ART coming up, reading 63.7 MB of boot image) |
| Preloading done | 43.6 | 3.4 (4715 classes in 2.2 s, resources in 0.36 s) |
| system_server running | 44.1 | |
| **The screen is handed to the reader** | **52.0** | 7.9 |

**Those 20.9 seconds happen only on the first boot after the image changed** (afterwards the record in `SystemStamp.h` hits and it is skipped), so an ordinary boot should be about 31 seconds (52 minus 20.9). Measured on its own once on 2026-11-02: from the first kernel line to `SystemServiceManager`'s `Starting phase 1000` took **34.5 seconds**, the same order (one run only).

**Those 20.9 seconds are in fact repeated work**: the installer had already read the image back past the host's cache and checked its SHA256 by the release checklist. PID 1 cannot trust that, because the record (`/data/pw3/system-verified`) is on ext4 and the stock kernel has no EXT4 driver, so the installer cannot write it.

### Measured and found not worth doing: a note from the installer saying "I checked this" (settled 2026-10-21)

The idea: the installer leaves a note on the FAT (size, mtime, SHA256), and when `/data` has no usable record PID 1 checks the file's identity against the note and skips the full 512 MiB read if they agree. **Not done.** Three reasons, in order of weight:

**One, the gain is 7% of one upgrade.** Those 20.9 seconds are not in an ordinary boot; they happen on the one boot after the image changed, and that boot follows a system-only upgrade **measured at about 5 minutes** (`docs/install.en.md`, computed from the log folder timestamps on the device on 2026-10-19); a full install is about 18 minutes. Removing it turns 5 minutes 21 into 5 minutes for the reader.

**Two, the binding would be weaker than today's, and weaker where it matters.** The installer has no access to the extent list PID 1 uses: `install.c` counts only the **number** of clusters (`clusters()`, to check there is room) and uses neither `FIBMAP` nor `FIEMAP`. So the note can carry only the size, the mtime and the hash -- and **the system image's size is a fixed 512 MiB**, so size cannot tell two different images apart and all that is left is the FAT's mtime, with its 2-second resolution. The consequence is concrete: somebody replaces `android/system.img` with another image of the same size after the installer has run, the hash in the note is still the one the installer verified and equals the hash this configuration expects, and the identity check passes -- **PID 1 would mount it**. Today's full read refuses it on the spot.

Restoring that strength would mean the installer computing the FAT extents itself (`FIBMAP`, and segment for segment identical to `TestFileMapper.h`'s algorithm). That is not "write one more file", it is reimplementing PID 1's mapping logic on the stock side and keeping the two in step -- not worth it for 20.9 seconds.

**Three, it adds one boot's worth of exposure.** An image damaged in place after the installer checked it and before the first start (with the size and the mtime unchanged) is caught today (no record -> full read -> hash mismatch -> refused); trusting the note, it is not caught and has to wait for the background `pw3-system-verify`, which is to say "this start ran, the next one refuses". That is the same class of exposure the record itself has (`SystemStamp.h`, lines 14-23 say so), but it is **one more**.

## 2. Memory: 511 MB, and who is using it

PSS from `dumpsys meminfo` (idle, on the home screen, with WeRead not open):

| | PSS |
|---|---|
| surfaceflinger | 64.0 MB |
| system_server | 46.8 MB |
| systemui | 42.7 MB |
| settings / launcher3 | 22.3 / 22.2 MB |
| zygote | 18.8 MB |
| The pinyin keyboard | 10.1 MB |
| **Services for hardware this machine does not have** | **about 20 MB** (below) |

### Hardware that is not there, with its services still running

The only features the image declares are `touchscreen / faketouch / wifi / webview / ram.low / GLES2` -- **no camera, no audio output, no Bluetooth, no telephony** (read with `pm list features`, and the image really does contain no Bluetooth package or library). And these are still running:

| | PSS | Usable on this machine? |
|---|---|---|
| audioserver | 3.7 MB | No speaker, no headphone socket, no Bluetooth |
| cameraserver | 2.8 MB | No camera |
| mediaserver | 2.8 MB | |
| media.extractor | 2.4 MB | |
| audio@2.0-service | 1.9 MB | |
| drmserver | 1.8 MB | No content path that needs DRM |
| media.metrics | 1.6 MB | It only reports, and this machine reports nothing |
| media.codec | 1.6 MB | An e-paper panel cannot play video |
| mediadrmserver | 0.9 MB | |
| cas@1.0-service | about 0.7 MB | |

**These services are kept.** Camera, DRM, CAS and media.metrics are usually checked for as features before being used, so removing them carries little risk; **audio and the codecs are different** -- WeRead has read-aloud, and an application that cannot reach `audioserver` may not be "silent" but crash, so each would have to be tried one at a time before going. The image-size side of this is section 7.

## 3. Measured and found not worth doing: windows in RGB_565

`dumpsys SurfaceFlinger`'s buffer listing: **39.7 MB, every one of them four bytes per pixel** (RGBx/RGBA_8888), on a panel with 16 grey levels. It looks like half of that is free.

The arithmetic says otherwise:

- **Composition was never the bottleneck.** Measured on 2026-11-02 over 20 page turns in a real book: 57 frames composed, 12.4 ms each on average; per page that is 35.2 ms of composition and 59.3 ms of sending to the panel (the framebuffer layer's `prepare` 52.8 ms plus `send` 6.5 ms), about 95 ms on the system side in all. Meanwhile **the panel's own GL16 takes several hundred milliseconds**. Halving composition saves about 18 ms a page. (This line used to say "composing 10.6 ms, sending 3.6 ms, halving it saves 1% of a page turn": those two numbers were per-frame means over 93 presents, read as if they were per page. 10.6 ms a frame is the same order as 11-02's 12.4 ms a frame; the mistake was reading it as per page.)
- **And it would not necessarily be faster.** `LumaCompose` has its own path for 8888, and 565 would take the general "convert each pixel to 32 bits" path (`LumaCompose.h` says so). Being faster would need a path written for 565 first.
- The memory saved is real (about 20 MB), but it means touching where SurfaceFlinger resolves `PIXEL_FORMAT_OPAQUE` plus a new path in the composer -- **two pieces of core code for 4% of memory**, which is not worth it.

The method: **look at what share of the whole thing an item is before deciding whether to touch it.**

Also, this machine **already uses the software Canvas** (`ro.pw3.software_canvas=true`, a product policy in `ActivityThread`), not HWUI plus SwiftShader. On a machine with no GPU that is the right path.

## 4. Compiling everything at install: 22 minutes 29 seconds

What is done now: an application is installed with AOSP's default `quicken` (no machine code; about 2 minutes of compiling for WeRead, end of section 6), and the background compile job takes it to `speed` once the device is idle (section 12). This section records what compiling everything at install costs, which is the reason it is not done.

During development, installs were compiled to `speed`. WeRead 2.1.2, a 127.5 MB APK, 13 dex files, 70.4 MB of bytecode:

```
+0:00   download finished
+0:42   dex2oat starts (--compiler-filter=speed)
+23:11  dex2oat took 1349.213s (1213.043s cpu) (threads: 1)
produced: base.odex 108.3 MB + base.vdex 70.7 MB; the whole application occupies 349 MB
```

`MemFree` fell to **6 MB** during it, 100 MB was pushed into zram, and the whole machine was unusable.

**What made it unusable is not CPU priority**: after boot, installd already puts dex2oat in the background scheduling group (`SetDex2OatScheduling(boot_complete)`, and the marker for that install was `boot_complete`). What made it unusable is memory:

```
dalvik.vm.dex2oat-Xmx = 512m     <- this machine has 511 MB in total
measured dex2oat RSS 241 MB
```

**That 512m is AOSP's default for a phone** (`build/make/target/product/runtime_libart.mk`); the Android Go configuration does not change it, and neither does this project. The compiler is allowed to ask for more memory than the whole machine has, so everything else is swapped out. **That is a plain mismatch rather than a trade-off** -- but what to change it to has to be measured, because `ro.config.low_ram=true` already gives dex2oat a swap file, and too low a limit moves the paging from memory onto /data.

**And what is expensive is not the compiling but compiling the 95% that is never used**: the same application with a usage profile compiles in **181 seconds** (not 1410) and produces 75 MB (not 168). But **the first install has no profile to use**.

Compiling everything at install rested on one measurement from 2026-10-13: without compiling, WeRead took 129 seconds to its bookshelf and raised "not responding". That number later stopped reproducing (section 6).

### The space of solutions (2026-10-20)

Turning 70 MB of bytecode into machine code is about 1350 CPU seconds on this core. **There are only three ways out**: do less, do it on another machine, or do it at another time.

| | How | Does it work here |
|---|---|---|
| **Do less** | Compile only the hot methods by the usage profile: **181 seconds**, 75 MB | **The first install has no profile.** Android P's `.dm` (a profile shipped alongside the APK) **does not exist at all** in 8.1 -- there is not one `dex_metadata` in `installd` or `PackageManagerService` (checked 2026-10-20). Implementing it means changing the framework, and a profile is bound to the APK's dex checksums, so one Tencent update makes it mismatch and falls back to producing nothing |
| **Another machine** | Compile the odex on the build machine and ship it with the image | Technically possible (the boot image and the APK are both known), but an odex is derived from somebody else's dex, so **publishing it is redistributing Tencent's software**, the same problem as bundling the APK |
| **Another time** | (1) usable at once, slow, compiled in the background afterwards (AOSP's way) (2) put the wait where the reader is already waiting (the first boot after installing the system, which has a progress screen) | **(1) is what is done now**: install at `quicken`, compile to `speed` in the background when idle (section 12). The feared "129 seconds and a not-responding dialog when nothing was compiled" does not reproduce (section 6). (2) is the same total time, gathered into one wait |

One more fact about application size: **the cost is proportional to the bytecode size**, about 3.1 MB per minute. WeRead's 70 MB is the extreme; an ordinary application with 5 MB of bytecode takes about a minute and a half.

## 5. Measuring a cold start here: the traps to avoid first

All of them met while measuring between 2026-10-20 and 10-27:

1. **The screen has to be on, and held on.** With the screen off the framework stops the application immediately after it resumes (`am_on_stop_called: ...,sleeping` in the event log) and the window is never drawn. `input keyevent KEYCODE_HOME` does not wake the device; it takes `KEYCODE_WAKEUP` plus `svc power stayon true`. Neither `am start` nor `input` resets the screen-off timer, so read `dumpsys power | grep mWakefulness=` before every timed set: one set on 2026-10-27 was taken with the screen asleep, the application started with no resumed activity, and a cold start cost 10.9 seconds of CPU and settled in 22 -- five times faster, and thrown away.
2. **For this application, the framework's own `am_activity_launch_time` can only be read for `totalTime`, not `thisTime`.** WeRead's first activity (`LauncherActivity`) is a forwarding shell that finishes before drawing, so the launch transition is cancelled and its own entry is never recorded; in the entry of the `WeReadFragmentActivity` it forwards to, `thisTime` counts from that activity's own start, which is the time "after the process was already up". A "first frame in 16.8 seconds" was once read off it during development; the real cold start is twice that. The same entry's `totalTime` counts from the first activity of the whole launch, so it includes the forwarding shell (checked on 2026-11-03 against the gap from `START` to `Displayed` in the same launch's log: 18.86 s against 18.78 s); the 10-23 set at the end of section 6 and the cold-start numbers after it all use it.
3. **The main log ring is only 256 KB** (`logcat -g`), which WeRead fills in under a minute, so nothing can be read back afterwards.
4. **The page cache has to be dropped before every start, not only the first.** Three measurements in a row fall steadily, and **they fall more for `speed` than for `quicken`** -- `speed` reads in 105 MB of odex and `quicken` only 1 MB, so the "warm" runs systematically favour `speed`.
5. **The same application with different data is two different amounts of work.** WeRead signed in with books on its shelf (a few hundred MB of data, things to sync) starts cold in 22-41 seconds; with the data cleared and the same `speed` compile it takes **10.6 / 9.7 / 9.5 seconds**. "The first opening after installing" and "the opening of something used daily" have to be reported separately and cannot be averaged together.
6. **The measurement itself must not start processes on the device periodically.** On this single core a script that starts a process for every sample costs 6%-22% of the CPU itself, more than the thing being measured. Take one snapshot before and one after, and leave at most one resident stream running in between (a single `adb logcat`, say).

The reliable test is the window manager saying this package's window has been drawn:

```sh
dumpsys window windows | grep -A3 'weread.eink/com' | grep -c HAS_DRAWN
```

(The pattern may not contain `Window{`: the grep on the device reads the braces as an interval expression and matches nothing.)

### Clearing an application's data to measure a first run, and putting the data back as it was

After `pm clear` the uid does not change, so the backup only needs a tar on the device (`cd /data/data && tar -cf ... com.tencent.weread.eink`) pulled to the computer, and restoring needs unpacking, `chown -R <uid>:<uid>` and `restorecon -R`. (Measuring fresh data under a second system user, leaving the existing data alone, is not possible: `config_multiuserMaximumUsers` is 1.)

**But the `*-shm` files have to be deleted before starting.** `-shm` is SQLite's WAL shared-memory index; put a stale `-shm` back alongside a `-wal` and SQLite ignores what is in the WAL. WeRead keeps its sign-in in one WAL file: put the `-shm` back with it and the application does not recognise the session, truncates the WAL on the spot and the sign-in is lost; delete the `-shm` before restoring and the data comes back as it was.

```sh
find com.tencent.weread.eink -name '*-shm' -delete
```

### To grow a usage profile again, empty the profile file; do not delete it

`/data/misc/profiles/{cur/0,ref}/<package>/primary.prof` is made by installd at install time, and the application cannot create the owner and the security label itself (`cur` is the application's own uid, `ref` its shared app uid, and both are `u:object_r:user_profile_data_file:s0`). **`rm` the file and ART's profile saver has nowhere to write** -- that was done once on 2026-10-20, the application ran through three complete times, the profile was still empty, and `speed-profile` then compiled a 1 MB odex from an empty profile, voiding that set of measurements, with no error anywhere along the way.

To start again, empty them:

```sh
: > /data/misc/profiles/cur/0/<package>/primary.prof
: > /data/misc/profiles/ref/<package>/primary.prof
```

If one has already been deleted, recreate it from another application on the same machine (owner `u0_aNN` / `u0_a4NNNN`, mode 600, `chcon u:object_r:user_profile_data_file:s0`).

## 6. What full compilation actually buys (measured the evening of 2026-10-20)

The method: all three compilation levels' output was built and stored in advance, the loop only put them back and compiled nothing, the order alternated ABBA, and the page cache was dropped before every start. The same user data, six rounds, cold start to the bookshelf.

| Round | Measured first | `speed` | `quicken` | Difference |
|---|---|---|---|---|
| 1 | speed | 35.4 | 39.9 | +4.5 |
| 2 | quicken | 34.0 | 43.3 | +9.3 |
| 3 | speed | 36.2 | 40.4 | +4.2 |
| 4 | quicken | 41.2 | 40.0 | **-1.2** |
| 5 | speed | 37.9 | 40.1 | +2.2 |
| 6 | quicken | 37.8 | 42.0 | +4.2 |
| **Mean** | | **37.1 s** | **41.0 s** | **+3.9 s** |

The differences have a standard deviation of 3.4 s and a standard error of 1.4, giving **t = 2.77 (5 degrees of freedom), p = 0.04, a 95% interval of [0.3, 7.5] seconds**. Three rounds of each order, with close sample means (speed first +3.6, quicken first +4.1) -- three to a group is not enough to prove the order effect negligible, only that none was seen this time.

Resident memory was 206.9 MB for `speed` and 183.6 MB for `quicken`; the output 176 MB against 76 MB; the compile 1214 seconds against 60-93.

A second set of six paired rounds, `speed` against `speed-profile`, was taken under the same conditions at another time: 34.7 against 36.6 seconds, a difference of +2.0 s, SD 4.0, t = 1.22, p = 0.28, a 95% interval of [-2.1, 6.1] seconds, two of the six rounds negative; resident 210.6 against 181.1 MB; compiling 1214 against 159 seconds, output 176 against 78 MB. Each set has its own baseline, so the two cannot be put in one table, nor subtracted to give `quicken` against `speed-profile`.

So, **for one cold start before signing in with an empty shelf**, what full compilation buys is a few seconds, with an uncertainty nearly as large, and it costs twenty minutes, 100 MB of disk and 23 MB of memory. Measured over a whole use (loading the shelf, opening a book) the answer is the opposite: full compilation nearly halves the time (section 12).

Both have only tens of KB of JIT code (`quicken` 60-132 KB, `speed` 32-44 KB). **That is size, not execution time** -- a small amount of hot code can still run a great many times, so it only says "the JIT was not forced into large-scale compilation" and cannot establish that those few seconds all come from having machine code or not.

### Two ways of measuring that look right and are biased

**Dropping the page cache only before the first start of a block.** Put `speed` back and measure three times, compile `quicken` and measure three times, twice: `speed` 29.6 s and `quicken` 35.5 s, both standard deviations around 7 seconds and heavily overlapping intervals. The other two starts in each block ran on a warm cache, which helps `speed` more (105 MB of odex against 1 MB): `speed`'s three blocks all fell steadily (40.8 -> 21.9 -> 18.0) while `quicken`'s went up then down. That is a systematic bonus to `speed` and an inflated variance for both.

**Compiling inside the loop.** Paired within a round, dropping the page cache every time, gave **+9.9 seconds, all five rounds in the same direction, t = about 12**, which looked very convincing. But every round was "put speed back, measure -> compile quicken, measure", so **the quicken measurement always came straight after a dex2oat that had saturated the single core for eighty-odd seconds**, with neither zram nor free memory recovered; the speed measurement came only after a file copy. Dropping the page cache does not drop those. Moving the compile out of the loop and alternating the order, on the same machine with the same data in the same week, took the difference from +9.9 to +3.9 and the standard deviation from 1.9 to 3.4.

**The lesson**: pairing absorbs variation between rounds and does not absorb a difference in conditions between the two measurements inside one round. As long as some condition always falls on one side, pairing carries its effect into the difference intact -- and because the variance drops, it looks more significant.

### Why section 4's 129 seconds does not reproduce

That was measured on 2026-10-13, and compiling everything at install rested on it. Without compiling it is now about 41 seconds to the bookshelf, with no dialog; in the install-path measurement of 2026-10-23 none of three openings produced an `am_anr`, and the slowest was 26.7 seconds (end of this section).

The likeliest explanation is that the Java heap's room was changed afterwards (`heapminfree`, `heapmaxfree`, `foreground-heap-growth-multiplier`; see `pw3.mk`): before that, WeRead's 12 MB live heap was collected every 0.4 seconds, taking 0.2-0.45 seconds each time, and most of those 129 seconds would have been garbage collection rather than interpretation. That is a reading, not a measurement: nobody has rerun the compile settings of that time against the new heap values on their own.

### One more thing only a profile-guided compile produces: the application image

AOSP's installd is explicit (`frameworks/native/cmds/installd/dexopt.cpp`, `maybe_open_app_image`):

```c
// Use app images only if it is enabled (by a set image format) and we are compiling
// profile-guided (so the app image doesn't conservatively contain all classes).
if (!profile_guided) { unlink(image_path.c_str()); return Dex2oatFileWrapper(); }
```

`dalvik.vm.appimageformat=lz4` is on on the device, so **the only reason WeRead has no `base.art` is that it is not compiled from a profile**. None of the filters used here for installed applications -- `quicken` at install, `speed` in the background job, `speed` for code another application loads -- is profile-guided, so none of those applications has an application image.

**But the thing itself is not worth much, and that was measured**: three of each alternating on 2026-10-16 gave 17.6 / 17.2 / 18.4 seconds with an application image and 18.5 / 18.0 / 17.8 without -- **a mean difference of about 0.4 seconds, inside the spread** (`docs/design/app-install.en.md`). So what profile-guided compilation is good for is compile time and disk, not the application image.

### A usage profile is private device data

`profman --dump-classes-and-methods` turns a profile back into a listing **down to the method signature** (`HPLandroidx/compose/material/IconKt;->Icon-ww6aTOc(...)`, H=hot, S=startup, P=post-startup). Which is to say a real profile is "which methods the reader of this machine executed", from which the features used can be inferred. It is also bound to the APK's dex checksums and mismatches on a different version.

**So a profile belongs with the waveform, the calibration and the adb key: not in a package, not in a log, not sent to anybody.** No scheme that ships a profile with the system holds; a profile can only grow on the reader's own machine, out of the reader's own use.

### The set with fresh data (measured the same evening, dropping the page cache every time)

The six rounds above all measured **an old installation**. With the data cleared (`pm clear`, profile and all) and measured again:

| | Compile time | Three cold starts |
|---|---|---|
| `speed`, everything compiled | 1219 s | **10.6 / 9.7 / 9.5 s** |
| `quicken`, nothing compiled | -- | Not obtained that evening, below; filled in on 2026-10-23 (end of this section) |

The spread nearly vanishes (1.1 seconds across three) because the page cache was dropped every time and there is no shelf of books to load. **So "WeRead starts cold in thirty-odd seconds" is only true of an account that has been used for a while; freshly installed it is ten seconds.**

The `quicken` set was not obtained that evening: halfway through, `system_server` fell into `UsageStatsService`'s self-exciting loop (next subsection) and held the whole core, leaving dex2oat 0.36% and not even letting `pm clear` into the binder queue.

### A defect hit while measuring: UsageStatsService exciting itself

After repeated data clearing, force-stops and start timeouts, `system_server` held the single core for over thirty minutes, with the thread stacks pointing at `UsageStatsService.onStatsReloaded -> postOneTimeCheckIdleStates` and the log scrolling with `Time changed in UsageStats by +2/+3/+4/-3 seconds` and `Flushing usage stats to disk`.

It is not the clock's rate: with the screen on, two 90-second measurements put the wall clock and the monotonic clock within +/-0.07% of each other. The cause is how the service decides: `UsageStatsService` reads the wall clock and `elapsedRealtime` in two adjacent statements, and when they diverge by more than 2 seconds it decides "the time changed", reloads the whole of the usage statistics and queues another check of the idle states. With dex2oat saturating the single core, the thread can be off the CPU for seconds between the two reads, and that gap reads as a clock change; the reload costs CPU and widens the gaps. Patch 0050 reads an interval of uncertainty instead and decides only when the whole interval crosses 2 s; in the verification after it went in, `Time changed in UsageStats` no longer appeared in the log. The analysis of the loop and its guards are in the "UsageStatsService can feed itself into a full-core spin" section of `docs/known-issues.en.md`.

### The install path, and `quicken`'s cold start with fresh data (2026-10-23)

The installer put WeRead on by itself, end to end (download -> PackageInstaller -> first opening); every number below comes from the device's own event log (`logcat -b events`), not from a watch:

| Step | From the install job starting | How long |
|---|---|---|
| The install job starts (11 s after boot) | 0:00 | |
| Download and write into the session | 0:03 -> 1:43 | about 100 s |
| `dex2oat took 124.717s (100.492s cpu) (threads: 1)` | 1:43 -> 3:47 | **124.7 s** |
| The install completes (`firstInstallTime`) | 3:47 | |
| The home screen's cell is handed to WeRead | 3:48 | |
| **Together** | | **3 min 48 s** |

Against section 4's run (the same 127.5 MB package on the same machine, with `speed`): 1349 seconds of compiling and over twenty minutes in all. **`quicken` takes the compile from 22 min 29 s down to 2 min 5 s.**

The cold start has to be given three times over, because **the first opening and the third differ by a factor of four** (the same installation and the same fresh data; `am_activity_launch_time`'s `totalTime`):

| Which opening | The activity itself | The whole launch to the first frame |
|---|---|---|
| The first (never opened since the install) | 22.1 s | **26.7 s** |
| The second (after `am force-stop`) | 8.3 s | **13.0 s** |
| The third | 4.0 s | **6.6 s** |

So those first 26.7 seconds are **not the price of having no machine code**; they are one-time work. The log shows it bringing up two WebView services along the way (`webview_service`, `webview_apk`), and it has databases to create and assets to unpack. By the third opening it is 6.6 seconds -- faster than the `speed` set with cleared data above (10.6/9.7/9.5 s), although that set dropped the page cache before every start and these three could not (a release build has no root), so the two are not directly comparable. What can be said is that **the first opening is slow and the rest settle at a few seconds**; installing at `quicken` does not make a freshly installed application misbehave.

## 7. What can come out of the image: 56.3 MB measured, and where it is stuck (2026-10-21)

Section 2's table is **resident memory**. This section is **image size**, from the file listing of one development build's exported image (`tools/inspect-system-image.py`, 478.5 MB in 1346 ordinary files):

| Category | Size | Files | Usable here? |
|---|---|---|---|
| **The fonts that are neither Chinese nor English** | **about 17 MB** | 171 | No. `/system/fonts` is 43.00 MB in total, of which `NotoSansCJK-Regular.ttc` is 18.67 MB (Chinese needs it) and `NotoColorEmoji.ttf` 7.28 MB (below), and the rest is Egyptian hieroglyphs (0.51), cuneiform (0.50), Tibetan (0.77), Arabic, Bengali, Devanagari, Armenian... while this project's settled scope is **Chinese and English only** |
| ICU localisation data | 23.32 MB | 1 | Part of it is unusable, but **the time zone data is in the same file**, and cutting wrongly means a wrong clock |
| RenderScript / LLVM | 13.77 MB | 9 | There is no GPU and this machine uses the software Canvas; but an application calling RenderScript itself would crash, so it has to be confirmed first that no application uses it |
| The colour emoji font | 7.28 MB | 1 | Colour emoji mean nothing on a 16-level grey screen. **Replaced, not removed** -- see stage 2 below |
| Telephony / RIL | 5.07 MB | 9 | There is no modem. But many applications check the SIM state, and removing `TelephonyProvider` might make them throw |
| Video codecs | 3.54 MB | 8 | E-paper cannot play video. Removing them makes `<video>` in a web page fail a different way |
| Camera | 2.16 MB | 11 | There is no camera. Applications usually check for the feature, so the risk is small |
| DRM / CAS | 1.14 MB | 16 | There is no content path that needs DRM |
| Audio codecs | 1.44 MB | 13 | **Keep**: WeRead has read-aloud |

**Removing the unusable ones above is about 56.3 MB, 11.8% of the image**, plus section 2's roughly 20 MB of resident memory.

### Where it is stuck: AOSP 8.1 has no "remove from PRODUCT_PACKAGES"

The facts, checked:

- `LOCAL_OVERRIDES_PACKAGES` is only wired up in `build/make/core/package_internal.mk` and `prebuilt_internal.mk`, and **does nothing for `BUILD_EXECUTABLE` or shared libraries**. So an APK (`TelephonyProvider`, `Dialer`, `Mms`) can be displaced by a supported mechanism and `cameraserver`, `drmserver`, `libstagefright_soft_*` and `libLLVM.so` cannot.
- They come from `pw3.mk` inheriting `core_base.mk` -> `core_minimal.mk` -> `base.mk`, about 236 packages across the three files.
- **Overriding an rc file or making an empty module of the same name is a stopgap and is not done.** The right way is not to inherit `core_base.mk` and to list what this machine needs; that is a structural change, and the way to verify it is to diff `installed-files.txt` before and after and see what actually left.

### The fonts are different: we already control the mechanism

AOSP installs all the Noto fonts through `external/noto-fonts/fonts.mk`, and that file is only a list of `PRODUCT_PACKAGES`. `pw3.mk` does not inherit it and lists the fonts to install itself, at the top of the file. That is a supported thing to do and leaves the inheritance chain alone.

The risk was checked: when `fonts.xml` names a file that is not there, `Typeface.java` treats that family as a system error and `return null`s (line 913), and the loop that builds the fallback families does `if (fontFamily == null) continue` (line 960) -- **the fallback family is skipped, not a fatal error**. The default family is Roboto and is kept. So the consequence is boxes on the screen, not a machine that will not start. Doing it cleanly means cutting `fonts.xml` too, or every boot logs 171 errors and mmaps 171 files that are not there.

#### Cutting `fonts.xml` has its own trap, better hidden than that one (measured 2026-10-21)

The package list can be written by hand, but **`fonts.xml` itself cannot be displaced**, and the way it cannot is silent:

- `LOCAL_OVERRIDES_PACKAGES` is wrapped in `ifeq ($(LOCAL_MODULE_CLASS),APPS)` at line 250 of `prebuilt_internal.mk` and is **not read at all** for an `ETC` prebuilt; `LOCAL_OVERRIDES_MODULES` at lines 246-256 of `base_rules.mk` knows only `EXECUTABLES` and `pretty-error`s for anything else.
- Two modules installing to the same path **do not conflict either**: kati merges the two `copy-one-file` rules into **one** build statement with both intermediates as its prerequisites, and keeps the first recipe -- AOSP's.
- So the image built that way is **AOSP's 116 families with 37 font files**, with fonts.xml naming 136 files that do not exist, and not a sound out of the build. Dropping the inheritance does not help either, because that copy rule is generated unconditionally, whether or not the module is in the product list.

The supported way is to have **the same module** copy a different file. AOSP already has `ADDITIONAL_FONTS_FILE` (adding families to fonts.xml), and patch 0052 adds `REPLACEMENT_FONTS_FILE` in the same shape (replacing the whole file), with the device pointing at it in `BoardConfig.mk`. One module holds one path.

This trap was caught by a test, not by looking: `tools/tests/test_font_config.py` has an item that "`/system/etc/fonts.xml` in the staging directory must be byte-identical to the one in the repository". Whether a configuration is right and whether the build accepts it are two different things.

### In stages, ordered by risk

| Stage | What | Saves | Risk | How to verify |
|---|---|---|---|---|
| 1 | Non-Chinese, non-English fonts plus cutting `fonts.xml` | about 17 MB | Low (the scope is already settled as Chinese and English only) | Chinese, English and symbols render; no font family errors in the log |
| 2 | Colour emoji | **about 6.9 MB** | None: emoji still render, only in one colour | Replaced with a monochrome font, not deleted. Below |
| 3 | Camera + DRM + CAS + media.metrics | 3.3 MB of disk plus about 12 MB of memory | Low | The services are gone; WeRead and the browser carry on |
| 4 | RenderScript / LLVM | 13.77 MB | Medium | It has to be confirmed first that no application uses RenderScript |
| 5 | ICU data cut to Chinese and English | at most 23.32 MB | **High**: the time zone data is in the same file | Dates, the clock, Chinese collation, the keyboard; get it wrong and the clock is wrong |
| 6 | Telephony / RIL, video codecs | 8.6 MB | Medium | Every application has to be looked at for whether it throws |

The "Saves" column is the estimate made from the file listing before any change.

### Stages 1 and 2 are done; 3 to 6 are not (2026-10-21)

The risk in stages 1 and 2 is held by mechanical guards: the export refuses "`fonts.xml` names a file that is not there", `tools/check-image-references.py` checks the files and library dependencies loaded at run time (522 files and 4544 dependencies in that image), and `tools/tests/test_font_config.py` checks the configuration item by item against what the build produced.

**What stages 1 and 2 measured**: `/system/fonts` went from 173 files and 43.00 MB to **37 files and 33.0 MB**; `fonts.xml` from 116 families to **15**; `usr/hyphen-data` from 70 files in 35 languages to **4 in two English ones**. What was actually saved: 8.43 MB of fonts, and a net 5.30 MB from replacing the emoji font (the variable font chosen is larger than the one in the estimate; below).

**3 to 6 are not done.** What decided it is not the risk but where the gain falls:

- **Image size is not on the waiting path of every boot.** The system image is read in full only on the first boot after it changed (section 1, `SystemStamp.h`): when the size, mtime, cluster allocation and expected hash in the record all match, the recomputation is skipped, so those 20 seconds are **once per version**; a boot that skipped the read has the image re-checked in the background 90 seconds after `boot_completed`, at the lowest CPU and I/O priority (`pw3-system-verify`). So cutting the image buys "seconds once per version" and download size, not a faster boot.
- **What stages 1 and 2 bought is not size but work zygote does.** `Typeface.init()` is in a static initialiser and `android.graphics.Typeface` is line 814 of `preloaded-classes`: every boot, zygote walks every family in fonts.xml -- mmap, `addFontFromBuffer`, `FontFamily.freeze()` (reading the cmap and building the whole coverage bitmap). 101 fewer families is 101 fewer of those at every boot, and on a 1 GHz single core that is real.
- **3 to 6 do not buy that.** `ZygoteInit.preloadSharedLibraries()` loads only `android`, `compiler_rt` and `jnigraphics`; `libRS` and `libLLVM` load only when an application calls RenderScript, and the ICU data is mmapped with a cost only for the pages actually read. So they save download size only, in exchange for "an application that calls RenderScript crashes" and "time zone and collation behaviour changes". By Occam's razor, not done.

### Stage 2: the colour emoji replaced by a monochrome font (2026-10-21)

Removing `NotoColorEmoji.ttf` alone would turn emoji in books and web pages into empty boxes. But **a monochrome emoji font already exists**: upstream Noto Emoji, SIL OFL 1.1, from the same project and under the same kind of licence as the Noto fonts already in the image. AOSP 8.1's `external/noto-fonts` has only the colour one (`emoji/NotoColorEmoji.ttf`, 7,279,008 bytes, and only this one goes into `/system/fonts`; `emoji-compat/NotoColorEmojiCompat.ttf` is another 7.4 MB as a support-library resource and does **not** go into /system, which is why this item is 7.28 MB rather than 14).

So this is a replacement, not a removal: emoji still render. And monochrome is the correct rendering on a 16-level grey panel anyway: a colour bitmap (CBDT) has to be dithered to grey here, and resolves less detail than a monochrome vector outline.

The font is pinned by size and SHA-256 in `device/amazon/pw3/apps/prebuilts.json` (as WebView and EinkBro are), fetched and checked by `tools/fetch-prebuilts.py`, and does not go into the repository; `THIRD_PARTY.en.md` has a SIL OFL 1.1 entry for it.

#### The variable font, not the static one

Upstream `googlefonts/noto-emoji` has not carried a static monochrome version since v2.038 (the last was v2.034's `NotoEmoji-Regular.ttf`, 418,804 bytes); what is maintained now is Google Fonts' `ofl/notoemoji/NotoEmoji[wght].ttf`, 1,982,596 bytes, with a weight axis of 300-700.

Both were fetched, rendered at this machine's real sizes (`ro.sf.lcd_density=300`, so 14 sp = 26 px, 18 sp = 34 px, 22 sp = 41 px), quantised to 16 grey levels and compared side by side with the colour one:

- **The colour one really is a grey smear on this panel**: a star, a sun and a tick are pale grey blocks and books are a dark grey smudge, barely distinguishable from each other at 14 sp. Both monochrome ones are pure black and white with their contrast intact.
- **Static against variable is a real trade-off**: the static one draws symbols solid (a heart, a flame, a tree and a tick are recognisable at a glance) but **draws faces as solid black blobs**, so a grinning face, a laughing face and folded hands are one black lump at 14 sp; the variable one is outlined, which keeps a face's expression and makes the solid symbols thinner and busier.
- What appears most in reading is faces and gestures (book reviews, notes, body text on the web), and losing all of their information costs more than symbols becoming outlines, so **the variable one was chosen**. It costs 1.56 MB more and still saves 5.30 MB net against the colour one.
- The variable one's weight axis is **pinned at 500**: 400 is too thin at 26 px and 700 starts filling in trees and suns. `fonts.xml` uses `<axis tag="wght" stylevalue="500.0" />`, the same shape as AOSP's own `NotoSansKhmer-VF.ttf` -- **variable fonts were already in use in this fonts.xml** and this is not a new mechanism.

#### The monochrome font has no cmap format 14 subtable, so one was added

A monochrome font has no cmap format 14 subtable, and `FontFamily::hasGlyph(codepoint, variationSelector)` **reads only** `mCmapFmt14Coverage` when there is a variation selector. So all **351** emoji Unicode defines with a "U+FE0F spelling" (a heart, for instance) would be reported as undrawable even though the drawing path can draw them. An application that asks before drawing (`EmojiCompat` goes through `PaintCompat.hasGlyph`) would fall back to a font of its own. AOSP's colour font carries all 351.

The fix: `device/amazon/pw3/emoji/add-emoji-variation-sequences.py` generates the subtable from the tree's `external/unicode/emoji-variation-sequences.txt` (the same file AOSP's own font is based on), and what goes into the image is "the bytes as fetched plus a 4,088-byte table". The font **covers every one** of the 351 base characters. The table is written as default UVS records ("supported, with the glyph the ordinary cmap gives"), and Minikin's `CmapCoverage.cpp` merges default and non-default records into one coverage table, so they are read all the same. The guard is `tools/tests/test_emoji_vs.py`: it reads the result back with a parser that shares no code with the generator, and checks that every other table is byte-identical, that the recorded sequences are exactly Unicode's set, and that the subtable is registered at platform 0 / encoding 5; FreeType renders the font pixel-identically before and after the rewrite.

**The first version of the program that writes that table computed the `head` table's directory checksum wrongly** (it computed each table's checksum over the original bytes and zeroed `checkSumAdjustment` afterwards, and the directory entry is never corrected again; OpenType's order is the other way round). One development image carried that wrong value -- the font taken out of the exported image had `0x7bacb7ad` in the directory where the specification says `0x2832657a`. The whole-file checksum was right the whole time, so checking only that one would not have found it. There is no evidence that Android 8.1 rejects a font for it, but it does not meet the specification and these are bytes that get published, so the image was rebuilt with it corrected; the two versions of the font differ in exactly 8 bytes (`head`'s directory checksum and `checkSumAdjustment`). The test now checks every table's checksum, with `head` not skipped.

**Which code points the cuts lost was measured**: the cmaps of AOSP's original 173 font entries and this project's 37 were all parsed and the difference taken, and of the 27 ranges relevant to Chinese and English typesetting (CJK Unified Ideographs 20945, Extension B 2051, halfwidth and fullwidth 225, general punctuation 100, arrows 112, box drawing 128, mathematical 256, miscellaneous symbols 256, dingbats 191, enclosed alphanumerics 160) **not one lost a single code point**; of the 12328 lost, 9384 are the letters of the scripts that are not typeset here, and 539 are symbols and number forms (counting by Unicode category and by cmap difference gives the same figure), 11 of them currency symbols.

#### The currency symbols that came with the scripts that were cut, recorded by range

These eleven -- `֏ ؋ ৲ ৳ ৻ ૱ ௹ ฿ ៛ ﷼ ꠸` -- have no font coverage any more, and of them the ones actually in use are the baht, the taka and the rial. Covering those three means putting Thai, Bengali and Arabic back (about 238 KB, and three more font family loads in zygote at every boot), which is not worth it.

But the range is **checked** rather than spoken: `COVERAGE` in `tools/tests/test_font_config.py` lists 18 groups of "code points this set of fonts must be able to draw" and checks them by reading the cmaps of the font files installed in the image, in 0.13 seconds. Any future slimming that touches these fails on the spot rather than showing boxes on the device.

### The largest piece of the image cannot be compressed (measured 2026-10-21)

Where the 478.5 MB is:

| Where | Size |
|---|---|
| `app/SystemWebView` (the APK itself 86.63 MB) | 95.2 MB |
| `lib` (including `libLLVM.so` at 11.41 MB) | 94.1 MB |
| The ART boot image under `/system/framework` (arm; **untouchable** -- touch it and all the machine code in `/data` is void) | 65.2 MB |
| `fonts` (down to 33.0 MB after the slimming) | 43.0 MB |
| `app/EinkBro` (odex 21.80 MB) | 30.5 MB |
| `usr/icu/icudt58l.dat` | 23.3 MB |

Taking the first apart: of `SystemWebView.apk`'s 86.63 MB, **73.64 MB is the single file `lib/armeabi-v7a/libwebviewchromium.so`**, and it is `stored` in the APK (the WebView loader mmaps it directly, so it has to be page-aligned and uncompressed), so it costs full size in the image too. Its sections: `.text` 58.7 MB, `.rodata` 8.4 MB, and **no `.symtab` and no debug section -- it has already been stripped**. There is one ABI. All that could be saved is the 79 unused language packs in `assets/stored-locales/`, about 1.3 MB -- not worth re-signing a third-party APK for.

**The conclusion: the floor of the image's size is set by Chromium, not by us.** Short of building Chromium ourselves, 95 MB is the floor.

## 8. WeRead once signed in: a full measurement (2026-10-27)

Everything above was measured with the application installed but not signed in. This section is the same device after signing in with the QR code, with books on the shelf. The cable was in, the screen was held on with `svc power stayon true`, and each timed set was preceded by checking the screen was awake (section 5, item 1).

### The four things the reader does

| action | until fully quiet | the application's own CPU | downloaded |
|---|---|---|---|
| cold start (restoring into the book being read) | about 40 s | 28 s | about 16 MB |
| opening a book from the shelf that was never downloaded | 51 s | 31 s | 0.6 MB |
| opening that same book again | about 20 s (30 s to full quiet) | 11 s | 0.13 MB |
| turning one page | under 1 s | 0.44-0.81 s | -- |

"Quiet" means the process's `utime+stime` grew by less than 200 ms in each of three consecutive 5-second samples. The cold-start row is the same order as the "about 37 seconds to its bookshelf" in section 6, so signing in and carrying real data did not make it worse.

Those 16 MB are downloaded again on every cold start, and `/data` grew by 540 KB across two of them, so none of it is kept. That is the application's own sync and nothing on the system side can change it, but it is 16 of the 40 seconds.

### What it is not

**Not the network.** A strong Wi-Fi signal, four pings to `weread.qq.com` with 0% loss and 6-95 ms. The 16 MB of a cold start arrive in 16 seconds, 1.08 MB/s. Opening a book that was never downloaded pulls 634 KB and spends 31 seconds of CPU -- that step barely touches the network at all.

**Not the governor.** This CPU has two steps, 996 MHz and 396 MHz, it has spent 87% of the time since boot at 396 MHz, and `ondemand` judges load against the current frequency with `up_threshold=80`, all of which looks suspicious. Reconciling `time_in_state` against `/proc/stat` over the same 54-second window: 996 MHz 36.4 s against 35.1 s of the whole machine busy; 396 MHz 17.6 s against 18.9 s idle (iowait included). The two sides match almost exactly -- **full speed whenever there is work, low only when there is none** -- and the lower bound on busy time spent at 396 MHz is zero. This one is ruled out.

**Not invalidated machine code.** `dumpsys package dexopt` reports `kOatUpToDate`: the machine code has not been invalidated by a changed boot image. **That is worth checking on the device after every release**: during development a changed boot image once invalidated all the machine code in `/data`, and opening a book took 53.7 seconds, which at the surface is indistinguishable from "the hardware is simply this slow". How much the compiler filter itself matters is section 12 (the whole package was also compiled to `speed` once here -- 20 minutes of CPU, 98 MB more in `/data` -- with no clear change in the numbers; that run had no alternating control, and section 12 is the one to go by).

**Not something left in the system.** Every process other than WeRead adds up to about 120 MB of PSS: surfaceflinger 31.5, system_server 27.9, SystemUI 14.2, Launcher3 7.3, android.process.media 5.6, the pinyin keyboard 2.6, and nothing else above 1 MB. There is no large block left to remove. WeRead itself is 128.8 MB, plus 22.0 MB for its `:seg` process: 151 MB of a 511 MB machine.

**Not SwiftShader drawing.** Neither WeRead nor Launcher3 has a RenderThread (SystemUI has one): this system's product policy (`ro.pw3.software_canvas=true`, section 3) puts application windows on the software Canvas, which does not go through SwiftShader. Not one frame of those 31 seconds was drawn by SwiftShader.

### The first sync after signing in: 2.75 times slower, all of it memory

What was measured during the first sync after signing in: `MemFree` down to 5,940 KB, 115 MB pushed into zram, `pgmajfault` up by 1,423 inside a single load, and a cold start that burned 72 seconds of CPU over 110 seconds of wall clock. The same cold start, once the sync was done and 221 MB were free again, takes 40 seconds. **Same machine, same application, same account, 2.75 times apart, and all of the difference is memory.** It happens once -- and it happens in the first quarter of an hour the reader spends with the device.

### One number that is ours, and still not worth acting on

In the 18 seconds of opening a new book the main process ran five concurrent copying collections of 902, 956, 1938, 543 and 493 ms, 4.83 seconds together, and the `:seg` process one more of 853 ms. Every one of them reports "34-36% free, 28-30MB/44-46MB": about 30 MB live with 16 MB of room. That 16 MB is `dalvik.vm.heapmaxfree=8m` times the foreground multiplier of 2, while the rule of thumb recorded beside the heap values in `pw3.mk` (a collection costs about 0.02 s per MB live; GC takes a tenth of the time when the room is about as large as the live heap) asks for room around 30 MB.

Raising `heapmaxfree` to `16m` would make the room 32 MB, roughly halve the number of collections and save about 2.4 seconds -- **5% of 51** -- at the price of up to 14 MB more per foreground application, on a 511 MB machine that was 2.75 times slower an hour earlier for want of memory. Occam says no. The same question, worked over a whole cold start, is in section 10.

### The remaining 26 seconds are still not broken down

Of the 31 seconds of CPU to open a new book, 4.8 are GC and about 26 are unaccounted for. By thread, on the cold start: main thread 25.0 s, the four `WRCommonPool` threads 14.8 s together, `RxComputationScheduler` 3.6 s, the four `WRNetworkPool` threads 6.2 s, `HeapTaskDaemon` 1.7 s, and the JIT thread pool only 0.3 s (an idle JIT does not show that nothing is interpreted; see section 12). Going further needs simpleperf (section 9).

## 9. Not measured yet

- ~~Whether an ordinary boot (the one where the image did not change) really is about 31 seconds~~: one reboot measured on 2026-11-02, 34.5 seconds from the first kernel line to `Starting phase 1000`; see section 1. Two more runs would give a steadier number.
- `services.jar` is `speed-profile` today (Android Go's default, to save space: `PRODUCT_SYSTEM_SERVER_COMPILER_FILTER` in `go_defaults_common.mk`, which governs only system_server's few jars and is not affected by the `speed` default this project sets for applications; in the build output of 2026-11-03 the header of `services.odex` says `speed-profile`, while Settings' odex from the same build says `speed`), and it is code every interaction goes through; whether `speed` is worth it has not been compared on the device.
- ~~How many milliseconds a full-screen composition really takes on a page turn~~: measured on real book pages on 2026-11-02, 12.4 ms a frame on average and 35.2 ms of composition a page; see section 3.
- Where the application's own CPU goes when it opens a book: the machine code is in use and only about 90 MB is read from disk (about 3 seconds); section 8 broke it down by GC and by thread as far as that goes, and the rest -- class loading, static initialisers, the application's own initialisation or paging -- cannot be told apart. simpleperf is the way to see: the `user` image that is released does not have it and will not sample a non-debuggable application; a `userdebug` image carries `/system/xbin/simpleperf`, so that is where to measure it, which has not been done yet.

## 10. Whether WeRead can be made faster: the whole core, accounted for (2026-10-28)

Section 8 measured how long each action takes. This one answers the next question: **how much of that time is ours to move**.

**Method.** A cold start resumes straight into the test book last opened (about 800 pages), so one cold start is one complete "open a book" with no tap whose coordinates drift. "Quiet" is the process's `utime+stime` gaining under 0.15 s across two consecutive 2 s windows. Every set reads `dumpsys power | grep mWakefulness=` first (section 5, item 1). What matters is putting `/proc/stat` and `/proc/<pid>/stat` in the **same window**, so that "the application was not on the CPU" stops being a remainder and becomes a statement about where time went.

### A process's CPU is read per process, not summed per thread

Summing threads from `/proc/<pid>/task/*/stat` and calling that the process's CPU **misses every thread that exited during the run**: the same open is 53.3 s by thread and 74.2 s by `/proc/<pid>/stat`, a third less, which looks like 30-40% of the time spent off the CPU. Read per process, **there is no gap**.

### Where the core goes

A 95 s window, cold start to quiet:

| | time | of wall |
|---|---|---|
| WeRead | 74.2 s | **78%** |
| every other process | 14.0 s | 15% |
| the `:seg` process | 2.1 s | 2% |
| core idle | 4.2 s | 4% |
| of that, waiting on disk | 0.0 s | **0%** |

Splitting "other", in a second 79 s window: the core is busy 74.6 s (94%), WeRead 54.4 s (69%), `surfaceflinger` 2.7 s (3%), `:seg` 1.8 s (2%), `system_server` 1.2 s (2%), SystemUI 0.1 s, and **14.4 s (18%) unattributed**. That last part is kernel threads, Wi-Fi softirq and the eMMC driver; it cannot be read per process and was not broken down further.

WeRead itself has 69-78% in both. **The wait is that application computing on one core.**

### What is ruled out

- **Not runtime compilation.** Within a single cold start the JIT thread pool used 0.0-0.6 s. An earlier reading of "21.3 s in the JIT thread pool" was the total over an **18-minute session** (signing in, the first sync, downloads, several books opened), not the cost of one open. Note that this rules out JIT compiling and **not interpretation**: interpretation is charged to the thread running the code, not to the JIT thread (section 12).
- **Not composition.** `surfaceflinger` is 2.7 s of 79 s, 3%. Worth checking on a machine with no GPU where a full-screen window is 6.2 MB a frame for the CPU to move -- and, having been checked, closed.
- **Not the segmenter.** `:seg` is 1.8-2.1 s, 2%. It is a separate process and every call is a binder round trip, so it was worth checking too; also closed.
- **Not a missing NEON.** `BoardConfig.mk` says `armv7-a-neon` / `cortex-a9`, and the device's `/proc/cpuinfo` Features line has `neon`.
- **Not the disk.** iowait is 0 across the whole window.
- **Not the network.** 18.5 MB arrives during the window, but the core is idle only 4-6%: **the download overlaps the computing throughout**, and nothing stops to wait for it.

### The other runtime lever: room between collections

Each open runs 8-9 concurrent copying collections, **11.4 / 12.8 / 13.8 s** in three opens, 17-25% of the application's own CPU. At every collection the heap is "19-30 MB live, 32-46 MB footprint" -- 16 MB of room, which is `dalvik.vm.heapmaxfree=8m` times the foreground multiplier (`foreground-heap-growth-multiplier=1.0`, to which ART's concurrent collector adds 1).

**Why it allocates that much.** The test book is about 800 pages, over 300,000 characters, and the open allocates close to a hundred megabytes: roughly **250 bytes per character typeset** (a `CharElement` and a `Paragraph$Word` each, `int[]`, boxing). The measurements agree: a collection every 9.6 s, 8-16 MB freed each time, 230,000-370,000 objects cleared. A collection costs by **object count** rather than by bytes, so the total is set by **how many collections happen**, and that is allocation divided by room -- **and the room is the value we set**.

Twice the room is half the collections, about 6 s of 95, **6-7%**. That is more than section 8's sum (2.4 s of 51) because that measurement covered the 18 s of opening a new book and this one covers a whole cold start. And **moving the multiplier is cheaper than moving `heapmaxfree`**: `foreground-heap-growth-multiplier` applies only to the one process in the foreground, while `heapmaxfree` is paid by every process. AOSP's own default is 2.0, and `pw3.mk` sets 1.0 the way Android Go does.

**It costs the foreground application memory, and this machine has little to spare**: during the measurement `MemFree` was 43 MB with 125 MB already in zram, and both the Pinyin input method and `:seg` **were killed** (`has died: prcp IMPB` and `cch CEM`, with an empty `logcat -b crash`, so killed rather than crashed).

**Measured in pairs on the device on 2026-10-29, and the two arms could not be told apart.** Four rounds A B B A, A = multiplier 1.0 (16 MB of room), B = 2.0 (24 MB, what AOSP gives a 1 GB phone). Each round: change the property -> restart the framework (`dalvik.vm.*` is read when the zygote starts, and `setprop` alone does nothing) -> drop the page cache -> force-stop WeRead -> tap the icon to cold-open the same book -> read process CPU and the collection records after a fixed 115 s window. The two A rounds were almost identical (7 collections each, 6.9 / 6.1 s of collecting, RSS 57 KB apart); the two B rounds collected for 2.3 / 11.2 s, and the second spent 30.4 s of process CPU, twice the other three -- the application did different work that round (most likely a network sync), which the multiplier did not cause. The two paired differences have opposite signs (-4.6 s and +5.1 s), so the set supports no conclusion; no process was killed for low memory in any round. So the multiplier stays at 1.0. Measuring it again means holding the workload still first: network off, the same book and page, at least three rounds per arm.

### What is left is not on the system side

Once compiled to `speed` (section 12), the application's own roughly 30 s of CPU to open a book is the parsing, typesetting and syncing it does on one core. The footer reads "page / total pages", so it is paginating the whole book; if WeRead's own reading-progress setting can be switched to chapters or a percentage, paginating the whole book may be skipped entirely. That is a setting inside the application and **has not been tried**.

## 11. Twenty seconds of the framebuffer are black while a book opens (measured 2026-10-28, **the conclusion overturned 2026-11-02**)

> **2026-11-02: the panel does show it, and the cause has been found.** The sentence below --
> "this is not a defect the reader meets" -- **does not hold**. What it rested on was the owner
> watching two cold starts and reporting "no black screen, a white screen for a while": that was
> true, but **the conditions were different**. WeRead was not logged in then and drew its white
> login page inside five seconds. Logged in, its first frame takes twelve to thirty seconds, so
> every launch now hits `APP_TRANSITION_TIMEOUT_MS = 5000` in `AppTransition.java:179`: on timeout
> the transition runs anyway and the launcher gets `setVisibility(false)`, leaving the status bar
> and the navigation bar over nothing. That composition is black, and that frame **is** given to
> the panel (GC16, 823,296 pixels, the same number on five independent runs). What the owner saw on
> 2026-11-02 was "the top half goes black first, then all white, then one all-black before the
> shelf". The full timeline, the evidence and the fix are in that row of `docs/open-items.en.md`
> and in `patches/aosp-frameworks-base/0067-pw3-the-transition-waits-for-the-first-frame.patch`.
>
> One more thing: the conclusions below that rest on `screencap` sizes and dark-pixel ratios have a
> **hole in the method**. `screencap` goes through SurfaceFlinger's `captureScreen`, which
> **recomposes the current layers on the spot** and does not pass through the panel driver, so it
> is neither "the last frame the panel was given" nor something that can be differenced against the
> panel counters. To know what the panel shows there are three ways only: have the owner look, turn
> on `mxc_epdc_debug` on a `userdebug` build, or let the panel counters say "how many updates and
> how large" and nothing about content. The numbers below remain valid as a record of what was in
> the framebuffer.

While measuring section 10, five frames were captured at fixed times during a cold start. The PNG sizes give it away on their own (a black screen compresses to nothing, a page of text does not):

| at | PNG | screen |
|---|---|---|
| t=24 s | 18 KB | entirely black |
| t=32 s | 14 KB | entirely black |
| t=48 s | 244 KB | text |
| t=57 s | 244 KB | text |
| t=79 s | 241 KB | text |

A second run sampling the fraction of dark pixels has the same shape: 6.8% at t=17 s, 87.5% at t=32 s, settling at 7.1% from t=50 s. **Both show over twenty seconds of black with nothing in between.**

### In the middle of the black, nobody is drawing

Reading the windows and layers 26 s after launch, inside the black stretch:

```
Window{… com.tencent.weread.eink/…ReaderFragmentActivity}
    mHasSurface=true  isReadyForDisplay()=true
    mDrawState=DRAW_PENDING          ← not one frame drawn
Layer (…ReaderFragmentActivity#0)
    activeBuffer=[0x0: 0, 0]  queued-frames=0   ← the layer has no buffer
Window{… com.android.launcher3/…Launcher}
    mDrawState=HAS_DRAWN  mLastHidden=true      ← the previous screen is hidden
```

The wallpaper layer has no buffer either. **The foreground window has no content, the previous screen is hidden, and what composition produces is black** -- that black is not drawn by anyone, it is the absence of anyone drawing.

### It comes from patch 0008, whose premise does not hold

`0008-pw3-no-starting-window-opaque-surfaces.patch` has `ActivityRecord.showStartingWindow()` return at once under `ro.pw3.disable_starting_window`, which is `true` on the device. Its stated reason:

> A preview window costs its own buffers (6 MB each), a full-screen draw and an e-paper update of 134 ms or more, only to be replaced by the application's first frame. **The previous screen stays until that frame.**

**At the level of SurfaceFlinger that last sentence is not true.** The window state above contradicts it directly: `mLastHidden=true`, the previous screen is already gone. The patch itself ends with "Not run on the device yet when this patch was written".

On a phone it costs nothing -- an application draws its first frame in a couple of hundred milliseconds. On this machine an application takes twenty to forty-five seconds, so the framebuffer holds more than twenty seconds of black. Those black frames never reach the panel (below), though, so the 134 ms panel update saved is still a real saving.

### Two ways to change it

Today the panel does nothing for those twenty seconds, and putting the starting window back would give it one more full-screen update, so **the present behaviour may already be right**. If it is to change, there are two ways. **Both were made on 2026-11-03 and verified on the device**: the first is patch 0068 (the starting window on by default, on the panel in 0.7 s, WeRead's being its white page with the emblem), the second is patch 0067 with 0069-0073 (the transition waits for the first frame, and a deadline no longer takes the previous screen away; on 2026-11-04 every state that could be staged was run on the userdebug twin of the release build) -- see section 16 of `known-issues` and `patches/VERIFICATION.md`. The two paragraphs below are the original text of 10-28:

1. **Put the starting window back** (set the property `false`, or drop the first half of the patch). It costs one more full-screen panel update per launch (134 ms or more). **The colour is undecided**: a starting window draws the activity theme's `windowBackground`, and whether WeRead's is white or black has not been checked -- if it is black, this change does not help.
2. **Keep the starting window off, but make the previous screen really stay** until the incoming application draws its first frame. That is what the patch meant to do; it means changing the window transition, which is more work than the first.

### What the panel actually shows: a white screen, then one black flash

For the two cold starts the reader watched (at `speed`), the frame sequence was the same as above both times, 10.2% → 91.7% → 100% → 6.1%; on the panel there was a white screen and then one black flash when the text arrived. So the panel stayed on the first of those frames -- 10.2% dark, a nearly white loading page -- and **not one of the black frames was ever sent to it**.

**The framebuffer is not the panel.** `screencap` reads SurfaceFlinger's composition, not what this display is showing; e-paper holds its last picture until something pushes a new one. This repository records the same kind of lesson about waveforms (the one requested is not the one executed); this is the same mistake from the other side. **Any conclusion about what is on the screen has to be confirmed by someone looking at it, or by the kernel log.**

The flash is `replaced()` in `device/amazon/pw3/epdc/Panel.h`. When a window appears or goes away, that area is charged to the limit and the update drawing the new content pays it off with one GC16. Its origin is the Wi-Fi password dialog leaving its ghost behind on the list when GL16 redrew over it (2026-10-23). So WeRead's reader window drawing its first frame counts as a window appearing, one GC16 for the whole screen -- **by design, and the design's goal is exactly "flash once"**. Whether to change it is a separate question, but it is not a defect and not a consequence of the starting-window setting.

### One thing still unexplained

It happened once on 2026-10-28: after the book had downloaded, the page still said "loading". The framebuffer captured at that moment already held **a fully typeset page of text**, and the application did not use another tick of CPU after it, while the device went to sleep only nearly ten minutes later -- ten minutes with the screen on. SurfaceFlinger recorded `activeBuffer=[1072x1448:1072, 2]  queued-frames=0` for that layer, so the buffer had been consumed. **Why that finished frame never reached the panel is not understood, and it has not reproduced.** Finding out means capturing `dumpsys SurfaceFlinger` and `/sys/class/graphics/fb0` on the spot when it happens (the latter is not readable from the shell; it needs a userdebug image or root).

## 12. Full machine code halves the time to open a book (2026-10-28)

Section 6's six paired rounds measured a cold start before signing in, with an empty shelf. This section measures the compiler filter over a whole use: loading the shelf and opening a book. It was prompted by the reader reporting that the device was slower than when it was first used.

**Method.** The same device, the same signed-in account and the same shelf, started by **tapping the icon** on the home screen (not `am start`). The order is A A B B A A: two rounds at `speed-profile`, compile to `speed`, two rounds, compile back, two rounds. After each compile the device is **left alone for three minutes**, because section 6 records that a round taken right after dex2oat at full tilt is not comparable.

| round | filter | odex | tap to quiet | process CPU | GC |
|---|---|---|---|---|---|
| base A | `speed-profile` | 7.1 MB | 99.8 s | 77.6 s | 9, 10.3 s |
| base B | `speed-profile` | 7.1 MB | 88.9 s | 69.7 s | 8, 11.6 s |
| **B1** | **`speed`** | **100.3 MB** | **55.4 s** | **30.8 s** | 9, 11.5 s |
| **B2** | **`speed`** | 100.3 MB | **47.3 s** | **29.5 s** | 10, 8.0 s |
| A3 | `speed-profile` | 1.0 MB | 104.8 s | 74.3 s | 8, 12.6 s |
| A4 | `speed-profile` | 1.0 MB | 89.0 s | 70.5 s | 9, 15.5 s |

**Wall 95.6 → 51.4 s, down 46%. Process CPU 73.0 → 30.2 s, down 59%.** The four `speed-profile` rounds fall between 88.9 and 104.8, the two `speed` rounds between 47.3 and 55.4: **the groups do not overlap.** The device was then compiled back to `speed` and timed twice more, 56 s and 50 s, with the B group.

**Three things make this hold up:**

1. **A3/A4 come back to 89-105 s**, so the machine did not drift over the hour and the difference is not the order the rounds were taken in.
2. **The bias runs against the finding.** The two `speed` rounds followed 1279 s of dex2oat at full tilt with only three minutes of settling, which by section 6 should handicap them. They were twice as fast anyway.
3. **GC barely moves** (10.3-15.5 s for A, 8.0-11.5 s for B). The 43 s saved is all in the application's own CPU: **it is the cost of interpretation.**

### Why section 6's six rounds did not see this

They measured a cold start **before signing in, with an empty shelf** (about 37 s). Far less code runs on that path, and most of what does is already in the profile. Loading a shelf and opening a book runs the EPub parser, the CSS parser and per-character typesetting -- methods the profile does not have, so they run interpreted from end to end. **Same application, two workloads, opposite conclusions**; section 6's numbers are not wrong, they answer a different question.

**An idle JIT does not mean nothing is interpreted.** The JIT thread only *compiles*; a method with no machine code runs in the **interpreter**, whose cost is charged to the thread running it and never passes through the JIT thread at all. The 0.0-0.6 s section 10 measured for the JIT thread pool can mean precisely that code is being interpreted.

### What it costs

| | `speed` | `speed-profile` |
|---|---|---|
| one compile | 1250-1279 s | 144 s |
| odex | 100.3 MB | 1.0-7.1 MB |
| resident (section 6) | 210.6 MB | 181.1 MB |

`/data` is 786 MB used with 978 MB free, so the space is there. The extra resident memory is not small on a machine that kills the input method, so this has to be weighed together with section 10's lever (room between collections); what most of that figure actually is, see item 5 of the verification below. Related: at `speed` the total CPU is 30 s while GC is still 8-11.5 s, so **GC goes from a sixth of the application's CPU to a third**, and that lever gets heavier.

### The design: install at `quicken`, compile to `speed` when idle

Installs stay at `quicken` (about two minutes, end of section 6); the background compile job takes an application to `speed` while the device is idle (`pm.dexopt.bg-dexopt=speed` in `pw3.mk`), once, and it lasts. A Kindle is idle most of the time. Taking AOSP's job as it is meets two obstacles, each removed by one patch:

- **AOSP's background compile job requires the device to be idle and on a charger**, and a Kindle is rarely on a charger. `patches/aosp-frameworks-base/0066` replaces "on a charger" with "battery not low" and keeps "idle".
- **dex2oat writes in place.** installd deletes the old artifact and creates a new one (the readings taken mid-compile on 2026-10-28 are below). The old machine code is gone the moment compilation starts and the package is `invalid` for the whole window -- open it then and **there is no machine code at all, everything interprets**, which is worse than not compiling; and if the compile is interrupted, installd's cleanup deletes the half-written file, leaving the application with neither old code nor new. The job is cancelled the moment the reader touches the device, so a twenty-minute compile is easily interrupted. `patches/aosp-frameworks-native/0009` builds each artifact at `<path>.staging` and renames it over the old one when dex2oat succeeds (the oat last), leaving everything alone until then.

```
base.odex   0.6 MB   ← growing from zero towards 100.3 MB
dumpsys package dexopt →  arm: invalid[]
```

What this gives up is the reference profile, which only a profile-guided filter writes. Nothing here needs it: `speed` compiles every method, and the application image that profile-guided compilation also brings was measured at about 0.4 s (section 6). The applications in the image are not affected: they are all compiled to `speed` ahead of time (`PRODUCT_DEX_PREOPT_DEFAULT_COMPILER_FILTER`), and the background job does not recompile them into `/data`.

### Verified on the device (2026-10-28)

**1. The invariant holds on the device.** Compiling WeRead back from `speed` to `quicken`, read every 10 s through the whole run:

| at | the real `base.odex` | `.staging` | `dumpsys package dexopt` |
|---|---|---|---|
| before | 105,193,896 | — | `kOatUpToDate` |
| 10 s | **105,193,896** | 0 | **`kOatUpToDate`** |
| 20-60 s | **105,193,896** | 596,372 | **`kOatUpToDate`** |
| 70 s | 1,057,192 | gone | `kOatUpToDate` |

Before the change the same thing looked like this: `base.odex` growing from zero, and `arm: invalid[]` for a full twenty minutes.

**2. The job registered with the new constraints**: `Requires: charging=false batteryNotLow=true deviceIdle=true`.

**3. The device compiled it by itself.** Left at `quicken` with the screen off and read every 10 minutes (`dumpsys` is not user activity, so it does not reset the idle timer):

```
minute 0     IDLE unsatisfied    filter=quicken
minute 10    IDLE satisfied      filter=quicken     <- the job starts
minute 20                        filter=quicken     <- compiling
minute 30                        filter=speed       <- done
```

Seventy-one minutes of screen-off (`IdleController`'s `INACTIVITY_IDLE_THRESHOLD`, plus up to five minutes of window slop), then about twenty minutes to build a 100.3 MB odex, `kOatUpToDate`, no staging file left behind. **Nobody touched the device.**

**4. Opening a book.** Two rounds: 65.9 s / 38.5 s CPU, and 54.6 s / 31.0 s CPU, against the four `speed-profile` rounds above at 88.9-104.8 s / 69.7-77.6 s CPU. The first round is high because the page cache was cold -- the device had just been idle and 100 MB of machine code has to come off the eMMC; the second sits with the manually compiled group (47-56 s / 29.5-30.8 s CPU). **CPU goes from just over 70 seconds to 31.**

**5. Resident memory: section 6's difference is not about the thing it sounds like** (evening of 2026-10-28, WeRead at `speed`, one cold start behind it):

```
.oat  mmap     3,245 KB   <- 3.2 MB resident of a 100.3 MB odex
.dex  mmap    63,201 KB
.art  mmap     1,944 KB
Dalvik Heap   20,882 KB
TOTAL        142,877 KB (139.5 MB)
:seg          21,327 KB
machine-wide: MemFree 50 MB, Cached 269 MB, 73 MB in zram
```

**Machine code is a file mapping, not something read into memory.** Of a 100 MB odex only the 3.2 MB actually executed is resident, and those are **file pages** -- the kernel can drop them under pressure and read them back when needed. What pushes other processes towards being killed is anonymous memory (the heap), and `speed` adds almost none. So section 6's resident differences (`speed` 23 MB above `quicken` and 30 MB above `speed-profile`) measured something else (how the reading was taken was not written down) and cannot be taken as evidence that this machine will kill processes more often because of it.

Whether days of ordinary use kill processes more often **has not been verified**. The device keeps its own record of it (`dumpsys procstats --hours 24`, and `am_kill` in the events buffer).

**6. A baseline over the first two and a half hours** (from 2026-10-28, a read-only sample every half hour, at `speed`):

```
MemFree      37-51 MB      steady
Cached      262-267 MB     steady
zram in use  71-73 MB      not growing
pswpout      49531 -> 49531   not one page swapped out in the whole stretch
pgmajfault   169 over two and a half hours
processes killed   0
```

The device was mostly idle over that stretch, so this is **a baseline, not a conclusion**: it says `speed` creates no memory pressure at rest, which agrees with machine code being a file mapping.

**One behaviour worth recording**: the log carries `JobServiceContext: Client timed out while executing (no jobFinished received), sending onStop`. JobScheduler allows one execution ten minutes and compiling WeRead takes twenty, so the stop signal always arrives part way through. The compile still finishes, because `abortIdleOptimizations()` is checked **between packages** and does not interrupt the one in flight. The cost is that when more than one package needs compiling, an idle window finishes only the first and the rest wait for the next one. Here the only packages are the ones the reader installed, so that is enough -- but it is behaviour, not luck, and it is written down so nobody later files it as a defect.

Also: dex2oat runs at nice 10 and **is not in the background cpuset** (`cpuset:/`, `cpu:/`). By CFS weights a foreground application gets roughly nine tenths of the core and the compile drops to a tenth of its speed -- so it does not freeze the device, but both sides are slow.
