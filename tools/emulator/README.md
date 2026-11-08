# tools/emulator — Voyage synthetic panel, waveform and QEMU launcher

This directory builds and runs the WSL/QEMU Kindle Voyage environment used to
validate the Voyage EPDC / device-data work without a physical device. It gives
the emulator a **synthetic Voyage panel flash image** and the matching **2048-byte
WRF waveform**, so the stock 5.13.6 display stack comes up with
`/proc/eink/waveform/source = stored` and hands out byte-exact panel/waveform
data for `boot/devicedata/devicedata.c` and the Android 0400/0700 patches.

Environment (already provisioned): WSL distro `KO-test`,
`/root/kvemu/eink-emulator` (QEMU 11.0.2 fork, instance `machines/kv`,
firmware `kindle-voyage/5.13.6`). Guest login: `root@127.0.0.1:2222`, key
`/root/kvemu/eink-emulator/build/ssh/id_ed25519`.

Produced artifacts live in `readom-port/emu-voyage/`:

| path | content |
|---|---|
| `panel/panel.img` | 512 KiB mx25l4005a NOR image: WBF at 0x886, panel info page at 0x70000 |
| `panel/synth.wrf.gz` | gzip(stored) of the 2048-byte WRF the stock driver loads (hidden FAT / override) |
| `panel/synth.pw3.gz` | gzip(stored) of the `PW3WFP1` envelope `dd_envelope()` builds |
| `reads/` | the bytes the running guest returned from `/proc/eink/...` plus decoded state |
| `boot-evidence/` | serial log of the verified boot and its EPDC/display lines |
| `shots/home-synthetic-panel.png` | Home rendered through the synthetic panel (1072×1448, QMP screendump) |

## Files here

| file | role |
|---|---|
| `make_voyage_panel.py` | deterministic generator of `panel.img`, `synth.wrf.gz`, `synth.pw3.gz`; self-validates against replicas of `dd_envelope()`, `dd_gzip_stored()`, `android_wf_header_valid/container/wrf_layout`, the stock `process_fw` rules and the panel-driver decode |
| `verify-voyage-guest.py` | verifies the bytes the **running guest** returned against the generated files (must be run after the guest dump) |
| `start-voyage.sh` | starts QEMU directly with the panel backing file; no `-no-reboot`; rotates the serial log; prints the QEMU pid |
| `prepare-voyage-guest.sh` | guest-side prep: disables deviced, stops wand/mwan, writes the waveform to the hidden FAT and installs the stock `/opt/eink/images/<PANEL_CODE>/` override |
| `prepare-voyage-suspend.sh` | guest-side prep for **real-suspend** runs: stops the UI jobs that hold `/dev/keypad` and `/dev/input/event2`, then unloads `fsr_keypad` (+ its `drv26xx_haptics` dependency) so the stock suspend path can complete on QEMU |

Scratch/one-shot diagnostics from the bring-up are in
`readom-port/emu-voyage/tmp/` (stop/status/dump helpers referenced below).

## Quick start

Run everything from WSL (`wsl -d KO-test -e bash -lc '...'`; the repository is
at `/mnt/d/ankindle/readom`). Windows PowerShell users can prefix commands with
`wsl -d KO-test -e bash -lc "..."` the same way.

