#!/bin/bash
# The loader-stage kernel: the first Linux of the handoff, on QEMU vexpress-a9.
# boot/handoff is loaded into it as a module, exactly as it is loaded into the
# stock kernel on the device, and it is the kernel that jumps to the second
# Linux. boot/profiles/qemu-vexpress.json names this tree and this output
# directory; boot/handoff/tests/{run,run_qemu,run_client_test}.py are the three
# tests that need them.
#
# It is the smoke kernel (tools/build-kernel-smoke.sh) with loadable modules and
# the emulated SD card, which is why both configurations come out of the one
# generator. It was built by hand once and the tree was later cleaned, so the
# three tests stopped running for weeks and nothing said so. This is that build,
# written down.
set -euo pipefail
[[ "$(uname -s)" == Linux && "$(hostname)" == ${READOM_BUILD_HOST:-lima-pw3} ]] || exit 1
ROOT=/home/builder/pw3-build
SRC="$ROOT/linux-loader-stage"
OUT="$ROOT/out-loader-stage"
ART=/mnt/build-share/loader-stage-kernel
SMOKE=/mnt/build-share/kernel-smoke
ARCHIVE=/mnt/kernel-inputs/amazon-5.16.2.1.1/linux-3.0.35.tar.gz
TOOLCHAIN=/home/builder/toolchains/aosp-arm-gcc49
mkdir -p "$OUT" "$ART"
echo "fa89453deb9947700cbebde2e840230e69969549be2116a0a72331ac5d2fe6c9  $ARCHIVE" | sha256sum -c -
if [[ ! -d "$SRC" ]]; then mkdir "$SRC"; tar --no-same-owner -xzf "$ARCHIVE" -C "$SRC"; fi
python3 /mnt/port/tools/apply-patch-series.py "$SRC" \
  /mnt/port/patches/*.patch /mnt/port/patches/android-core/*.patch \
  /mnt/port/patches/test-kernel/*.patch

# This kernel carries the port's whole patch series, and the claim that the
# series boots and answers the kernel interface suite is the smoke kernel's to
# make. So the smoke kernel's result has to describe this patch series: when it
# describes an older one, the handoff tests would be running on a base nobody
# checked, and the thing to do is rebuild the smoke kernel and run its suite.
python3 - "$SRC" "$SMOKE" <<'PY'
import hashlib, json, sys
from pathlib import Path
source, smoke = Path(sys.argv[1]), Path(sys.argv[2])
proof = json.loads((smoke / 'result.json').read_text())
for name in ['zImage', 'kernel.config', 'patches.json']:
    got = hashlib.sha256((smoke / name).read_bytes()).hexdigest()
    assert got == proof['sha256'][name], 'The smoke kernel result does not describe ' + name
assert proof['kernel_interface_suite_passed'], 'The smoke kernel did not pass its suite'
here = {e['name']: e['sha256'] for e in
        json.loads((source / '.pw3-applied-patches.json').read_text())}
test_only = {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
             for p in sorted(Path('/mnt/port/patches/test-kernel').glob('*.patch'))}
for name in test_only:
    assert here.pop(name, None) == test_only[name], 'Test-only patch not applied: ' + name
base = {e['name']: e['sha256'] for e in json.loads((smoke / 'patches.json').read_text())}
if base != here:
    missing = sorted(set(here) - set(base))
    stale = sorted(k for k in set(here) & set(base) if here[k] != base[k])
    raise SystemExit('The smoke kernel result is for another patch series: %d patches it has '
                     'not seen (%s), %d changed since (%s). Run tools/build-kernel-smoke.sh '
                     'and tools/run-kernel-smoke.sh first.'
                     % (len(missing), ', '.join(missing[:3]) or '-',
                        len(stale), ', '.join(stale[:3]) or '-'))
print('Base proof: the smoke kernel passed its suite on this patch series')
PY

# The handoff's memory footprint is a formula derived by hand against these two
# files (boot/handoff/derive_kernel_footprint.py). If either changes, the
# formula needs reviewing rather than rerunning, so the build stops here.
python3 - "$SRC" <<'PY'
import hashlib, sys
from pathlib import Path
source = Path(sys.argv[1])
locked = {'arch/arm/boot/compressed/head.S':
          '7cba08a3c830058b537183f9c7dd1def7e6a6c7939e96bb5bff32091188e0a52',
          'arch/arm/boot/compressed/vmlinux.lds.in':
          'ec0fec295f09aa3c12ab52d793344f275469b3d8a725423bf00233a881d82221'}
for name, want in locked.items():
    got = hashlib.sha256((source / name).read_bytes()).hexdigest()
    assert got == want, '%s changed; review derive_kernel_footprint.py' % name
print('Footprint inputs as locked')
PY

export ARCH=arm CROSS_COMPILE="$TOOLCHAIN/bin/arm-linux-androidkernel-"
export KCFLAGS='-fno-pic -fno-pie'
if [[ ! -f "$OUT/.config" ]]; then make -C "$SRC" O="$OUT" vexpress_defconfig; fi
python3 /mnt/port/tools/make-test-kernel-config.py --profile loader-stage \
  --output "$ROOT/loader-stage.config"
signature_files=("$ROOT/loader-stage.config" "$OUT/.config" /mnt/port/tools/merge-kconfig.py)
signature=$(python3 /mnt/port/tools/config-signature.py "$SRC" "${signature_files[@]}")
if [[ ! -f "$OUT/.pw3-config-signature" ]] || [[ "$signature" != "$(cat "$OUT/.pw3-config-signature")" ]]; then
  python3 /mnt/port/tools/merge-kconfig.py "$OUT/.config" "$ROOT/loader-stage.config"
  make -C "$SRC" O="$OUT" oldnoconfig
  python3 /mnt/port/tools/merge-kconfig.py --check "$OUT/.config" "$ROOT/loader-stage.config"
  python3 /mnt/port/tools/config-signature.py "$SRC" "${signature_files[@]}" > "$OUT/.pw3-config-signature"
fi
python3 /mnt/port/tools/merge-kconfig.py --check "$OUT/.config" "$ROOT/loader-stage.config"
# `modules` for Module.symvers: boot/build_module.py builds against this output.
make -C "$SRC" O="$OUT" -j3 zImage modules
cp "$OUT/arch/arm/boot/zImage" "$ART/zImage"
cp "$OUT/.config" "$ART/kernel.config"
cp "$SRC/.pw3-applied-patches.json" "$ART/patches.json"
cp "$SMOKE/result.json" "$ART/base-proof.json"
python3 - "$ART" "$SRC" "$OUT" <<'PY'
import hashlib, json, sys
from pathlib import Path
art, source, output = (Path(p) for p in sys.argv[1:4])
record = {'schema': 2,
          'built_by': 'tools/build-loader-stage-kernel.sh',
          'scope': 'VExpress only; adds emulated SD and loadable modules; not PW3 hardware',
          'physical_device_access': False, 'compiled': True, 'runtime_tested': False,
          'test_only_patch_sha256':
              {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
               for p in sorted(Path('/mnt/port/patches/test-kernel').glob('*.patch'))},
          'base_proof': {'file': 'base-proof.json',
                         'copied_from': '/mnt/build-share/kernel-smoke/result.json',
                         'says': 'the same patch series passed the kernel interface suite'},
          'module_tree': {'source': str(source), 'output': str(output)},
          'sha256': {name: hashlib.sha256((art / name).read_bytes()).hexdigest()
                     for name in ['zImage', 'kernel.config', 'patches.json', 'base-proof.json']}}
(art / 'build-result.json').write_text(json.dumps(record, indent=2) + '\n')
print(json.dumps(record['sha256'], indent=2))
PY
echo 'Built the loader-stage test kernel. Not a Kindle boot image.'
