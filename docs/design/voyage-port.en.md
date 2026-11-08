# Porting to the Kindle Voyage

The Voyage (7th generation, 2014) is the second model this system is for. Its device directory, boot
profile, identity checks and kernel patches are in the repository, and **the complete start chain has
run in the emulator** (QEMU with the stock 5.13.6 firmware): handoff into the Voyage kernel →
handoff-init → preinit mounting the images → Android `boot_completed=1`, the Android desktop read back
through an emulator-only display-source option, and **the release installer's whole install flow, touch
input and a long-press-power restart back to the stock system all verified as well**. **Not one line has
run on real hardware**, so every device conclusion is still open. This document says where it differs
from the PW3, what the repository has, what the emulator proved, which switches belong to the emulator
only, and what real hardware still has to be tested.

## 1. Where it differs from the PW3

| Item | PW3 | Voyage | Why |
|---|---|---|---|
| SoC | i.MX6SL | the same part | the board file in Amazon's published GPL sources serves both |
| Kernel configuration | `imx60_wario_defconfig` | the same file | the source archive's own `BUILD_HOW-TO.txt` says so |
| Panel | 1072×1448, 300 ppi | same size, same density | the two panels are the same, so rotation, density, type sizes and margins all carry over |
| Memory and split | 512 MiB, 3G/1G | the same | one `imx60_wario_defconfig`, so `identity_limit` carries over too |
| Power management | max77696 | the same part | same board, same defconfig |
| Touch | Cypress cyttsp4 | the same part | as above |
| Wi-Fi | AR6003 (ath6kl) | the same part | as above; the **paths** of the calibration and firmware are still read, section 2 |
| Hall sensor | present | present | as above, the magnetic cover sleeps through the same path |
| Machine / board ID | 4091 (Wario) | 4091 (Wario) | the board ID comes from IDME and is read at run time |
| Stock firmware | 5.16.2.1.1 | 5.13.6 | the two stock systems differ: `/sbin/init`, `/etc/upstart/init.sh` and `/usr/sbin/eips` have different bytes and hashes |
| Stock kernel build string | `#8 PREEMPT Tue Aug 1 12:49:59 UTC 2023` | `#2 PREEMPT Thu Apr 8 09:39:57 UTC 2021` | the handover runs the stock kernel's own function bodies; the release is the same, the build string is not |
| CPU identity (MIDR) | `0x412fc09a` | the same Wario silicon | the same value on hardware; QEMU's Cortex-A9 reports `0x410fc090`, synthesised only in an emulator build |

**The Voyage and the PW3 differ in product identity and firmware, not in hardware.** What this side
changes is the product name, the board name, the boot profile, the identity table, and reading "which
firmware is this machine" from the profile instead of the literals written for the PW3.

## 2. What the repository already has

| File / object | What it does |
|---|---|
| `device/amazon/voyage/voyage.mk`, `BoardConfig.mk`, `AndroidProducts.mk`, `vendorsetup.sh` | inherit the pw3 product and board configuration and override only `PRODUCT_NAME`, `PRODUCT_DEVICE`, `PRODUCT_BRAND`, `PRODUCT_MODEL` and `TARGET_BOOTLOADER_BOARD_NAME` |
| `boot/profiles/voyage-5.13.6.json` | all the firmware facts: stock cmdline, kernel release/version, the four stock function bodies' sizes/hashes, the four stock files (init, init.sh, init.exe, eips) with path/bytes/sha256, `stock_cpuid`, and the Wi-Fi calibration/firmware paths |
| `boot/devicedata/devicedata.c/.h` | a `dd_voyage_5_13_6` struct and `dd_identity_voyage_5_13_6[]`, referenced by `dd_profile`; `DD_PROFILE` selects the struct at build time |
| `boot/launcher/helper.c` | `original_boot()` and the eips check read the identity table from `DD_PROFILE`; the archive limit reads the profile's `device_archive_max` |
| `boot/launcher/launcher-template.sh`, `boot/build_launcher.py` | the fallback screen's eips hash becomes `@EIPS_HASH@`, substituted from the profile at build time; `--profile` selects the model |
| `boot/build_entry.py` | `--profile` / `--kernel-source`; passes `-DKERNEL_RELEASE=<kernel.release>` to handoff-init |
| `boot/build_module.py`, `boot/handoff/` | target-lock's `TARGET_STOCK_RELEASE` / `TARGET_STOCK_VERSION` / `TARGET_CPUID` come from the profile; `PWS_EMULATOR` and `direct_commit` exist only in an emulator build |
| `patches/android-core/1060-…patch` | release series patch 86: a Cortex-A9 TLB lockdown probe bypass gated by `readom.emulator=1`, a no-op without the token |
| `tools/build-preinit.py`, `tools/preinit-sha256.c` | build the Voyage `pw3-testfile-init` outside AOSP with musl and a project-owned SHA-256 |
| `tools/build-voyage-release.py` | bootstrap the Voyage system/data images and a draft manifest from the released v0.1.0 artifacts (validation only, section 6) |
| `tools/emulator/` | synthetic Voyage panel/waveform generator, QEMU launcher, guest preparation/suspend preparation scripts, read-back verifier |
| `installer/build_payload.py`, `tools/prepare-aosp-device.sh`, `build-aosp-system.sh`, `run-aosp-build-job.sh` | the installer's C is unchanged; payload size, device-tree sync and the AOSP product all follow the model |
| `tools/tests/` | `test_profile_matches_devicedata.py` holds the profile and the C struct together; each new tool has host tests; `patch-series` covers 1060 |

