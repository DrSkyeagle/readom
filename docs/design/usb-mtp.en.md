# File transfer over USB under Android (MTP): the research and the plan

How file transfer over USB (MTP) and adb are wired onto the PW3's Linux kernel 3.0 on the Android side: what is wanted, what the research found, the plan for the kernel and for user space, what has to be verified on the device, and three problems met during development. Researched 2026-10-15, implemented 2026-10-16.

## What is wanted

- An ordinary reader plugs the Kindle into a computer and copies files: Windows, macOS (Android File Transfer or OpenMTP) and Linux all know MTP, with no driver to install.
- A release (a `user` build) has adb off by default (see `release-build.en.md`), with developer options able to turn it on alongside MTP, and the Kindle showing the authorisation dialog the first time a computer connects.
- USB has to work under Android always: outside the stock system it is the only channel between a computer and Readom, because the stock MTP is not there once Android is running.

## Where this started

Before this design, USB on the Android side existed only for adb during development:

- Kernel: Linux kernel 3.0.35's `g_ffs` (`CONFIG_USB_FUNCTIONFS=m`), one FunctionFS instance, for adb. The UDC is `arcotg`, loaded as a module.
- **The USB modules were loaded only from the development test entry** (`ro.boot.pw3.adb=1`). A normal start had no USB data function under Android at all.
- `g_ffs` has no `/sys/class/android_usb`, so:
  - AOSP 8.1's UsbService creates no UsbDeviceManager, and there is no USB state, no MTP and no "USB use" choice;
  - there is no UsbDebuggingManager either, so the authorisation dialog for a computer's first adb connection has nothing behind it and the host's public key has to be put there in advance.
- And 8.1's SystemServer starts UsbService only when the device declares USB host or accessory, and the PW3 declares neither.

## What the research found

| Question | Answer | What it rests on |
|---|---|---|
| Which kernel interface does 8.1's MTP use | `MtpServer` picks FunctionFS (`MtpFfsHandle`) when `/dev/usb-ffs/mtp/ep0` exists, and otherwise the kernel's `f_mtp` through `/dev/mtp_usb` (`MtpDevHandle`) | `frameworks/av/media/mtp/MtpServer.cpp` |
| Could MTP go through FunctionFS | No: it needs `FUNCTIONFS_ENDPOINT_ALLOC`, `FUNCTIONFS_ENDPOINT_DESC`, a multi-instance `g_ffs` and v2 descriptors, and 3.0's `f_fs.c` has none of them | 3.0's `f_fs.c`; `g_ffs.c` has one static instance |
| **Does 8.1's adbd still know android-3.0's `f_adb` (`/dev/android_adb`)** | **No**: `usb_init()` goes straight to `usb_ffs_init()` and the old interface is gone | `system/core/adb/daemon/usb.cpp` |
| Then how do adb and MTP work together | Put one FunctionFS function in a composite gadget: the android-3.4 kernel's `android.c` has it (the "ffs" function, with "adb" as an alias), and that is how every device of the 8.x era on a 3.4 kernel does it | AOSP `kernel/common` deprecated/android-3.4 (40bb591c), `android.c` |
| Can 3.0's `f_fs.c` be wired into that | Yes: the callback interface (`functionfs_ready_callback`, `closed`, `check_dev`) is the same as 3.4's | This kernel's `f_fs.c` and `g_ffs.c` |
| What does the reader have to do after plugging in | 8.1 locks data by default: the Kindle says only "charging over USB", and the notification shade's "transfer files" has to be tapped before the computer sees anything -- again after every replug | `UsbDeviceManager.java`: `mUsbDataUnlocked` resets on disconnect |

## The plan

### Kernel

| Patch | What it does |
|---|---|
| `android-core/1040-android-gadget-mtp-ptp-functionfs.patch` | android-3.0's (563897de) composite gadget, keeping only MTP and PTP; adb is android-3.4's "ffs" function (FunctionFS) with "adb" as its alias; `usb_remove_config()` added to `composite.c`; built as the module `g_android.ko`. It registers the functionfs filesystem itself, so `g_ffs` is no longer built |
| `android-core/1041-wakeup-source-stats-follow-android.patch` | Nothing to do with MTP, only built in the same batch: the wakeup source statistics take Linux kernel 3.4's format, which is the one Android reads (`hit_count` becomes `wakeup_count`, and `expire_count` is added) |
| `android-core/1042-usb-no-keep-awake-debug.patch` | The debug switch `pw3_keep_awake` (keeping the USB PHY active and holding a clock reference) removed: once 1039 let a cable wake the controller, cable detection was correct and the switch had no use left |

