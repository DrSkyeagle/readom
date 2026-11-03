# Porting to other Kindle models

This document answers one question: **what has to be done to move this Android to another Kindle, and what is already there.**
It comes from an inventory of this repository (the patch counts are an actual count of the patch directories on 2026-10-29) and from measurements on each model's stock source release (2026-10-20 to 10-23), not from imagining. **Not one line of it has run on a second machine**: this document exists to get the amount of work and the boundaries right, not to claim support.

## 1. The prerequisites: which models could work

| Condition | Why | How the PW3 stands |
|---|---|---|
| It can be jailbroken and run a `.sh` from the library | The whole install, and every entry into Android, depends on it | 5.16.2.1.1 can be jailbroken |
| Amazon published the GPL source for that firmware | We patch the stock kernel source to build a kernel that can run Android | `Kindle_src_5.16.2.1.1` |
| The stock kernel is Linux 3.0.35 | The 84 kernel patches, the handover module and preinit are all written against it | 3.0.35-lab126 |
| The display controller is an i.MX EPDC | The composer, gralloc and the waveform interface are written against `mxc_epdc` | The i.MX6SL's EPDC |
| At least 512 MB of memory, and about 2.1 GB that can be freed on the store | 512 MB on this machine already needs zram to have the browser and the reader open at once | 512 MB / 4 GB |

**Neither of the first two is the bottleneck**:

- **Jailbreaking**: by KindleModding's model table (read 2026-10-20), every generation from the Kindle 2 to the Colorsoft has a working jailbreak; only the three newest (Scribe 3, Scribe Colorsoft, the 2026 Kindle) say "None Available". The WinterBreak/WinterBreak2 the PW3 uses covers every firmware before 5.16.4.
- **GPL source**: Amazon published source for **every** generation of Kindle, right up to the models on sale today (the Source Code Notice page, read the same day). There is no model with a jailbreak but no source.

The real bottleneck is the third. The table below was read out of Amazon's own source archives rather than inferred:

| Model | Generation | Firmware checked | Stock kernel | Memory | Screen |
|---|---|---|---|---|---|
| Paperwhite 1 | 5 | 5.6.1.1 (its last) | **2.6.31** | 256 MB | 758x1024 |
| Paperwhite 2 | 6 | 5.12.2.2 (its last) | **3.0.35** | 256 MB | 758x1024 |
| Kindle 7 (KT2) | 7 | 5.12.2.2 (its last) | **3.0.35** | 256 MB | 600x800 |
| Voyage | 7 | 5.13.6 (its last) | **3.0.35** | 512 MB | 1072x1448 |
| **Paperwhite 3** | 7 | 5.16.2.1.1 (its last) | **3.0.35** | 512 MB | 1072x1448 |
| Oasis 1 | 8 | 5.16.2.1.1 (its last) | **3.0.35** | 512 MB | 1072x1448 |
| Kindle 8 (KT3) | 8 | 5.16.2.1.1 (its last) | **3.10.53** | 512 MB | 600x800 |
| Oasis 2 | 9 | 5.16.2.1.1 (its last) | **4.1.15** | 512 MB | 1264x1680 |
| Paperwhite 4 | 10 | 5.18.1.1.1 | **4.1.15** | 512 MB | 1072x1448 |
| Kindle 10 (KT4) | 10 | 5.18.1.1.1 | **4.1.15** | 512 MB | 600x800 |
| Oasis 3 | 10 | 5.18.2.1.1 | **4.1.15** | 512 MB | 1264x1680 |
| Paperwhite 5 | 11 | 5.19.2 | **4.9** | 512 MB | 1236x1648 |

How to read it (anybody can check it again, without downloading and unpacking a whole archive):

```bash
curl -sL '<that model's Kindle_src_*.tar.gz from the Source Code Notice page>' \
  | tar -tzf - | grep -m1 '^gplrelease/linux-'
```

The table says two things:

1. **The same generation and the same firmware do not mean the same kernel.** The Kindle 8 (KT3), the PW3 and the Oasis 1 all stop at 5.16.2.1.1 and all use the i.MX6SoloLite, but its kernel is 3.10.53 and the other two are 3.0.35. So grouping "by SoC" is wrong; the grouping is by kernel version.
2. **A newer model is not necessarily harder.** A batch of the kernel patches backport things that only arrived in mainline later (ambient capabilities is mainline 4.3, `process_vm_readv/writev` is 3.2, binder and ashmem were still in staging at 3.0). The newer the kernel, the less of that is needed -- a 4.1.15 machine is easier in this respect, and its hardware is better (a dual-core Cortex-A7 i.MX7D, 8 or 32 GB of storage). The price is relanding the whole series on a new kernel, once.

What this table means for this project, by group:

