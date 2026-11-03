#!/bin/sh
# Name: Android Installer Check
# SPDX-License-Identifier: Apache-2.0
#
# What the device-side installer relies on, tried once on this Kindle's stock
# system (installer/README.md, the two "尚未在真机上验证的" lists): the installer's screens
# through eips -- and, from the display driver's own log, the waveform that
# actually reached the panel -- the download through the stock curl with the
# installer's CA bundle (and two other sites, to tell a TLS problem from a network
# that cannot reach GitHub), resuming, the lipc readings, and pw3-unpack's speed
# and memory. Writes only in its own folder /mnt/us/android-check-<time>/ and
# deletes its temporary files; what stays is report.txt and epdc.txt, the display
# driver's lines for those updates. Records no device identifiers and no network
# names or addresses.
umask 022
PATH=/sbin:/bin:/usr/sbin:/usr/bin
export PATH LC_ALL=C
B=/bin/busybox
SELF=$0
OUT=/mnt/us/android-check-$("$B" date +%Y%m%dT%H%M%S)
"$B" mkdir "$OUT" || exit 1
R=$OUT/report.txt
say() { echo "$*" >> "$R"; }
now_ms() { "$B" awk '{ printf "%d\n", $1 * 1000 }' /proc/uptime; }
row() { /usr/sbin/eips 1 "$1" "$2                                        " >/dev/null 2>&1; }
mem() { "$B" awk '/^(MemFree|Cached):/ { printf "%s=%s ", $1, $2 }' /proc/meminfo; }
extract() {	# NAME FILE SHA256
	"$B" sed -n "/^__${1}__\$/,/^__END_${1}__\$/p" "$SELF" | "$B" sed '1d;$d' | "$B" base64 -d > "$2"
	[ "$("$B" sha256sum "$2" | "$B" cut -d ' ' -f 1)" = "$3" ]
}

