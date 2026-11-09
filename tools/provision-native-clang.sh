#!/bin/bash
# Optional verified host-native compiler. A missing activation keeps stock tools.
set -euo pipefail
[[ $(uname -s) == Linux && $(hostname) == ${READOM_BUILD_HOST:-lima-pw3} ]] || exit 77
ACTIVATION=/mnt/build-share/native-clang/activation.json
CHROOT=/srv/aosp-x86
if [[ ! -f "$ACTIVATION" ]]; then
  # The build scripts select the compiler from this marker. Removing the
  # activation must not silently retain an earlier host-native selection.
  sudo rm -f "$CHROOT/opt/pw3-native-clang-enabled.json"
  echo 'No native Clang activation; using the original AOSP host compiler.'
  exit 0
fi
SOURCE=$(python3 - "$ACTIVATION" <<'PY'
from pathlib import Path
import hashlib,json,re,sys
activation=json.loads(Path(sys.argv[1]).read_text())
key=activation['prefix_key']
assert re.fullmatch(r'[0-9a-f]{20}',key)
root=Path('/home/builder/toolchains/pw3-native-clang')/key
raw=(root/'manifest.json').read_bytes();manifest=json.loads(raw)
assert hashlib.sha256(raw).hexdigest()==activation['prefix_manifest_sha256']
assert manifest['key']==key
assert manifest['compiler_sha256']==activation['compiler_sha256']
proof_path=Path('/mnt/build-share/native-clang/verified-comparison.json')
proof_raw=proof_path.read_bytes();proof=json.loads(proof_raw)
assert hashlib.sha256(proof_raw).hexdigest()==activation['comparison_sha256']
assert proof['passed'] is True and proof['compiler_stable'] is True
assert proof['compiler_sha256']==manifest['compiler_sha256']
assert proof['harness_sha256']==hashlib.sha256(Path('/mnt/port/tools/native-clang/compare.py').read_bytes()).hexdigest()
assert len(proof['cases'])==4 and all(c['code_equal'] and c['dependencies_equal'] for c in proof['cases'])
assert sorted(c['variants'][0]['machine'] for c in proof['cases'])==[3,40,40,62]
prefix=root/'host/linux-x86/clang-4053586'
assert hashlib.sha256((prefix/'bin/clang.native').read_bytes()).hexdigest()==manifest['compiler_sha256']
for name in ('clang','clang++'):
 assert hashlib.sha256((prefix/'bin'/name).read_bytes()).hexdigest()==manifest['wrapper_sha256'][name]
 assert hashlib.sha256((prefix/'bin'/(name+'.real')).read_bytes()).hexdigest()==manifest['adapter_sha256']
print(root/'host')
PY
)
DEST="$CHROOT/opt/pw3-native-clang-prebuilts"
sudo mkdir -p "$DEST"
if ! mountpoint -q "$DEST"; then
  sudo mount --bind "$SOURCE" "$DEST"
  sudo mount -o remount,bind,ro "$DEST"
fi
python3 - "$SOURCE" "$DEST" <<'PY'
from pathlib import Path
import os,sys
suffix='linux-x86/clang-4053586/bin/clang.native'
assert os.path.samefile(Path(sys.argv[1])/suffix,Path(sys.argv[2])/suffix)
mounts=Path('/proc/self/mountinfo').read_text().splitlines()
assert any(v.split()[4]==sys.argv[2] and 'ro' in v.split()[5].split(',') for v in mounts)
PY
sudo install -m 0644 "$ACTIVATION" "$CHROOT/opt/pw3-native-clang-enabled.json"
echo 'Verified native Clang prefix is read-only; original AOSP compiler is preserved.'