- Compile check (2026-10-16): with all three applied, `g_android.ko` is 79 KB, its vermagic matches the current kernel, every module's relocation types are supported, and nothing is unresolved.
- `KCFLAGS='-fno-pic -fno-pie'` is required (this GCC emits position-independent code by default and 3.0's module loader cannot handle it).

### User space

| Where | Change |
|---|---|
| `tools/kernel-hardware/init.pw3.usb.modules.rc` | Every boot loads `fsl_otg_arc -> arcotg_udc -> g_android`, with `pw3-usb-check` verifying the binding after each step; it completes synchronously in `post-fs`, so `/sys/class/android_usb` is there by the time system_server starts UsbService |
| `pw3-usb-check` (`UsbGate.cpp`) | Lets USB through only on a build whose adb requires authentication (`ro.secure=1`, `ro.adb.secure=1`); checks stage by stage, `prepare -> otg -> udc -> gadget`, the last stage checking `g_android`, `android_usb` and FunctionFS; `g_ffs` and every other gadget module count as conflicting; a `serial` stage generates a USB serial number once per machine at random, kept in `persist.pw3.usb.serial`, and never reads the factory one |
| `device/.../init.pw3.usb.rc` | Once the gadget has passed its check, at boot it writes the vendor, the product name, the serial number and the ffs alias, and mounts adb's FunctionFS; `pw3-ffs-check` confirms the mount is the real one and adb authentication is on before it sets `sys.pw3.ffs_ready=1`, and without that adbd does not start. The switches for `none`, `adb`, `mtp`, `mtp,adb`, `ptp` and `ptp,adb` are all in this file (AOSP's `init.usb.rc` is not imported), in the standard shape of a non-configfs AOSP device's `init.<hw>.usb.rc`: VID 18d1, PID 4EE7 for `adb`, 4EE1 for `mtp`, 4EE2 for `mtp,adb`, 4EE5 for `ptp` and 4EE6 for `ptp,adb`. **A configuration with adb in it takes two steps**: `start adbd` first, and only once it has written the FunctionFS descriptors and set `sys.usb.ffs.ready=1` does `enable 1` follow; the property is cleared when adbd stops or restarts (why: "Plugged in, and nothing on the bus" below) |
| Product properties | `persist.sys.usb.config=mtp` (`initialization.mk`). In a `userdebug` build `post_process_props.py`'s `mangle_default_prop` appends `,adb` to it (on `ro.debuggable=1`), and a `user` build does not. After boot `UsbDeviceManager` strips `mtp` and `ptp` back out of that property (AOSP's own comment: an OTA does not reset it), and `getDefaultFunctions()` returns `adb` when adb is on and `mtp` when it is not. So once the framework has run once, a device with adb on boots as an adb gadget, and MTP is switched on by UsbDeviceManager after the framework is up (the frameworks/base row below); the product default `mtp` matters for the very first boot only |
| The adbd patches (`system/core`) | `0002-pw3-adbd-usb-only`: USB only, no TCP, authentication required, and a check under adbd's final user and SELinux context that FunctionFS's ep0 opens. `0005-pw3-adbd-without-boot-opt-in`: drops 0002's original requirement that a boot argument enable adb explicitly (`ro.boot.pw3.adb=1`), leaving whether adb runs to Android's own switch (developer options, `sys.usb.config`) |
| frameworks/base | `0032-pw3-usb-service-for-usb-device`: UsbService starts when the kernel provides `/sys/class/android_usb` as well. `0033-pw3-usb-files-without-choice-when-unlocked`: with no secure lock screen (PIN, pattern or password) and no restriction forbidding USB file transfer, the default state after boot completes and after every unplug is MTP with data unlocked (the same as the reader tapping "transfer files"; `mtp,adb` when adb is on); with a password set it keeps AOSP's behaviour; a password set while unplugged locks the next connection again |
| SELinux | MTP uses AOSP's own policy (`/dev/mtp_usb` is already labelled `mtp_device`). This project adds rules only for its own checker: `pw3_init_check` may look at adb's FunctionFS (search and getattr on `functionfs`), and the `persist.pw3.usb.` properties belong to `pw3_boot_prop` (`device/amazon/pw3/sepolicy`) |

**Why plugging in unlocks by itself**: a stock Kindle shows a drive as soon as it is plugged in, and a reader who only knows how to plug in a cable will not go and tap a notification on the Kindle. With no lock screen password, anybody holding the device can already see everything on it through the interface, so unlocking automatically exposes nothing more; with a password set, it has to be unlocked and "transfer files" chosen, exactly as in AOSP. The unlocked state is set while the cable is out, not switched to when it goes in: switching functions restarts the gadget, and with adb on, adbd restarts with it, which can outlast UsbDeviceManager's one-second disconnect debounce -- and when the debounce runs out the defaults come back and the data is locked again.

