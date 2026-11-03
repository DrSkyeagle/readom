#!/bin/bash
# The Java crash of a device that cannot be reached over adb.
#
# When the launcher crashes at every start, nobody can walk into Settings and
# turn USB debugging on, so adb is not available and logcat is out of reach.
# The crash is not lost, though: ActivityManager writes every uncaught
# exception into DropBox under /data/system/dropbox, and /data is a file on the
# Kindle's own user store. The stock kernel has no ext4 (docs/design/
# diagnostics.md), so the image cannot be mounted on the device -- but it can
# be mounted in the build VM, which is Linux.
#
# So: stock system, cable in, run this. It copies the image out, mounts it read
# only in the VM, and prints what is in DropBox, the tombstones and the ANR
# traces.
#
# Usage: tools/read-crash-from-data-image.sh [KINDLE_VOLUME]
#   KINDLE_VOLUME defaults to /Volumes/Kindle
set -u
VOLUME=${1:-/Volumes/Kindle}
IMAGE="$VOLUME/android/data.img"
SHARE=${PW3_BUILD_SHARE:-"$HOME/Downloads/Kindle-微信读书/work/android-port/build-share"}
COPY="$SHARE/data-image-under-inspection.img"
export LIMA_HOME=${LIMA_HOME:-"$HOME/Downloads/Kindle-微信读书/work/vm"}

[ -f "$IMAGE" ] || { echo "没有找到 $IMAGE。设备在原厂系统里、线插着吗？" >&2; exit 1; }
echo "把数据镜像拷出来（$(du -h "$IMAGE" | cut -f1)，几分钟）……"
cp "$IMAGE" "$COPY"
echo "拷完，校验："
shasum -a 256 "$COPY" | cut -c1-32

limactl shell pw3 bash -lc '
set -u
IMG=/mnt/build-share/data-image-under-inspection.img
M=/tmp/pw3-data
sudo mkdir -p $M
sudo umount $M 2>/dev/null
sudo mount -o loop,ro "$IMG" $M || { echo "挂不上：这不是一个 ext4 镜像，或者它坏了" >&2; exit 1; }
echo "=== DropBox 里的崩溃条目 ==="
sudo ls -la $M/system/dropbox 2>/dev/null | tail -20 || echo "（没有 dropbox 目录）"
echo
echo "=== 最近的崩溃全文 ==="
latest=$(sudo ls -t $M/system/dropbox/*crash* 2>/dev/null | head -1)
if [ -n "${latest:-}" ]; then
  echo "--- $latest ---"
  case "$latest" in
    *.gz) sudo zcat "$latest" | head -120 ;;
    *)    sudo head -120 "$latest" ;;
  esac
else
  echo "（dropbox 里没有 crash 条目）"
fi
echo
echo "=== tombstones ==="
sudo ls -la $M/tombstones 2>/dev/null | tail -8 || echo "（没有）"
echo "=== ANR ==="
sudo ls -la $M/anr 2>/dev/null | tail -8 || echo "（没有）"
echo "=== 桌面数据库在不在 ==="
sudo ls -la $M/data/com.android.launcher3/databases 2>/dev/null | tail -5 || echo "（没有，说明桌面一次都没写成）"
sudo umount $M
'
echo
echo "看完之后删掉拷贝：rm \"$COPY\""
