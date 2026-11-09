# Known issues

What is written down here is known, has been looked into, and has been decided
against changing; log noise that was looked into and found harmless is here too,
so that the same line is not looked into twice. Sections 6 to 9 are defects that
came up during development and are fixed; the fixes carry trade-offs and limits
of their own, which is why they are here. Chinese: `known-issues.md`.

Each one says what is seen, what it turned out to be, why it stays, and what
would change that.

## 1. The browser sometimes dies as it starts

**Seen**: tapping EinkBro, the application disappears about 4.5 s later and
the system says "EinkBro has stopped". Tapping it again works.

**Cause**: EinkBro constructs a `TextToSpeech` on a coroutine and hands it a
`Context` whose base is not attached yet, so `mContext.getContentResolver()`
inside `TtsEngines.getDefaultEngine()` throws. It is the application's own
race; nothing on this path is changed by this project.

**When it happens**: only while the device is very busy. It happened once
during development, on a first boot, with dexopt having just compiled WeRead
and the verification scripts running; three launches straight afterwards on the
same build were clean (4.8 s and 4.5 s cold starts).

**On 2026-11-02 it was chased deliberately and would not reproduce.** Two
builtin busy loops on the device (`sh -c 'while :; do :; done'`, nothing
spawned per round) held the single core down and pushed the one-minute load
from 1.68 to **9.19** -- higher than the 9.11 that starved the system UI into
an ANR in section 14. Under that load the browser was cold-started **8 times
and survived all 8**, with `logcat -b crash` empty throughout. So the frequency
here is "once during development, and nothing in 8 attempts under load", not
"it crashes when the machine is busy".

**If it ever becomes reproducible, the fix is on our side, but it is not catching the one line
that throws**: that line is AOSP's own, `mContext.getContentResolver()` in
`TtsEngines.getDefaultEngine()`
(`frameworks/base/core/java/android/speech/tts/TtsEngines.java`). Catching it there is not
enough: the next step is `getHighestRankedEngineName()` → `getEngines()`, where the same
unattached `Context` throws again on `mContext.getPackageManager()`, and `TtsEngines` cannot
reach the callback anyway. The catch belongs in `TextToSpeech.initTts()`, around the whole
engine lookup, followed by `dispatchOnInit(ERROR)` -- `TextToSpeech` already has an error
channel for exactly this, `OnInitListener.onInit(ERROR)`, which is what "no engine in the
system" uses.
Turning a constructor-time NPE into that `ERROR`, rather than letting the
process die, is what the channel is for. **Not done now**: what cannot be
reproduced cannot be verified, and a try/catch nobody can test is no better
than none (the lesson behind `checks-can-pass-while-switched-off`); besides,
this device has no speaker, TTS is useless on it, and the crash has been seen
once.

**Why it stays**: the APK is upstream's signed release and its code is not
changed (`device/amazon/pw3/apps/EinkBro/PW3-SOURCE.md`). The system carries
upstream's 16.7.1 (released 2026-10-11), and its notes do not mention this. The
device has no speaker, so TTS is useless here, but the crash happens before any
engine is looked for and no setting avoids it.

**What would change it**: upstream fixing it, or the crash becoming ordinary
rather than rare -- that would mean choosing a different browser.

## 2. avc denied in the log

SELinux is enforcing. One ordinary boot plus a six-minute soak leaves a dozen
or so `avc: denied` lines. All of them have been looked into:

| Line | What it is | Does it matter |
|---|---|---|
| `untrusted_app` → `graphics_device:dir search` (about 6, `comm="Chrome_InProcGp"`) | WebView's GPU process looking at `/dev/graphics` | No. AOSP does not let ordinary applications see that directory; WebView falls back to software, which is what this device does anyway |
| `netd`/`wificond` → `module_request kmod="netdev-wlan0"` | the kernel asking to auto-load the network module by alias | No. init loads the module explicitly (system/core patches 0003/0004), `wlan0` is already there and Wi-Fi works |
| `netd` → `module_request kmod="netdev-dummy0"` | netd wanting a dummy interface | No. There is no use for dummy0 here; the interfaces are `lo`, `sit0` and `wlan0` |
| `netd` → `module_request kmod="ipt_TCPMSS"` | an MSS-clamping rule in netd's rule set | No. That is for tethering and VPNs, and this device has neither: of the 72 iptables rules in place, none is a TCPMSS rule |
| `netd` → `SELinux: unrecognized netlink message type=20 for sclass=32` | netd asking to enumerate or destroy sockets by UID. 20 is `SOCK_DIAG_BY_FAMILY`, and this kernel is 3.0.35, while `sock_diag` arrived in Linux 3.3 | **A little.** SELinux's message table does not know it, so the request comes back EINVAL (the next lines in the log are `Failed to dump IPv4 sockets for UID: Invalid argument` and `Error destroying sockets`); and were it allowed, this kernel does not implement it either. The consequence is that **when Wi-Fi drops, an application's established TCP connections are not forced shut** and have to time out by themselves -- an application hangs a moment longer after the network goes away instead of failing at once. Once per network state change; it does not loop. **Why it stays**: the fix is not the one missing table entry but the whole of `sock_diag` (`net/core/sock_diag.c`, the `inet_diag` rework, `udp_diag` -- hundreds of lines across net/core, net/ipv4 and net/ipv6), which is a lot of network stack to move for a benefit nobody has measured. **This kernel does have a path of its own that does the same thing.** Lab126 added `/proc/tcpreset` in `net/ipv4/tcp_ipv4.c` (`proc_tcpreset_write`, under `CONFIG_LAB126`): writing "42" or "43" plus a list of ports to skip resets this machine's IPv4 TCP connections by mode -- which is what netd wants `SOCK_DESTROY` for. So fixing this is **not** the same as backporting all of `sock_diag`; nor is it a one-liner, though: netd's `SockDiag::destroySockets` **enumerates** with `SOCK_DIAG_BY_FAMILY` before destroying, and `/proc/tcpreset` has one granularity only -- reset everything, skipping these ports -- so destroying by UID is not available, and putting a path only this machine has into a core daemon like netd is a shim. **Whether to touch it, measure first**: how long an application actually takes to report an error after the network goes away. A few seconds and it is not worth touching; tens of seconds and it is worth discussing. |
| The first line of every boot: `proc_dir_entry '/proc/tcpreset' already registered`, with a page of registers and a backtrace | **The stock kernel's own defect, nothing to do with this project.** Lab126 creates `/proc/tcpreset` inside `tcp_proc_register()`, and that function is called once per address family -- `tcp4_proc_init()` and `tcp6_proc_init()` -- so the second call hits a name that already exists and `proc_register()` answers with a WARN and a full backtrace. The stack says so frame by frame: `tcp6_proc_init` -> `tcp_proc_register` -> `create_proc_entry` -> `proc_register` -> `warn_slowpath_fmt` | **None**: the second registration fails, `/proc/tcpreset` still exists once and still works (it is an IPv4 facility that IPv6 has no use for). It is worth fixing anyway: the kernel ring is 32 KB and this spends about 1.5 KB of it at 0.47 seconds, which is the part a panic capture needs most; and more to the point, the entry's log filter keeps `WARNING`, `Backtrace:` and register dumps **on purpose** (`boot/launcher/userlog.c`), so those twenty-odd lines go into every `android/logs/<time>/` folder the owner hands on, at the top, where they read as this project's own backtrace. This project's kernel fixes it (`patches/android-core/1056-tcpreset-registered-once.patch`, one `if`): the release kernel no longer prints it, and only logs from a kernel without that patch still carry it |
| `init` → `module_request kmod="configfs"` | It looks like AOSP's generic init.rc mounting configfs, but it was **this port's own `init.pw3-main.rc`**: that line and the two `/config/sdcardfs` lines under it, plus a `mount pstore` elsewhere with six lines under it. This kernel has neither filesystem, so ten init commands failed at every start | No. USB here is the legacy gadget path, configfs is not mounted, and adb and MTP both work. **Those ten lines are deleted** (`init.pw3-main.rc` keeps a note saying why), and a release's log no longer has this denial |

Of the denials in the table, all but the `sock_diag` one are "the kernel asked,
was refused, and everything carried on", not failures; **the only reason to
change any of them** is wanting the feature itself one day (tethering, say), and
then what has to change is the kernel configuration, not the SELinux policy. The
`sock_diag` one is different: it does stop something from working, and what is
missing there is not policy either but an interface this kernel never had.