```bash
REPO=/mnt/d/ankindle/readom
V=/mnt/d/ankindle/readom-port/emu-voyage

# 1. Generate (or regenerate) the panel and waveform files; hashes are fixed.
python3 $REPO/tools/emulator/make_voyage_panel.py

# 2. Start QEMU with the synthetic panel attached (background, serial log + QMP).
bash $REPO/tools/emulator/start-voyage.sh
#    ...or inspect the argv without starting anything:
bash $REPO/tools/emulator/start-voyage.sh --print-command

# 3. Wait for the guest (TCG: ~3 minutes to Home). SSH answering is enough:
until ssh -i /root/kvemu/eink-emulator/build/ssh/id_ed25519 \
      -o StrictHostKeyChecking=no -p 2222 root@127.0.0.1 true 2>/dev/null; do sleep 5; done

# 4. Install the synthetic waveform into the guest (first boot only, persistent
#    in the qcow2 overlay; re-run after `eink reset` or on a new instance).
#    This uploads synth.wrf.gz to /tmp and runs the guest-side prep script.
cat $V/panel/synth.wrf.gz | ssh -i /root/kvemu/eink-emulator/build/ssh/id_ed25519 \
    -o StrictHostKeyChecking=no -p 2222 root@127.0.0.1 'cat > /tmp/synth.wrf.gz'
ssh -i /root/kvemu/eink-emulator/build/ssh/id_ed25519 -o StrictHostKeyChecking=no \
    -p 2222 root@127.0.0.1 bash -s < $REPO/tools/emulator/prepare-voyage-guest.sh

# 5. Cold boot once more so /etc/upstart/display follows the override path.
bash $V/tmp/stop-qemu.sh
bash $REPO/tools/emulator/start-voyage.sh

# 6. Wait for SSH again, then read state and verify.
bash $V/tmp/dump-reads.sh      # via ssh: dump /proc bytes to /var/local/emu-voyage-read
bash $V/tmp/fetch-reads.sh     # copy them to readom-port/emu-voyage/reads/
python3 $REPO/tools/emulator/verify-voyage-guest.py
```

Expected final state (raw output in
`readom-port/emu-voyage/environment-report.md`):

```
source: stored
panel_bcd: EBA
panel_human_version: V220_EBAL_ED060TC1_3CE
waveform_human_version: V220_EBAL_3CE
panel_embedded_checksum: 0xF30F6883   (== computed)
panel_size: 456570      wrf_size: 2048
```

## The panel backing file: `-global mx25l4005a.drive=...`

`qemu/hw/arm/imx6sl-kindle.c:kindle_imx6sl_attach_panel_flash()` creates the
mx25l4005a and **only** programs the in-memory synthetic AC header + barcode
when the device has no drive (`if (!m25p80_get_blk(flash))`). Attaching a raw
drive therefore replaces the synthetic NOR with our image:

```
-drive if=none,id=panelwbf,file=.../panel.img,format=raw,readonly=on
-global mx25l4005a.drive=panelwbf
```

This was verified on the running QEMU build, not assumed: with a throw-away SD
image and `-S`, `info qom-tree` shows the mx25l4005a and
`qom-get /machine/unattached/device[13] drive` returns `"panelwbf"`
(`tmp/test-global.sh`, `tmp/test-global2.sh`). The guest then reports the
image's panel strings (see the report), which the default synthetic NOR (only
`wf_type` at 0x899 and barcode `EBA` at 0x70050) cannot produce.

## What the generator writes

### `panel.img` (exactly 524288 bytes)

| region | content |
|---|---|
| 0x000000–0x000885 | 0xFF (erased) |
| 0x000886–0x010885 | WBF, 65536 bytes, `file_length = 0x10000` |
| 0x010886–0x06FFFF | 0xFF (erased tail of the 456570-byte waveform region) |
| 0x070000–0x0703FF | panel information page (E Ink alphabet; see below) |
| 0x070400–0x07FFFF | 0xFF (unused panel info / test sections) |

WBF header (offsets are the kernel's bit-field byte layout in
`struct waveform_data_header`; field order checked against the 5.13.6 GPL
`drivers/video/mxc/mxc_epdc_fb.c`):

