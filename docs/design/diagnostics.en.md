# Diagnostics, and the way out of a failed start

This document covers two things: when something goes wrong in a release, how the evidence (the report area and the log folder) gets from the device into somebody's hands; and when an Android start fails, how the device gets itself back to the stock system and how the start screen tells the reader about that way out.

## What has to be solved

When something goes wrong in a release and there is no channel made for it, there is **no evidence to be had**:

- The reader has no adb. A `user` build deliberately has it off (`release-build.en.md`), and on a `userdebug` build adb may be exactly what is broken -- during development there was a start with nothing on the USB bus after the cable went in, so even "why not" could not be read.
- The kernel's ring buffer is 32 KB (`log_pos_goal=0x8fff0000`, shared with the stock system). After a few hours of use the boot is long overwritten: one `previous-kernel.txt` collected during development had three lines left, all from the PMIC.
- User-space state (`sys.usb.config`, `init.svc.adbd`, `sys.pw3.init_error`, the refresh counters, memory) is not in the kernel log at all. The sampler `pw3-boot-report` does not print them into the kernel log either; it writes them only into the report area described below.

What is wanted is exact: **an ordinary reader plugs in a cable and sends the whole of `android/logs/<time>/`, and the state of the last Android start is in it.** The reader does not have to understand anything.

## Three ways, two of them refused

### No: loop-mount the data image from the stock side

The easiest thing to write would be for the launcher (running under the stock kernel, as root) to `mount -o ro,noload -o loop android/data.img` and read what Android wrote into `/data`. **The stock kernel has no ext4**:

```
CONFIG_BLK_DEV_LOOP=y
CONFIG_EXT2_FS=y
CONFIG_EXT3_FS=y
(no CONFIG_EXT4_FS)
```

