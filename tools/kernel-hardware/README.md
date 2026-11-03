# PW3 Android 8.1 public hardware component

This component carries the rebuilt kernel modules of the Android image (touch 3, Wi-Fi 2, EPDC 1, USB 3), their same-build evidence, the native USB gate, init integration and the policy for them. It contains no private firmware and no ADB key. USB is part of every boot: MTP, and adb when Android turns it on (docs/design/usb-mtp.md).

## Verified build and export

Run with Python 3.9 or newer, after the kernel build completes:

```sh
python3 export_component.py --build-dir /path/to/android-core-kernel --output /new/public-usb-component
```

The exporter verifies the immutable receipt and actual bytes of six build artifacts; the pinned Amazon source archive; USB Kconfig composition; actual ELF32 ARM module headers, vermagic, GPL declarations, every non-weak undefined symbol against that build's Module.symvers, exact modules.dep closure, actual relocation types against the build audit, and that the Android gadget's only module parameters are composite.c's generic ones, which init never sets (its strings, IDs and functions come from `/sys/class/android_usb`). It reads the tar in memory and never extracts arbitrary entries. A changed receipt or an existing output is refused. The default expected receipt is the final 44-patch build below; a future build requires explicitly passing its receipt SHA256 via `--expected-receipt-sha256`.

`tools/prepare-kernel-hardware.sh` passes the receipt of the current kernel build, after checking that its patch set is the current one. `build-verification.json` preserves all patch hashes and the matching kernel hashes. A module build is not hardware validation.

The order is `fsl_otg_arc → arcotg_udc → g_android`. Only g_android → arcotg_udc is an ELF dependency. The first edge is an additional runtime dependency: with USB_OTG=y the UDC calls otg_get_transceiver; the OTG module must have successfully bound its platform device and registered the transceiver first. Module init success alone cannot establish this.

## Integration

1. Put the exported component at a stable path in the AOSP tree. In PW3 BoardConfig, set `PW3_USB_COMPONENT_ROOT` to that path and include `$(PW3_USB_COMPONENT_ROOT)/userspace-patch/BoardConfig-usb.mk`. This enables the PW3-only adbd macro **and includes its policy directory**. The file_contexts rule labels `/system/bin/pw3-usb-check` as the existing `pw3_init_check_exec`; the helper reuses that read-only preflight domain. Do not omit this policy when copying files manually.
2. Include `$(PW3_USB_COMPONENT_ROOT)/pw3-usb-component.mk` in the product. Despite the retained USB filename, this is the complete nine-module public component and installs all three init fragments plus the ueventd fragment; do not also include the old 36-patch private module bundles. Make the exported `native/Android.mk` visible to Android's make discovery, or include it explicitly from the PW3 device Android.mk. It builds a platform `/system/bin/pw3-usb-check`, using libbase and liblog.
3. Apply `userspace-patch/0002-pw3-adbd-usb-only.patch`, then `0005-pw3-adbd-without-boot-opt-in.patch`, to `platform/system/core` at `android-8.1.0_r81` (the same as `patches/aosp-system-core`). The BoardConfig flag affects adbd only. Other products have byte-equivalent original main.cpp after preprocessing the new flag off.
4. The device's `init.pw3.usb.rc` sets the gadget up at boot once the gate reports `gadget-bound`: identity strings, the per-device serial, the FunctionFS mount for adb (verified by `pw3-init-check usb`, which sets `sys.pw3.ffs_ready`), and the `sys.usb.config` handlers. This fragment never asserts `sys.pw3.ffs_ready` or `sys.usb.ffs.ready` itself.
5. Import `/vendor/etc/ueventd.pw3.fragment.rc` from the active device ueventd configuration, or merge its single framebuffer permission rule. The public hardware/EPDC init fragments retain existing touch order, deferred Wi-Fi load/unload and the EPDC startup gate contract. Device-private board/firmware/waveform bytes must be separately injected under `/firmware/image` and relabeled before ueventd or module requests can consume them. Their request names remain unchanged; no firmware is copied by this product make fragment.
6. Build and validate the combined platform SELinux policy. Existing pw3_init_check policy supplies sysfs/proc read and sys.pw3 property rights; AOSP 8.1 public/domain.te already grants random_device access, so the component adds no duplicate entropy-device grant. No new module, mount, key, firmware, block or network access is granted to the helper. Init remains the module loader.