| off | field | value |
|---|---|---|
| 0x00 | checksum | CRC-32 over 65536 bytes with bytes 0..3 zeroed (`0xF30F6883`) |
| 0x04 | file_length | 0x00010000 |
| 0x08 | serial | 0x0000BEEF |
| 0x0C | run_type / 0x0D fpl_platform / 0x0E fpl_lot | 0 / 3 / 0 |
| 0x10 | mode_version / wf_version / wf_subversion / wf_type | 7 / 0 / 0 / 0x4B (AC) |
| 0x14 | panel_size / amepd_part_number / wf_revision / frame_rate | 60 / 1 / 0 / 0 |
| 0x18 | reserved1_0 / vcom_shifted / reserved1_1 | 0 / 0 / 0 |
| 0x1C | xwia = 0x000200, cs1 (sum-8 of bytes 0..30) at 0x1F | |
| 0x20 | wmta=58, fvsn=1, luts=0, mc=6, trc=1, advanced=0, eb=0xFF, sb=0xFC | |
| 0x30 | TRT = `00 28 00` (trc+1 entries + terminator) | |
| 0x200 | XWI: length byte 22 + `V220_EBAL_ED060TC1_3CE` | |

The rest of the WBF is zero: this image is read for its header, checksum and
XWI only; the waveform instructions the EPDC executes come from the WRF.

`cs1`/`cs2` are sum-8 values the stock driver only reads when `file_length == 0`
(not this image); they are written for completeness, computed over the
checksum-zeroed header before the CRC-32 is stored.

### Panel information page at 0x70000 (E Ink "custom alphabet")

`panel_data_translate()` (`mxc_epdc_fb_lab126.c`) maps the raw bytes:
`0x00..0x09 -> '0'..'9'`, `0x0A -> '_'`, `0x0B -> '.'`, `0x0C -> '-'`,
`0xCB..0xE4 -> 'a'..'z'`, `0xE5..0xFE -> 'A'..'Z'`; every other byte becomes
`'!'` and terminates the string when the caller asks for stripping. The
generator encodes with the inverse and terminates strings with a **0x20** byte
(0x00 would decode to `'0'`, and 0xFF would decode to `'!'`).

| offset | encoded text | read by |
|---|---|---|
| 0x00 | `ED060TC1` | part number (`panel_get_id`) |
| 0x10 | `-2.05` (exactly 5 bytes) | VCOM (`panel_get_vcom`) |
| 0x20 | `V220E_EBAL_ED060TC1` | waveform name / panel id |
| 0x40 | `A123` | FPL lot, `panel_get_id` uses bytes +1..+3 |
| 0x50 | `EBA` + 0x20 | barcode (`panel_get_bcd` → `EBA`) |
| 0x80 | `1072x1448` + 0x20 | resolution |
| 0x90 | `107.2x144.8` + 0x20 | dimensions |
| 0xB0 | `3.3` + 0x20 | VDD |
| 0x300 | `1.0` + 0x20 | panel info version |

### `synth.wrf.gz` (2048 bytes uncompressed)

* bytes 0..50: identical to the WBF header + TRT (the `dd_envelope()` contract)
* payload at 51: eight little-endian u64 mode pointers
  `0x40 0x80 0xC0 0x100 0x140 0x180 0x1C0 0x200` (`mc + advanced + 1 = 8`)
* payload + 0x200 (WRF 0x23B): length byte 13 + `V220_EBAL_3CE`
* everything else zero
* compressed with a **stored-block gzip** identical to `dd_gzip_stored()`:
  fixed 10-byte header `1f 8b 08 00 00 00 00 00 00 ff`, stored deflate blocks of
  at most 65535 bytes, CRC-32 + ISIZE trailer (10 + 5 + 2048 + 8 = 2071 bytes)

### `synth.pw3.gz` (67676 bytes)

The 64-byte `PW3WFP1` envelope of readom `patches/android-core/0700`:
magic `PW3WFP1\0`, version 1, header size 64, `raw_length = 65536`,
`wrf_length = 2048`, `wrf_offset = 64 + align8(65536) = 65600`, CRC-32 of WBF,
CRC-32 of WRF, CRC-32 of the header with its own CRC field zeroed, zero
reserved bytes; then the WBF, zero padding to `wrf_offset`, then the WRF.
This is byte-for-byte what `dd_envelope(panel export, wrf)` produces, gzipped
with `dd_gzip_stored()`.