| Group | Models | What it means here |
|---|---|---|
| **A: the same kernel (3.0.35)** | Voyage, Oasis 1 | The source-level measurements of section 6: the Oasis 1's kernel source is **byte for byte the PW3's**; the Voyage's differs, but all 79 Android patches of the time apply, and Amazon itself states that the Voyage and the PW3 use **the same defconfig**. What is left is the panel parameters, the device differences in PMIC, front light and touch, the handover profile, and measuring again. **The Voyage is the best second model**: the same configuration, the same resolution (1072x1448), the same memory (512 MB). The Oasis 1 uses another defconfig (duet, a 2G/2G split), so the handover's private page table needs changing, and its Wi-Fi is a Broadcom chip whose driver has to be found elsewhere (sections 6 and 7) |
| **A-: the same kernel, not enough memory** | Paperwhite 2, Kindle 7 | The kernel is the same, but 256 MB does not hold this Android (512 MB here already needs zram). It could be built; the experience is not this project's aim |
| **B: 3.10.53** | Kindle 8 | One kernel relanding, plus redoing the interface for 600x800 (density, text sizes and margins all have to be recomputed) |
| **C: 4.1.15** | Oasis 2, Paperwhite 4, Kindle 10, Oasis 3 | One kernel relanding buys four models, on better hardware that Amazon is still updating (which means watching the firmware for the jailbreak). **The largest long-term gain** |
| **D: MediaTek, 4.9 and later** | Paperwhite 5, Kindle 11, Scribe, Paperwhite 6, Colorsoft | Not an i.MX EPDC. The display layer (gralloc, the composer, the waveform interface) would be rewritten against MediaTek's EPD driver -- that is redoing it, not porting it |

The PW1's 2.6.31 is out of scope: that generation's kernel is too old, and supplying what Android 8.1 wants comes to about as much work as a rewrite.

## 2. The parts that already work on a different model

There are 84 kernel patches: 83 in `patches/android-core/`, plus 1 in `patches/` itself, a fix that lets 3.0.35's build scripts run under a current Perl (the 2 in `patches/test-kernel/` are for the QEMU test kernels only and are not counted). **55 of them do not touch hardware code** (binder, ashmem, SELinux, seccomp, qtaguid, routing, ASLR, capabilities, TCP and so on) and **29 do** -- about two thirds are hardware-independent.

The deciding test is the files each patch changes: if any one of them belongs to a specific chip's driver or to the SoC and board code (`arch/arm/mach-mx6/`), the patch touches hardware. The chip drivers are the EPDC and PXP (`drivers/video/mxc`, `drivers/dma/pxp`), the USB controller (`drivers/usb/gadget/arcotg_udc.c`), the sub-drivers of the max77696 power management chip (`drivers/power`, `drivers/mfd`, `drivers/hwmon`, the backlight, the power key), the Cypress touch controller, the Hall sensor (`drivers/char/wario_hall.c`) and ath6kl (`compat-wireless/drivers/net/wireless`). What does not count: the generic frameworks under `drivers/` (binder and ashmem in `drivers/staging/android`, the wakeup sources in `drivers/base/power`, `evdev`, the USB gadget function layer `android.c`/`f_mtp.c`, `uid_cputime`), the ARM architecture's common code (`arch/arm/kernel`, `arch/arm/mm`) and cfg80211 in `compat-wireless`. By that test the 29 hardware patches change nothing but hardware files; no patch mixes a driver change with a generic one (`1000` declares the option controlling boardid in `drivers/staging/android/Kconfig`, and counts by the code that option controls).

On the AOSP side there are 156 patches across 25 projects (65 in `frameworks/base`, 21 in `packages/apps/Launcher3`, 11 in `system/core` and so on). They are all user space and change no driver, and carry over to another model largely as they are. A few are compatibility work written for this machine's CPU or kernel version: `bionic` and `swiftshader` each have a fix for the Cortex-A9, and `wificond`'s `0004` and `0005` let it understand the attributes the 3.0 kernel's cfg80211 leaves out or sends in the older form, which on a newer kernel become fallbacks that are never taken. Two more encode facts about this device and have to be rechecked per model; see item 8 of section 3.

These are model-independent too:
- The CPU composer (`composer/`): it works in pixel formats and rectangles, and there is not one resolution constant in its source (`composer.cpp`, `LumaCompose.*` and `BufferFormat.h` were checked); flipping and 90/180/270-degree rotation are already supported.
- The structure of gralloc and of the power and lights HALs (only the node paths are device-specific).
- The whole approach to the boot handover (`boot/`): install a module in the stock kernel and have it jump into ours; **one JSON profile per model and firmware** (`boot/profiles/*.json`), not a code change.
- The keyboard: every size is in `dimens.xml` in dp and scales with the density; the only 1072 in its source is in a comment explaining a layout trade-off.
- The C code the installer runs on the device: the screen geometry comes from the generated `screens.h` (`PW3_SCREEN_REGION_W` and so on), with no resolution written into the code.
- The USB composition for MTP and adb.

The two programs that generate bitmaps are not in this list: the resolution is fixed in module constants in those programs, see item 5 of the next section.

## 3. What every new model requires

Largest piece of work first:

1. **The stock kernel source plus adapting the patches** (the largest piece)
   - 29 patches touch hardware code: 9 for the EPDC including the PXP (the waveform file, temperature, DMA, the request queue snapshot, the first picture at boot, suspend and so on), 8 for the USB controller, 5 for the max77696 power management chip (battery and charger, fuel gauge, front light, I2C, power-off), 2 for the Hall sensor and the power key, 2 for Wi-Fi, 1 for touch, and 2 for board code, IDME and the bus frequency.
   - If the new model is still the i.MX6SL plus max77696 plus ath6kl set, most of them apply as they are; the Voyage shares the PW3's defconfig and is the likeliest case, but which chips are really on its board still has to be checked on the machine. A different SoC (i.MX6SLL or 6ULL), PMIC or Wi-Fi chip means rewriting them to the same intent; the Oasis 1's Wi-Fi is known to be Broadcom (section 6).