## Verifying on the device

0. **Started with no cable, plugged in afterwards**: the computer has to find the device (this is the one that failed in "Plugged in, and nothing on the bus" below). Starting with the cable already in has to work too.
1. Windows, macOS (OpenMTP or Android File Transfer) and Linux (gvfs) all see the storage and read and write large files.
2. Debug build: adb and MTP at the same time, with the authorisation dialog appearing on the Kindle the first time a computer connects over adb.
3. Release build settings: adb turned on and off in developer options.
4. Fifty plug cycles, half awake and half asleep; plugging in during sleep wakes it (kernel 1036, 1039).
5. It sleeps on a charger and does not sleep on a computer.
6. The transfer speed for a large file.

What has been measured (2026-10-23, a development build before the public release; `tools/test-mtp.sh` with libmtp on macOS): the device is found over MTP; 32 MB of random data goes up in 5.1 s (6.3 MB/s) and comes back in 1.6 s (20.0 MB/s); the file is found on the device over adb at the same time, and its SHA-256 matches on both sides. That covers the macOS part of item 1, "adb and MTP at the same time" in item 2, and item 6. The fifty plug cycles and reading and writing from Windows and Linux have not yet been verified on the device; the other items have no measurement of their own on record.

## Problems met on the way

Three problems met on the device during development, each written as what happened, why, and what is done now.

### adbd becomes ready with no cable in and the system hangs

**What happened**: on 2026-10-16, the first time this gadget ran on the device: starting Android from the entry with the cable out, the system froze after about 40 seconds and the watchdog reset it back to the stock system.

**The evidence** (the previous start's kernel log, kept by the stock kernel):
- The first 40 seconds were normal: the storage checks, the services and the USB interface check (`pw3-ffs-check`) all passed.
- adbd started at 40.6 s, and at 40.87 s there were two FunctionFS `read descriptors` lines (the new format refused by 3.0, then the old one written) -- and after that nothing, not even the watchdog's pre-timeout interrupt output (`imx2_wdt.c`'s pre-timeout interrupt prints registers).
- So the CPU stopped with interrupts off: it touched a peripheral whose clock was already off, the bus never answered, and 60 seconds later the watchdog reset the machine.

**The cause**:
- After adbd writes its strings, `functionfs_ready_callback` -> `android_enable` -> `usb_gadget_connect` -> `fsl_pullup`, which reads and writes USBCMD without regard to the controller's state.
- With no cable in, the controller is stopped (`udc->stopped`; after binding at boot it suspends and waits for OTG detection), the PHY is in low power and the clock is off. The driver's own `nuke()` says as much: "the clock is off when stopped, the FIFO cannot be flushed".
- Lab126's own gadget never called connect or disconnect, so the stock system never met this; the `g_ffs` used on the Android side before does not call them either. Android's composite gadget calls them by the common convention, and the first time this path ran it hung.
- The other places that touch registers with no cable in: `usb_ep_dequeue(ep0)` inside `android_disable` (UsbDeviceManager switching functions, adbd restarting), the endpoint dequeue when an f_mtp or f_fs read or write is interrupted (the MTP service waking up after the cable is pulled), and disabling an endpoint.

**What is done now** (kernel 1044, `arcotg_udc.c`):
- The driver's existing rule, "do not touch registers while the controller is stopped", is carried through to every operation a gadget can call: pull-up, endpoint dequeue, endpoint disable, FIFO flush, reading the frame number, remote wakeup. While stopped, only software state changes.
- The pull-up splits into two conditions: whether the gadget wants to be connected (`softconnect`, changed only by connect and disconnect) and whether this cable session allows a pull-up (`dp_allowed`, set by `dp_pullup()` when the cable goes in and cleared when it comes out). The run bit is driver && wants to connect && VBUS && session allows, written while the controller is running.
  - The stock driver forced `softconnect` to 1 when the cable went in, so with the gadget not yet ready (at boot before adbd, or during a function switch) the computer could enumerate a device with no configuration and might need a replug to see it.
  - Now the pull-up waits for the gadget to be ready.
- What this rests on: what Lab126's own later kernels do on the same controller -- 3.10's (KT3) `ci_udc_pullup` does not touch the hardware without VBUS, and 4.1's (PW4) powers the controller up before writing.