**Do not read that as "every `avc: denied` is harmless."** Every `scontext` in
the table above is a domain AOSP brings with it. During development
(2026-10-21) one build installed and then refused to start because of a denial
whose `scontext` was `u:r:pw3_init_check:s0` -- one of this project's own
domains, missing one rule of its own policy, after which the start check
reported something that had nothing to do with the cause. The way to tell them
apart is direct:

- `scontext` is `u:r:pw3_*` -> **not normal.** One of this project's programs is
  being stopped by this project's policy. Add the rule its `tcontext` and
  `tclass` name (`sepolicy-device-paths` in `tools/run-tests.py` checks the
  `/dev` paths of this kind statically).
- `scontext` is another domain and is in the table -> looked into, carry on.
- `scontext` is another domain and is not in the table -> not looked into yet.

The log handed back to the Kindle drive keeps denial lines from `pw3_*` domains
(`boot/launcher/userlog.c`), so that there is something to read when this
happens.

## 3. `adb devices` goes empty after `adb root`: the device moved to the other adb server

This section concerns development `userdebug` builds only: a release is a
`user` build (`ro.debuggable=0`) and has no `su`.

**Seen** (measured 2026-10-21). After running `adb root`:

- the device is **still on USB** (`ioreg` still shows "Kindle Paperwhite 3
  development port" with the right serial);
- Android is **still running** (no Kindle drive appeared, so it did not go back
  to the stock system);
- but `adb devices` is empty from then on, and stayed empty for 80 seconds.
  The record from that day says it reappeared after the cable was replugged,
  but which adb server picked it up was not checked then, so that part is not a
  finding.

**Cause**: there are **two adb servers** on the development computer, and the
device moved from one to the other. `adb root` makes adbd exit and restart, the
server holding the device loses it, and the other one picks it up.

**What was measured** (2026-10-29, a `userdebug` build): one framework restart
(`su 0 stop`, three seconds, `su 0 start`), watching four things every 12
seconds for 100 seconds --

- the "Kindle Paperwhite 3 development port" entry in `ioreg` **never went away;
  the match count stayed at 3 throughout**;
- the device **moved from adb server 5097 to 5038**: before the trigger
  `5097=1`, `5038=0`; twelve seconds later `5038=1`, `5097=0`, and so it stayed.

So **a framework restart does not touch USB at all**; an empty `adb devices` only
means the server being asked no longer holds the device. The other ports were not
checked during the `adb root` occasion, but the symptom is the same one, and by
Occam's razor it is the same thing.

**Two explanations ruled out.** One was that configurations carrying adb are
enabled in two steps, and that after `adb root` the new adbd setting
`sys.usb.ffs.ready` to 1 again would be no change for init to trigger on, so the
gadget would never be taken through `enable 0` / `enable 1`. The sources say
otherwise: `PropertySetImpl()` calls `property_changed()` after every successful
update with no comparison to the old value, `property_changed()` queues it, and
`Action::CheckPropertyTriggers()` matches the trigger's value against the **new**
one and never looks at the old (`system/core/init/property_service.cpp`,
`init.cpp`, `action.cpp`). Setting 1 over 1 fires the section. The other was
that the gadget really does go down, on the evidence of
`system_profiler SPUSBDataType | grep -c` matching 0 times. That evidence was
bad: the command printed **nothing at all** in the restricted shell it was run
in (the sandbox blocked it), so "matched 0 times" said nothing about the bus. The
USB bus is read with `ioreg -p IOUSB -l -w 0`.

**What to do**: when `adb devices` is empty, **ask the other ports first** (5038,
5097, 5037) before suspecting hardware; to find out whether the device is on the
bus, use `ioreg -p IOUSB -l -w 0 | grep -i "Kindle Paperwhite 3"` and **not
`system_profiler`**. A measurement that needs a framework restart -- a paired
comparison, say -- only has to resolve the port again after each round; no replug
is needed.

**And it is not sleep** (measured 2026-10-29 on a development build).
"The device went to sleep, so it left the bus" is the obvious guess and it is
wrong: with the cable in the computer, 100 seconds of screen-off left the device
on the bus the whole time (`ioreg` steady at 3 matches, `adb` at 1, polled every
5 seconds) and the kernel log shows no `PM: suspend` in that window at all --
plugged into a host, this device does not suspend on screen-off. So when nothing
is on the bus, the cable is what to look at, not the screen.

**When root is needed**: do not use `adb root`. A `userdebug` build has `su`, so
`su 0 <command>` gets root without restarting adbd.
`tools/measure-apk-install.sh` and `tools/verify-device.sh` both do that, and the
second asks ports 5097 and 5038 when the default one has nothing, printing the
command to use.

The `onrestart setprop sys.usb.ffs.ready 0` line in `init.pw3.usb.rc` stays, but
**it is not a fix for this**. Its reason is this: adbd has gone, so the
descriptors it wrote have gone with it, and the property should not go on
claiming they are there. Nothing triggers on the 0, so no USB controller register
is touched while there may be no cable -- touching those registers with no cable
in hangs the device.

## 4. A folder appears on the Kindle drive for every tap (only for an entry built without the installer)

**Seen**: opening the Kindle drive from a computer shows a row of
`pw3-first-kernel-<timestamp>` folders, one more for every tap.

**Who meets it**: **nobody, after 2026-10-29** -- `build_entry.py`'s five
installer options are required now, so no such entry can be built. This entry
stays because a drive may still carry folders from before. Who met it then was
whoever built an entry with `boot/build_entry.py` and left out `--installer`;
what can still build such a launcher is `build_launcher.py`, and the only thing
that does is one test (`boot/handoff/tests/run_client_test.py`), which runs in
QEMU and touches no real drive. **The release's `Readom.sh` does not do this**: it
writes into `android/logs/<time>/`, the same folder the installer owns, and
keeps only the five most recent by itself.

**Why it stays**: the automatic removal only happens inside the installer's own
folder, and only removes the shape the entry itself writes -- a name in the
entry's timestamp format, holding plain files and nothing else -- with no prefix
matching and no recursion (`boot/launcher/userlog.c`). An entry without the
installer writes at the top of the drive, and removing things from the top of
someone's drive by prefix is not what this project does (the note above
`PW3_OUTPUT_PREFIX` in `boot/launcher/helper.c`).

## 5. The minute or two before the background re-check

**Seen**: on a start that skips the complete verification, the system image is
not read again until about two minutes after boot.

**Cause and trade-off**: see `design/release-build.en.md`. What the start checks
is the FAT table and the directory tree (about 85 ms), which finds another
file claiming the image's clusters; the content itself is verified in the
background afterwards, which is what saves some 19 s of every start (20.9 s measured when the image was 576 MiB, extrapolated at the same rate for 512 MiB and not measured again).

**Why it stays**: the alternatives are paying those 19 s every start, or
porting dm-verity (a driver backport for 3.0.35, and on 511 MB of RAM every
re-read would hash again, which costs more). **Damage is certainly found
within two minutes**; and against someone who can run code on the Kindle side,
who could replace the launcher itself, those two minutes change nothing.

## 6. A release that changes the default home screen only adds what is missing

**A defect during development**: after upgrading to a release that changes the default home screen, the home screen was back to the shipped layout -- icons the owner had moved, pages they had added and the icons of applications they had installed were gone. Applications and their data were untouched; what was cleared is which icon sits in which cell.

**Why**: the default layout is applied once, when the database is first created (`loadDefaultFavoritesIfNecessary`), so a release that changes the layout does nothing at all on a device that already has the system. The way this port took was to move `SCHEMA_VERSION` by one: that is what AOSP uses to decide whether a database can be carried forward, and when it cannot it calls `createEmptyDB()` and the layout is seeded again. So every edit to the layout deleted the whole workspace table -- the only record there is of what somebody had arranged. Three further changes were then made to pay for that cost (WeRead named in the layout, everything non-system queued back after the table was emptied, `downgrade_schema.json` kept in step), and a mechanism that needs three of those is the wrong mechanism: the shape of the tables and the contents of the home screen are two things, and they were one number.

**What is done now** (`aosp-launcher3/0015`): two numbers. `SCHEMA_VERSION` is only about the tables (29, unmoved); the layout carries its own version, `pw3_workspace_layout_version` in `res/values/config.xml`, which moves whenever the layout is edited. A device that already has a home screen is given the **difference**:

