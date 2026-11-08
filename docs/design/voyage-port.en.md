# Porting to the Kindle Voyage

The Voyage (7th generation, 2015) is the second model this system is for. Its device
directory, boot profile and build wiring are in the repository, **but not one line of it has
run on a Voyage**: the profile still carries `TODO(voyage-emulator)` for the facts of the
stock 5.13.6 firmware, which are read off the machine. This document is two things — where it
actually differs from the PW3, and what is still missing.

## 1. Where it differs from the PW3

| Item | PW3 | Voyage | Why |
|---|---|---|---|
| SoC | i.MX6SL | the same part | the board file in Amazon's published GPL sources serves both |
| Kernel configuration | `imx60_wario_defconfig` | the same file | the source archive's own `BUILD_HOW-TO.txt` says so |
| Panel | 1072×1448, 300 ppi | same size, same density | the two panels are the same, so rotation, density, type sizes and margins all carry over |
| Memory and split | 512 MiB, 3G/1G | the same | one `imx60_wario_defconfig`, so `identity_limit` carries over too |
| Power management | max77696 | the same part | same board, same defconfig |
| Touch | Cypress cyttsp4 | the same part | as above |
| Wi-Fi | AR6003 (ath6kl) | the same part | as above; the **paths** of the calibration and firmware are still read, section 4 |
| Hall sensor | present | present | as above, the magnetic cover sleeps through the same path |
| Machine / board ID | 4091 (Wario) | 4091 (Wario) | the board ID comes from IDME and is read at run time |

**What that means: the Voyage and the PW3 differ in product identity and firmware, not in
hardware.** So this side changes in four places: the product name, the board name, the boot
profile, and the one decision in the helper about which firmware this machine is.

## 2. What changed

| File | What changed |
|---|---|
| `device/amazon/voyage/voyage.mk` | inherits `device/amazon/pw3/pw3.mk` and overrides only `PRODUCT_NAME`, `PRODUCT_DEVICE`, `PRODUCT_BRAND`, `PRODUCT_MODEL` |
| `device/amazon/voyage/BoardConfig.mk` | `include`s the pw3 file and overrides only `TARGET_BOOTLOADER_BOARD_NAME` |
| `device/amazon/voyage/AndroidProducts.mk`, `vendorsetup.sh` | point at `voyage.mk`; the `voyage-user` and `voyage-userdebug` lunch combos |
| `boot/profiles/voyage-5.13.6.json` | the new boot profile: machine, RAM plan, ATAG kinds, the E101 wording, and the firmware facts still to be read |
| `boot/devicedata/devicedata.c`, `devicedata.h` | a `dd_voyage_5_13_6` struct and the `DD_PROFILE` macro the helper selects it with |
| `boot/launcher/helper.c` | the four `&dd_pw3_5_16_2_1_1` become `&DD_PROFILE`; the `stock_profile` check reads release and version out of the struct |
| `boot/build_launcher.py` | `--profile`; passes `device_data_profile` as `-DDD_PROFILE`; checks the E101 wording against that profile |
| `boot/build_entry.py` | `--profile`, handed down to `build_launcher.py`; a profile still carrying `TODO(` is refused outright |
| `installer/build_payload.py` | `--model`: the data image's size and the AOSP product directory come from the model |
| `tools/build-aosp-system.sh`, `run-aosp-build-job.sh` | `PW3_AOSP_PRODUCT` decides lunch, the output directory and the makefile stamp |
| `tools/prepare-aosp-device.sh` | synchronises the model's device directory as well as the pw3 one, because inheriting needs both in the AOSP tree |
| `tools/tests/test_profile_matches_devicedata.py`, `tools/fill-voyage-profile.py` | hold the profile and the C struct to each other; fill the profile from device facts and rewrite the struct with it |

Not one line of the installer's C changed: the model name on the E101 screen comes from the
profile's `supported_zh` / `supported_en`, `render_screens.py` has taken a `--profile` for a
while, and a Voyage build only has to have that profile filled in.

## 3. Why inherit rather than copy

`device/amazon/voyage/` holds four files; everything else comes from `device/amazon/pw3/`.
That is deliberate: the two boards are the same part in every place this system touches, and a
copy would be three hundred files that have to be changed in pairs — and the first time one is
missed, the two machines quietly run different systems.

