#!/bin/bash
# What a computer can actually do with the device over MTP, measured.
#
# The point of MTP here is the one thing the reader will do with a cable:
# put a book or an APK on the device and take a file off it. "The gadget
# enumerates" is not that -- V58 enumerated and could not be written to -- so
# this writes, reads back, compares, and times both directions.
#
# It also crosses MTP with adb: the file the computer wrote is looked for on
# the device's own filesystem, and its SHA-256 compared there. That is what
# turns "the transfer reported success" into "the bytes are on the device".
#
# Nothing here changes the device except the one test file, which is deleted
# at the end over the same path that wrote it.
#
# Usage: tools/test-mtp.sh [MEGABYTES]
#   MEGABYTES  size of the large transfer (default 32)
#   ADB=...    path to adb (default: the Homebrew command-line-tools one)
#   ADB_PORT=  adb server port, if another server has the device
set -u
MB=${1:-32}
ADB_BIN=${ADB:-/opt/homebrew/share/android-commandlinetools/platform-tools/adb}
ADB_PORT=${ADB_PORT:-}
adb_() { if [ -n "$ADB_PORT" ]; then "$ADB_BIN" -P "$ADB_PORT" "$@"; else "$ADB_BIN" "$@"; fi; }
pass=0; fail=0; unknown=0
ck() { if [ "$2" = 1 ]; then echo "ok   $1"; pass=$((pass+1)); else echo "FAIL $1${3:+  ($3)}"; fail=$((fail+1)); fi; }
na() { echo "--   $1${2:+  ($2)}"; unknown=$((unknown+1)); }

work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT
name="pw3-mtp-test-${MB}m.bin"
sent="$work/$name"
back="$work/back.bin"

for tool in mtp-detect mtp-files mtp-sendfile mtp-getfile mtp-delfile; do
  command -v "$tool" >/dev/null || { echo "需要 libmtp 的 $tool（brew install libmtp）"; exit 2; }
done

# mtp-detect holds the device open, so everything after it has to wait for it
# to let go; libmtp has no shared session.
detect=$(mtp-detect 2>&1)
case "$detect" in
  *"No raw devices found"*|*"Unable to open raw device"*)
    ck "设备在 MTP 下被电脑看到" 0 "mtp-detect 没找到设备：线插着吗？Android 起来了吗？"
    echo; echo "通过 $pass，失败 $fail，没看成 $unknown"; exit 1 ;;
esac
ck "设备在 MTP 下被电脑看到" 1
model=$(printf '%s\n' "$detect" | sed -n 's/^ *Model: *//p' | head -1)
serial_present=$(printf '%s\n' "$detect" | grep -c "Serial Number:")
echo "     型号 ${model:-未报}（序列号不记在这里：$serial_present 行已跳过）"

# A file of random bytes: compressible test data would measure the transport's
# luck rather than its speed, and nothing in MTP compresses anyway.
dd if=/dev/urandom of="$sent" bs=1048576 count="$MB" 2>/dev/null
want=$(shasum -a 256 "$sent" | cut -d' ' -f1)

# The destination is a bare name. A leading slash makes libmtp look for a
# parent folder to put it in and fail with "Parent folder could not be found",
# which is what the first attempt at this ran into; the root of the primary
# storage is what a bare name means. (libmtp 1.1.23 then stores the file under
# the source's own basename regardless, which is why the two are the same here
# and why the check below looks for that name on the device.)
start=$(date +%s.%N)
out=$(mtp-sendfile "$sent" "$name" 2>&1)
status=$?
end=$(date +%s.%N)
up=$(echo "$end $start" | awk '{printf "%.1f", $1-$2}')
# The id the device gave it. mtp-getfile takes that, not a name: passing the
# name fails with libmtp's banner and no reason, which is how the read half of
# this test read as broken on 2026-10-23 while the transport was fine.
file_id=$(printf '%s\n' "$out" | sed -n 's/^New file ID: *\([0-9][0-9]*\).*/\1/p' | tail -1)
if [ $status -eq 0 ]; then
  ck "写：$MB MB 送上设备" 1
  echo "     $up 秒，$(echo "$MB $up" | awk '{printf "%.1f", $1/$2}') MB/s，设备给的文件号 ${file_id:-没报}"
else
  ck "写：$MB MB 送上设备" 0 "$(printf '%s' "$out" | tail -2 | tr '\n' ' ')"
fi

# Where MTP put it, as the device itself sees it. MTP's root is the primary
# external storage, which on this device is the emulated one.
on_device=""
if adb_ get-state >/dev/null 2>&1; then
  for path in "/sdcard/$name" "/storage/emulated/0/$name"; do
    if adb_ shell "[ -f '$path' ] && echo yes" 2>/dev/null | tr -d '\r' | grep -q yes; then
      on_device=$path
      break
    fi
  done
  if [ -n "$on_device" ]; then
    there=$(adb_ shell "sha256sum '$on_device'" 2>/dev/null | tr -d '\r' | cut -d' ' -f1)
    ck "设备上确实有这个文件，而且内容一致" "$([ "$there" = "$want" ] && echo 1 || echo 0)" \
       "设备上 ${there:-没算出来}，电脑上 $want"
    echo "     $on_device"
  else
    ck "设备上确实有这个文件" 0 "/sdcard 和 /storage/emulated/0 下都没有"
  fi
else
  na "设备上确实有这个文件" "adb 连不上，这一项只能靠 MTP 自己的读回"
fi

start=$(date +%s.%N)
if [ -n "$file_id" ]; then
  out=$(mtp-getfile "$file_id" "$back" 2>&1)
  status=$?
else
  out="mtp-sendfile did not report a file id, so there is nothing to ask for"
  status=1
fi
end=$(date +%s.%N)
down=$(echo "$end $start" | awk '{printf "%.1f", $1-$2}')
if [ $status -eq 0 ] && [ -s "$back" ]; then
  ck "读：同一个文件取回电脑" 1
  echo "     $down 秒，$(echo "$MB $down" | awk '{printf "%.1f", $1/$2}') MB/s"
  got=$(shasum -a 256 "$back" | cut -d' ' -f1)
  ck "取回来的内容与送上去的一致" "$([ "$got" = "$want" ] && echo 1 || echo 0)" "$got"
else
  ck "读：同一个文件取回电脑" 0 "$(printf '%s' "$out" | tail -2 | tr '\n' ' ')"
  na "取回来的内容与送上去的一致" "没取回来"
fi

# By id as well: -f takes the name the device stored it under, and that is the
# source's basename rather than the destination asked for.
out=$(mtp-delfile -n "$file_id" 2>&1)
status=$?
ck "删：测试文件从设备上清掉" "$([ $status -eq 0 ] && echo 1 || echo 0)" "$(printf '%s' "$out" | tail -1)"
if [ -n "$on_device" ] && adb_ get-state >/dev/null 2>&1; then
  left=$(adb_ shell "[ -f '$on_device' ] && echo yes" 2>/dev/null | tr -d '\r')
  ck "设备上也看不到它了" "$([ -z "$left" ] && echo 1 || echo 0)" "$on_device 还在"
fi

echo
echo "通过 $pass，失败 $fail，没看成 $unknown"
[ "$fail" = 0 ]