Not one line of the installer's C changed: the model name on the E101 screen comes from the profile's
`supported_zh` / `supported_en`, and `render_screens.py` has taken a `--profile` for a while. The device
directory holds four files and inherits everything else from pw3 on purpose: the two boards are the same
part everywhere this system touches, and a copy would be hundreds of files changed in pairs. Three things
are **deliberately not overridden**, because they name the hardware implementation and not the product:
`ro.hardware.gralloc/hwcomposer/lights/power = pw3`, the module names such as `gralloc.pw3`, and
`init.pw3*.rc`, `fstab.pw3*`, `pw3-preinit.conf` and `androidboot.hardware=pw3` on the entry's command
line -- init looks `/init.<name>.rc` up by `ro.hardware`, and renaming it only buys a system that cannot
find its rc.

## 3. What the emulator proved

| Milestone | Result | Evidence (in `readom-port/`) |
|---|---|---|
| Synthetic panel/waveform accepted by the stock 5.13.6 driver (`source=stored`) | ✅ | `emu-voyage/` environment report and the `emo/` panel read-back |
| Identity checks + `dd_archive` (panel/WRF/device data) | ✅ | `voyage-e2e-report` section 4 |
| Probe goes through the **real** `pm_suspend` (`pm_test=core`, with fsr_keypad unloaded) | ✅ | `voyage-e2e-report` section 6B.1 (`elapsed_ms=908`, `suspend debug: Waiting for 5 seconds`) |
| Commit goes through the real suspend → transition into the Voyage kernel | ✅ | `voyage-e2e-report` section 6B.2 |
| Patch 1060 takes effect under the test cmdline (`TLB lockdown probe skipped`) | ✅ | `voyage-e2e-report` section 6B.2, `kernel-fix-report` section 5.2 |
| handoff-init (BEGIN/DRIVER/SCREEN/END, PMIC keeper restored) | ✅ | `voyage-e2e-report` section 6B.3 |
| preinit (V10 contract, dm-linear mount, system SHA256) | ✅ | `voyage-e2e-report` section 6B.4 |
| Android init to `sys.boot_completed=1` | ✅ | `voyage-e2e-report` section 6B.5 |
| Android's composition readable on the panel (GTK console + `update-source`) | ✅ | `voyage-e2e-report` section 6C (`task23-evidence.txt`; a complete 1072×1448 desktop: status bar, search box, five icons, bottom navigation) |
| Touch (QMP-injected tap/back) | ✅ | `voyage-e2e-report` section 6C: a tap on the Settings icon opens Android Settings, a tap on Back returns Home; `shots/task23-*.png` |
| Long-press power → Android power menu → restart to stock | ✅ | `voyage-e2e-report` section 6C: QMP holds POWER for 20 s, "restart to Kindle" is chosen, and the U-Boot reboot lands on the stock Home (`source=stored`, lab126_gui running, fsr modules reloaded) |
| Installer release end to end (install → start the installed system) | ✅ | `installer-e2e-report`: release-v2 installs, the strict module is correctly refused by the hardware gate, and the installed images start to `boot_completed=1` (93.0 s) |

