#!/bin/bash
# Copy only this project's owned product, then apply its pinned AOSP changes.
set -euo pipefail
[[ "$(uname -s)" == Linux && "$(hostname)" == lima-pw3 ]] || exit 1
SRC=/home/builder/android-src
# Which product the build is for. The name is the device directory's under
# device/amazon/ and the product's in the AOSP tree (tools/build-aosp-system.sh
# lunches "$PW3_AOSP_PRODUCT-<variant>"), so one variable names all of it.
# pw3 is always copied, whatever the model: a second model inherits it
# (device/amazon/voyage/voyage.mk) and the build reads both directories.
MODEL="${PW3_AOSP_PRODUCT:-pw3}"
[[ "$MODEL" =~ ^[a-z0-9][a-z0-9-]*$ ]] || { echo 'PW3_AOSP_PRODUCT: lower-case letters, digits, "-"' >&2; exit 2; }
bash /mnt/port/tools/prepare-kernel-hardware.sh
python3 /mnt/port/tools/fetch-prebuilts.py --check   # the fetched APKs, before anything is copied
models=(pw3)
[[ "$MODEL" == pw3 ]] || models+=(pw3 "$MODEL")
for model in "${models[@]}"; do
  DEST="$SRC/device/amazon/$model"
  if [[ -e "$DEST" && ! -f "$DEST/.pw3-owned-copy" ]]; then
    echo 'Refusing to overwrite a device tree not owned by this port.' >&2
    exit 1
  fi
  mkdir -p "$DEST"
  # Keep unchanged inputs stable; timestamp changed copies at synchronization so
  # make never mistakes an older host mtime for source already compiled in VM.
  rsync -ac --no-times --delete "/mnt/port/device/amazon/$model/" "$DEST/"
  touch "$DEST/.pw3-owned-copy"
done
grep -v '^#' /mnt/port/patches/projects.txt | while read -r project series; do
  [[ -n "$project" ]] || continue
  python3 /mnt/port/tools/apply-patch-series.py "$SRC/$project" /mnt/port/patches/"$series"/*.patch
done