**An instrument added along the way** (kernel 1043, `printk.c`):
- The kernel log is written to the fixed piece of memory the stock system keeps, but whatever is still in the CPU's cache is lost when the watchdog resets. That is why the end of this log was truncated.
- Now every line is written back out of L1 and L2 as soon as it is logged, and once more after the old log is cleared at boot. From here on, after any hang, the `previous-kernel.txt` saved the next time the entry is tapped in the stock system holds the log up to the moment it stopped (filtered by the entry log's allow list; see "Plugged in, and nothing on the bus").

### Pulled out and plugged back in, nothing enumerates (not reproduced)

- **What happened**: twice on the evening of 2026-10-16: a connected USB cable pulled out and put back, and the computer saw no device (the PMIC saw the cable and was charging).
- **Reproducing it**: on later development builds under five conditions, all normal: the first plug after boot; plug cycles within three minutes of boot (`pw3_diag` sampling: after the pull the controller is stopped and in low power while the gadget still wants to connect, and 0.4 s after the plug it is configured at high speed); plug cycles after the screen is turned off and on with the cable in; a software-simulated plug cycle (the controller side only); and a plug after the system had really suspended and woken (the UIC went through the same `notify_otg_out` branch as the failure).
- **The cause**: not found yet. The suspect is the path itself: under the earlier `g_ffs`, adbd unregistered and re-registered the whole gadget driver on every reconnect and the controller state was initialised from scratch; the composite gadget stays bound and only connect and disconnect are called, and the stock driver has never been exercised on that path.
- **What is done now**: kernel 1046 logs a line (`pw3-usb:`) on plug, on unplug, on a VBUS change and on every pull-up decision, so if it happens again one `previous-kernel.txt` shows which step it stopped at. Item 4 of "Verifying on the device", fifty plug cycles, covers both awake and asleep.

### Plugged in, and nothing on the bus

- **What happened**: on 2026-10-20, on a build before the public release, the reader plugged in under Android and `ioreg` on the computer showed no new device at all -- not MTP unrecognised, nothing enumerated. The same cable and the same hub brought up "Amazon Kindle" the moment the stock system was back. Reading `data.img` off the device offline: `persist.sys.usb.config` was `adb`, and `/data/misc/adb/adb_keys` was timestamped from the first install's start -- **adb worked on the first install and never again**.
- **The cause** is the order: adb goes through FunctionFS, adbd writes the descriptors, and the gadget cannot bind the function before they exist. Every AOSP device that puts adb on FunctionFS waits for `sys.usb.ffs.ready=1` before `enable 1` (`init.usb.configfs.rc`, `device/linaro/hikey`); this project had copied the one-step handling from AOSP's generic `init.usb.rc`, which is for a kernel with its own adb function and needs nothing from user space. On the kernel side, since 1044 `fsl_apply_pullup()` touches registers only while the controller is not stopped, so with the descriptors not ready the gadget simply never connects, which looks exactly like nothing on the bus.
- **Why the five plug conditions of the previous section did not catch it**: all of those used the development test entry (removed before v0.1.0), which held the framework back until released, so **UsbDeviceManager had never run**. As soon as it does it rewrites `sys.usb.config` (AOSP comment b/23631400: unconditionally) and goes on switching functions by cable and data-unlock state, so `enable 0` / `enable 1` happen after adbd -- a path the device had never taken.
- **What is done now**: a configuration with adb takes two steps (`init.pw3.usb.rc`; "User space" above), and item 0 of "Verifying on the device" was added to check a plug after boot.
- **The log was missing a link too**: the `pw3-usb:` lines added by 1046 were not then in the entry log's allow list, so the one piece of evidence that could have answered "which step did it stop at" was filtered out (that start's `previous-kernel.txt` kept three lines, all from the PMIC). The allow list now takes every line this project prints with a `pw3-` prefix (`boot/launcher/userlog.c`; only `pw3-boot-report`, which relays logcat text, is kept out).

## Risks

- The composite gadget's connect and disconnect (`usb_gadget_connect/disconnect`) and the cable handling of 1036 and 1039 all touch the UDC. Touching registers with no cable in hangs the machine, and 1044 keeps every operation a gadget can reach away from a stopped controller; a pull and replug that did not enumerate happened twice and could not be reproduced, with the cause unknown -- see "Problems met on the way".
- Transferring a file over 4 GB through 3.0's `f_mtp` has not been measured. The PW3 cannot reach it: MTP serves the storage in `/data`, and `/data` is 1.75 GiB (`BoardConfig.mk`); a port to a model with more storage has to measure it.
- The ffs function keeps the whole gadget disconnected until adbd opens its endpoints, so under `mtp,adb` MTP waits for adbd too. That is correct behaviour.