What the emulator **can** prove: the handoff chain, the kernel starting, handoff-init, preinit, Android
up to `boot_completed`, that the composer's output reaches the EPDC panel and is readable, that injected
touch drives the Android UI, that a long-press of power goes through Android's own menu back to the stock
system, and that the release installer's whole install flow and its installed images boot. What it
**cannot** prove: real EPDC waveform instructions/greys/ghosting/refresh timing (QEMU models scanout
only), the real SoC's low-power suspend and wake sources, front light/touch calibration/Wi-Fi
calibration/battery/charging behaviour, or eMMC performance -- those need the device. The full list is in
`voyage-e2e-report` section 6C.1.

## 4. Emulator-only switches

| Switch | Where and default | What it does |
|---|---|---|
| `readom.emulator=1` (kernel cmdline) | repository: patch 1060; **only the test entry's** cmdline carries it, a release cmdline never does | skip the `bus_freq` Cortex-A9 TLB lockdown probe and fill the i.MX6SL static map's VA/PA/attr instead; a no-op without the token, so device behaviour is unchanged |
| `PWS_EMULATOR` (module compile define) | repository: `boot/build_module.py --emulator`; a strict/release module never defines it | synthesise the hardware attestation only (MIDR/DCP/USB/engine fields); probe and the default commit still use the real `pm_suspend`; the module gains the `direct_commit` parameter |
| `direct_commit` (module parameter) | repository: emulator module only, 0644, default 0 | 0 = real suspend (the production path); 1 = commit by a direct jump that skips `pm_suspend`, a run-time fallback for a guest whose driver wedges the suspend |
| `-global driver=imx.epdc,property=update-source,value=on` | QEMU fork (not this repository); default off; `tools/emulator/start-voyage.sh --epdc-update-source` adds it | scan the panel out of the EPDC update buffer so Android's composition is readable; without it the output is pixel-identical to before. Touch validation also needs `--gui` + Xvfb. The short form `imx.epdc.update-source=on` does not work (the type name has a dot); use the long form |
| Disable `deviced` (`/etc/upstart/deviced.conf.off`) | guest, runtime (emulator only) | the emulator does not implement fsr_keypad's I2C; deviced blocks in D state and breaks the freeze |
| Unload `fsr_keypad` (+ `drv26xx_haptics`) after stopping `lab126_gui`/`x`/`lab126` | guest, runtime: `tools/emulator/prepare-voyage-suspend.sh` | on the emulator the stock suspend hangs in fsr_keypad's suspend callback waiting on a mutex; unloading it lets the real suspend complete. A device/release path never depends on this |
| Serial console (`ttymxc0`) + a rate-limited character tool | emulator run procedure | the entry must run from the serial console; an SSH shell makes the pm freeze fail. TCG drops UART characters, so long commands need pacing |

## 5. What real hardware must still test

- **Real panel waveform/greys/ghosting/refresh timing**: QEMU executes no waveform instructions; the
  synthetic panel only proves the data path.
- **Front light and VCOM**: the brightness curve, the VCOM calibration value and temperature compensation
  all have to be measured on this panel.
- **Touch and keys**: cyttsp4 touch calibration, the physical keyboard and power key, and the Hall cover's
  sleep and wake.
- **Wi-Fi calibration**: the profile records the WAN calibration file read from the running 5.13.6; a
  Wi-Fi-only unit may name the WFO file, so confirm before installing on hardware.
- **Suspend power and the PMIC keeper**: the current draw of a real suspend, the wake sources, and the
  voltages after LDO7/BUCK1 are restored.
- **Installation on hardware**: the installer end to end (download/verify/write/start after install) is
  only progressing on the emulator; a real device has not run it.
- **Restart back to the stock system**: long-press power from Android, and the automatic fallback when a
  start fails.

## 6. The bootstrap system image versus a full AOSP build

| Route | What it is | Use and limits |
|---|---|---|
| bootstrap images (`tools/build-voyage-release.py`) | the released v0.1.0 PW3 `system.img` with the Voyage kernel's 32 modules swapped in and seven identity properties rewritten in `build.prop`; the data image comes from the released template | only for validating the handoff/kernel/preinit/Android start on the emulator; **not an AOSP build product**, the userspace is still the PW3 one, so it cannot ship as a release image |
| Full AOSP build (`PW3_AOSP_PRODUCT=voyage`) | build the repository's Voyage product (inheriting pw3) through the whole AOSP build to get a Voyage-identity system image | a real release must use this; it needs an AOSP tree and build VM, which this machine does not have, so it has not been done |
| Test entry (`build_launcher.py --emulator`) | a start-only entry with no installer, embedding the Voyage kernel, the emulator module, the public initramfs and the device data | emulator e2e only; it cannot be installed on hardware (the helper and module are test-only artefacts) |

