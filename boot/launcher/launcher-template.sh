#!/bin/sh
# Name: PW3 First Kernel
# Starts Android on this Kindle: loads a kernel into RAM and hands the device over to it.
# Everything it needs is in this one file. Tapping the entry in the library runs it.
set -eu
umask 077
PATH=/sbin:/bin:/usr/sbin:/usr/bin
export PATH
export LC_ALL=C
B=/bin/busybox
SELF=/mnt/us/documents/PW3-First-Kernel.sh
[ "$#" -le 1 ]
# Where this run's evidence goes: one folder per tap, named for the time. The
# helper is compiled with the same prefix (PW3_OUTPUT_PREFIX in helper.c) and
# refuses anything else, and the release entry's prefix is inside the folder the
# installer owns, which is why the directory above it is made first. The name is
# settled here; the folders themselves are made after the traps, below.
OUT=${1:-@LOGPREFIX@$("$B" date +%Y%m%dT%H%M%S)-$$}
case "$OUT" in @LOGPREFIX@?*) ;; *) exit 77;; esac
suffix=${OUT#@LOGPREFIX@}
case "$suffix" in ''|*[!0-9TZ-]*) exit 77;; esac
[ "${#suffix}" -ge 2 ] && [ "${#suffix}" -le 80 ]
[ ! -e "$OUT" ] && [ ! -L "$OUT" ]
TMP=/tmp/pw3-first-kernel-launch-$$
tmp_created=0
helper_verified=0
stop_asked=0
helper_exit=125
finish() {
    # 3: the helper stopped and its reason is on the screen
    fallback_shown=0
    if [ "$helper_exit" != 0 ] && [ "$helper_exit" != 3 ]; then
        eips_hash=$("$B" sha256sum /usr/sbin/eips 2>/dev/null | "$B" cut -d ' ' -f 1)
        if [ "$eips_hash" = @EIPS_HASH@ ]; then
            /usr/sbin/eips 1 1 '@STOPPED_MESSAGE@' && fallback_shown=1 || true
        fi
    fi
    # The line is painted over by the library about a second after this script
    # exits (the launcher that runs it repaints), so it is held until the
    # reader taps -- by the helper, and only by one whose hash was checked: a
    # helper that failed its check is not run for anything, and not after a
    # request to stop, which is what ended the run. Before the record below,
    # which then covers the hold's own line too, as the helper's hold is
    # covered when the helper holds a screen of its own.
    if [ "$fallback_shown" = 1 ] && [ "$helper_verified" = 1 ] && [ "$stop_asked" = 0 ]; then
        /usr/sbin/eips 1 3 'TAP THE SCREEN TO GO BACK TO THE LIBRARY.' || true
        "$B" sync
        if [ -d "$OUT" ]; then
            "$TMP/helper" --hold-screen || true
        else
            "$TMP/helper" --hold-screen > /dev/null 2>&1 || true
        fi
    fi
    # The folder is not there when the store had no room for it, which is the
    # one failure this has to survive rather than add to.
    if [ -d "$OUT" ]; then
        echo "helper_exit=$helper_exit" > "$OUT/launcher-complete.txt" 2>/dev/null || true
        (cd "$OUT" && for f in *; do
            [ "$f" != SHA256SUMS ] || continue
            [ -f "$f" ] && [ ! -L "$f" ] || continue
            "$B" sha256sum "$f"
        done) > "$OUT/SHA256SUMS" 2>/dev/null || true
    fi
    "$B" sync
    if [ "$tmp_created" = 1 ]; then
        "$B" rm -f "$TMP/helper"
        "$B" rmdir "$TMP" || true
    fi
}
# Before the folders are made, not after: `set -e` on a store with no free
# cluster left used to end the script here, with no trap, no message and nothing
# in the library to show a tap had happened at all. The reason for the folder is
# to record what went wrong, so failing to make it must not be the thing that
# goes unrecorded.
trap finish EXIT
trap 'stop_asked=1; exit 130' INT
trap 'stop_asked=1; exit 143' HUP TERM
# The helper, out of this file and into /tmp, checked against its hash. Every
# step says when it failed rather than leaning on `set -e`, which a function
# called as a condition does not have: the store-full branch below calls it
# that way, and a check that did not stop it there would mark an unchecked
# helper as checked.
unpack_helper() {
    [ "$("$B" id -u)" = 0 ] || return 1
    [ -f "$SELF" ] && [ ! -L "$SELF" ] || return 1
    "$B" mkdir "$TMP" || return 1
    tmp_created=1
    "$B" sed '1,/^__PW3_FIRST_KERNEL_HELPER_BASE64__$/d' "$SELF" | "$B" base64 -d > "$TMP/helper" || return 1
    hash=$("$B" sha256sum "$TMP/helper" | "$B" cut -d ' ' -f 1) || return 1
    [ "$hash" = @HELPER_HASH@ ] || return 1
    "$B" chmod 0700 "$TMP/helper" || return 1
    helper_verified=1
}
if ! "$B" mkdir -p @LOGPARENT@ || ! "$B" mkdir "$OUT"; then
    helper_exit=70	# not 3: the trap's line is the only thing that can be said
    # That line is the one about a full store, and it is gone a second after
    # this script exits unless something holds it (finish). /tmp is not the
    # store, so the helper can still be unpacked to hold it.
    unpack_helper > /dev/null 2>&1 || true
    exit 70
fi
exec > "$OUT/launcher.log" 2>&1
unpack_helper
set +e
"$TMP/helper" "$OUT"
helper_exit=$?
set -e
exit "$helper_exit"
__PW3_FIRST_KERNEL_HELPER_BASE64__
@BASE64@
