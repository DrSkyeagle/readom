#!/bin/bash
# Same patch series, a VExpress board config, and no physical device access.
set -euo pipefail
[[ "$(uname -s)" == Linux && "$(hostname)" == ${READOM_BUILD_HOST:-lima-pw3} ]] || exit 1
ROOT=/home/builder/pw3-build
SRC="$ROOT/linux-smoke"
OUT="$ROOT/out-smoke"
ART=/mnt/build-share/kernel-smoke
ARCHIVE=/mnt/kernel-inputs/amazon-5.16.2.1.1/linux-3.0.35.tar.gz
mkdir -p "$OUT" "$ART"
rm -f "$ART/result.json" "$ART/test-build.json"
echo "fa89453deb9947700cbebde2e840230e69969549be2116a0a72331ac5d2fe6c9  $ARCHIVE" | sha256sum -c -
if [[ ! -d "$SRC" ]]; then mkdir "$SRC"; tar -xzf "$ARCHIVE" -C "$SRC"; fi
python3 /mnt/port/tools/apply-patch-series.py "$SRC" /mnt/port/patches/*.patch /mnt/port/patches/android-core/*.patch
export ARCH=arm CROSS_COMPILE=/home/builder/toolchains/aosp-arm-gcc49/bin/arm-linux-androidkernel-
export KCFLAGS='-fno-pic -fno-pie'
if [[ ! -f "$OUT/.config" ]]; then make -C "$SRC" O="$OUT" vexpress_defconfig; fi
# config/android-core.config without the device's drivers, plus the VExpress
# differences and this kernel's own two choices. The same generator makes the
# loader-stage kernel's configuration, so the two test kernels cannot drift
# apart in anything but what each one declares.
python3 /mnt/port/tools/make-test-kernel-config.py --profile smoke --output "$ROOT/smoke.config"
signature_files=("$ROOT/smoke.config" "$OUT/.config" /mnt/port/tools/merge-kconfig.py)
signature=$(python3 /mnt/port/tools/config-signature.py "$SRC" "${signature_files[@]}")
if [[ ! -f "$OUT/.pw3-config-signature" ]] || [[ "$signature" != "$(cat "$OUT/.pw3-config-signature")" ]]; then
  python3 /mnt/port/tools/merge-kconfig.py "$OUT/.config" "$ROOT/smoke.config"
  make -C "$SRC" O="$OUT" oldnoconfig
  python3 /mnt/port/tools/merge-kconfig.py --check "$OUT/.config" "$ROOT/smoke.config"
  python3 /mnt/port/tools/config-signature.py "$SRC" "${signature_files[@]}" > "$OUT/.pw3-config-signature"
fi
python3 /mnt/port/tools/merge-kconfig.py --check "$OUT/.config" "$ROOT/smoke.config"
make -C "$SRC" O="$OUT" -j3 zImage
cp "$OUT/arch/arm/boot/zImage" "$ART/zImage"
cp "$OUT/.config" "$ART/kernel.config"
cp "$SRC/.pw3-applied-patches.json" "$ART/patches.json"
bash /mnt/port/tools/build-smoke-initramfs.sh
echo 'Built VExpress test kernel and diskless initramfs; not a Kindle boot image.'
