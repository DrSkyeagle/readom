#!/bin/bash
# VExpress test kernel with an emulated SD reader for a read-only system image.
# Does not replace the diskless interface-test kernel or the PW3 device kernel.
set -euo pipefail
[[ $(uname -s) == Linux && $(hostname) == ${READOM_BUILD_HOST:-lima-pw3} ]] || exit 77
ROOT=/home/builder/pw3-build
SRC="$ROOT/linux-aosp-runtime"
OUT="$ROOT/out-aosp-runtime"
ART=/mnt/build-share/aosp-runtime-kernel
mkdir -p "$OUT" "$ART"
rm -f "$ART/build-result.json"
if [[ ! -d "$SRC" ]]; then cp -a "$ROOT/linux-smoke" "$SRC"; fi
python3 - <<'PY'
import hashlib,json
from pathlib import Path
port=Path('/mnt/port');src=Path('/home/builder/pw3-build/linux-smoke')
base=Path('/mnt/build-share/kernel-smoke')
record=json.loads((base/'result.json').read_text())
assert record['kernel_interface_suite_passed']
for name,digest in record['sha256'].items():
    assert hashlib.sha256((base/name).read_bytes()).hexdigest()==digest,name
assert (base/'patches.json').read_bytes()==(src/'.pw3-applied-patches.json').read_bytes()
config=(base/'kernel.config').read_text()
assert '# CONFIG_MMC is not set' in config
config=config.replace('# CONFIG_MMC is not set','CONFIG_MMC=y\nCONFIG_MMC_BLOCK=y\nCONFIG_MMC_ARMMMCI=y')
Path('/home/builder/pw3-build/out-aosp-runtime/.config').write_text(config)
Path('/mnt/build-share/aosp-runtime-kernel/base-proof.json').write_text(json.dumps(record,indent=2)+'\n')
PY
python3 /mnt/port/tools/apply-patch-series.py "$SRC" /mnt/port/patches/*.patch /mnt/port/patches/android-core/*.patch /mnt/port/patches/test-kernel/*.patch
export ARCH=arm CROSS_COMPILE=/home/builder/toolchains/aosp-arm-gcc49/bin/arm-linux-androidkernel-
export KCFLAGS='-fno-pic -fno-pie'
make -C "$SRC" O="$OUT" oldnoconfig
python3 - <<'PY'
from pathlib import Path
config=Path('/home/builder/pw3-build/out-aosp-runtime/.config').read_text().splitlines()
assert all('CONFIG_'+name+'=y' in config for name in ('MMC','MMC_BLOCK','MMC_ARMMMCI','EXT4_FS','SECURITY_SELINUX'))
assert 'CONFIG_ARCH_MMAP_RND_BITS=16' in config
PY
make -C "$SRC" O="$OUT" -j2 zImage
cp "$OUT/arch/arm/boot/zImage" "$ART/zImage"
cp "$OUT/.config" "$ART/kernel.config"
cp "$SRC/.pw3-applied-patches.json" "$ART/patches.json"
python3 - <<'PY'
import hashlib,json
from pathlib import Path
root=Path('/mnt/build-share/aosp-runtime-kernel')
base=json.loads((root/'base-proof.json').read_text())
assert hashlib.sha256(Path('/mnt/build-share/kernel-smoke/patches.json').read_bytes()).hexdigest()==base['sha256']['patches.json']
result={'schema':1,'scope':'VExpress only; adds emulated SD reader to the passing interface-test configuration',
        'test_only_patch_sha256':{p.name:hashlib.sha256(p.read_bytes()).hexdigest()
                                 for p in sorted(Path('/mnt/port/patches/test-kernel').glob('*.patch'))},
        'physical_device_access':False,'compiled':True,'runtime_tested':False,
        'sha256':{name:hashlib.sha256((root/name).read_bytes()).hexdigest() for name in
                  ('zImage','kernel.config','patches.json','base-proof.json')}}
(root/'build-result.json').write_text(json.dumps(result,indent=2)+'\n')
PY