2. **The boot handover profile** (`boot/profiles/<model>-<firmware>.json`)
   - The machine number, the RAM layout, the stock command line, the kind of ATAGs, and the bytes and hashes of the stock functions (the handover module has to find them inside the stock kernel).
   - All of it has to be read off the target machine; none of it can be copied.
3. **The display contract** (`device/amazon/<model>/epdc/EpdcContract.h`)
   - The resolution, the rotation and the bits per pixel; this machine has 1072x1448 and `FB_ROTATE_CCW` written in, to be changed to that model's values.
   - The waveform comes from the device's own partition (never packaged); the grey curve has to be measured again for that panel (how it was done here is in `eink-ui.en.md`).
4. **The AOSP device directory** (`device/amazon/<model>/`)
   - The density (`ro.sf.lcd_density=300` and `xhdpi` here), the overlays for text sizes and margins, the navigation bar's size: recomputed from the resolution and the physical size.
   - The partition and data image sizes (512 MiB of system and 1.75 GiB of data here, 2304 MiB together; the read-only one is the smallest power of two the build fits in, the data one is what the device measured plus room).
5. **The two bitmaps the device shows** (`installer/screens/render_screens.py`, `device/amazon/pw3/sleep/make-sleep-picture.py`)
   - The resolution and every pixel size are module constants in those programs, **not command-line parameters**: `SCREEN_W/SCREEN_H`, `WIDTH/HEIGHT`, and the text sizes derived from the density 1.875 (a 28 sp title, 20/17 sp body), the line spacing, the margins, the progress bar's height. Changing model means recomputing that whole set for the new density, not editing two numbers; a 600x800 machine (KT3, KT4) needs the layout redone above all, because a page laid out for 1072 px does not fit in 800.
   - The sleep picture also has to be drawn again: `make-sleep-picture.py` requires the source image's height to equal the screen's exactly and its width to be no less, and exits otherwise.
   - The C side needs no change: the geometry comes from the generated `screens.h`.
6. **The installer's model check**: a model or firmware that does not match shows E101 and stops, a low battery shows E102, too little space shows E401. What is checked (the stock kernel's version string and so on) and the model name the E101 screen shows both come from the handover profile of item 2, so a new model writes them there as well.
7. **Measuring again**: cold start, page-turn latency, power, grey levels. This machine's numbers cannot be carried over. Power is logged with `tools/power-log.c`, which reads and writes the PW3's max77696 fuel gauge and front light and needs changing for another PMIC; this project has not yet written a measured `power_profile.xml`.
8. **Rechecking two framework patches per model**: what they encode is a fact about this machine, not a defect in AOSP.
   - `0043` (battery statistics only asks the radios that will answer): the ath6kl here records no link-layer statistics and there is no modem, so neither is ever asked for. A Wi-Fi chip that does report link-layer statistics means rechecking it -- the patch caches the result of `getSupportedFeatures()` and asks once, so a 0 returned before the Wi-Fi service is ready would be remembered as "not supported".
   - `0044` (answer "no events" directly when there is no calendar provider): the calendar application was cut from this image. On a model whose image puts it back, this one is redundant.

## 4. The repository layout to move to (when there really is a second model)

Everything device-specific is in the one `device/amazon/pw3/` directory today. When a second model appears, these move up into `device/amazon/common/`:
the composer, gralloc, the general parts of the power and lights HALs, the keyboard, the installer, the sleep picture and the general rules of `sepolicy`.