row 1 "ANDROID INSTALLER CHECK: ABOUT 5 MIN"
row 2 "PLEASE DO NOT TOUCH THE KINDLE."
say "schema=android-installer-check-1"
say "firmware=$("$B" head -n 1 /etc/version.txt 2>/dev/null | "$B" tr -d '\r')"
say "kernel=$("$B" uname -r)"
say "user_store_kb=$("$B" df -k /mnt/us | "$B" awk 'NR == 2 { print $2 " total " $4 " free" }')"
say "eips_sha256=$("$B" sha256sum /usr/sbin/eips | "$B" cut -d ' ' -f 1)"
ok=1
for f in UNPACK:pw3-unpack:@UNPACK_SHA@ TESTIMG:test.zst:@TESTIMG_SHA@ CA:ca-bundle.pem:@CA_SHA@ \
	 SCREENA:screen-a.png:@SCREENA_SHA@ SCREENB:screen-b.png:@SCREENB_SHA@; do
	name=${f%%:*}; rest=${f#*:}; file=${rest%%:*}; sum=${rest#*:}
	extract "$name" "$OUT/$file" "$sum" || { say "extract_$file=failed"; ok=0; }
done
say "extract_ok=$ok"
if [ "$ok" != 1 ]; then	# nothing was changed yet; a missing file must not pass as a result
	"$B" rm -f "$OUT/pw3-unpack" "$OUT/test.zst" "$OUT/ca-bundle.pem" "$OUT/screen-a.png" "$OUT/screen-b.png"
	row 1 "ANDROID INSTALLER CHECK: DAMAGED FILE.       "
	row 2 "COPY IT AGAIN.                              "
	exit 1
fi
"$B" chmod 0755 "$OUT/pw3-unpack"

# lipc: what the installer reads, and holding the screen saver off.
say "battery=$(/usr/bin/lipc-get-prop com.lab126.powerd battLevel 2>&1) charging=$(/usr/bin/lipc-get-prop com.lab126.powerd isCharging 2>&1)"
say "wifi_cm_state=$(/usr/bin/lipc-get-prop com.lab126.wifid cmState 2>&1)"
before=$(/usr/bin/lipc-get-prop com.lab126.powerd preventScreenSaver 2>/dev/null)
/usr/bin/lipc-set-prop com.lab126.powerd preventScreenSaver 1 >/dev/null 2>&1
say "screen_saver_held_rc=$? before=$before now=$(/usr/bin/lipc-get-prop com.lab126.powerd preventScreenSaver 2>/dev/null)"
say "clock_utc=$("$B" date -u +%Y-%m-%dT%H:%M:%SZ)"

# The installer's screens: cleared once, then two screens as the installer draws them (GL16).
#
# What eips prints is the waveform it asked for: it prints it before the ioctl,
# and the driver is free to substitute (mxc_epdc_fb.c picks a mode of its own for
# AUTO, and merges a pending update into the new one keeping the higher-fidelity
# waveform of the two). The waveform that reaches the panel is the one in the
# driver's "Sending update. waveform:..." line, printed only while its debug
# switch is on. Whether these screens flash is that line's answer, not eips's.
EPDCDBG=$("$B" find /sys/devices -maxdepth 4 -name mxc_epdc_debug 2>/dev/null | "$B" head -n 1)
say "epdc_debug=${EPDCDBG:-none}"
epdcwas=
if [ -n "$EPDCDBG" ]; then
	epdcwas=$("$B" cat "$EPDCDBG" 2>/dev/null)
	echo 1 > "$EPDCDBG" 2>/dev/null
fi
# Where the kernel log stands now, so that only the lines these updates add are
# read. The log itself is left alone: clearing it would take away whatever the
# stock system had put there.
klog0=$("$B" dmesg | "$B" wc -l)
/usr/sbin/eips -c >/dev/null 2>&1
say "eips_clear_rc=$?"
t0=$(now_ms)
/usr/sbin/eips -g "$OUT/screen-a.png" -x @REGION_X@ -y @REGION_Y@ -w gl16 > "$OUT/eips-a.txt" 2>&1
say "eips_screen_a_rc=$? ms=$(($(now_ms) - t0)) out=$("$B" head -c 200 "$OUT/eips-a.txt" | "$B" tr '\n' ' ')"
"$B" sleep 8
t0=$(now_ms)
/usr/sbin/eips -g "$OUT/screen-b.png" -x @REGION_X@ -y @REGION_Y@ -w gl16 > "$OUT/eips-b.txt" 2>&1
say "eips_screen_b_rc=$? ms=$(($(now_ms) - t0)) out=$("$B" head -c 200 "$OUT/eips-b.txt" | "$B" tr '\n' ' ')"
"$B" sleep 8
# Only this driver's lines are kept. The stock kernel log of the same minutes can
# hold Wi-Fi messages with addresses in them, and nothing here writes those down.
"$B" dmesg | "$B" tail -n +$((klog0 + 1)) | "$B" grep mxc_epdc_fb > "$OUT/epdc.txt"
[ -n "$EPDCDBG" ] && echo "${epdcwas:-0}" > "$EPDCDBG" 2>/dev/null
say "epdc_sent=$("$B" grep -o 'Sending update. waveform:[^ ]* ([^)]*)' "$OUT/epdc.txt" | "$B" tr '\n' ' ')"
say "epdc_lines=$("$B" wc -l < "$OUT/epdc.txt")"
"$B" rm -f "$OUT/eips-a.txt" "$OUT/eips-b.txt"
/usr/sbin/eips -c >/dev/null 2>&1
row 1 "ANDROID INSTALLER CHECK: NETWORK"

# HTTPS: two other sites first, then GitHub, all with the installer's CA bundle only.
CURL="/usr/bin/curl --location --max-redirs 5 --proto =https --proto-redir =https --silent --show-error --connect-timeout 30 --cacert $OUT/ca-bundle.pem"
for site in https://www.baidu.com/ https://www.cloudflare.com/ https://api.github.com/ https://github.com/; do
	$CURL --head --max-time 60 -o /dev/null -w 'http=%{http_code} verify=%{ssl_verify_result} tcp_ms=%{time_connect} tls_ms=%{time_appconnect}' "$site" > "$OUT/site.txt" 2> "$OUT/site-error.txt"
	say "head $site rc=$? $("$B" cat "$OUT/site.txt") error=$("$B" head -c 160 "$OUT/site-error.txt" | "$B" tr '\n' ' ')"
done
URL=@DOWNLOAD_URL@
SHA=@DOWNLOAD_SHA@
PART=$OUT/download.part
t0=$(now_ms)
$CURL --fail --speed-limit 1024 --speed-time 60 --continue-at - --output "$PART" --write-out 'http=%{http_code} bytes=%{size_download} speed_Bps=%{speed_download}' "$URL" > "$OUT/dl.txt" 2> "$OUT/dl-error.txt"
rc=$?
say "download rc=$rc ms=$(($(now_ms) - t0)) $("$B" cat "$OUT/dl.txt") error=$("$B" head -c 200 "$OUT/dl-error.txt" | "$B" tr '\n' ' ')"
if [ -f "$PART" ]; then
	say "download_sha_ok=$([ "$("$B" sha256sum "$PART" | "$B" cut -d ' ' -f 1)" = "$SHA" ] && echo 1 || echo 0)"
	# Resuming: keep the first 1,000,000 bytes and continue.
	"$B" dd if="$PART" of="$PART.cut" bs=1000000 count=1 2>/dev/null && "$B" mv "$PART.cut" "$PART"
	$CURL --fail --speed-limit 1024 --speed-time 60 --continue-at - --output "$PART" --write-out 'http=%{http_code} bytes=%{size_download}' "$URL" > "$OUT/dl.txt" 2> "$OUT/dl-error.txt"
	say "resume rc=$? $("$B" cat "$OUT/dl.txt") error=$("$B" head -c 200 "$OUT/dl-error.txt" | "$B" tr '\n' ' ')"
	say "resume_sha_ok=$([ "$("$B" sha256sum "$PART" | "$B" cut -d ' ' -f 1)" = "$SHA" ] && echo 1 || echo 0)"
fi
"$B" rm -f "$PART" "$OUT/site.txt" "$OUT/site-error.txt" "$OUT/dl.txt" "$OUT/dl-error.txt"

# pw3-unpack: speed and memory on this Kindle's FAT user store.
row 1 "ANDROID INSTALLER CHECK: UNPACKING"
say "mem_before $(mem)"
t0=$(now_ms)
"$OUT/pw3-unpack" "$OUT/test.zst" @TESTIMG_BYTES@ @TESTIMG_SHA@ "$OUT/unpack.img" @IMG_BYTES@ @IMG_SHA@ > "$OUT/unpack.txt" 2> "$OUT/unpack-error.txt"
rc=$?
say "unpack rc=$rc ms=$(($(now_ms) - t0)) image_bytes=@IMG_BYTES@ packed_bytes=@TESTIMG_BYTES@ error=$("$B" head -c 200 "$OUT/unpack-error.txt" | "$B" tr '\n' ' ')"
say "unpack_progress_lines=$("$B" wc -l < "$OUT/unpack.txt")"
say "mem_after $(mem)"
"$B" rm -f "$OUT/unpack.img" "$OUT/unpack.txt" "$OUT/unpack-error.txt" "$OUT/test.zst" "$OUT/pw3-unpack" "$OUT/ca-bundle.pem" \
	"$OUT/screen-a.png" "$OUT/screen-b.png"

/usr/bin/lipc-set-prop com.lab126.powerd preventScreenSaver "${before:-0}" >/dev/null 2>&1
say "screen_saver_restored=$(/usr/bin/lipc-get-prop com.lab126.powerd preventScreenSaver 2>/dev/null)"
say "done=1"
"$B" sync
row 1 "ANDROID INSTALLER CHECK: DONE                "
row 2 "RESULT: android-check-* FOLDER              "
exit 0