## Why a cold boot ends with `source=stored`

The emulator's initramfs loads `mxc_epdc_fb` at ~2.9 s, before the hidden
waveform FAT can be mounted; that first firmware request falls back to the
kernel's built-in default waveform
(`[ 2.9] imx_epdc_fb: Failed to load fw! (err -2) Trying fallback fw`). This is
harmless and expected. On the real rootfs, `/etc/upstart/display`:

1. mounts the hidden FAT (`/usr/sbin/wfm_mount`, offset `4096*7233` in
   `/dev/mmcblk0`),
2. finds `source = built-in` and clears `waveform_to_use/*.wrf.gz`,
3. follows its stock **override** path: `/opt/eink/images/<PANEL_CODE>/`
   (`PANEL_CODE = EBAL_3CE` for this panel). `prepare-voyage-guest.sh` puts
   `synth.wrf.gz` there, so the script copies it into
   `waveform_to_use/synth.wrf.gz`, unloads and reloads `mxc_epdc_fb`,
4. the reload's firmware request is served by `/lib/udev/bin/waveform.sh`
   (udev rule `20-waveform.rules`) from the now-mounted FAT → `source=stored`,
   and `/proc/eink/waveform/data` is the synthetic 2048-byte WRF.

The serial log of the verified boot
(`readom-port/emu-voyage/boot-evidence/epdc-display-lines.txt`) shows the
override being taken:

```
info display:update:Checking '/opt/eink/images/EBAL_3CE' for overrides
info display:update:Overriding  with synth.wrf.gz
info display:update:Copied waveform from /opt/eink/images/EBAL_3CE/synth.wrf.gz into /mnt/wfm/waveform_to_use
```

If the guest overlay is reset, or a fresh instance is used, re-run step 4
(prepare) and reboot. The `/opt/eink` override and the hidden-FAT copy both
survive reboots; the reprepare is only needed after `eink reset`/new instance.

## Validation

`make_voyage_panel.py` asserts, on the generated bytes:

* WBF header fields, CRC-32 (with the embedded checksum zeroed) and the
  `android_wf_header_valid()` type/version/size rules of patch 0400;
* `dd_envelope()` (replica of `boot/devicedata/devicedata.c`) on the driver's
  real 456570-byte panel export + the 2048-byte WRF, including its CRC-32 and
  header/TRT comparison, and that its gzip is exactly `synth.pw3.gz`;
* `dd_gzip_stored()` (stored-block gzip, no mtime/name);
* the 0700 envelope: `android_wf_container()` + `android_wrf_layout()`
  (magic/version/header size/wrf offset/zero padding/CRCs, mode pointer bounds,
  XWI bounds) and that `advanced_wfm_flags = 0` means no VC section;
* the stock `process_fw` rules (gzip header, deflate, TRT/offset, bit depth,
  pointer switch) and `eink_get_wfm_human_version()` returning
  `V220_EBAL_3CE`;
* the panel driver decode: `panel_get_format` (no BUG), `panel_get_bcd` = EBA,
  `panel_get_vcom` = -2.05 V, `panel_get_id` =
  `V220_123__E_ED060TC1`, resolution/dimensions/VDD/version strings, and the
  XWI at `waveform_addr + xwia`.

Regenerating twice is byte-identical: `--out /tmp/gen-repeat` produced the same
three sha256 values.

`verify-voyage-guest.py` repeats the critical checks on the bytes the guest
actually returned (`/proc/eink/panel/waveform/data` and
`/proc/eink/waveform/data`): byte equality with `panel.img[0x886:0x70000]` and
`gunzip(synth.wrf.gz)`, the `dd_envelope` equality above, the 0700 container
checks, and the decoded panel strings.

## Real suspend on the emulator (required for suspend-based entries)