**Not refactored in advance**: with no second machine to verify against, splitting it only adds the risk of breaking the working system for no gain (this repository's habit is to measure before changing). Writing the boundaries down here is what makes that day's split knowable.

## 5. The conclusion

- This system **supports several models structurally**: configuration is separable from hardware code, 55 of the 84 kernel patches have nothing to do with hardware, and the boot handover is already configured per model.
- But **every new model is still a real porting job**, not a parameter change: it takes that model's jailbreak, its source, one of the machines, and measuring again. The jailbreak and the source are available for nearly every model (section 1), so the real threshold is **the stock kernel version** and having a machine in hand.
- v0.1.0 has been verified on the Kindle Paperwhite 3 (7th generation) with firmware 5.16.2.1.1 and nothing else. The documents and the installer are written to that fact and claim no other model.
- **How far the source level has been measured is in sections 6 and 7**: the Oasis 1 and the PW3 get the same kernel source, the Voyage's source differs but all 79 patches of the time apply,
  and Amazon itself states that the Voyage and the PW3 use the same defconfig. Both models' kernels build, each with all 147 of its kernel configuration requests in effect;
  the Oasis 1 still lacks a Wi-Fi driver and one change to the handover's private page table.
  None of this has run on any second machine.

## 6. Measured: applying this series to two other models' stock kernels (2026-10-21 to 10-23)

Sections 2 and 3 judge the kernel patches from an inventory; this section applies them to two other stock kernel trees and goes on to build.

The method: the patches are an ordered accumulation (the later ones depend on the earlier), so each one is dry-run first and only applied if that passes, and a failure is skipped while the rest continue -- which makes a failure "this one does not apply" rather than "everything from here on collapses". **The control is the PW3's own clean tree**: every patch has to apply, or the method itself is wrong. The control passed. At the time `patches/android-core/` held 79 patches; the 4 added since (`0403`, `1055`, `1056`, `1057`) have not been applied to the other two trees.

| Model | Firmware | Stock kernel tree | This project's 79 patches |
|---|---|---|---|
| PW3 (control) | 5.16.2.1.1 | 42,058 files, total hash `5d6b2cd6...` | **79/79** |
| Oasis 1 (8th generation) | 5.16.2.1.1 | **Byte for byte the PW3's**: 42,058 files, the same total hash | **79/79** (the same tree) |
| Voyage (7th generation) | 5.13.6 (its own last) | 42,058 files, total hash `a609502a...` -- **different from the PW3's** | **79/79** |

Two conclusions:

**One, the Oasis 1 and the PW3 get the same kernel source.** The archive hashes differ (by 3,827 bytes), but unpacked, the total hash over all 42,058 files' contents is identical and the difference is only in the packaging metadata. So for the Oasis 1, "landing the patches" is **zero work** -- the patches apply to the very tree its firmware was built from.

**Two, the Voyage's tree differs from the PW3's, and all 79 patches still apply cleanly.** A different firmware, a different source, and the patches still land; this one carries more information than the Oasis 1's.

### The model differences are in the defconfig, and Amazon states which machine uses which

`BUILD_HOW-TO.txt` in the source archive gives the mapping outright:

```
Kindle Oasis:                                                          imx60_duet_defconfig
Kindle Paperwhite (6th & 7th generation), Kindle Voyage (7th generation),
Kindle (7th generation):                                               imx60_wario_defconfig
```

So **the Voyage and the PW3 use the same defconfig** and the Oasis 1 uses duet. That makes the Voyage the cheapest second
model: the same configuration, every patch usable, the same 1072x1448 resolution and the same 512 MB of memory.

`imx60_duet_defconfig` and `imx60_wario_defconfig` differ by **103 lines**, of which these matter here:

| The difference | What it means here |
|---|---|
| `VMSPLIT_2G` / `PAGE_OFFSET=0x80000000` (the PW3 is 3G / `0xC0000000`) | A different kernel virtual layout. **The boot handover depends on the memory layout**; what that means is in the handover part of "The two things left" in this section |
| `CONFIG_FALCON=y`, with the BIOS fixed at `0x9F5E0000`/`0x9F5F0000` | The PW3 is `FALCON=n`. That physical memory is occupied |
| `FB_MXC_EINK_WORK_BUFFER_ADDR=0x9F000000`, reserving 4 MB | The handover's destinations have to avoid this reservation; this machine's rule that destinations stay in the low 128 MiB already excludes it |
| A different Wi-Fi stack: the PW3 has ath6kl / AR6003 (a module), duet has cfg80211 plus LIB80211 and HOSTAP built in (and brcm in the board code) | The two patches that touch Wi-Fi (`0310`, `1045`) have to be rewritten for the new chip; the ath6kl firmware policy (`CONFIG_LAB126_ANDROID_ATH6KL_FIRMWARE`) is meaningless to it and is switched off in duet's model fragment |
| The proximity sensor goes from `PIC12LF1822` to `SX9500` | |
| The command line has `no_console_suspend` as well | |

### The stock kernel tells models apart by a board ID at run time, not at compile time

This tree has only two compile-time variants (`MX6SL_WARIO_BASE`, `MX6SL_WARIO_WOODY`), while the board code tells more boards apart
with **run-time** tests like `lab126_board_is(BOARD_ID_WHISKY_WFO)` (the board ID comes from IDME). What WOODY guards is the
"one-shot" control of the Hall sensor and a branch of the battery driver -- hardware of the magnetic-cover-with-a-battery kind.

Which is to say: one stock kernel binary serves several boards, and a fair part of the model difference is not in a source branch
but in a board ID read at run time. Porting means finding out which branches that machine's board ID walks into, not looking for
"where this model's code is".

### One level further down: the Voyage's kernel builds

Applying is not building, so it was built once through this project's own flow (the same `aosp-arm-gcc49` toolchain, the same
`KCFLAGS=-fno-pic -fno-pie`, the same fixed timestamps):

| Step | Voyage (the 5.13.6 tree) |
|---|---|
| `patches/*.patch` plus `patches/android-core/*.patch` | **All 80 applied** (79 + 1 at the time) |
| `imx60_wario_defconfig` plus this project's configuration fragments | **All 147 Kconfig options this project requires verified one by one** |
| `make uImage` | **Exit code 0, zero errors**, a 3,075,740-byte `uImage` |

The second row deserves more attention than the third: `merge-kconfig.py --check` verifies item by item that "every requested
option really has the requested value in the final `.config`", and all 147 passing says **this kernel configuration is entirely
satisfiable on the Voyage's tree**, with not one item falling away for a missing dependency or being overruled by another option.

### The Oasis 1 builds too, but the first build silently missed 8 options

The same flow with `imx60_duet_defconfig`. The kernel **builds** (a 3,245,276-byte `uImage`, zero errors),
but `merge-kconfig.py --check` reports **8 requested options that did not take effect**. This is exactly why that check exists: without it,
what comes out is a kernel silently missing those features.