- an entry already on the screen is passed over -- wherever it has since been moved to, and whether it is in a cell or inside a folder. Entries are told apart by the activity they open, or, for an entry that opens none (the one for all applications), by its action. Not by the stored intent: the layout parser and an install write different flags for the same application;
- an entry that is not on the screen is placed: in the cell the layout asks for while that cell is free, in the first free cell of the same screen when it is not, and not at all when the screen is full, with a line in the log. An arrangement the owner made is never moved aside for an entry of the image's;
- nothing already in the table is removed or moved. Those rows belong to the owner; the layout is the floor of what the home screen has, not a mirror of it. Row 0 of the first screen is the search box and not a row of this table, so the difference marks that row occupied exactly as `LoaderCursor` does -- without it, an entry placed behind the box would be dropped by the next load rather than shown.

**What it still costs**: moving an entry from one cell to another, or taking one out of the layout, reaches a first open and nobody else; and the four shipped icons the owner removed by hand (the entry for all applications cannot be removed) come back once, in the release that edits the layout. Avoiding even that would mean keeping a list of what was removed -- more state than that gain is worth. **Also not delivered by the difference**: a folder, a widget, a hotseat cell and a `<resolve>`, none of which can be told apart from what is on the screen already, where placing one twice is worse than not placing it. This device's layout has none of them; `tools/tests/test_home_screen_entries.py` keeps it that way, and holds the version number against a hash of the layout file, so an edit that forgets the version is told which number to move.

**This path has run on the device once** (V84, 2026-10-25): the upgrade that moved the layout version from 1 to 2 put a **second** all-applications entry on the home screen of the one device that then had one. The rename had moved that cell's action from `org.kdandroid.action.ALL_APPS` to `org.readom.action.ALL_APPS`, and that cell names no activity and is told apart by its action, so the row on the screen did not answer to the new key. The mechanism did what it is for; the fault was renaming the key. A key stored on an owner's device cannot be renamed -- renaming one is removing an entry, and the difference has no way to express a removal. Since then `tools/tests/test_home_screen_entries.py` has `LAYOUT_KEYS`, which pins the layout's five keys on their own, so a changed key is named (commit `2c22d1a`).

Which cell an application the owner installs lands in is a separate question: see `design/app-install.en.md`.

## 7. A system image damaged without changing size: tapping again finds it and writes it again

**A defect during development**: after tapping Android the starting screen came up, a minute or two later the stock system was back, and no error code was shown. Tapping again did the same.

**Why**: the installer decides whether the image is already in place from the hash in its state file, rather than reading 512 MiB again on every tap (some 19 seconds each time, extrapolated from the 20.9 s measured at 576 MiB). When the content is damaged without the size changing, the state file still says it is ready, so the installer hands over; the preinit on the Android side reads the whole image, finds the hash wrong, refuses to start and restarts. Each side was right on its own, and together they were a loop with no way out -- the only way was to connect a computer and delete `android/system.img`, which is beyond somebody whose whole toolkit is a USB cable.

**What is done now**: the way `design/diagnostics.en.md` sets out -- the stock side **reads** that Android refused the image instead of guessing.

- Every `stop()` in the preinit writes its reason into the report area (`report_stop` in `PreInit.c`). The area's block device is mapped *before* the image is checked, so that path is open; the stops that happen before the mapping have nowhere to write and write nothing.
- On the next tap the installer reads the area (`read_report` in `install.c`). When the reason is the system image's own hash, it clears `system=ready` from the state file and writes the image again, showing a screen that says so first (`PW3_SCREEN_REPAIR`). The data image is untouched.
- A report already acted on is not acted on twice: the state file keeps `android=<start number>`, and a report is new when the number *differs*. Not "is greater" -- when the area could not be read as a report (a write cut off by a flat battery leaves a checksum that does not match), the next write starts counting from 1 again, and under "greater" this device would never be heard from again and the loop would be back exactly as it was.
- The other way round, "compiling finished" in section 8 asks for exactly that: a *greater* number. The two mistakes do not cost the same: a refusal never acted on is a permanent dead end, while a notice cleared early has somebody hold the power button in the middle of dexopt. So the two directions are asked separately.
- A damaged first line in the area itself does not kill the channel either: with the magic put back the first line still parses and the body still checksums, which says it is this project's own report damaged, so it is emptied and started again. Without that test, one byte flip has the installer let go (the size is right) and PID 1 let go (it is not ours), and no report can ever be written again.
- Refusals that writing the image again cannot fix (a FAT cross-link, a failed mount) are only written to the log -- but the owner now has a sentence to read and to hand on, instead of a loop with no way out.
- **"Could not read it" and "the digest is wrong" are two findings, and only the second is installed over.** A transient read error sends that boot back to the stock system while the image is in fact intact; taking it for damage would ask for 196 MB (and the local payload is deleted once installed, so offline that is E202), turning something the next tap would have read again into a dead end. So the preinit says which it was: read through and not matching is `SHA256 rejected`, not read through is `could not be read through`, and the installer acts on the first alone.

The text on the starting screen no longer tells anyone to connect a computer and delete `system.img`: it says that if the device ends up back in the library, tapping again finds the damage and writes the file again, with no computer needed.

## 8. The "still compiling" notice after an upgrade: it goes when compiling is done, not after a number of taps

**A defect during development**: an upgrade that changes the boot image has to compile the applications in `/data` again. The screen that says so was shown only on the first three taps after that install; if all three were interrupted before compiling finished, the fourth showed the ordinary "about three minutes, hold the power button if nothing happens in ten" -- while the device was in fact still compiling, which is advice to interrupt it once more.

**What is done now**: the same report as section 7. The compiling happens in `updatePackagesIfNeeded()` inside `SystemServer.startOtherServices()`, and `ActivityManagerService.systemReady()` is more than 700 lines further down the same function, so `sys.boot_completed=1` is necessarily later than compiling being done. That is the line the installer reads.

**A report existing is not itself evidence.** The sampler is deliberately not started at `boot_completed` -- it starts when the storage is up, because a start that never completes is the start whose report is worth having (`pw3-boot-report.rc`), and it writes after sampling for two minutes. So an upgrade whose compiling runs longer than that has its report written while it is still compiling, and reading "there is a new report" as "compiling finished" would clear the notice in the middle of the work, which is the direction the old behaviour was wrong in. The installer therefore looks for the last sampled `sys.boot_completed=1` in the report and nothing else.

The cost is one notice too many: the report from that upgrade's own boot says it did not complete, so the tap after it still shows the notice, by which time compiling has in fact finished. One too many is read and ignored; one too few has somebody hold the power button in the middle of dexopt. The direction is right.

The state file holds `compiling=1`, together with `compiling_after=`, the report area's start count at the moment it was set. It is set when a system with a different boot image is installed, and cleared by a report that meets three conditions at once: it reached `boot_completed`, it was written by the system this entry installs (the report carries the build number), and its count is greater than `compiling_after`. **It is never counted down by taps, and has no ceiling.** The last two conditions keep an old report from passing for a new one: the same system may have completed a start before this install, and a copy of a report can be put back from a computer (the note on `compiling_after` in `installer/install/install.c`).

**How long the compiling takes**: after a boot image change, the applications in `/data` are compiled again with `pm.dexopt.ab-ota=speed-profile` (frameworks/base patch 0047). An application that has a reference profile is compiled from it, 159 s measured for WeRead; one that has not has its dex quickened instead, 60 to 93 s (frameworks/base patch 0051, frameworks/native patch 0008). Without that second path, no profile meant compiling all of it, 1214 s.

**Two more cases measured on 2026-11-04**: an upgrade that keeps the boot image and changes only the build fingerprint (f to g to h) runs dexopt on WeRead at boot with `dexoptNeeded=-3` (the old oat usable, recompiled only for the filter), **about 70 s**, both times; a manual `cmd package compile -m speed -f` of WeRead took **21 min 41 s**. So each set of numbers in these documents has its condition: by profile 159 s, no profile quickened 60-93 s, same image with a new fingerprint 70 s, a full `speed` compile 1214-1410 s (`design/app-install.en.md`) or 1301 s (11-04); the "about three to five minutes per application" on the install screen is an upper bound kept for the profiled case, not a measured mean.