A real `pm_suspend(PM_SUSPEND_MEM)` (`pm_test=core`, the production path: the SoC
never actually sleeps, the kernel just runs its suspend/syscore path) deadlocks on
this QEMU machine in `fsr_keypad`'s suspend callback. The ring-buffer evidence is in
`readom-port/voyage-e2e-report.md` §5.1: the `helper` task blocks >120 s inside
`dpm_suspend`, backtrace `fsr_i2c_suspend -> fsr_i2c_poweroff` waiting for a mutex
whose holder is stuck on the unimplemented kl05/fsr I2C — the same emulator
limitation that makes `prepare-voyage-guest.sh` disable `deviced`.

`prepare-voyage-suspend.sh` makes the real path work on the emulator: it stops
`lab126_gui x lab126` (Xorg holds `/dev/keypad` + `event2`, keeping the module
refcount non-zero) and unloads `fsr_keypad` (which also drops `drv26xx_haptics`).
After that, from the serial console:

```
echo core > /sys/power/pm_test; echo mem > /sys/power/state; echo rc=$?
```

returns 0 (~4-5 s round trip in TCG) with, in the ring/kernel log:

```
PM: suspend of devices complete after 110.149 msecs
PM: late suspend of devices complete after 0.765 msecs
suspend debug: Waiting for 5 seconds.
PM: resume of devices complete after 714.058 msecs
PM: Finishing wakeup.
Restarting tasks ... done.
```

Run order for a suspend-based entry (e.g. the Android test entry):

```
# 1. clean boot first (a reboot reloads fsr_keypad)
bash $V/tmp/stop-qemu.sh && bash $REPO/tools/emulator/start-voyage.sh
# 2. wait for SSH, then prepare both the waveform environment (if needed) and
#    the suspend environment:
ssh ... bash -s < $REPO/tools/emulator/prepare-voyage-guest.sh
ssh ... bash -s < $REPO/tools/emulator/prepare-voyage-suspend.sh
# 3. run the entry FROM THE SERIAL CONSOLE (see tmp/serial-console.py):
#    sh /mnt/us/documents/Readom-Voyage-Test.sh
```

The script is emulator-only and does not persist anything: the modules are reloaded
by the next boot, and the UI jobs come back with it.

## Known limits / risks

* **The synthetic waveform is structural, not optical.** The WRF payload is
  zeroed mode data; the QEMU EPDC model does not execute waveform instructions,
  so Home renders correctly in the emulator (`shots/home-synthetic-panel.png`),
  but this says nothing about real panel grey levels or timing.
* **One extra boot is part of the recipe.** On first boot after prepare the
  guest may already report `stored` (if the FAT copy is visible before
  `mxc_epdc_fb` is first loaded); after a FAT clean it takes the override path.
  Both end in the same verified state. Do not expect `source=stored` on a bare
  instance with no prepared override/FAT file — the initramfs fallback is
  unavoidable there.
* **The QEMU fork's initramfs always tries the per-panel firmware before the
  rootfs exists** and logs `Failed to load fw!`; that line is not an error in
  this setup.
* **Do not use `eink run`**: it adds `-no-reboot`, and this guest can hard-reset
  itself when the unimplemented `fsr_keypad` I2C device stalls it. The launcher
  here deliberately omits `-no-reboot`.
* The panel drive is `readonly=on`; the shipped 5.13.6 `mxc_epdc_fb.ko` is not
  built with `DEVELOPMENT_MODE`, so the guest cannot write panel flash anyway.
* `prepare-voyage-guest.sh` remounts `/` read-write briefly (qcow2 overlay
  only) to disable `deviced` and install `/opt/eink`; it does not touch the
  shared base image.
* Headless mode (`-display none`) still gives the panel through QMP:
  `python3 /root/kvemu/eink-emulator/scripts/eink-qmp.py --machine kv screendump out.png`.
* `start-voyage.sh` refuses to start a second instance; use `--force` to replace
  the recorded pid. Tracked by `machines/kv/kv.pid`.