| What did not take effect | Why | How it is handled |
|---|---|---|
| `LAB126_ANDROID_POWER_SUPPLY_ABI`, `LAB126_ANDROID_BACKLIGHT_BLANKING`, `LAB126_ANDROID_PMIC_I2C_STRICT`, `WARIO_HALL_INPUT`, `LAB126_ANDROID_DISABLE_TOUCH_FW_UPDATE`, `LAB126_ANDROID_PRIVATE_IDENTIFIERS` (6 of them) | **This project's own patches wrote `depends on MX6SL_WARIO_BASE` in Kconfig**, and the Oasis 1 is `WOODY`. The chips they otherwise depend on (the MAX77696's battery, charger, backlight and MFD, `WARIO_HALL`, the Cypress touch controller) are **all enabled** in the duet configuration | The dependency names only the driver it changes; see the next subsection |
| `LAB126_ANDROID_ATH6KL_FIRMWARE` | **It really is another Wi-Fi chip**: there is no `ATH6KL_SDIO` in the duet configuration | Off is right, and duet's model fragment says so; the two Wi-Fi patches have to be rewritten for the new chip |
| `CONFIG_ARCH_MMAP_RND_BITS`: 16 asked, 8 given | It uses a 2G/2G split, so the user address space is half the size and the upper bound on mmap randomisation is lower | That is a hard constraint of that machine's memory layout; the ASLR entropy can only reach 8, and the model fragment writes 8 as it is rather than forcing it |

Of the 8, 6 are this project's own way of writing things and 2 are facts about the hardware; the next two subsections deal with them.

### Kconfig dependencies name the hardware, not a board

`MX6SL_WARIO_BASE` ("Classic Wario") and `MX6SL_WARIO_WOODY` ("Woody based Wario") are two arms of the same
`choice`, the PW3 being the first and the Oasis 1 the second. And each of these 7 options (the 6 above plus the ath6kl firmware one) changes **one driver**,
which is what the dependency should name:

| Option | The file it changes | The dependency before | The dependency now |
|---|---|---|---|
| `LAB126_ANDROID_POWER_SUPPLY_ABI` | `drivers/power/max77696-{battery,charger}.c` | `MX6SL_WARIO_BASE && BATTERY_MAX77696 && CHARGER_MAX77696` | `BATTERY_MAX77696 && CHARGER_MAX77696` |
| `WARIO_HALL_INPUT` | `drivers/char/wario_hall.c` | `WARIO_HALL && INPUT && MX6SL_WARIO_BASE` | `WARIO_HALL && INPUT` |
| `LAB126_ANDROID_BACKLIGHT_BLANKING` | `drivers/video/backlight/max77696-bl.c` | `BACKLIGHT_MAX77696 && MX6SL_WARIO_BASE` | `BACKLIGHT_MAX77696` |
| `LAB126_ANDROID_DISABLE_TOUCH_FW_UPDATE` | `drivers/input/touchscreen/cyttsp4_loader.c` | `MX6SL_WARIO_BASE && TOUCHSCREEN_CYPRESS_CYTTSP4` | `TOUCHSCREEN_CYPRESS_CYTTSP4` |
| `LAB126_ANDROID_PMIC_I2C_STRICT` | `drivers/mfd/max77696-i2c.c` | `MFD_MAX77696 && MX6SL_WARIO_BASE` | `MFD_MAX77696` |
| `LAB126_ANDROID_PRIVATE_IDENTIFIERS` | `arch/arm/mach-mx6/boardid.c` | `MX6SL_WARIO_BASE` | `LAB126` -- `boardid.o` was always built by `obj-$(CONFIG_LAB126)` |
| `LAB126_ANDROID_ATH6KL_FIRMWARE` | `compat-wireless/.../ath6kl/` | `MX6SL_WARIO_BASE && LAB126 && ATH6KL_SDIO` | `LAB126 && ATH6KL_SDIO` |

The grounds are not "it looks like it should work": before the change, the string `WARIO_BASE` appeared **exactly once** in each of those 7 patches, on that
`depends on` line, and nowhere in the code itself. So the board symbol was only the board the author happened to have, not a condition.

