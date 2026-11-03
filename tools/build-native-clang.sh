#!/bin/bash
# Optional host compiler for the ARM64 build VM; never changes target ABI.
set -euo pipefail
[[ $(uname -s) == Linux && $(hostname) == lima-pw3 && $(uname -m) == aarch64 ]] || exit 77
ROOT=/home/builder/pw3-native-clang
LOG=/mnt/build-share/native-clang
command -v cmake >/dev/null && command -v ninja >/dev/null && command -v g++ >/dev/null || {
  echo 'The VM needs native cmake, ninja-build and g++ packages.' >&2; exit 1;
}
mkdir -p "$ROOT" "$LOG"
rm -f "$LOG/native-build-result.json"
for component in llvm clang; do
  case "$component" in
    llvm) revision=40629e6d4dbbaa22cf42174790b45a8075ed3971 ;;
    clang) revision=d3aef9bb1abef05fe425b86e8023214d39fe6acd ;;
  esac
  source="$ROOT/$component"
  if [[ ! -e "$source/.git" ]]; then
    git init -q "$source"
    git -C "$source" remote add origin "https://android.googlesource.com/platform/external/$component"
  fi
  [[ $(git -C "$source" remote get-url origin) == "https://android.googlesource.com/platform/external/$component" ]]
  [[ -z $(git -C "$source" status --porcelain) ]] || { echo 'Compiler source is modified.' >&2; exit 1; }
  if ! git -C "$source" cat-file -e "$revision^{commit}" 2>/dev/null; then
    git -C "$source" fetch --depth=1 origin "$revision"
  fi
  git -C "$source" checkout -q --detach "$revision"
  [[ $(git -C "$source" rev-parse HEAD) == "$revision" ]]
  git -C "$source" show -s --format='%H %s' > "$LOG/$component-source.txt"
done
cmake -S "$ROOT/llvm" -B "$ROOT/build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DLLVM_EXTERNAL_CLANG_SOURCE_DIR="$ROOT/clang" \
  -DLLVM_TARGETS_TO_BUILD='ARM;X86;AArch64' -DLLVM_DEFAULT_TARGET_TRIPLE=x86_64-unknown-linux \
  -DLLVM_VERSION_MAJOR=5 -DLLVM_VERSION_MINOR=0 -DLLVM_VERSION_PATCH=300080 \
  -DLLVM_VERSION_SUFFIX= -DCLANG_VENDOR=Android -DCLANG_REPOSITORY_STRING= \
  -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF \
  -DLLVM_ENABLE_TERMINFO=OFF -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_LIBEDIT=OFF \
  -DLLVM_ENABLE_ASSERTIONS=OFF > "$LOG/configure.log" 2>&1
ninja -C "$ROOT/build" -j2 clang > "$LOG/build.log" 2>&1
python3 - "$ROOT" "$LOG" <<'PY'
from pathlib import Path
import hashlib,json,struct,subprocess,sys
root=Path(sys.argv[1]);log=Path(sys.argv[2]);binary=root/'build/bin/clang-5.0';data=binary.read_bytes()
assert data[:6]==b'\x7fELF\x02\x01' and struct.unpack_from('<H',data,18)[0]==183
result={'schema':1,'compiler_sha256':hashlib.sha256(data).hexdigest(),'bytes':len(data),
 'version':subprocess.check_output([str(binary),'--version'],text=True),
 'source_revisions':{n:subprocess.check_output(['git','-C',str(root/n),'rev-parse','HEAD'],text=True).strip() for n in ('llvm','clang')},
 'native_host_compiler':subprocess.check_output(['g++','--version'],text=True).splitlines()[0],
 'aosp_build_activated':False,'comparison_required':True}
(log/'native-build-result.json').write_text(json.dumps(result,indent=2)+'\n')
print('Built native Clang; activation still requires the real-command comparison.')
PY