The bootstrap route is worth it because it validates the kernel, the handoff, preinit and Android start
against the real firmware without an AOSP build first; its risk is that "identity" is only the
`build.prop` and the modules -- every other byte is still the PW3's -- so a release has to replace it
with a full AOSP build and go through acceptance on hardware again. Both routes share the same profile
and identity checks.

## 7. What is not verified yet, and the risks

- **Not one line has run on real hardware**: every item in section 5 is still open.
- **The installer passed end to end in the emulator, but a real-hardware install is untested**: release-v2's install, the strict module's refusal at the hardware gate and the installed images' start all passed on the emulator (`installer-e2e-report`); a device install and the HTTPS download route have not run.
- **Touch and restart passed through QMP injection, on the emulator only**: real touch calibration, the power key and the Hall cover still need testing.
- **Display quality is not verified**: the emulator only proves that composition reaches the panel and is readable; the greys and refresh policy are the PW3 panel's measurements.
- **The bootstrap images are not AOSP build products**: they must be replaced before a release, and the release image must be accepted on hardware again.
- **The kernel/module bytes depend on the build-tree path**: 3.0.35 writes absolute source paths into the image, so byte-for-byte reproducibility holds only when building at `/root/readom-kernel`, see `kernel-fix-report` section 4.3 and `release-build`.
- **The profile's Wi-Fi path records the WAN file**: a Wi-Fi-only unit may differ, see section 5.

## 8. How to build it (both routes) and the reproducibility constraint

```sh
# AOSP route (the real release; this machine has no AOSP tree, so it has not run)
PW3_AOSP_PRODUCT=voyage PW3_BUILD_VARIANT=user bash tools/build-aosp-system.sh
python3 installer/build_payload.py --model voyage --data-format 1 --version vX --output <payload>
python3 installer/screens/render_screens.py --manifest <payload>/manifest.json \
  --profile boot/profiles/voyage-5.13.6.json --output <screens> ...
python3 boot/build_entry.py --profile boot/profiles/voyage-5.13.6.json --name Readom.sh ... --output <entry>

# bootstrap route (emulator validation)
python3 tools/build-preinit.py --output <preinit> --board voyage
python3 boot/build_module.py boot/profiles/voyage-5.13.6.json --kernel <zImage> \
  --public-initramfs <initramfs> --cmdline <cmdline> --output <module> --emulator
python3 boot/build_launcher.py ... --profile boot/profiles/voyage-5.13.6.json --emulator --output <entry>
```

Byte-for-byte reproducibility of a release requires the build tree at `/root/readom-kernel` (a bind
mount to `/home/builder/readom-kernel` also works); otherwise the source-path strings inside the kernel
and modules change and the hashes with them. This holds for any artefact that embeds the kernel or its
modules. The emulator test entry appends `readom.emulator=1` to its cmdline and a release entry never
does; `direct_commit=1` is set by hand only when QEMU's suspend has to be bypassed, and the default 0
uses the real suspend a device uses.

## 9. Evidence index

- `readom-port/voyage-e2e-report`: the emulator end to end (section 6A.7 is the can/cannot-prove list, section 6B the v3 full chain).
- `readom-port/qemu-epdc-report`: the `update-source` option and the quantitative proof that Android is readable on the panel.
- `readom-port/display-analysis`: root cause of the panel noise (fixed scan-out address versus the update buffer).
- `readom-port/kernel-fix-report`: the patch 1060 root cause, the release kernel rebuild, and the path dependency (section 4.3).
- `readom-port/qemu-suspend-research`: the research showing the real suspend is the production path.
- `readom-port/bootstrap-build-report`: building and hashing system/data/payload/screens.
- `readom-port/preinit-build-report`: the independently built Voyage `pw3-testfile-init`.
- `readom-port/test-entry-report`, `release-entry-report`: the test-entry v3 and release-v2 artefacts and checks.
- `readom-port/installer-e2e-report`: the installer's emulator end-to-end progress (in progress).
- `readom-port/emu-voyage/`: runs, shots, panel and waveform evidence, start and preparation scripts.