## Runtime behavior

The three modules are loaded in one init action at post-fs, so that `/sys/class/android_usb` exists before the framework starts: UsbService creates its device manager only if it does then. The native helper requires `ro.secure=1` and `ro.adb.secure=1`, and refuses known competing gadget modules (g_ffs among them: the Android gadget registers functionfs itself), preexisting target modules at prepare, absent/error module metadata, incorrect platform binding, incorrect stage transitions and unsuccessful property writes. A failed stage sets `sys.pw3.usb.phase=error`; init still runs the remaining insertions (it has no conditional), but every later stage fails and the gadget is not set up.

The helper checks `/sys/module/<name>/initstate == live`, and canonical driver links for `fsl-usb2-otg` and `fsl-usb2-udc` (both platform IDs are -1). After g_android it also checks `/sys/class/android_usb/android0/enable` and the registered `functionfs` filesystem. Each stage makes 100 retries at 20 ms intervals plus syscall and scheduling time. Failure stops progression; it does not unload drivers, reset hardware or toggle power. These snapshots do not prevent a separate privileged process from changing hardware afterward.

At boot, once persistent properties are loaded, the `serial` stage keeps a 32-character lowercase hexadecimal USB serial in `persist.pw3.usb.serial`, made once from 16 bytes of `/dev/urandom`: a computer knows the device again instead of seeing a new one at every boot. It never reads or logs the factory serial. It is not an authentication secret. The identity strings are `ro.product.manufacturer` and `ro.product.model`, so that USB, MTP and `adb devices -l` show one name.

The adbd patch (system/core 0002, 0005) excludes three inbound TCP startup paths: the root-mode smartsocket listener, positive service/persist tcp.port properties, and default port 5555 when USB is missing. adbd requires effective authentication, actually opens ep0 O_RDWR under its final identity, and exits with a clear error if the open fails. Whether it runs is Android's own switch (developer options, `sys.usb.config`); UsbDebuggingManager asks before a computer may use it. No authentication algorithm or key handling is changed.

The probe closes ep0 without writing any descriptor. In this exact Linux 3.0 f_fs, last close synchronously calls ffs_data_reset and returns to FFS_READ_DESCRIPTORS. FFS_FL_CALL_CLOSED_CALLBACK is zero until successful descriptor/string activation, so this empty probe does not reach the gadget's closed callback.

AOSP adbd first writes FunctionFS v2 descriptors, then falls back to v1 on a failed v2 write. The old kernel rejects the v2 magic without advancing state, so that fallback is source-compatible. MTP and adb through the Android gadget, the authorisation dialog, cable/charger behaviour and suspend are to be verified on the device. No mass storage, RNDIS or TCP ADB is included.

## Validation and source disclosure

`python3 tests/run_tests.py --build-dir /path/to/android-core-kernel --kernel-source /path/to/linux-3.0.35` runs ASan/UBSan native gate/POSIX cases, actual AOSP adbd functions with the board flag on/off, full source flag-off equivalence, patch apply/reverse SHA checks, FunctionFS lifecycle cases, and actual-build artifact/closure checks plus corruption and wrong-composition refusals. Test fixture files are temporary; no Kindle is accessed. Android cross compilation and a combined policy build are performed by the main integration workflow, not claimed by these host tests.

`adb-contract/REPORT.md` and its source hashes retain primary-source references. `COPYING.kernel` is the original kernel GPL text; the USB, touch, cfg80211 and EPDC modules declare GPL, and ath6kl_sdio declares Dual BSD/GPL. The export includes source URL/member hash, patch hashes and compiler provenance. A public release must also provide the complete corresponding kernel source and all applied patches/build configuration; this small component alone is not a complete GPL source distribution. Private firmware is deliberately outside this package.

The integrated exporter includes only `native/Android.mk` from its generated root makefile. AOSP patch fixtures are omitted from that component to prevent accidental duplicate adb/adbd module discovery.