Three things are **deliberately not overridden**, because they name the hardware implementation
and not the product: `ro.hardware.gralloc/hwcomposer/lights/power = pw3`, the module names
`gralloc.pw3` / `hwcomposer.pw3` / `lights.pw3` / `power.pw3` / `libhealthd.pw3`, and
`init.pw3*.rc`, `fstab.pw3*`, `pw3-preinit.conf` and `androidboot.hardware=pw3` on the entry's
command line. The last one is the easiest to get wrong: init looks `/init.<name>.rc` up by
`ro.hardware`, and the rc files this product installs are the pw3 ones — renaming it only buys
a system that cannot find its rc.

## 4. What the boot profile is still missing

Six fields in the profile are empty or say `TODO(voyage-emulator)`. They are **facts read off
that machine** and cannot be copied from the PW3: one wrong token and every Voyage stops at
E101 before anything is downloaded.

| Field | Where it comes from |
|---|---|
| `cmdline_base` | the running system's `/proc/cmdline`, without `root=` and `rootwait` |
| `stock_kernel_version` | `uname -v` (the release is known to be `3.0.35-lab126`; the build string is not) |
| `stock_functions` | sizes and SHA-256 of `pm_test_show`, `l2x0_flush_all`, `l2x0_disable`, `l2x0_inv_all` from the stock kernel, plus the kernel image's own SHA-256 |
| `wifi_board`, `wifi_firmware` | the AR6003 calibration and firmware paths that firmware's `/etc/upstart/wifim` selects; the emulator log showed a WAN calibration file where the PW3 uses WFO, so this one may be a real difference |
| `module_kernel_source`, `module_kernel_output` | the Voyage kernel tree and its build directory, once the kernel side has built one |
| `footprint_profile` | `pw3` for now: one defconfig, the same 3G/1G split, and the decompressor's physical envelope is derived from that kernel's own build files rather than copied |

`tools/fill-voyage-profile.py` is the step that fills them in: it takes a facts JSON read off
the device, checks it (one line with no `root=`, hashes of 64 hex digits, no zero-byte function
body, absolute paths), writes the profile back, **and rewrites the struct in `devicedata.c`
with the same values** — so the two copies cannot drift. `--check` only reports whether a
`TODO` is left; it changes nothing.

## 5. How to build one

On the build machine the order is the release checklist's, with one extra `--model` /
`--profile`:

```sh
PW3_AOSP_PRODUCT=voyage PW3_BUILD_VARIANT=userdebug bash tools/build-aosp-system.sh
python3 installer/build_payload.py --model voyage --data-format 1 --version vX --output <payload dir>
python3 installer/screens/render_screens.py --profile boot/profiles/voyage-5.13.6.json --output <screens dir> ...
python3 boot/build_entry.py --profile boot/profiles/voyage-5.13.6.json --name Readom.sh ... --output <entry dir>
```

Without `--model` / `--profile` everything falls back to pw3, so the PW3 build command does not
change by a single word.

## 6. What has not been verified

- **Not one line has run on a Voyage.** The device directory and the build wiring are written from the sources and the documents, not from a measurement.
- Six fields of the boot profile are still unfilled, and `build_entry.py` refuses to build a Voyage entry because of it (that is deliberate).
- Whether the board-level init really takes the same branch is unchecked: the stock kernel branches on a run-time board ID, and which branches a Voyage's ID takes has to be read on the machine.
- Display, touch, front light, battery and sleep have not been measured on a Voyage panel; the PW3's grey curve and refresh policy are that panel's measurements.
- The installer still knows only the PW3: on a Voyage it stops at the first step with E101 and changes no file.

## 7. How to check it in the emulator

The first step is not a whole system but **identity**: run the stock 5.13.6 in QEMU, read
`/proc/cmdline`, `/proc/version` and the board ID out of it, fill them into the profile and the
struct, and then verify `dd_device_matches` and nothing else — it passing is what says this
"machine" is recognised as a Voyage rather than as a PW3. The second step is the handover:
build a start-only, install-nothing entry with `boot/build_launcher.py`, and watch the kernel
being placed in memory, jumped to, and Android's init coming up. Display and power on a real
device still have to be measured separately.
