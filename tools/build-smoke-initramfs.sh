#!/bin/bash
# Rebuild diskless test userspace against the already-built VExpress kernel.
set -euo pipefail
[[ "$(uname -s)" == Linux && "$(hostname)" == lima-pw3 ]] || exit 1
ROOT=/home/builder/pw3-build
SRC="$ROOT/linux-smoke"
OUT="$ROOT/out-smoke"
ART=/mnt/build-share/kernel-smoke
rm -f "$ART/result.json" "$ART/test-build.json"
grep -q 'CONFIG_LOCALVERSION="-pw3-core-smoke"' "$ART/kernel.config"
test -s "$ART/zImage"
mkdir -p "$ROOT/smoke-user/headers" "$ROOT/smoke-user/source"
rsync -ac --no-times --delete /mnt/port/tools/kernel-smoke/ "$ROOT/smoke-user/source/"
cp "$SRC/drivers/staging/android/uapi/binder.h" "$ROOT/smoke-user/headers/binder.h"
cp "$SRC/include/linux/ashmem.h" "$ROOT/smoke-user/headers/ashmem.h"
CC=/home/builder/toolchains/musl-arm/bin/musl-gcc
for program in init platform-smoke seccomp-smoke alarmfd-smoke namespace-hidepid-smoke binder-sg-smoke binder-hardening-smoke filecaps-smoke aslr-smoke siginfo-smoke process-vm-smoke thread-procfd-smoke; do
  "$CC" -std=gnu99 -static -Os -Wall -Wextra -Werror -pthread \
    -include linux/types.h -DBINDER_IPC_32BIT \
    -I"$ROOT/smoke-user/headers" -idirafter /usr/arm-linux-gnueabi/include \
    "$ROOT/smoke-user/source/$program.c" -o "$ROOT/smoke-user/$program"
done
NETWORK_SMOKE_OUT="$ROOT/smoke-user" bash "$ROOT/smoke-user/source/network/build-arm.sh"
"$CC" -std=gnu99 -static -Os -Wall -Wextra -Werror \
  -I"$ROOT/smoke-user/source/network" -idirafter /usr/arm-linux-gnueabi/include \
  "$ROOT/smoke-user/source/network-gid/network-gid-smoke.c" \
  "$ROOT/smoke-user/source/network/netlink-wire.c" -o "$ROOT/smoke-user/network-gid-smoke"
"$CC" -std=gnu99 -static -Os -Wall -Wextra -Werror \
  -I"$ROOT/smoke-user/source/network" -idirafter /usr/arm-linux-gnueabi/include \
  "$ROOT/smoke-user/source/inet-permission/inet-permission-smoke.c" \
  "$ROOT/smoke-user/source/network/netlink-wire.c" -o "$ROOT/smoke-user/inet-permission-smoke"
cat > "$ROOT/smoke-user/cpio.list" <<EOF
dir /dev 0755 0 0
nod /dev/console 0600 0 0 c 5 1
file /init $ROOT/smoke-user/init 0755 0 0
file /platform-smoke $ROOT/smoke-user/platform-smoke 0755 0 0
file /seccomp-smoke $ROOT/smoke-user/seccomp-smoke 0755 0 0
file /alarmfd-smoke $ROOT/smoke-user/alarmfd-smoke 0755 0 0
file /namespace-hidepid-smoke $ROOT/smoke-user/namespace-hidepid-smoke 0755 0 0
file /binder-sg-smoke $ROOT/smoke-user/binder-sg-smoke 0755 0 0
file /binder-hardening-smoke $ROOT/smoke-user/binder-hardening-smoke 0755 0 0
file /filecaps-smoke $ROOT/smoke-user/filecaps-smoke 0755 0 0
file /aslr-smoke $ROOT/smoke-user/aslr-smoke 0755 0 0
file /siginfo-smoke $ROOT/smoke-user/siginfo-smoke 0755 0 0
file /process-vm-smoke $ROOT/smoke-user/process-vm-smoke 0755 0 0
file /thread-procfd-smoke $ROOT/smoke-user/thread-procfd-smoke 0755 0 0
file /network-smoke $ROOT/smoke-user/network-smoke 0755 0 0
file /network-wire-selftest $ROOT/smoke-user/network-wire-selftest 0755 0 0
file /network-gid-smoke $ROOT/smoke-user/network-gid-smoke 0755 0 0
file /inet-permission-smoke $ROOT/smoke-user/inet-permission-smoke 0755 0 0
EOF
"$OUT/usr/gen_init_cpio" "$ROOT/smoke-user/cpio.list" | gzip -n > "$ART/initramfs.cpio.gz"
python3 - <<'PY'
import hashlib,json,struct
from pathlib import Path
root=Path('/home/builder/pw3-build/smoke-user'); art=Path('/mnt/build-share/kernel-smoke')
def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()
binaries={}
for name in ['init','platform-smoke','seccomp-smoke','alarmfd-smoke','namespace-hidepid-smoke','binder-sg-smoke','binder-hardening-smoke','filecaps-smoke','aslr-smoke','siginfo-smoke','process-vm-smoke','network-smoke','network-wire-selftest','network-gid-smoke','inet-permission-smoke']:
    path=root/name; data=path.read_bytes()
    assert data[:6]==b'\x7fELF\x01\x01' and struct.unpack_from('<H',data,18)[0]==40
    binaries[name]={'bytes':len(data),'sha256':sha(path)}
record={'scope':'Static ARM test userspace; no device runtime claim',
    'sources':{str(p.relative_to(root/'source')):sha(p) for p in sorted((root/'source').rglob('*')) if p.is_file() and p.suffix in ('.c','.h','.inc')},
    'kernel_uapi':{p.name:sha(p) for p in sorted((root/'headers').glob('*.h'))},
    'binaries':binaries,'zImage_sha256':sha(art/'zImage'),'initramfs_sha256':sha(art/'initramfs.cpio.gz'),
    'kernel_config_sha256':sha(art/'kernel.config'),'kernel_patches_sha256':sha(art/'patches.json')}
(art/'test-build.json').write_text(json.dumps(record,indent=2)+'\n')
PY
echo 'Built verified ARM test executables and diskless initramfs.'
