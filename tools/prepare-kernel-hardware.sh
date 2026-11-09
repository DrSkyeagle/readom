#!/bin/bash
# Verify the current owned kernel patch set before importing public modules.
set -euo pipefail
[[ $(uname -s) == Linux && $(hostname) == ${READOM_BUILD_HOST:-lima-pw3} ]] || exit 77
# The same model rule as tools/build-kernel.sh: the default model keeps the
# directory name it has always had, and both halves of the configuration are
# checked. Passing only the base would verify 144 of the 147 options and say
# nothing about the three that are facts about this machine -- including
# CONFIG_LOCALVERSION, which names the directory these modules are loaded from.
MODEL="${PW3_KERNEL_MODEL:-pw3}"
suffix=''
[[ "$MODEL" == pw3 ]] || suffix="-$MODEL"
BUILD=/mnt/build-share/android-core-kernel$suffix
python3 /mnt/port/tools/merge-kconfig.py --check "$BUILD/kernel.config" \
  /mnt/port/config/android-core.config "/mnt/port/config/models/$MODEL.config"
DEST=/home/builder/android-src/vendor/amazon/pw3-hardware
if [[ -e "$DEST" && ! -f "$DEST/.pw3-owned-copy" ]]; then
  echo 'Refusing an existing hardware component not owned by this port.' >&2; exit 1
fi
receipt=$(python3 - "$BUILD" <<'PY'
import hashlib,json,sys
from pathlib import Path
root=Path(sys.argv[1]);port=Path('/mnt/port')
raw=(root/'build-result.json').read_bytes();record=json.loads(raw)
patches={str(p.relative_to(port)):hashlib.sha256(p.read_bytes()).hexdigest()
         for directory in (port/'patches',port/'patches/android-core') for p in sorted(directory.glob('*.patch'))}
if record.get('patches')!=patches:raise SystemExit('Built kernel patch set differs from the current owned source')
print(hashlib.sha256(raw).hexdigest())
PY
)
stage=$(mktemp -d /home/builder/pw3-build/hardware-component.XXXXXX)
trap 'rm -rf "$stage"' EXIT
python3 /mnt/port/tools/kernel-hardware/export_component.py \
  --build-dir "$BUILD" --expected-receipt-sha256 "$receipt" --output "$stage/component"
mkdir -p "$DEST"
rsync -ac --no-times --delete "$stage/component/" "$DEST/"
touch "$DEST/.pw3-owned-copy"