**And it is not paid once**: until the background job has compiled an application to `speed`, every boot after a version change compiles it again at `quicken`, and it comes out `quicken` again -- the upgrade boot asks "does this need compiling" against `speed-profile`, and with no reference profile falls back to `quicken`, so what it asks about and what it writes never meet. On 2026-11-04 the boots of builds k and l each did this to WeRead, **70 s and 74.6 s**. The fix is recorded in `open-items.en.md`.

Note that "has been used" is not the same as "has a reference profile": an application writes a *current* profile as it runs, and only a profile-guided filter writes the reference one (`PackageDexOptimizer.isProfileUpdated()`). This device's background compile job compiles to `speed` (`pm.dexopt.bg-dexopt=speed` in `pw3.mk`, see section 12), which writes no reference profile, so an application the owner installed usually takes the quickened path after such an upgrade, and the background job takes it to `speed` again the next time the device is idle.

## 9. UsageStatsService once fed itself into a full-core spin: the detector read scheduling gaps as clock changes

**Seen**: during the experiments of 2026-10-20, after repeatedly clearing application data, force-stopping and timing out launches, `system_server` held the single core for over thirty minutes, squeezing a running dex2oat down to 0.36% and leaving `pm clear` unable to get through the binder queue. It settled on its own.

**The lead**: the thread dump taken with SIGQUIT (`/data/anr/trace_NN`) points at `UsageStatsService.onStatsReloaded` calling `postOneTimeCheckIdleStates`. The log shows `Time changed in UsageStats by +2/+3/+4/-3 seconds` and `Flushing usage stats to disk` over and over.

**Ruled out**: the rate the clocks run at. With the screen awake, two 90-second measurements put the wall clock within +-0.07% of the monotonic one. (A first measurement said 1.5%, but it was taken while dex2oat had the CPU and is scheduling delay, not drift.)

### What the loop actually is

Following it through the source, **it is not a loop in the code**:

- `checkAndGetTimeLocked()` has exactly five callers -- `reportEvent()` (every usage event) and the three query interfaces `queryUsageStats` / `queryConfigurationStats` / `queryEvents`, plus backup and restore.
- A decision leads to `onTimeChanged()` -> `persistActiveStats()` + `loadActiveStats()` -> `mListener.onStatsReloaded()` -> `postOneTimeCheckIdleStates()` -> `checkIdleStates()`.
- And **`checkIdleStates()` never reads the wall clock** -- only `elapsedRealtime()`. The `MSG_INFORM_LISTENERS` it sends has two subscribers in this process (`AppIdleController`, `NetworkPolicyManagerService`), and both only ask `isAppIdle` / `isAppIdleParoleOn`; neither touches `checkAndGetTimeLocked`.

So the loop runs through CPU contention:

> a false decision -> the whole of the usage statistics written out and loaded back (expensive) -> the single core starved -> a longer gap between the two clock reads -> a false decision

**All of that loop's gain is in the detector.** frameworks/base patch 0050 makes the detector stop reading a scheduling gap as a clock change, after which no amount of starvation produces a false decision, the gain is zero, and there is nothing to light. This is not "the ignition is gone but the circuit remains": a circuit with no gain is not a circuit.

### Were any of those divergences a real clock change

The old code read the wall clock first and the monotonic clock second, so a gap of g gives a difference of about -g; it then reset the snapshot from that skewed pair, which makes **the next difference `g_previous - g_current`** -- the difference of two scheduling gaps. Three checkable consequences follow:

1. the first firing is minus the first gap;
2. every later one is bounded by **how much the gaps vary**, not by anything to do with a clock;
3. consecutive firings tend to alternate in sign, because each one resets the snapshot again.

Run the old arithmetic over the gaps a core starved by dex2oat actually produces (2.2-6.0 s): 300 calls fire 76 times, with magnitudes of **+/-2, 3 and 4 seconds** and signs `-+-+-+-+...`. **The log said `+2/+3/+4/-3`: shape, magnitude and sign all match.**