**The result** (the same flow, `imx60_duet_defconfig` plus this project's fragment): the silent misses go from **8 down to 2**,
and both of those are facts about the hardware rather than something this project was blocking itself -- `ARCH_MMAP_RND_BITS`
gives 8 for a requested 16 (the hard constraint of that machine's 2G/2G split), and `LAB126_ANDROID_ATH6KL_FIRMWARE`
comes out n (there is no ath6kl on a duet board, so off is right; the trouble is the fragment asking for it
unconditionally, see the next subsection).

The Oasis 1's kernel **builds, 0 errors**, with a uImage of **3,245,980 bytes** -- **704 bytes** more than the 3,245,276 before
the dependencies changed, which is those 6 pieces of code being compiled in for the first time; 28 modules.

**The control, where the PW3 must not change by one character**: the same modified tree with `imx60_wario_defconfig` and the same
fragment, all 147 requests taking effect, and the `.config` produced **byte for byte identical** to the release kernel's. Kconfig text does not
enter a binary (a kernel is a function of "source plus .config"), so changing the dependencies has zero effect on the PW3 kernel. Rather than
leave that as an inference, the release kernel was rebuilt in full from a freshly unpacked source tree: the `zImage` (which is what the release
ships), `System.map`, `Module.symvers`, the kernel configuration and the 28 modules are all byte for byte the same; the `uImage` differs only
in its 64-byte U-Boot header, which is the creation time `mkimage` writes and the header checksum that changes with it, the data checksum being the same in both.

### The fragment split in two: one half is what this Android wants, the other is what this board has

With the dependencies written correctly, duet's 2 remaining misses come from the **fragment**: it asks unconditionally for
`LAB126_ANDROID_ATH6KL_FIRMWARE=y` (that board has no such chip) and `ARCH_MMAP_RND_BITS=16` (that machine's ceiling is 8).
And there is a third, better hidden -- `CONFIG_LOCALVERSION="-lab126-pw3-android81"` **is accepted on any board**, so a kernel built for
the Oasis 1 would call itself pw3, and `/system/lib/modules` is named after that very string. Kconfig does not refuse it, so no check
would catch it.

So the fragment is split by "which question it answers":

| File | What it answers | Items |
|---|---|---|
| `config/android-core.config` | What this Android requires of **any** kernel (binder, ashmem, cgroups, SELinux, filesystems...) | 144 |
| `config/models/pw3.config` | The facts about this PW3: a 3G/1G split so 16 bits of randomisation, an AR6003 so the ath6kl firmware policy, a kernel calling itself `-lab126-pw3-android81` | 3 |
| `config/models/duet.config` | The facts about this Oasis 1: a 2G/2G split so 8 bits, no ath6kl so off, calling itself `-lab126-duet-android81` | 3 |

None of these fragments **touches** VMSPLIT or PAGE_OFFSET; those follow the defconfig -- one thing less to change when porting.

`merge-kconfig.py` takes several fragments and **refuses** to let the same option be requested in two places, or twice within one fragment -- rather than
overriding in order. Overriding in order makes the value depend on the argument order, with the reasoning written in two places and nothing
saying which one wins; somebody editing the base fragment cannot see that a model quietly overrode it long ago. For a model to want a different
value, the base fragment must not have that value.

**The result**: both models are **147 requests, 0 misses**, and the PW3's `.config` is still **byte for byte** the release kernel's.
Six items in `tools/tests/test_kconfig_fragments.py` guard the split: the two halves may not intersect, a fragment may not repeat itself,
every model `build-kernel.sh` knows must have a fragment, **every** caller has to pass both halves, and `merge-kconfig.py` itself refuses
and merges as described. The caller rule is there because passing only the base fragment still "passes the check" while nobody asks about
the model's 3 items -- `prepare-kernel-hardware.sh` runs on every AOSP build, and at first it passed only the base fragment.

### The two things left: Wi-Fi and the handover

**One, Wi-Fi: the chip is a Broadcom BCM4343W, and there is no usable driver yet.** `imx60_duet_defconfig` has `CONFIG_WLAN=y`, while **all three ath6kl options are `is not set`, and no Broadcom or Marvell driver is enabled either**; of the 31 options the whole defconfig builds as modules (`=m`), not one is a Wi-Fi driver. The board file names the chip.

Comparing the two defconfigs: `imx60_duet_defconfig` has `CONFIG_MX6SL_WARIO_WOODY=y`,
`CONFIG_BLUETOOTH_PWRCTRL=y` and `CONFIG_WARIO_WOODY_WAN=y`, and `imx60_wario_defconfig` (the PW3)
has none of the three. The code `CONFIG_MX6SL_WARIO_WOODY` guards is in `arch/arm/mach-mx6/board-mx6sl_wario.c`
(duet has no board file of its own; it shares wario's and is told apart at run time by `lab126_board_is()`),
commented `/* Broadcom 4343W Bluetooth structures */`, with `brcm_gpio_wifi_bt_init()`,
`brcm_gpio_wifi_power_enable()` and `brcm_gpio_wifi_set_normal_mode()` alongside, and Bluetooth over UART3
(`imx6sl_add_imx_uart(2, &mx6sl_wario_uart3_data)`, enabled only for `BOARD_ID_WHISKY_WFO`,
`BOARD_ID_WHISKY_WAN` and `BOARD_ID_WOODY`).

**The BCM4343W is a combined Wi-Fi and Bluetooth chip**, with Wi-Fi over SDIO. Its FullMAC driver is `brcmfmac`,
and this archive's `compat-wireless/drivers/net/wireless/brcm80211/brcmfmac/` has 18 files
(with another 51 in `brcmsmac`, 41 in `ath/ath6kl` and 36 in `mwifiex` beside it). The PW3 pulls that compat-wireless
tree in through the kernel configuration (`CONFIG_COMPAT_WIRELESS=y`, `CONFIG_ATH6KL=m`,
`CONFIG_ATH6KL_SDIO=m`), and duet does not, so its wireless module is built outside the kernel configuration.
Section 7 built it: the brcmfmac in this archive does not compile against the archive's own kernel, and the driver the stock Oasis 1 runs is not in the GPL release.

So what is left of this item, in order:

1. On the machine, find the path and the name of the firmware and NVRAM calibration files in the stock partition. The BCM4343W needs them as ath6kl does;
   they belong to each device and, as this project does with device-specific data, are read from that machine's stock system on the spot like the screen's waveform, never packaged;
2. Get a driver that loads; the three routes are in the table at the end of section 7, and backporting mainline `brcmfmac`'s support for the BCM4343W is route B;
3. This project's two patches (`0310`, the firmware and board data policy, and `1045`, waiting for the interface to be recognised) encode facts
   about ath6kl and have to be rewritten for the new driver -- which is the last step.

**Also worth noting**: this `imx60_duet_defconfig` comes from **the PW3's 5.16.2.1.1 source archive**. Amazon puts many models' defconfigs in one tree, and only the one matching that release is necessarily current. So "duet's defconfig has no Wi-Fi driver" proves it of **the defconfig in this archive**, which is not the same as saying the Oasis 1's shipped firmware was built that way. Every measurement in this section rests on this archive, and that holds equally for every conclusion in it.

**Two, the handover: on 2G/2G lowmem is itself the identity mapping, and only one thing in the private page table has to change.**

The easy wrong conclusion is "the mechanism does not hold on 2G/2G": taking `identity_limit` to be `TASK_SIZE`, which is `0x7F000000` under 2G/2G, below the start of physical memory at `0x80000000`, there seems to be not one legal destination. The step it leaves out is that on that machine `PAGE_OFFSET` and `PHYS_OFFSET` are equal, so lowmem is already an identity mapping. The reasoning follows.

First, why this machine builds a window of its own. The handover's physical plan has one contract: every destination must be below `identity_limit`. At 3G/1G here, `PAGE_OFFSET=0xC0000000` while the i.MX6SL's `PHYS_OFFSET=0x80000000`, and the two are **unequal** -- `__phys_to_virt(x) = x - PHYS_OFFSET + PAGE_OFFSET`, so a lowmem virtual address is `0x40000000` above its physical one and the kernel's own mapping is **not** the identity. So `prepare()` in `stage.c` zeroes the 3056 1 MiB sections `[0, 0xBF0)` of the private page table and writes the identity sections it needs itself, and copies `[0xBF0, 4096)` from the live table (the module's own code and the kernel are up there, and `pw3_enter_private_mapping` has to execute one more `bx` after writing TTBR0 before it jumps into the control page, so it must be kept). `identity_limit` therefore equals `IDENTITY_SECTIONS<<20`, and that number happens to equal this machine's `TASK_SIZE` -- a coincidence, not a cause.

duet is `CONFIG_VMSPLIT_2G` (where the model fragment's `ARCH_MMAP_RND_BITS=8` comes from): `PAGE_OFFSET=0x80000000`, and the MX6SL's `PHYS_OFFSET` is `0x80000000` too (`plat-mxc/include/mach/memory.h`; this tree takes it at run time under `CONFIG_RUNTIME_PHYS_OFFSET`, and `stage.c` reads the run-time `PHYS_OFFSET`). Putting equal values into the formula above: **`virt == phys`, and the kernel's lowmem mapping is an identity table ready made**.

So on that machine there is no need to "build a window in the user half" at all; the kernel already provides the 1:1 mapping the handover wants. Only one thing really has to change, and locally: `prepare()` today zeroes `[0, identity_limit)` unconditionally, a layout assumption that puts the window in the user half; on duet the destinations are in the kernel half and already 1:1, while the module's own code is at `0x7F000000`-`0x80000000` (`MODULES_VADDR = PAGE_OFFSET - 16 MiB`) -- **inside the range to be zeroed**, and zeroing it leaves `pw3_enter_private_mapping` unable to fetch its next instruction after writing TTBR0. By that machine's facts, the private table should copy the live table as it is and zero nothing.

So the contract is best put this way: `identity_limit` is "the upper bound of the 1:1 region in the table we are about to run on" -- `0xBF000000` here (a window the module built itself), and the end of lowmem on duet (512 MiB, which is `0xA0000000`, provided by the kernel). The `identity_limit > boot_ram_base` check in `pw3_plan.c` **passes** on duet (`0xA0000000 > 0x80000000`); what it refuses is a **plan written wrongly**, with a bound below the start of memory, not a class of machine.

**Corroboration**: the stock kernel's own `setup_mm_for_reboot()` is `identity_mapping_add(current->active_mm->pgd, 0, TASK_SIZE)` (`arch/arm/mm/idmap.c`), after which kexec does `cpu_reset(reboot_code_buffer_phys)`. Which is to say this project's `identity_limit` contract and the kernel's own soft boot are the same thing, not an invention of ours; and on a machine where `PAGE_OFFSET == PHYS_OFFSET`, the kernel's `identity_mapping_add(..., 0, TASK_SIZE)` does not cover RAM either -- it does not need to, because lowmem is already the identity.

**This one is enforced by the build, not by this document.** `IDENTITY_SECTIONS` in `stage.c` is derived from the profile's
`identity_limit` rather than written out as a second constant, with an assertion beside it checking, against **the kernel's own**
`MODULES_VADDR`, that the module area is above the bound:

```c
BUILD_BUG_ON(TARGET_IDENTITY_LIMIT>(unsigned int)MODULES_VADDR);
```

The two are equal here (both `0xbf000000`) so it passes; fill in an `identity_limit` for duet (which has to cover the top of memory,
`0xA0000000`) and it is above that machine's module area at `0x7f000000`, so **the module does not compile** and the error lands on this
very line. This was checked the other way round: with the PW3 profile's `identity_limit` changed to `0xc0000000`, the build stops on this
line with `size of unnamed array is negative`. Whoever does duet is stopped at build time and pointed at this section by the comment, rather
than debugging a device that stopped after TTBR0 and cannot even write a log. What that port has to do is have the private page table keep
a second region copied from the live table (the module area), not write a larger bound into the profile; this project does not write a path
in advance that no machine can run.

Two more points:

- The `0x9F000000`-`0x9F610000` that Falcon and the e-ink work buffer occupy **need not be written into `forbidden`** separately: this machine's rule is "destinations stay in the low 128 MiB" (`0x80000000`-`0x88000000`), and that rule already excludes it;
- Before going near the real machine, get a `VMSPLIT_2G` test kernel working under QEMU's vexpress (the test kernel is built by `tools/build-loader-stage-kernel.sh`, and its address-space split is set in `config/test-kernel/vexpress.config`). Note that vexpress's RAM starts at `0x60000000`, which is not equal to `PAGE_OFFSET=0x80000000`, so it cannot reproduce "lowmem is the identity" -- reproducing that means aligning the test kernel's `PHYS_OFFSET` too, or accepting that it only verifies the "copy the live table" path itself.

### What this section has not proved

Applying is **not** building, and building is **not** running. Everything above is a source-level conclusion, and not one line of it has been
executed on a second machine. Two things are left on the Oasis 1's kernel side: **the Wi-Fi driver** (the chip is known to be the BCM4343W;
where the driver can come from is in section 7), and **having the private page table keep the module area instead of zeroing a lowmem that
is already the identity** (that machine has `PAGE_OFFSET == PHYS_OFFSET`, so the kernel already provides the 1:1 mapping the handover wants,
which makes this item simpler there than here). The configuration itself is complete (147 requests, 0 misses), and the security parameter is
written into the model fragment as the hardware fact it is; the Voyage, although it shares the PW3's defconfig and is 3G/1G too so the handover
mechanism holds for it, still needs that machine, its board ID, a handover profile and measuring again.

## 7. Measured: the Oasis 1's Wi-Fi is not a configuration problem but a driver problem (2026-10-23)

Section 6 established that the chip is the BCM4343W and that the archive carries brcmfmac's source. Here duet's defconfig was really
built four times, and each failure answered one question.

**1. `CONFIG_BRCMFMAC=m` configures out as `n`.** The archive has `compat-wireless/drivers/net/wireless/brcm80211/`
(18 brcmfmac files), but `compat-wireless/Kconfig` only `source`s ath, and
`compat-wireless/drivers/net/wireless/Makefile` only has `obj-$(CONFIG_ATH_COMMON) += ath/`.
**The source is there; the wiring is not.**

On the PW3 side, for comparison: `imx60_wario_defconfig` has `CONFIG_COMPAT_WIRELESS=y` and
`COMPAT_WIRELESS_MODULES=m`, and the kernel's top-level Makefile has `drivers-y += compat-wireless/` --
so the ath6kl this project has been using all along is built out of compat-wireless, and that route itself works.

**2. With the Kconfig and the Makefile wired up, it fails to compile with `KERNEL_VERSION` undefined and `netdev_hw_addr` having no
`dmi_addr`.** One root cause for both: the first line of ath's Makefile is
`NOSTDINC_FLAGS += -I$(srctree)/compat-wireless/include`, and brcm80211's Makefile does not have that line,
so compat's shim headers are not on the include path at all.

**3. Supplying that turns it into a link conflict**: `multiple definition of wext_handle_ioctl`, `lib80211_get_crypto_ops` and more.
duet's defconfig builds the kernel's own wireless stack into the kernel (`LIB80211` and its three software ciphers, `HOSTAP`, `WEXT_SPY`,
`CFG80211=y`) while compat provides symbols of the same names; wario's defconfig has all of these off.
Turning them off the way wario has them, changing `CFG80211` to `m` and adding `NL80211_TESTMODE=y` (compat's
`nl80211.c` needs it to compile), this layer passes.

**4. Then the errors land in `drivers/staging/brcm80211/`** rather than compat's copy: this tree has brcm80211
**twice**, staging's being the older snapshot, and both declare **the same symbols** (`BRCMFMAC`,
`BRCMUTIL`), so they cannot be selected apart; and `CONFIG_STAGING=y` is something this project must have (binder and ashmem are there).
Staging's copy does not compile against this kernel: `wl_iw.c` uses `dhd_wait_pend8021x`, and the `num_private` and `private_args`
fields of `iw_handler_def` that disappeared long ago.

**The conclusion**: there is **no Broadcom driver in this archive that builds against the archive's own kernel**. The source of the driver
the stock Oasis 1 runs is not in this GPL release. So the Oasis 1's Wi-Fi driver has three routes. Route A is recommended: it needs no new
driver code, and it fits how this project treats proprietary binaries (never distributed, only read on the user's own device); whether it
holds depends on the stock module loading into this project's kernel, which has to be checked on the real machine.

| Route | What it is | The price |
|---|---|---|
| **A (recommended)** | Take the stock `.ko` off **the user's own device** at install time and load it | It requires this project's kernel to load a module built for the stock one (matching vermagic and symbols). There is no ready path for it: the boot program used to have a branch that loaded the stock REAGL module, removed before v0.1.0 because none of its three preconditions held (the kernel is built without REAGL, no step supplied the module, nothing switched it on); and the installer has no step yet that takes a module off the device. This option needs both parts added |
| B | Backport a newer brcmfmac to 3.0.35 | Real porting work, and the firmware and NVRAM still have to come off the device |
| C | Fix staging's copy's use of wext | It is a 3.0-era snapshot, and its support for the BCM4343W FullMAC part is itself in doubt |

What `config/models/duet.config` records is this conclusion, not a string of "enable brcmfmac" options --
the configuration could be written, but it would enable a driver that does not build, and that is hiding the problem inside the build.
