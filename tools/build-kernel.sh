#!/bin/bash
# Compile a named PW3 kernel profile on Linux. Never flash.
set -euo pipefail
if [[ "$(uname -s)" != Linux || "$(hostname)" != ${READOM_BUILD_HOST:-lima-pw3} ]]; then
  echo 'Run inside the build VM (READOM_BUILD_HOST, default lima-pw3).' >&2
  exit 1
fi
PROFILE="${1:-stock}"
case "$PROFILE" in stock|android-core) ;; *) echo 'Unknown kernel profile' >&2; exit 2 ;; esac
# Which Kindle this kernel is for. Amazon's own BUILD_HOW-TO says which board
# uses which defconfig, and the half of the configuration that is about the
# board rather than about this port is in config/models/<model>.config
# (docs/design/porting-to-other-models.md).
#
# The default model keeps the directory names it has always had: they are
# written in the release documents and passed to boot/build_entry.py --kernel
# by hand. Any other model gets its own, so that a kernel which has never run
# on a device cannot be picked up as the released one.
MODEL="${PW3_KERNEL_MODEL:-pw3}"
case "$MODEL" in
  pw3)  DEFCONFIG=imx60_wario_defconfig; suffix='' ;;
  voyage) DEFCONFIG=imx60_wario_defconfig; suffix="-voyage" ;;
  duet) DEFCONFIG=imx60_duet_defconfig; suffix="-$MODEL" ;;
  *) echo 'Unknown kernel model' >&2; exit 2 ;;
esac
MODEL_CONFIG="/mnt/port/config/models/$MODEL.config"
# The stock source this model is built from, and the GPL package it comes in.
# pw3 (and duet, which is the same 3.0.35 tree) is the 5.16.2.1.1 release that
# source-lock.json pins; Voyage is its own 5.13.6 package
# (Kindle_src_5.13.6_3731990038.tar.gz, sha256 4a01245d...), whose member
# gplrelease/linux-3.0.35.tar.gz hashes to the number below. The build VM keeps
# the extracted member under /mnt/kernel-inputs/<firmware>/; a machine that
# extracted it elsewhere points PW3_KERNEL_INPUTS_ROOT at that directory.
case "$MODEL" in
  voyage) KERNEL_FIRMWARE=5.13.6
          KERNEL_MEMBER_SHA256=a66a58b80869c65aedd5e1bb5b49a5f064731fab4997c5294c400570245144b0 ;;
  *)      KERNEL_FIRMWARE=5.16.2.1.1
          KERNEL_MEMBER_SHA256='' ;;   # the pinned source-lock.json value
esac
KERNEL_INPUTS_ROOT="${PW3_KERNEL_INPUTS_ROOT:-/mnt/kernel-inputs}"
KERNEL_ARCHIVE="$KERNEL_INPUTS_ROOT/amazon-$KERNEL_FIRMWARE/linux-3.0.35.tar.gz"
BUILD_ROOT=/home/builder/pw3-build
SOURCE_ROOT="$BUILD_ROOT/linux-$PROFILE"
OUTPUT_ROOT="$BUILD_ROOT/out-$PROFILE$suffix"
ARTIFACT_ROOT="/mnt/build-share/$PROFILE-kernel$suffix"
TOOLCHAIN=/home/builder/toolchains/aosp-arm-gcc49
COMPILER_ARCHIVE=/mnt/build-share/toolchains/aosp-arm-gcc49-0390252.tar.gz
mkdir -p "$BUILD_ROOT" /mnt/build-share/logs "$ARTIFACT_ROOT"
python3 - "$KERNEL_ARCHIVE" "$COMPILER_ARCHIVE" "$KERNEL_MEMBER_SHA256" <<'PY'
import hashlib, json, sys
from pathlib import Path
lock = json.loads(Path('/mnt/port/source-lock.json').read_text())
expected_member = sys.argv[3] or lock['kernel']['member_sha256']
for filename, expected in [
    (sys.argv[1], expected_member),
    (sys.argv[2], lock['cross_compiler']['archive_sha256']),
]:
    h = hashlib.sha256()
    with open(filename, 'rb') as stream:
        for data in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(data)
    if h.hexdigest() != expected:
        raise SystemExit('Source checksum mismatch: ' + filename)
