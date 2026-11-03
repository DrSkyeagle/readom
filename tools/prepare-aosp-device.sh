#!/bin/bash
# Copy only this project's owned product, then apply its pinned AOSP changes.
set -euo pipefail
[[ "$(uname -s)" == Linux && "$(hostname)" == lima-pw3 ]] || exit 1
SRC=/home/builder/android-src
bash /mnt/port/tools/prepare-kernel-hardware.sh
DEST="$SRC/device/amazon/pw3"
if [[ -e "$DEST" && ! -f "$DEST/.pw3-owned-copy" ]]; then
  echo 'Refusing to overwrite a device tree not owned by this port.' >&2
  exit 1
fi
python3 /mnt/port/tools/fetch-prebuilts.py --check   # the fetched APKs, before anything is copied
mkdir -p "$DEST"
# Keep unchanged inputs stable; timestamp changed copies at synchronization so
# make never mistakes an older host mtime for source already compiled in VM.
rsync -ac --no-times --delete /mnt/port/device/amazon/pw3/ "$DEST/"
touch "$DEST/.pw3-owned-copy"
grep -v '^#' /mnt/port/patches/projects.txt | while read -r project series; do
  [[ -n "$project" ]] || continue
  python3 /mnt/port/tools/apply-patch-series.py "$SRC/$project" /mnt/port/patches/"$series"/*.patch
done
