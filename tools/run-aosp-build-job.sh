#!/bin/bash
# Linux VM systemd job entry: preserve a terminal-independent exit receipt.
set -euo pipefail
[[ $(uname -s) == Linux && $(hostname) == lima-pw3 && $(id -un) == builder ]] || exit 77
ulimit -c 0
LOG=/mnt/build-share/logs/aosp-system-build.log
RECEIPT=/mnt/build-share/logs/aosp-system-job.json
started=$(date -u +%Y-%m-%dT%H:%M:%SZ)
# What build-aosp-system.sh will make of the same environment. The receipt says
# it because the export copies it into build-result.json, and an image's own
# files do not tell you whether the staged tree was cleared first: a module that
# stops being installed leaves its file behind, and the image carries it.
variant=${PW3_BUILD_VARIANT:-userdebug}
installclean=${PW3_AOSP_INSTALLCLEAN:-0}
# Fixed here rather than left to the build, so that the receipt can say which
# value the image was stamped with and a rebuild can be given the same one.
# What it settles is ro.build.date.utc and the fingerprint, not the container:
# the same sources with the same value do not produce a byte-identical
# system.img: the plain systemimage path passes no timestamp to make_ext4fs, so
# the inodes carry the time of the run.
build_datetime=${PW3_BUILD_DATETIME:-$(date +%s)}
export PW3_BUILD_DATETIME="$build_datetime"
# What the image will call itself in Settings > About tablet > Build number. The
# receipt says it because nothing in the images names the release: build.prop
# carries the string but an export is checked against hashes, and a reader
# reporting a problem reads out this one line.
build_version=${PW3_BUILD_VERSION:-}
export PW3_BUILD_VERSION="$build_version"
PRODUCT=/home/builder/android-src/out/target/product/pw3
# The images this build made, named by their contents. A time cannot say which
# build an image came from: output trees are swapped by renaming and restored
# from copies, both of which keep an image's timestamp, so a receipt newer than
# the files it finds is no evidence that it is their receipt. These hashes are,
# and the export refuses anything else.
record() {
  python3 - "$RECEIPT" "$started" "$1" "$2" "$variant" "$installclean" "${3:-}" "${4:-}" "$build_datetime" "$build_version" <<'PY'
import json,os,sys,tempfile,time
from pathlib import Path
p=Path(sys.argv[1]);fd,tmp=tempfile.mkstemp(prefix='.build-job-',dir=p.parent)
images={n:h for n,h in (('system.img',sys.argv[7]),('ramdisk.img',sys.argv[8])) if h}
with os.fdopen(fd,'w') as f:
 json.dump({'schema':5,'started_utc':sys.argv[2],'recorded_utc':time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),'state':sys.argv[3],'exit_code':None if sys.argv[4]=='none' else int(sys.argv[4]),'variant':sys.argv[5],'installclean':sys.argv[6]=='1','build_datetime':int(sys.argv[9]),'build_version':sys.argv[10] or None,'images':images,'systemd_unit':'pw3-aosp-system.service','device_access':False},f,indent=2);f.write('\n');f.flush();os.fsync(f.fileno())
os.replace(tmp,p)
PY
}
mkdir -p /mnt/build-share/logs
record running none
set +e
bash /mnt/port/tools/build-aosp-system.sh > "$LOG" 2>&1
result=$?
set -e
if [[ $result == 0 ]]; then
  record completed 0 "$(sha256sum "$PRODUCT/system.img" | cut -d' ' -f1)" \
                     "$(sha256sum "$PRODUCT/ramdisk.img" | cut -d' ' -f1)"
else
  record failed "$result"
fi
exit "$result"