(Evidence: this machine's `/proc/config.gz`, 555 options.) The data image is ext4 with extents, made by `make_ext4fs`, and the ext3 driver refuses outright on `INCOMPAT_EXTENTS`. So this way does not exist on the stock kernel.

### No: have Android mount the FAT user area read-write and write a file

Android has vfat, so technically `mmcblk0p4` could be mounted and a file written. The reason this is refused is that **the risk lands on the one partition that must not go wrong**: the system image and the data image are two files in that FAT partition. One more kernel writing FAT metadata is one more way to leave a dangling cluster chain when Android loses power, and the stock system's `dosfsck` would then "repair" the very table that holds the images. That is not worth a page of diagnostics.

### Yes: a pre-allocated report area of fixed size

Make the report area **an ordinary file whose size never changes**, so that Android writes only its own sectors and touches no FAT structure at all:

1. **The installer creates** `android/report.bin`, 64 KB (`ensure_report_area` in `install.c`). What it writes is a placeholder report -- a body of just `installer_made=1`, zeros after it -- written through once over the whole 64 KB, fully allocated and not sparse, like the images. A placeholder rather than zeros, so that the file can be recognised as this project's from its own bytes: turn over one byte of an area of zeros and neither the installer nor preinit will recognise it again, whereas a report whose magic is damaged still has a first line that parses and a body that checksums to what it declares, so it can be recognised and made again (`pw3_report_damaged_magic`). Every tap checks it: it is made if it is missing, and made again if its size is wrong or its magic is damaged. Two reasons -- a device that installed an older version needs one, and if somebody deleted or truncated it from a computer, preinit would map it short by its extents and Android would write into clusters that are no longer its own. It is diagnostics, so it gives way to everything (the `installer` tests cover the three rules below):

    - **If it cannot be made, it is not made**: one line in the log, and the install carries on.
    - **It does not overwrite a file that is not ours.** `android/` is the installer's folder, but that is a reason to own the five names it writes, not a reason to overwrite a sixth file whose name happens to match. When the size is wrong, the file itself decides, and two kinds count as ours: one that begins with the magic (Android wrote a report and a computer later truncated or extended it; a file shorter than the magic counts when it is the beginning of the magic, which is what a write cut short or a truncation leaves), and one no longer than a report area whose every byte is zero (including an empty file). Every byte, not the first few -- a file that starts with zeroes and continues with somebody's data is somebody's data; the cost is at most reading 64 KB, because a file longer than the report area cannot be the one that was never written. Anything else is left as it is and said so in the log. Nor does one that cannot be read count: "do not know" is exactly the case this rule exists for.
    - **It does not spend the last of the reader's storage.** This path is the one where there is nothing to install, so it does no space check at all; and this tap's own log is written after it, and those writes have to succeed (`write_result` in `helper.c` uses `require`). So it asks for more than 1 MB left after the report area, or it does not make one. The threshold is not the installer's 64 MB reserve: storage with 64 MB left does not need the report area withheld from it.
2. **preinit maps its sectors**: PID 1 in the new kernel (preinit) measures `system.img` and `data.img` by cluster with FIBMAP and checks that they do not cross (`preinit/TestFileMapper.h`, `FatCrossLink.h`, the `fat-cross-link` test). The report area is a third `pw3_file_mapping`, under the same checks and the same "FAT stays read-only", mapped as `dm-3` (`preinit/TestFileInit.h`). Only a file of exactly 64 KB that begins with the magic or is all zeros is mapped; clusters that cannot be measured, or a mapping that cannot be made, only mean this start writes no report and do not stop it; clusters that cross an image or belong to another file on the FAT refuse the whole start.
3. **Android writes it**: the oneshot service `pw3-boot-report` starts at `sys.pw3.storage_ready=1` and does not wait for `sys.boot_completed` -- a start that never gets that far is the one whose report matters most. It samples four rounds over two minutes and then writes the whole 64 KB in one go to `/dev/block/dm-3` (`initcheck/BootReport.cpp`, `initcheck/pw3-boot-report.rc`). What it writes is fixed fields plus how many warnings each of a few system components logged (component names and counts only, never the log text; see "What the report says" below); it reads no path name out of `/data`. Whether or not the write succeeded, it says so in the kernel log under the `pw3-report-area:` tag.

    PID 1 writes it too, but only when it **refuses to start**: `stop()` writes the line it has already assembled (with the first failed system call of this start) into it (`report_stop` in `preinit/PreInit.c`, with the write itself in `report/ReportStop.h`), under the field name `preinit_stopped=`, which is fixed in a header both sides share. It is written **before** the 120-second wait, so somebody who pulls the power still gets it. A `stop()` before the mapping has nowhere to write and does not -- and the two refusals that let the next tap act (the system image hash does not match, the system image's superblock is wrong) both come after the mapping.
4. **The next launcher takes it out**: the report area is an ordinary file in the FAT, the launcher reads it as it reads anything, and it goes into `android/logs/<time>/android-report.txt`, in the same folder as the kernel log (`save_android_report` in `helper.c`, right after the previous kernel log). The filter is `pw3_report_keep`: the kernel log's allow list decides who printed a line, and every line of the report is written by the sampler or by PID 1, so that gate has nothing to decide here; what remains is the second gate -- the identifier check (MACs, runs of twelve or more hexadecimal digits, `ssid`/`serial`/`command line`), which is written for exactly the line nobody anticipated. The file carries one `pw3-android-report-v1` line at each end with the boot number, the length and two counts, the same shape as a filtered kernel log, so whoever opens it can see at a glance that it was filtered and by how much.

    The same giving way as on the installer's side: this copy too is something left behind in passing, and the files this tap has to write come after it, so it asks how much storage is left first (`pw3_store_has_room`: more than 1 MB after it) and skips it if there is not enough. The previous kernel log, `previous-kernel.txt`, follows the same rule -- the two fail in exactly the same way: a file left in passing uses the last of the space, and then the start dies on a write that had to succeed. Storage that cannot answer (a failed `statvfs`) counts as no room -- not knowing cannot matter more than the start itself; what goes in the log is the number of bytes storage reported, and when it cannot answer, a number no storage could have, so that "why is this file not here" has an answer inside the folder that was handed over.
5. **Nothing has to be written back**: the launcher only reads; the installer writes only when it makes or remakes the area, Android only during a start, and the two never touch it at the same time. **Every tap takes whichever copy is valid then, and nothing records that it was taken.** Keeping the boot number in `android/state` and not taking a report twice was considered and not adopted: `android/state` is the installer's file with its format pinned by the installer's tests, and making the launcher a writer of it to save one duplicated 64 KB copy is not worth it; a duplicate is not an error anyway, since `boot=` says which start wrote it and the same number in two folders means one report read twice, which the reader can tell.

### What the report says

One item per line. The sampler takes a round at 0, 30, 60 and 120 seconds after the service starts (each round limited to 5 seconds), so a field appears more than once, and the last reading is how far this start got:

| Group | Fields |
|---|---|
| Start of each round | `snapshot=<round> uptime_s=<seconds since boot>` |
| This start | `ro.build.version.incremental` (which release wrote it); the boot number is `boot=` in the first line |
| Initialisation | `sys.pw3.storage_ready`, `sys.pw3.hardware_ready`, `sys.pw3.init_error` |
| USB | `sys.pw3.usb.phase`, `sys.pw3.ffs_ready`, `sys.usb.ffs.ready`, `sys.usb.state`, `init.svc.adbd` |
| Services and boot progress | `init.svc.zygote`, `init.svc.surfaceflinger`, `init.svc.bootanim`, `init.svc.pw3-fg-observe`, `sys.boot_completed`, `dev.bootcomplete`, `service.bootanim.exit` |
| Memory | `MemFree`/`Cached`/`Buffers`/`AnonPages`/`Slab`/`SwapFree` from `/proc/meminfo`, and `/proc/buddyinfo` |
| Input | Whether the power button's and the touch screen's input devices are there |
| Display | The framebuffer's geometry, the refresh modes the EPDC driver reports, and a sample of 16 rows of the screen's pixels (grey range, number of levels, a hash) |
| System log | `android_error_count`: how many logcat lines at warning level or above each of a fixed set of system components (SurfaceFlinger, AndroidRuntime, ActivityManager and others) logged, as in `AndroidRuntime=2`. Names and counts only, never the text: the text is whatever an application wrote and may carry a file name or a URL, and this file gets handed on. The names come from a fixed list in the code, never from a string the device supplied |
| End | `complete`; or `stopped at the snapshot deadline` when a round ran out of time |
| Refused start | `preinit_stopped=<reason>`, written only by PID 1 (step 3 above) |

64 KB is far more than four rounds need; a line that does not fit is not written.

The format (`device/amazon/pw3/report/ReportArea.h`, one header shared by both ends) is text rather than a struct: both sides are little-endian ARM today, but text needs no agreement about alignment or byte order, anybody can open it in an editor, and it can go through the same filter as the kernel log. The first line declares what follows:

```
PW3-REPORT-1 bytes=<n> crc32=<8 hex digits> boot=<n>\n
<n bytes of body, one item per line>
```

The checksum is CRC32 rather than SHA256 precisely because this line is copied into a folder that will be handed over, and the filter drops any line with twelve consecutive hexadecimal digits in it (`userlog.c`; that rule was written for this). A 64 KB write is not atomic, so the reading side treats a failed checksum as no report -- and all zeroes (an area made by an older installer) is not a report either, and not an error; the installer's placeholder is recognised by `installer_made=1` in its body and is not taken for the report of any start. `boot=` stops at 32 bits (4.2 billion starts is beyond this machine), because it is the one value in the report that goes into a handed-over folder unaltered and that the line filter has nothing to say about: ten decimal digits cannot make the run of twelve hexadecimal the filter is looking for. The `report-area` test covers these, including two known CRC32 answers (so that the writer on the other side has something to check against), a body that exactly fills the area, and a first line with no end at all.

## Why not "make the kernel ring buffer bigger"

The other idea is to enlarge the 32 KB ring so that the boot survives until it is collected. Refused: the address and size of that buffer are read from a fixed address by the stock side (`log_pos_goal`), so enlarging it means changing both sides, and **it would still hold only the kernel log** -- not one byte of the user-space state, the refresh counts or the memory. The report area solves what is in it, not how long it is.

## Two problems the report area settles at the same time

Both problems have one root: **the stock side cannot know what Android did**. Without a report area, both can only be guessed at by heuristics. With one, the installer reads it first on every tap (`read_report` in `install.c`) and reads instead of guessing.

1. **A system image damaged without changing size cannot be repaired by tapping again.** The installer does not reread the 512 MiB image on every tap (that would cost about 19 seconds each time, extrapolated from the 20.9 s measured at 576 MiB); it judges whether the image is in place by the hash in its state file. When the image's content is broken and its size is the same, the state file still says `system=ready` and the installer hands over as usual; preinit refuses the image (`pw3-preinit: test system image SHA256 rejected`) and restarts back to the stock system about two minutes later. Without a report area, tapping again repeats the same path -- **the reader sees only the start screen, with no error code, and the only way out is to connect a computer and delete `system.img`**, which is precisely what somebody who only knows how to plug in a cable cannot do. Now preinit writes the reason into the report area; the next tap, finding a report whose number differs from the one already handled and whose reason is a system image hash mismatch or a bad superblock, clears `system=ready`, shows a screen of explanation first (`PW3_SCREEN_REPAIR`) and installs the system image again, leaving the data image alone. An image that merely could not be read through (`could not be read through`) is not reinstalled: that says nothing about the content, and reading it again is enough. Without a report area, "this time's refusal" and "left over from last time" could only be told apart by scanning the kernel ring for that line plus a hash of the buffer's contents -- a heuristic, and so not done.
2. **The "still compiling" notice after an upgrade must not be used up by taps.** An upgrade that changed the boot image recompiles the applications in `/data` on its first start. If the notice showed only on the first three taps after the install, and all three were interrupted before the compiling finished, the fourth would show the ordinary screen, which says nothing about compiling while the device is in fact still compiling -- leaving the reader with no way to know why this start is taking so much longer. (That screen was worse before 2026-10-30: it said "hold the power button if nothing appears in ten minutes", which is an instruction to force a restart in the middle of a compile. It now names no duration and says not to force a restart, so what is missing here is only the "why".) So the state file holds `compiling=1`, which no tap counts down; it is cleared only by a report that proves the compiling finished: the last sample in it has `sys.boot_completed=1` (the compiling happens in `updatePackagesIfNeeded()` in `SystemServer.startOtherServices()`, and `systemReady()` comes over 700 lines later in the same function, so `boot_completed` is necessarily after the compiling), its `ro.build.version.incremental` is the release this entry installs, and it was written after this install (the number the installer plants when it makes the area is the floor). An application with no reference profile is compiled with `quicken`, 60-93 seconds each (a full compile takes 1214), so the "still compiling" window is minutes long.

## Verification on the device

- Verified on the device: Android writes a report, and on the next tap the launcher takes it into `android/logs/<time>/android-report.txt`.
- That the report area's sectors cross neither image does not rest on a one-off check on the device: preinit checks it on every start (step 2 above; `pw3_file_maps_bounds` in `TestFileMapper.h`, `FatCrossLink.h`).
- Verified on the device (2026-11-02): **one start writes one report, and two consecutive starts number them one apart.** Each of the two collected reports carries a single `pw3-android-report-v1 boot=` header, four samples inside, and `complete` with `kept=97 left_out=0` at the end; the numbers are `boot=1793478277` (the start of 11-01 21:45) and `boot=1793478278` (11-02 06:33). The state file's `android=1793478278` is the second of those to the character, so collecting the report and rewriting the state happen in the same tap.
- Verified on the device (2026-11-02): **the FAT is clean after a report has been written.** With the device back in the stock system and the user volume mounted on the computer, `fsck_msdos -n /dev/rdisk4s1` (through `diskutil verifyVolume`, read-only, writing nothing) passed all three phases with exit code 0: no cross-linked chains, no orphan clusters, nothing wrong in the directories. A second, more direct piece of evidence: `report.bin`'s mtime still stands at the moment the installer made the area (device time 2026-11-01 04:24), and two Android starts have written into it since -- **written through, with the mtime untouched, is what "it touches no metadata" means.** Note that this was macOS's `fsck_msdos`, not the stock system's `dosfsck`: the same FAT consistency, a different implementation.
- Verified on the device (2026-11-02): **the version the report records is the one this entry installs.** Every sample in the report says `ro.build.version.incremental=eng.builde.20261031.051622`, and `build_incremental` in the entry's `launcher/release.h` is that string to the character -- it is read out of the system image itself by `boot/build_launcher.py`, and it is the one `install.c` compares to decide whether the system **this** entry installs has finished compiling (`install.c:1578`). One thing not to "unify": the same image has `ro.build.version.incremental=eng.builde.20261031.051622` while the segment inside `ro.build.fingerprint` is `builde10310516`; both spell the same instant (10-31 05:16), and the installer uses the former.
- Not yet verified on the device: power cut at the moment a report is being written; afterwards the FAT is still clean and the report area holds either the old report or the new one (a 64 KB write is not atomic, which is why the header carries a length and a checksum and a failed check reads as no report).
- Not yet verified on the device: the path where preinit refuses to start, writes a report, and the next tap reinstalls because of it. It runs only on a refused start; seeing it means deliberately damaging one byte of `android/system.img` first.

## The way out of a failed start

### The problem

During development the device showed something only a real person using it would meet: with a defective system image, Android **restarts the framework over and over**, the screen flashes continuously, and there is neither a home screen nor USB. To somebody who "only knows how to plug a cable into a computer", that looks like a broken device.

### First, make the device get itself out

In that state Android's own `RescueParty` escalates all the way to "factory reset": that time `system_server` crashed every 14 seconds and reached the last level 44 times in 18 minutes, and the data survived only by luck -- this machine has no `/misc` partition and no recovery to enter, so every one of its reboot requests failed.

Patch `0045` (`ro.rescue_party.no_wipe=true`) changes the last level from "wipe user data" to "restart". A restart here means leaving Android and returning to the stock system, so that path stops being a last act of destruction and becomes **an automatic way out**:

- `RescueParty`'s threshold is five times in 300 seconds, one level per crossing, four levels in all. A restart loop like the one above, one every 14 seconds, reaches the fourth level in about 20 restarts, under five minutes.
- The fourth level runs inside `noteBoot()`, by which time `PowerManagerService` is published but not yet `systemReady`. AOSP leaves a path for this: `shutdownOrRebootInternal()`, finding the system not ready, calls `lowLevelReboot()` directly when `RescueParty.isAttemptingFactoryReset()` is true. And `incrementRescueLevel()` writes `sys.rescue_level` as 4 before executing, so that test holds and the restart happens at once.
- Should it not hold, `executeRescueLevel()` is wrapped in `try/catch (Throwable)`, so an exception is only logged and does not make `system_server` crash once more -- the safety net does not become a new source of crashes.

Which is to say: **with patch `0045`, a crash loop returns to the stock system by itself within a few minutes.** So do the two failed start checks -- storage and interfaces each ask for the restart themselves (`initcheck/pw3-boot-report.rc`) -- and so does a start the entry refuses, which `stop()` in `PreInit.c` restarts after 120 seconds. That is why the start screen says the Kindle *usually* restarts itself back into its own system. **"Usually" is not politeness**: a freeze that crashes nothing and fails no check is covered by none of those four, and holding the power button is what that case gets.

### First, state the facts

- **This machine has no automatic boot loop.** Android is entered once, from the library entry in the stock system; any restart -- including a forced one from holding the power button -- goes back to the stock system. If nobody taps, Android is not entered again.
- So the cost of one failure is "this attempt did not get in", not "the device cannot get in". The stock system is this machine's recovery environment: from there it can be reinstalled, an older version installed, or nothing done at all and the Kindle used as a Kindle.
- **A damaged image has its own check**: when there is no record of a verification, or the image file's size, modification time or extents on the FAT differ from the record, preinit reads the whole system image and verifies its SHA-256; when the record matches, it skips the full check on this start (saving about 20 seconds per start), and `pw3-system-verify` reads the image again in the background at the lowest priority once Android is up, deleting the record on a mismatch so that the next start verifies in full (`preinit/TestFileInit.h`, `SystemStamp.h`, `SystemRecheck.h`). When the check fails, preinit stops, writes the reason into the kernel log and the report area, restarts back to the stock system two minutes later, and the next tap reinstalls because of it. What a marker could catch beyond that is only "a build we released ourselves is defective".
- While the screen is flashing, no message can be shown at all, and preinit cannot draw on the panel either. **The only moment that can be seen is before the start.**

### What is done: the way out is written on the start screen

The device usually gets itself out, but the reader has to know that this is normal rather than one more thing broken; and a real freeze (nothing crashed, so `RescueParty` never fires) needs a manual way too. Both can only be said before the start, so both are on the start screen. **The screen does not sort the symptoms**: a screen that does not change is either a start that is working or the two minutes a refused one waits before restarting, the reader cannot tell them apart, and does not need to -- the answer is the same in both.

| Entry | Where | The words |
|---|---|---|
| Release | `BOOT` in `installer/screens/render_screens.py` | The screen may not change for a long time while Android starts, and that on its own does not mean it failed; do not force a restart. When a start fails, the Kindle usually restarts itself back to its own system within a few minutes, and your books are not touched. If it does not go back by itself, hold the power button until the screen changes -- up to about 40 seconds -- which goes to the same place. If you end up back in the library, tap it again: a damaged system file is found and written again on that tap, with no computer needed. Your books and app data are in a separate file and are not affected. |
| Release (the first time after an update) | `BOOT_UPGRADE`, same file | First that the installed apps are being compiled for the new system, three to five minutes per app, and not to force a restart; then a paragraph: "When a start fails, the Kindle usually restarts itself back to its own system within a few minutes, and your books are not touched." |
| The host client test | `display()` in `boot/launcher/helper.c` | `ANDROID BOOT 3 MIN. IF STUCK HOLD POWER 20S.` -- that program is built on its own by `boot/handoff/tests/run_client_test.py`, without the installer, and is in no entry |

When things go normally those words stay on the screen during the start, so on a failure they are the last screen the reader sees; the redraw after the handover is best effort (below), so "the last screen" is not something the code guarantees.

**The first screen names no duration** (2026-10-30, the owner's decision). Which screen is drawn *before* the handover depends on the case (`helper.c` picks between `BOOT` and `BOOT_UPGRADE` by `install_upgraded`); **after the handover it is always the generic `BOOT`**, because `start-picture.h` holds only that one. And after the handover it stands until Android's first frame: about 3 minutes on a plain start, and minutes for every installed app on an upgrade that changed the boot image. Any single duration is false in one of those two cases, and a reader who acts on the false one force-restarts a device that was working, in the middle of a compile.

**What also cannot be written is "a still screen means it is working"** (found by a Codex review, 2026-10-30). For part of that day this screen said "only a screen that flashes over and over means it could not start", and the counterexample is in this tree: `stop()` in `PreInit.c` writes the reason into the report area and then sleeps `STOP_SECONDS` -- 120 of them -- before rebooting, and draws nothing at all. A start the entry refuses stands perfectly still for two minutes and then goes back to the stock system on its own. Telling the reader that a still screen means a healthy start is a sentence this program itself disproves. So the screen names neither a clock nor a symptom: what the reader needs is that waiting costs nothing and that there is a way back whenever they want one, and those two are the same answer in both kinds of failure -- no need to make them sort their symptom first.

That takes two pieces of code to hold. After the handover comes the new kernel's display driver, which clears the panel unconditionally while initialising and then flushes the buffer it has just allocated -- and that buffer is zeroed, and zero is black; left alone, the screen the installer drew with eips would not survive the driver load and the panel would be black from that moment until Android's first frame. So the kernel fills the buffer white before that refresh (patch 0403), and the handover program redraws the generic `BOOT` screen with **the same layout and the same fonts** as soon as the driver is up (`boot/handoff-init/make-start-picture.py`, whose words come from the same constants in `render_screens.py`).

**That redraw is best effort, not a guarantee.** There are four ways it does not happen, each writing one line to the kernel log while the boot carries on (`pw3-handoff-init.c`):

1. **The driver did not load** -- `show_start()` is never called at all (`if (loaded)`);
2. **One of three early returns inside `show_start()`** -- `/dev/fb0` not there within five seconds, geometry unusable (not 8-bit, or the visible window reaching past the mapping), `mmap` failing;
3. **`refresh()` failed** -- the picture reached the buffer, but that EPDC update was not sent or never completed;
4. **The whole hardware child was skipped or killed on its timeout** -- it runs in a child precisely so that it can be bounded.

What is left on the glass depends on which one. In cases 2 and 3 the driver has initialised, and the kernel filled the buffer white before its refresh (patch 0403), so the panel is white with no words. In case 1 the driver never initialised, so the glass most likely still carries the screen the installer drew with eips. **What patch 0403 guarantees is "not black", not "words".** So the accurate claim is that both sides of the handover carry words when things go normally, and that "these words are on the screen for the whole start" is not something the code guarantees. Since 2026-10-30 case 3 writes `SCREEN refresh failed` rather than `SCREEN up`; before that it reported success.

The screens' region is as tall as the tallest of them, worked out by `render_screens.py` at build time, and the build stops if that does not fit below the status bar and inside the screen; the `BOOT` region the handover program redraws is 944x1193 (`boot/handoff-init/start-picture.h`). `installer/install/test_screen.py` checks that the screen the C code assembles on the device is pixel-identical to the preview from the build.

### What is not done: recording on disk whether the last start succeeded

Another 4 KiB file was considered, mapped by preinit as a dm device, written `ok` by Android at `sys.boot_completed=1` and read by the entry script before the next start, so that a previous `attempt` would prompt first rather than starting straight away. On consideration it is **not done**:

- All it would add is a prompt when somebody taps again after a failure. The device already returns to the stock system by itself (`0045`), and what the reader should do (change version or report it) is on the start screen, so stopping to prompt does not change what the reader can do.
- The cost is not small: one more dm mapping in a verified start path, a changed `pw3_file_maps` receipt format (`InitCheck` following it), a new SELinux domain and a new boot service, and one more branch taken at every start.
- More to the point, the direction is wrong: this code is a safety net, and a safety net that goes wrong becomes a new source of failed starts -- which is the very thing it is there for. The lesson of that restart loop is to put less into the start path that can only be verified on a device, not more.

If evidence ever appears of "the same version failing over and over with the reader unable to tell what is going on", it can be added then along the lines above: the file, the mapping and the write point are all thought through.