PY
if [[ ! -e "$SOURCE_ROOT/.pw3-source-verified" ]]; then
  if [[ -d "$SOURCE_ROOT" ]]; then
    echo 'Refusing to overwrite an unverified existing kernel source tree.' >&2
    exit 1
  fi
  mkdir "$SOURCE_ROOT"
  tar --no-same-owner -xzf "$KERNEL_ARCHIVE" -C "$SOURCE_ROOT"
  touch "$SOURCE_ROOT/.pw3-source-verified"
fi
patch_files=(/mnt/port/patches/*.patch)
if [[ "$PROFILE" == android-core ]]; then
  patch_files+=(/mnt/port/patches/android-core/*.patch)
fi
python3 /mnt/port/tools/apply-patch-series.py "$SOURCE_ROOT" "${patch_files[@]}"
mkdir -p "$TOOLCHAIN" "$OUTPUT_ROOT"
python3 /mnt/port/tools/ensure-toolchain.py "$COMPILER_ARCHIVE" "$TOOLCHAIN"
export ARCH=arm
export CROSS_COMPILE="$TOOLCHAIN/bin/arm-linux-androidkernel-"
export KBUILD_BUILD_USER=builder
export KBUILD_BUILD_HOST=pw3-build
export KBUILD_BUILD_VERSION=1
# Fixed for reproducible builds, in the form of date(1) that uname -v has
# everywhere: Android's Settings reads the kernel's build date from
# /proc/version with a pattern that starts at the weekday ("Unavailable" with
# the ISO form used before, Test V37).
export KBUILD_BUILD_TIMESTAMP='Wed Oct 7 00:00:00 UTC 2026'
# This Android GCC defaults to PIC. Linux 3.0's ARM module loader cannot
# relocate GOT references; the flags must also apply to external modules.
export KCFLAGS='-fno-pic -fno-pie'
"${CROSS_COMPILE}gcc" --version > /mnt/build-share/logs/gcc49-version.txt
if [[ ! -f "$OUTPUT_ROOT/.config" ]]; then
  make -C "$SOURCE_ROOT" O="$OUTPUT_ROOT" "$DEFCONFIG" \
    2>&1 | tee "/mnt/build-share/logs/$PROFILE-configure.log"
fi
if [[ "$PROFILE" == android-core ]]; then
  # oldnoconfig rewrites generated headers even when nothing changed. Retain
  # the cache only while both requested inputs and the actual config match.
  signature_files=(/mnt/port/config/android-core.config "$MODEL_CONFIG" \
                   /mnt/port/tools/merge-kconfig.py "$OUTPUT_ROOT/.config")
  signature=$(python3 /mnt/port/tools/config-signature.py "$SOURCE_ROOT" "${signature_files[@]}")
  if [[ ! -f "$OUTPUT_ROOT/.pw3-config-signature" ]] || \
     [[ "$signature" != "$(cat "$OUTPUT_ROOT/.pw3-config-signature")" ]]; then
    python3 /mnt/port/tools/merge-kconfig.py "$OUTPUT_ROOT/.config" \
        /mnt/port/config/android-core.config "$MODEL_CONFIG"
    make -C "$SOURCE_ROOT" O="$OUTPUT_ROOT" oldnoconfig \
      2>&1 | tee "/mnt/build-share/logs/$PROFILE-oldconfig.log"
    python3 /mnt/port/tools/merge-kconfig.py --check "$OUTPUT_ROOT/.config" \
        /mnt/port/config/android-core.config "$MODEL_CONFIG"
    python3 /mnt/port/tools/config-signature.py "$SOURCE_ROOT" "${signature_files[@]}" > "$OUTPUT_ROOT/.pw3-config-signature"
  fi
  python3 /mnt/port/tools/merge-kconfig.py --check "$OUTPUT_ROOT/.config" \
      /mnt/port/config/android-core.config "$MODEL_CONFIG"
fi
# scripts/mkcompile_h keeps an existing compile.h that differs only in
# UTS_VERSION, i.e. one made with another timestamp would stay.
compile_h=$OUTPUT_ROOT/include/generated/compile.h
if [[ -f $compile_h ]] && ! grep -qF " $KBUILD_BUILD_TIMESTAMP\"" "$compile_h"; then rm "$compile_h"; fi
make -C "$SOURCE_ROOT" O="$OUTPUT_ROOT" -j6 uImage modules \
  2>&1 | tee "/mnt/build-share/logs/$PROFILE-build.log"
python3 /mnt/port/tools/check-module-relocations.py "$OUTPUT_ROOT" \
  arm-linux-gnueabi-readelf > "$ARTIFACT_ROOT/module-relocations.json"
cp "$OUTPUT_ROOT/arch/arm/boot/uImage" "$ARTIFACT_ROOT/uImage"
cp "$OUTPUT_ROOT/arch/arm/boot/zImage" "$ARTIFACT_ROOT/zImage"
cp "$OUTPUT_ROOT/.config" "$ARTIFACT_ROOT/kernel.config"
cp "$OUTPUT_ROOT/Module.symvers" "$ARTIFACT_ROOT/Module.symvers"
cp "$OUTPUT_ROOT/System.map" "$ARTIFACT_ROOT/System.map"
# A fresh staging directory avoids retaining modules disabled by later configs.
module_stage=$(mktemp -d "$BUILD_ROOT/modules-$PROFILE.XXXXXX")
trap 'rm -rf "$module_stage"' EXIT
make -C "$SOURCE_ROOT" O="$OUTPUT_ROOT" INSTALL_MOD_PATH="$module_stage" modules_install \
  > "/mnt/build-share/logs/$PROFILE-modules-install.log" 2>&1
kernel_release=$(cat "$OUTPUT_ROOT/include/config/kernel.release")
cp "$OUTPUT_ROOT/include/config/kernel.release" "$ARTIFACT_ROOT/kernel.release"
rm -f "$module_stage/lib/modules/$kernel_release/build" "$module_stage/lib/modules/$kernel_release/source"
tar -czf "$ARTIFACT_ROOT/modules.tar.gz" -C "$module_stage" lib
python3 - "$PROFILE" "$ARTIFACT_ROOT" "$MODEL" "$KERNEL_FIRMWARE" "${patch_files[@]}" <<'PY'
import hashlib, json, datetime
import sys
from pathlib import Path
root = Path(sys.argv[2])
files = {}
for name in ['uImage', 'zImage', 'kernel.config', 'kernel.release', 'Module.symvers', 'System.map', 'modules.tar.gz', 'module-relocations.json']:
    p = root / name
    files[name] = {'bytes': p.stat().st_size, 'sha256': hashlib.sha256(p.read_bytes()).hexdigest()}
result = {
    'compiled_at_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
    'profile': sys.argv[1],
    # Which model and which stock firmware this kernel is for. The artifact
    # directory and the image's modules are per model; the receipt is what says
    # which Amazon source it came from (the cross-compiler is one for every
    # model, so its lock entry is not duplicated).
    'model': sys.argv[3],
    'firmware': sys.argv[4],
    'kcflags': '-fno-pic -fno-pie',
    'source_lock': json.loads(Path('/mnt/port/source-lock.json').read_text()),
    'artifacts': files,
    'patches': {str(Path(name).relative_to('/mnt/port')): hashlib.sha256(Path(name).read_bytes()).hexdigest() for name in sys.argv[5:]},
    'device_flashing_performed': False,
    'device_boot_verified': False,
    'android_runtime_verified': False,
}
(root / 'build-result.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result, indent=2))
PY