A clock somebody set has a different shape. With the patch, changes of +10, -10, +60, -3600 and +86400 seconds each fire **exactly once**, reporting the size of the jump (conservatively, with the snapshot's width taken off), and then go quiet. A stream of a few seconds going on and on is not something setting a clock can produce.

**So the conclusion is: explaining this stream of log lines needs no real clock change at all.** Shape, magnitude and sign are all reproduced by the gaps alone, while a real change large enough to pass the threshold shows up as a single firing the size of the jump, followed by quiet -- and the log has nothing like that.

That sentence is deliberately weaker than "it all came from scheduling gaps", because the evidence cannot carry the stronger one. What the old code reported is `previous gap - current gap + any real change in between`, three terms added together: a real change of +1 s with a current delay of 4 s also reports -3, and the next sample, with no delay, reports +4. So **an alternating stream does not rule out a real change below the threshold hidden inside it**. What is ruled out is a change worth reporting, and that is all this problem is about -- neither the patch nor the conclusion changes because of it.

### The guard

`tools/tests/test_usage_time_check.py` (`usage-time-check` in `run-tests.py`) turns all of the above into a repeatable check:

- four ranges of gaps (0-50, 2200-6000, 0-30000, 0-600000 ms), two thousand calls each, **zero false positives with the patch**;
- ten real changes (from +/-10 seconds to a whole day), each caught **exactly once**, never reported larger than the real jump;
- the **worst case for a wide sample** as its own group: a real change arriving on a 6, 20 or 120 second gap with no gap afterwards. This is precisely what the patch got wrong as first written -- it computed an interval for the current sample only and treated the snapshot as exact, so one correct decision manufactured a false one after it. Both widths are carried now;
- five sub-threshold changes (within +/-1.9 s) fire **not once**, so the small drift between a 1-second RTC and the monotonic clock across a resume does not disturb it;
- the old code **reproduces the logged signature** over the same gaps, which is the evidence for the conclusion above.

The model is only worth something while it is the same arithmetic as the Java, so the test pins each expression in the patch and checks that **the wall-clock read stays between the two monotonic reads**. **Seven mutations were each confirmed to fail it**, one of them being "do not carry the snapshot's width".

### What is left

Nothing. A real clock change still causes one reload, which is correct, and on this device they do not come in bursts: NTP polls on the order of a day, and `SET_TIME` is permission-gated.

**The same log line has a second source, which was our own defect and was fixed on 2026-10-31.** This section is about the detector reading scheduling gaps as a clock change; the other one is the kernel reporting a wrong number: `get_monotonic_boottime()` summed three `long` nanosecond fields in 32 bits and overflowed, so after every real suspend and resume some `CLOCK_BOOTTIME` readings are short by 2³² ns (4.295 s) — and `elapsedRealtime` is built on it. The patch `patches/android-core/0103` casts the first operand to `s64`, the same change as the upstream 3.4.y backport, and it was measured on the device across three real suspends. **So when `Time changed in UsageStats` appears, tell the two apart first**: this section's deviations are seconds-scale and load-dependent, while that one is a fixed 4.295 s and only after a resume.

## 10. What the `PW3_ANDROID_START` line settles, and what it does not

**Where**: at the end of `android/logs/<time>/launcher.log`, right after `PW3_ANDROID_REPORT`.

**What it reads**: the boot counter in the report area (`android/report.bin`). That number **only goes up when Android got as far as `pw3-boot-report` and that write succeeded**. The launcher compares the number read this time with the greatest any earlier tap recorded, and prints one of four things:

| The line | What it settles | What it does **not** settle |
|---|---|---|
| `reported previous_boot=N this_boot=M` | the kernel came up, init ran, storage was ready, `pw3-boot-report` ran, and the report area was writable | **whether the framework came up**. The service starts on `sys.pw3.storage_ready=1`, not on `sys.boot_completed` -- deliberately, because a boot that never completes is the boot whose report is worth having |
| `no_new_report previous_boot=N this_boot=N` | the same report was read twice | **whether anything ran**. A start that reached Android and did not get to write -- a refused write, a flat battery, a reboot first -- leaves the number where it was, and the number cannot tell that from nothing having run |
| `no_previous_report this_boot=M` | this is the first tap that found a report | -- |
| `unknown this_boot=M errno=N` | `android/logs/` could not be read through, so no comparison was made | this is not the same answer as there being nothing in it |

**Why it is worded this way**: this line once printed `started` and `not_started`, which are exactly the two things in the right-hand column -- it asserted both of the things this number cannot settle. So it now states only what was observed.

**Where to look after `no_new_report`**: `previous-kernel.txt`, in the same folder. A failed write of the report says why in the kernel log under the tag `pw3-report-area:` (`BootReport.cpp`, `KernelLine`); what it prints is only this project's own fixed text, a path and an error code, so the launcher keeps it in the log handed to the owner. But the kernel ring is only 32 KB and **the boot is out of it within a minute**, so that clue has to be collected early.

## 11. The one upgrade that changes the signing key: about 20 seconds longer, and system settings are reset

**When this happens**: when the system being installed is **signed with a different key** than the system already on the machine. Public releases are all signed with this project's one release key from v0.1.0 on (`ro.build.tags=dev-keys`), so upgrading between public releases never meets it. What does meet it is two builds with different keys installed over each other: a self-built system is not signed with this project's release key (without `PW3_RELEASE_KEY_DIR` it uses AOSP's public `test-keys`, with it the builder's own key; see `device/amazon/pw3/pw3.mk`), so installing a self-built system on a device that has had a release, or the other way round, changes the key once. The direction measured on the device is from `test-keys` to the release key.

**An upgrade that keeps the key (between two public releases, say) does not trigger it**, and that is written in AOSP's own code rather than inferred. `PackageManagerService.scanPackageDirtyLI` has two ways out of "a system package's signature changed", and what divides them is whether the package has a shared identity (`sharedUserId`):

```java
// The signature has changed, but this package is in the system image... let's recover!
pkgSetting.signatures.mSignatures = pkg.mSignatures;
// However...  if this package is part of a shared user, but it
// doesn't match the signature of the shared user, let's fail.
if (signatureCheckPs.sharedUser != null) {
    if (compareSignatures(...) != PackageManager.SIGNATURE_MATCH) {
        throw new PackageManagerException(INSTALL_PARSE_FAILED_INCONSISTENT_CERTIFICATES,
                "Signature mismatch for shared user: " + pkgSetting.sharedUser);
```

That boot's log splits into two disjoint lists which match this code one for one: the **18 packages with no shared identity** (EinkBro, Launcher3, PackageInstaller, the documents UI, the keyboard, WebView, the downloader itself and so on) say "signature changed; retaining data", while the **13 with one** (`android` itself, settings, systemui, shell, keychain, inputdevices, and the settings / telephony / media / downloads / userdictionary providers) fail their scan outright and are then judged, by the sweep that follows, to be system packages that no longer exist.

Reaching that branch requires `compareSignatures(...) != SIGNATURE_MATCH`, and **only a key change makes them differ**. A fingerprint change -- which every upgrade has -- never gets there: the signature check passes, so nothing is thrown and no package is judged missing. So those 20 seconds are paid on the key change alone.

**What is seen**: that boot takes about 20 seconds longer than usual, and system settings return to their defaults (brightness, the chosen keyboard, the download list, the user dictionary). **Applications the owner installed, and their books, are untouched.**

**What happens inside** (evidence collected on the device on 2026-10-22: a device whose data was made by a `test-keys` build, upgraded to a build signed with the release key):

Android's package manager finds at boot that the system image's signature has changed, and decides that the packages which share the `android.uid.system` identity -- the platform package `android` itself, plus settings, systemui, keychain, inputdevices and several providers -- cannot be found, so it wipes their data. With the platform package treated as gone, the next step asks which package is the uninstaller and compares the platform's own resolver activity by name; that name is null at this point, so system_server exits with a null pointer:

```
System package android no longer exists; it's data will be wiped
java.lang.NullPointerException: ... String.equals ...
  at com.android.server.pm.PackageManagerService.getRequiredUninstallerLPr(PackageManagerService.java:3499)
```

init then restarts zygote, and the **second** system_server starts from the rebuilt records and is fine -- so nothing is left behind beyond the wait and the reset defaults. The cost has an exact number: in the event log the first `boot_progress_start` is at 35.8 seconds and the second at 55.4, so **19.5 seconds are spent twice**.

**Why this is not "fixed"**: it is AOSP's own behaviour (a shared identity requires one signature across its members, or the whole group is unusable), and the trigger is a key change. What can be done is keeping public users from meeting it: the release key is not replaced, and public releases have used it from the first one (`design/release-build.en.md`). The upstream null pointer deserves a mention too: it reports "the platform package is gone" as a null pointer rather than as something a person could understand; but changing it means editing `framework.jar`, which is on the boot class path, and that changes all 45 ART boot files, so every application in `/data` has to be compiled again after the upgrade (60 to 93 s for WeRead alone) -- a minute or two more on every device's first boot after an upgrade, for the wording of one error. Not worth it.


## 12. WeRead takes tens of seconds to open a book

**What was reported** (by the owner, 2026-10-28): a book took about 20 seconds to respond to a tap; it
was still downloading once open, although the network was not slow; and the speed was very
unstable.

**How it was measured.** Every round first confirms the app is on the shelf (`mResumedActivity` is
`WeReadFragmentActivity` in `dumpsys activity`), then taps the same book, then calls it quiet when
two consecutive 2-second intervals use under 0.15 s of CPU. Note that **WeRead cold-starts back
into the last book read, not the shelf**; without first confirming the shelf, the tap lands inside
the reader, and such a round cannot be used.

**What was measured** (during development, `speed-profile` compiled code, application already
running, three rounds back to back):

| Round | Reader appears | Fully quiet | of which CPU | CPU share of wall | Major faults |
|---|---|---|---|---|---|
| 1 | 11.4 s | 77.1 s | 52.5 s | 68% | 4288 |
| 2 | 4.5 s | 62.6 s | 45.8 s | 73% | 2845 |
| 3 | 5.2 s | 43.6 s | 28.4 s | 65% | 610 |

The CPU in the table is the sum of the threads in `/proc/<pid>/task/*/stat`, which misses every
thread that exited during the run and undercounts the application by about a third. Reading
`/proc/<pid>/stat` and `/proc/stat` over the same window, **the core is busy 94-96%, WeRead itself
has 69-78%, idle is 4-6%, and iowait is 0** (section 10 of `design/performance.en.md`).

**It is the CPU.** The hot threads are `TypeSetting #1`, `ent.weread.eink` (main),
`WRNetworkPool`, `WRCommonPool` and `HeapTaskDaemon` (GC); the JIT compiler threads use 0.0-0.6 s
in a single open. Composition (`surfaceflinger`, 3%) and the segmenter process (2%) are ruled out
too. The clock is at **996 MHz for 100%** of it; `ondemand` never drops it to 396 MHz (page turns
measure 67% at 996 MHz, the rest being the idle between turns). The kernel has no `interactive`
governor compiled in, but this load is sustained rather than bursty, so one would not help.

**It is not the network.** The link: RSSI -51 dBm, 72 Mbps negotiated, 2.4 GHz, 0% loss, and a
round trip to `weread.qq.com` of 8.4 to 88.9 ms (46.6 average). One open was measured with packet
counters: uid 10022 sent and received **4 KB in 9 packets** over the whole thing; another showed a
single burst of about 270 KB over two seconds (127 KB/s). At the round trip measured, 9 sends
account for at most 0.4 s. The waiting is not transfer.

**And it is not memory.** The cumulative counters since boot (924 MB swapped out, 601,874 major
faults) look large, but this machine had just spent twenty-odd minutes under `dex2oat`, which
accounts for nearly all of them, so they say nothing about opening a book. Measured per operation
instead:

- opening a book costs the process 610 to 4288 major faults (about 2.4 to 17 MB), and system
  `pswpout` over the window is **zero**;
- over 15 idle seconds `pswpin` moves a single page;
- after `am kill-all`, `MemFree` does not move at all (32 MB to 32 MB), because **there are no
  background applications to kill**, and the 239 MB of page cache is reclaimable anyway. This
  machine is not short of memory at the moment a book opens.

So paging explains a few seconds at most, not tens of them.

**Half of it is interpretation.** Same device, same account, same shelf: compiling WeRead to
`speed` (full machine code) takes opening a book from **95.6 s to 51.4 s** and process CPU from
73.0 to 30.2 s, four rounds against two with no overlap; garbage collection barely moves, and the
43 s saved is all in the application's own CPU -- **interpretation**. Compared on a cold start
before signing in, with an empty shelf, `quicken`, `speed` and `speed-profile` cannot be told apart
(about 37 s, recorded in `pw3.mk`); that is a different workload, with far less code to run and
most of it already in the usage profile, whereas loading a shelf and opening a book runs the EPub
parser, the CSS parser and per-character typesetting, none of which the profile has. See
section 12 of `design/performance.en.md`.

**What is done now**: an application is installed with `quicken`, and the background compile job
takes it to `speed` while the device is idle (`pm.dexopt.bg-dexopt=speed` in `pw3.mk`).
frameworks/base patch 0066 has that job wait for "device idle, battery not low" instead of AOSP's
"device idle, on a charger"; frameworks/native patch 0009 has the compile write beside the old
files (`.staging`) and swap them in atomically on success, so the old compiled code stays usable
throughout. So a newly installed WeRead opens a book in about 50 s only once the device has first
been idle long enough (screen off for about 71 minutes) and then compiled for about twenty
minutes; until then most of its code is interpreted.

**Why the rest is not changed**: WeRead is an upstream-signed release, and changing it destroys the
reason this machine trusts it (the same argument as the EinkBro crash in section 1). What the
system side can do is done: the clock is at maximum, the code is compiled to `speed`, and the
system's own footprint is trimmed and not under pressure when a book opens. The 30 s or so of CPU
left after `speed` is the application's own parsing, typesetting and syncing on a 1 GHz
single-core Cortex-A9. The one system-side lever that still has a number on it is the room between
garbage collections (a ceiling of about 6-7%), and paired rounds on the device could not tell the
two settings apart, so it stays as it is (section 10 of `design/performance.en.md`).

**The way around it**: the reader page appears after about 5 seconds, and the tens of seconds that
follow are the book being typeset in the background -- **it can be read in the meantime**, with
page turns feeling sluggish until it finishes. It also **gets faster with use**: under
`speed-profile`, over three consecutive opens of the same book, the time to quiet fell from 77 s
to 44 s and the CPU from 52 s to 28 s.

**The first-day numbers (2026-11-04, release build h, measured after the owner reported "loading a new
book took over a minute")**. This time the measurement is of **a book not yet downloaded**: tapped on the
shelf, chapters fetched, parsed, typeset, to complete quiet. Two books in the freshly installed `quicken`
state first, then two after `cmd package compile -m speed -f com.tencent.weread.eink` (what the background
job does; measured at **21 min 41 s**) and a process restart. Four different books, so this is an order of
magnitude, not a paired comparison:

| State | Book | Download | Reader shown | Fully quiet | Process CPU |
|---|---|---|---|---|---|
| `quicken` | *The Inevitable* | 0.9 MB | 5.4 s | <= 46 s (the sampler lost its timeline) | 27.8 s |
| `quicken` | *Live Long, Die Short* (25 chapters) | 7.3 MB | 2.7 s | **57.4 s** | **35.2 s** |
| `speed` | *Excellent Advice for Living* | 0.4 MB | 9.6 s | 41.4 s | 25.9 s |
| `speed` | *Read Write Own* (26 chapters) | 0.7 MB | 2.4 s | **23.8 s** | **15.9 s** |

Same method as above (the process's `utime+stime`; two consecutive 2-second windows under 0.15 s count as
quiet). The sampler is one shell using builtins only (`read` on `/proc/<pid>/stat`, and `read -t` on a
stdin the host keeps open as the timer -- mksh drops a coprocess after its first timeout, and `mkfifo` is
refused in the shell domain), so nothing is spawned while it runs. The download is the uid's `rx_bytes` in
`/proc/net/xt_qtaguid/stats`, read once before and once after. What the owner met, "over a minute", is the
second row: just installed, interpreted, and a large illustrated book. **Wi-Fi and memory were not the
bottleneck this time either**: -48 dBm / 72 Mbps, 225 MB free; in that minute there were 114
`Deoptimizing ... JIT inline cache` lines, a 0.6-2.9 s GC every 5-6 s, and a 5-minute load of 4.2 on the
one core -- interpretation and JIT all over.

So "slow on the first day" is by design: the background job waits for the device to be asleep and idle for
about 71 minutes before compiling, and after that the bottom two rows are the level. (Seen once, 2026-11-04: WeRead set back to `quicken`, screen off at 09:07, charger connected; the job started by itself at 10:23, compiled for 21.5 minutes, and the filter read `speed` at 10:44. A connected charger does not stop it.) It was not compiled
back to `quicken` afterwards; the device stays at `speed`. **To measure a real first night's battery, run
`cmd package compile -m quicken -f` first and then unplug**, or that night is missing these 21 minutes of
compilation.

**What the screen shows** (as recorded on 2026-10-28): a `screencap` taken while a book opens
catches twenty-odd seconds of solid black; what was written then is that this is "in the framebuffer
only and never reaches the panel", because whoever watched the screen (with WeRead not yet signed in
at the time) saw a white screen for a while, then one black flash as the text came up. That flash is
`replaced()` in `Panel.h`, one GC16 by design. **Corrected on 2026-11-02**: once signed in, WeRead
takes 12-30 s to draw its first frame, runs into the application transition's 5 s timeout, and that
black **does reach the panel** (section 11 of `design/performance.en.md`). The image now (patches
0067-0073) keeps the previous screen until the application draws, and at a start the system first
draws WeRead's own start page (a black logo on white), so that black no longer appears; for a
transition interrupted by the screen going to sleep, see section 16.

## 13. The system asks for services that are not there, and each ask leaves a wtf in dropbox

**What happens** (during development, measured 2026-10-27): six `system_server_wtf` entries in the
dropbox over a day on the device.

- One at boot: `UsageStatsService.onBootPhase()` asks for `appwidget`. This system does not have
  it -- the home screen carries no widgets -- so `SystemServiceRegistry.onServiceNotFound()`
  writes 1718 bytes, on the `SystemServer.startOtherServices()` path.
- Five in Settings: `DevelopmentSettings.showEnableOemUnlockPreference()` asks for `oem_lock`.
  This device has no persistent data block and does no bootloader unlocking, so the same thing
  happens -- once from the search indexing thread (`DatabaseIndexingManager`), the rest from
  opening developer options.

**Neither is a fault.** Both callers test for null, so the interface is right (the OEM unlocking
entry is simply not shown), there is no crash and no ANR, and the owner sees nothing.

**Why it is not changed.** Only one fix would be the right one: let `UsageStatsService` and
`DevelopmentSettings` tolerate a service that is not there. That is a Java change to
`services.jar` and to Settings, bought for one exception not constructed and 1718 bytes not
written per boot. **That cost was never measured**, and on a 1 GHz single core it is of the order
of tens of milliseconds.

**When it would have to change**: measure first on the day it is touched -- if that one exception
on the boot path turns out to be worth hundreds of milliseconds, it becomes a fault to fix; or if
dropbox writes accumulate to something that matters on a 1.75 GiB partition. There is evidence for
neither today.

## 14. An open browser costs a sixth of the one core, and a heavy page can starve the system UI into an ANR

**Measured** (on v0.1.0, 2026-11-02): EinkBro sitting on a page doing nothing takes a steady
**17%-18% of the single core**, and it does not depend on the page -- a plain text page
(`example.com`) 18%, this project's own website 16% once it has settled. So this is not one page's
cost; it is the cost of having the browser open.

**Three things were added on the evening of 2026-11-02, and the first two retire the word
"resident"** (measured by taking `/proc/<pid>/task/*/stat` whole before and after and subtracting,
which costs the device almost nothing):

| Situation | Process, of the one core | System idle |
|---|---|---|
| Foreground, on `about:blank` | **22.9%** | 59.4% |
| Foreground, on a real page | 20.0% | 58.4% |
| **Sent to the background (home, process still alive)** | **0.0%** (one jiffy in 21.5 s) | **92.5%** |

1. **A blank page costs the same as a real one**: 22.9% on `about:blank`, slightly more than a real
   page. So it really is page-independent.
2. **It is not a resident cost; it is paid only while the browser is in the foreground.** In the
   background the whole process is 0.0% and the system is 92.5% idle -- Chromium throttles its
   periodic work when the renderer is not visible. So "having the browser open" in this section
   means **while a page is being looked at**; what is resident is nothing.
3. **None of that CPU became a frame**: over the same 20 seconds the panel side's `cpu_frames` went
   up by 1 and `presents` by 1 (16,384 pixels, the status bar clock). **Zero frames**, so it is not
   software composition or a refresh loop burning it; it is Chromium's own periodic work. By thread:
   `Chrome_InProcRe` (the WebView renderer's main thread) **10.0%**, three `ThreadPoolForeg` threads
   5.4% together, EinkBro's own UI thread 3.2%, `Compositor` 1.9%, `Chrome_IOThread` 1.4%.
   **Which of them it is has not been found yet** -- that needs a profile of this process on a
   `userdebug` build (on the release image it is not debuggable, and `simpleperf` would need root).
   Until then no WebView switch is changed on a guess.

**Loading a heavy page costs far more**: during the minute that this project's website was loading
(full-screen illustrations and an opening animation), the system's own attribution was
`89% info.plateaukao.einkbro: 88% user + 1.2% kernel`, the one-minute load average reached 9.11,
and `com.android.systemui` therefore missed a `TIME_TICK` broadcast and went to ANR, putting
"System UI isn't responding" on the screen. Tapping "wait" recovered it, nothing crashed, and
`logcat -b crash` stayed empty throughout.

**Why it is not changed**: this is what an upstream application plus a 1 GHz single core costs, not
a fault of this system -- what the system can do (software composition through SwiftShader, the
clock already at its ceiling, code compiled to `speed`) is done, and the ANR dialog is Android
telling the owner that something is holding the main thread, which is it working correctly. EinkBro
is an upstream-signed release, and changing it breaks the reason this device trusts it (section 1).

**When it would have to change**: making the website open more lightly on this machine is a job for
**the website**, not the system -- its opening animation and its full-screen illustrations -- and
the measure is the CPU attribution during that first minute on this device.
~~Also: close the browser before measuring idle power, because a resident 17% is enough to make that
measurement about the browser rather than the system.~~ **That sentence is retired as of
2026-11-02**: in the background it is 0.0% and cannot pollute an idle-power measurement. Closing it
first is still good practice, in case it happens to be in the foreground, but the reason is not "a
resident 17%".

## 15. "Wi-Fi will not come on" is two faults; tell them apart first

**One: the request arrives while the screen is dark** (adb can do this; a finger cannot). A minute
after the screen goes off, `WifiController` sets `mDeviceIdle` and enters `NoLockHeldState`, which
puts `WifiStateMachine` in `DISABLED_MODE` -- the policy this project wants (`frameworks/opt/net/wifi`
patch 0002: Wi-Fi sleeps with the screen, as on the Kindle). A request to turn it on then takes the
`mDeviceIdle == true` branch of `ApStaDisabledState`, which **does not send `CONNECT_MODE`**, and
goes back to `NoLockHeldState`: the parent state brings `wpa_supplicant` up while the child state is
`DISABLED_MODE`, and `SupplicantStartedState.enter()` takes the whole chain apart again. So the
switch looks thrown and the module is loaded, and no network ever arrives. `mDeviceIdle` is cleared
only by `CMD_SCREEN_ON`, so **waking the screen is the fix, not a restart**. A finger cannot reach
this one: opening Settings means waking the screen first.

**Two: the screen is on and it still will not come on.** This is what the owner reported on
2026-11-02, and **its cause was found on 2026-11-04; the SDIO card had nothing to do with it**.
Patch 0002 had made the "screen off for a minute" alarm a non-waking one, and AlarmManager holds a
non-waking alarm back while the screen is off, up to fifteen minutes, then delivers it the moment the
device becomes interactive -- the moment the screen comes on. So `CMD_DEVICE_IDLE` and
`CMD_SCREEN_ON` arrive 3 ms apart: `WifiController` enters `NoLockHeldState` (sending
`DISABLED_MODE`) and returns to `DeviceActiveState` (sending `CONNECT_MODE`); `WifiStateMachine`
takes the `CONNECT_MODE` while in `SupplicantStoppingState`, keeps only the mode from it, and ends in
`InitialState` with nobody to start the supplicant again, while the controller sits in
`StaEnabledState` and treats every "on" from Settings as already done -- the switch goes on, the page
reopens off. The `wifi module clean unload failed` of 2026-11-02 was that `DISABLED_MODE` unloading
the module as the device woke. Recovery on the spot: `svc wifi disable`, then `svc wifi enable`.
Fixed by `frameworks/opt/net/wifi` patch 0004 (images after v0.1.0): the idle timer is the state
machine's own delayed message, counting only time the system is awake with the screen off, removed
when the screen comes on; an idle message that arrives with the screen on is dropped as stale.
Measured on build k: off 63 s after the screen, connected within 25 s of waking.

**Both share one amplifier**: unloading the kernel module whenever Wi-Fi goes off is this project's
policy. Curing the second one properly means not unloading it on disable -- leaving the card in place
and only powering it down -- and the cost is resident power, which one power measurement has to
settle. Nothing moves before that measurement.

**How to measure it** (settled 2026-11-02, after measuring it wrongly another way):

- **Wake the screen first**: `input keyevent 224` (`26` is a toggle and turns a lit screen off),
  or what gets measured is the false fault above.
- **Read state before logs**: `mScreenOff`, `mDeviceIdle` and `curState` in `dumpsys wifi`. Do not
  reason upwards from insmod/rmmod in the kernel log.
- **Write the wait as polling with a ceiling, never a fixed sleep**: a fixed 10 s wait once reported
  10 of 10 healthy toggles as failures, while the same logs showed `wpa_supplicant` associating
  0.2 s after the request. Polling instead measured: **connected in 5 to 6 s when idle, over 10 s
  under load, and 5 of 5 successful**.
- **Read `ip addr show wlan0` itself**: `ip link show wlan0` answers `Permission denied` on a `user`
  build, which is not "there is no interface".

## 16. Put to sleep while a transition is still waiting, the screen wakes to the two bars around black until the application draws

**When it happens**: an application is tapped and has not yet drawn its first frame (the previous
screen is being held, or its own starting page is up); the power button puts the device to sleep,
and it is woken. The middle of the screen is black, with only the status bar and the navigation bar,
until that application draws.

**What was measured** (2026-11-04, the userdebug twin of build f, X5b/X5c/X5e; recorded in section
3a of the takeover notes): WeRead frozen with `kill -STOP` so that it cannot draw, a book opened,
sleep, wake -- at the moment of sleep the bookshelf layer is committed as hidden (flags `0x3` and an
empty visible region in `dumpsys SurfaceFlinger`), after waking the hold record is still there but
nothing shows it again; the panel counters show one full refresh at wake. **With the hold of the
previous screen switched off altogether (X5e, upstream behaviour only) it is black in the same
way**, so this is not something 0069-0073 brought in; it is "sleep while a transition waits"
itself: the sleep path commits the closing side's visibility as hidden, upstream's 5-second deadline
goes black too, and this build is no worse than upstream.

**Why it is not changed**: what would have to change is how the sleep path treats the closing
side's visibility, not the hold; and the window in which it can be hit is small -- the release
ships with starting windows on, all four installed applications have one, and the transition is
over within a second. If it does happen, the screen is normal as soon as that application draws,
or Home goes back to the launcher (which is fine). As seen in X5c, a frozen application that is
thawed draws and the hold lets go normally.

## 17. After WeRead is uninstalled, the tile on the home screen does not come back (v0.1.0; the next image puts it back)

**Symptom** (reported by the owner on 2026-11-04): WeRead uninstalled from Settings > Apps; the
"Install WeRead" tile that used to be on the home screen does not reappear; to reinstall, the
installer can only be found under "All apps".

**What happens**: the moment the installer has installed WeRead it **rewrites** its own tile on the
home screen into WeRead's icon (the same row, same cell), then disables its own launcher entry, so
there are never two tiles after an install. When WeRead is uninstalled the launcher deletes every row
that points at it, as it always does -- and that cell is empty. The installer **does** listen for
"WeRead was uninstalled" (`RemovalReceiver`, and `BootReceiver` checks the same at every boot): it
re-enables its entry, which is why the entry is back under "All apps"; but v0.1.0 does not add a row
back to the home screen, so the tile does not return. The installer itself is still there
(`/system/priv-app/WeReadInstaller`), listed under "All apps" as "Install WeRead"; tapping it
downloads and installs again, and goes straight into WeRead when done.

**Changed in the next image** (2026-11-04, `HomeScreenCell.takeBack`): when the cell is handed over
its screen and position are remembered; when WeRead is uninstalled and the entry comes back, the
entry is put back into that cell (or the first free cell of the first screen if it is taken). Only on
the way from disabled to enabled, so a reader who dragged the tile off the home screen themselves
does not get it back at every boot.

## 18. The first start after a version change goes black for about a minute and a half (v0.1.0; not in the next image)

**Symptom** (the owner, upgrading to build g on 2026-11-04: "the system is stuck on a black
screen"): after a system with a new version is installed, the boot picture appears and then the
screen turns black until the home screen comes up; measured at 85 s on g and 80 s on h (that stretch
is WeRead being recompiled, section 11), and about fifteen seconds at an ordinary start.

**Cause**: the handoff does draw the `BOOT` picture (kernel log: `pw3-handoff: SCREEN up`), but
SurfaceFlinger starts composing as soon as it is up, and until the first window exists every frame
of its holds only its own target buffer, **cleared to black**; this port's composer posted it to the
panel all the same, and the picture was painted over (the composer's `empty_frames=6` counts those
frames).

**Changed in the next image** (2026-11-04, `composer.cpp`): a frame with no layer at all is no
longer posted; the panel keeps what it shows -- an e-ink panel does not change unless a frame is
pushed -- so the boot picture stays until the first window draws. Those frames are still counted,
only not sent.

**What to do now**: nothing; the black minute is the compile, and the home screen follows. Holding
the power button only interrupts it.

## 19. Voyage emulator limits, and what is not yet a conclusion

- **A stock suspend on QEMU needs fsr_keypad unloaded first**: the emulator does not implement fsr_keypad's kl05 I2C, and its suspend callback waits on a mutex until the kernel's hung-task detector writes the stack into the log; `tools/emulator/prepare-voyage-suspend.sh` stops `lab126_gui`/`x`/`lab126` and then unloads `fsr_keypad` (with `drv26xx_haptics`) so the real suspend can complete. A real device needs none of this.
- **QEMU's Cortex-A9 TLB lockdown model differs from hardware**: the QEMU fork records any TLB fill performed while the lockdown bit is set into the lockdown entry, so after the transition the new kernel's `bus_freq` probe reads back the probe function's own code page and loops forever; release-series patch 1060 skips the probe when the `readom.emulator=1` token is present, and the token only appears on the test entry's cmdline, so a device/release is unaffected.
- **imx_epdc's fixed scan-out address does not show Android's composition**: the QEMU machine hard-codes `fb-addr=0x81000000` for EPDC while Android composes into the update buffer; a new QEMU-side `update-source` property, off by default (`tools/emulator/start-voyage.sh --epdc-update-source` adds the long `-global driver=imx.epdc,property=update-source,value=on` form), is what makes the desktop readable. With `--gui` + Xvfb the complete Android desktop has been verified readable and touch-operable; with the property off the output is pixel-identical to before.
- **Touch injection needs a display console**: with `-display none` there is nothing to inject into; with `-display gtk` (Xvfb :9 + openbox) QMP `input-send-event` drives cyttsp4, and taps on Settings and Back have been verified, while `xdotool click` on the GTK window does nothing. The coordinate formula and panel positions are in `tools/emulator/README.md`.
- **The bootstrap system image is not an AOSP build product**: it is the released v0.1.0 PW3 image with the Voyage modules swapped in and the identity properties rewritten, and it exists for emulator validation; a real release has to replace it with the `PW3_AOSP_PRODUCT=voyage` full AOSP build.
- **The installer end to end passed in the emulator, but has not run on hardware**: release-v2 completed the install (artifact hashes as expected, report area created, payload deleted), the strict module was correctly refused at the emulator's hardware gate and cleaned up, and the installed images booted to `boot_completed=1` through the v3 chain; a device install, the HTTPS download route and device watchdog/FSP timing are still untested.
- **Large writes under TCG trip the imx2 watchdog during an install**: the kernel timer feeds it every 15 s, but the FSP/vfat write load can starve the timer softirq for long stretches. The emulator-only workaround is `devmem 0x020bc000 16 0xFF3D` (WCR timeout 60 s to 128 s) plus a user-space WSR feed every 4 s while the guest writes the 2.3 GiB; the device's eMMC path does not need it.

## Voyage: the pw3-init-check storage service is disabled (v8.2)

The prebuilt `pw3-init-check` binary was compiled against
`PW3_FILEMAP_MAX_EXTENTS=1024`; the V11 receipt this port writes holds 8192 entries
(a real Voyage `system.img` showed 1,560 runs, the emulator 1,575), so the old
receipt-size and extent checks can never agree, and this build machine has no AOSP
userspace toolchain to rebuild the binary. From v8.2 the `on pw3-verify-storage`
action no longer starts that service; independent storage verification is carried
by preinit's full SHA/extent/dm checks and by `pw3-system-verify`'s dm-1 SHA256
recheck, while the ffs/interface checks are unaffected. Rebuild the binary with V11
semantics and restore `exec_start` once an AOSP build is available.

### Update (v8.3): the storage action only publishes readiness

Commenting out the service alone left `sys.pw3.storage_ready` unset, so every
trigger in the Android second half (post-fs/late-fs/post-fs-data/zygote-start/boot)
stalled after `security.perf_harden=1` with the panel on "starting Android". v8.3
turns the action into `setprop sys.pw3.storage_ready 1`; the emulator then reaches
`boot_completed`. PagePress run time (the actual page-turn direction) can only be
verified on hardware, and Android suspend must not be exercised in the emulator
(the fsr_i2c_poweroff path waits on the unmodelled kl05 mutex; the device has kl05).

## Voyage device results, and the boundary of this verification (v8.3)

**Verified on hardware** (Kindle Voyage 7th generation, firmware 5.13.6): after copying
`documents/Readom.sh` and the offline payload into `documents/`, tapping Readom installed the
images, Android 8.1 booted, and WeRead works. The emulator additionally verified probe/commit to
`boot_completed`, a deliberately fragmented `system=1575` extents against limit 8192, the PagePress
preinit load and its `PW3_PAGEPRESS` line, and that a write to the read-write store reaches the
host-side FAT.

**Not yet verified**: the actual page-turn direction of PagePress on hardware (awaiting the user's
reply), real app reads/writes under `/kindle-us` (the AOSP vfat policy rules were checked), and the
device panel waveform/ghosting/frontlight/VCOM/touch calibration/Wi-Fi calibration/suspend power.

**The release image used for this verification is a bootstrap rebuild** (released v0.1.0 system +
Voyage modules + rewritten identity + a debugfs-injected `/vendor/usr/keylayout/fsr_keypad.kl`),
not an output of this repository's AOSP build chain; this machine has no AOSP userspace toolchain.
An upstream rebuild needs `PW3_AOSP_INSTALLCLEAN=1`, plus rebuilding `pw3-init-check` with V11
semantics and restoring its storage `exec_start` in `init.pw3.rc` (today that action only publishes
`sys.pw3.storage_ready=1`).

## Voyage v8.7 hardware results and their boundary (2026-11-09)

**Verified on hardware** (Kindle Voyage 7th generation, firmware 5.13.6): tapping
`documents/Readom.sh` installed the images and Android 8.1 booted; Wi-Fi, display and standby are
normal; WeRead works (1.88 recommended); the four PagePress contacts page by their top/bottom
partition (top = previous, bottom = next); press feedback now vibrates (AAC waveform programmed);
the stock library appears read-only as "Internal storage/documents" (visible in the Files app and
in readers without using a second copy of the space); the navigation bar is hidden by default, a
swipe up from the very bottom edge inside an app peeks it back temporarily, and `nav=show` in
`documents/readom.conf` restores it permanently after a reboot; long-pressing power restarts into
the stock system.

**Boundaries and known issues**: occasional ghosting (normal e-ink range); the book-review page is
a little slow; the navigation-bar peek needs the swipe to start at the very bottom edge inside an
app; `/data/system/users/0/settings_global.xml.readom-bak`, when written, is owned by root; if
`settings_global.xml` does not exist yet on the first boot, the navigation-bar write is skipped and
takes effect on the next boot. Only the behaviour above was tested; no long-term stability testing
was done.

**The .readom-bak file and read-only semantics**: preinit backs up the settings XML once to
`/data/system/users/0/settings_global.xml.readom-bak` (root-owned, expected) for manual rollback;
the stock FAT library stays read-only through `umask=0022` and root ownership, so Android-side
changes never write back to the stock store.
