#!/bin/sh
# Name: Android Installer Probe
# SPDX-License-Identifier: Apache-2.0
#
# What a device-side installer can count on, measured on this Kindle's stock
# system (installer/README.md, "探针要量的未知项"). Reads, one download of a public
# file from GitHub, and a 64 MiB write test in its own folder; changes nothing
# else. Writes no device identifiers: no serial number, MAC address, network
# name or address. Result: /mnt/us/android-probe-<time>/report.txt.
umask 022
PATH=/sbin:/bin:/usr/sbin:/usr/bin
export PATH LC_ALL=C
B=/bin/busybox
OUT=/mnt/us/android-probe-$("$B" date +%Y%m%dT%H%M%S)
"$B" mkdir "$OUT" || exit 1
R=$OUT/report.txt
say() { echo "$*" >> "$R"; }
now_ms() { "$B" awk '{ printf "%d\n", $1 * 1000 }' /proc/uptime; }
show() { /usr/sbin/eips 1 3 "$1                                        " >/dev/null 2>&1; }

show "ANDROID PROBE: RUNNING, ABOUT 3 MIN"
say "schema=android-probe-1"
say "firmware=$("$B" head -n 1 /etc/version.txt 2>/dev/null | "$B" tr -d '\r')"
say "kernel=$("$B" uname -r) $("$B" uname -v)"
say "cpu_hardware=$("$B" sed -n 's/^Hardware[[:space:]]*: //p' /proc/cpuinfo)"
say "mem_total_kb=$("$B" awk '/^MemTotal/ { print $2 }' /proc/meminfo)"
say "mem_free_kb=$("$B" awk '/^MemFree/ { print $2 }' /proc/meminfo)"
say "battery_percent=$(/usr/bin/lipc-get-prop com.lab126.powerd battLevel 2>/dev/null)"
say "charging=$(/usr/bin/lipc-get-prop com.lab126.powerd isCharging 2>/dev/null)"
say "user_store_kb=$("$B" df -k /mnt/us | "$B" awk 'NR == 2 { print $2 " total " $4 " free" }')"

# Keeping the device awake while an installer works: the property and whether it can be set.
say "powerd_state=$(/usr/bin/lipc-get-prop com.lab126.powerd state 2>/dev/null)"
before=$(/usr/bin/lipc-get-prop com.lab126.powerd preventScreenSaver 2>/dev/null)
say "prevent_screensaver_before=$before"
/usr/bin/lipc-set-prop com.lab126.powerd preventScreenSaver 1 >/dev/null 2>&1
say "prevent_screensaver_set_rc=$? now=$(/usr/bin/lipc-get-prop com.lab126.powerd preventScreenSaver 2>/dev/null)"

# Network: the stock tools, TLS to GitHub, a release download and its speed.
say "wifi_state=$(/usr/bin/lipc-get-prop com.lab126.wifid cmState 2>/dev/null)"
say "curl=$(/usr/bin/curl --version 2>/dev/null | "$B" head -n 1)"
say "ca_files=$("$B" ls /etc/ssl/certs 2>/dev/null | "$B" wc -l)"
URL=https://github.com/plateaukao/einkbro/releases/download/v16.7.1/app-armeabi-v7a-release.apk
SHA=4bf5a7245141022c0bc29434afc81e7665ea3d55af718aefe3e9e12417d673b6
t0=$(now_ms)
/usr/bin/curl -sS -L --max-time 180 -o "$OUT/download.bin" -w 'http=%{http_code} ssl_verify=%{ssl_verify_result} bytes=%{size_download} speed_Bps=%{speed_download} redirects=%{num_redirects}' "$URL" > "$OUT/curl.txt" 2> "$OUT/curl-error.txt"
rc=$?
t1=$(now_ms)
say "download_rc=$rc ms=$((t1 - t0)) $("$B" cat "$OUT/curl.txt") error=$("$B" head -c 200 "$OUT/curl-error.txt" | "$B" tr '\n' ' ')"
if [ -f "$OUT/download.bin" ]; then
	say "download_sha256_ok=$([ "$("$B" sha256sum "$OUT/download.bin" | "$B" cut -d ' ' -f 1)" = "$SHA" ] && echo 1 || echo 0)"
fi
# Without certificate checks, if the checked download failed: is it the certificates?
if [ "$rc" != 0 ]; then
	/usr/bin/curl -sS -k -L --max-time 60 -o /dev/null -w 'http=%{http_code}' -r 0-1023 "$URL" > "$OUT/curl-k.txt" 2>&1
	say "download_unchecked_rc=$? $("$B" cat "$OUT/curl-k.txt" | "$B" head -c 200)"
fi
say "range_request=$(/usr/bin/curl -sS -L --max-time 60 -o /dev/null -w '%{http_code}' -r 1000-1999 "$URL" 2>/dev/null)"
"$B" rm -f "$OUT/download.bin"

# Storage: writing and reading back 64 MiB in this folder.
t0=$(now_ms)
"$B" dd if=/dev/zero of="$OUT/write.tmp" bs=1048576 count=64 2>/dev/null && "$B" sync
t1=$(now_ms)
echo 3 > /proc/sys/vm/drop_caches
"$B" dd if="$OUT/write.tmp" of=/dev/null bs=1048576 2>/dev/null
t2=$(now_ms)
say "write_64MiB_ms=$((t1 - t0)) read_64MiB_ms=$((t2 - t1))"
"$B" rm -f "$OUT/write.tmp"

# Showing a picture with the stock tool, for prompts that need Chinese text.
"$B" printf 'P5\n8 8\n255\n' > "$OUT/test.pgm"
"$B" dd if=/dev/zero bs=64 count=1 2>/dev/null >> "$OUT/test.pgm"
say "eips_help=$(/usr/sbin/eips -h 2>&1 | "$B" grep -E -- '-g|-b' | "$B" head -n 3 | "$B" tr '\n' ' ')"

/usr/bin/lipc-set-prop com.lab126.powerd preventScreenSaver "${before:-0}" >/dev/null 2>&1
say "prevent_screensaver_restored=$(/usr/bin/lipc-get-prop com.lab126.powerd preventScreenSaver 2>/dev/null)"
say "done=1"
"$B" sync
show "ANDROID PROBE: DONE. RESULT IN android-probe-*"
