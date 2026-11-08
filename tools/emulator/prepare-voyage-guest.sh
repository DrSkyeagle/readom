#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Run INSIDE the Voyage emulator guest (as root, e.g. over SSH):
#
#   ssh -i build/ssh/id_ed25519 -p 2222 root@127.0.0.1 \
#       bash -s < tools/emulator/prepare-voyage-guest.sh [WAVEFORM_WRF_GZ]
#
# Optional argument: path of the synthetic waveform gzip inside the guest
# (default /tmp/synth.wrf.gz, copy it there with scp first). It must be the
# file tools/emulator/make_voyage_panel.py produced as synth.wrf.gz.
#
# What this does, and why (all emulator-only, no effect on a real Kindle):
#
#   1. Stops deviced and disables it for the next boot. The emulator does not
#      implement the fsr_keypad I2C device; deviced blocks in D state on it,
#      which breaks the freeze/thaw cycle the emulator relies on.
#      /etc/upstart/deviced.conf -> deviced.conf.off (rootfs briefly remounted
#      read-write; this is the persistent qcow2 overlay, not the shared base).
#   2. Stops wand/mwan when they are running (no WAN hardware in the
#      emulator; their retry loops only add boot noise).
#   3. Replaces whatever waveform the hidden waveform FAT (volume label
#      MFWWAVEFORM inside mmcblk0p4 at byte offset 4096*7233) holds with the
#      synthetic one. The stock /etc/upstart/display job and the udev
#      waveform.sh helper only look for any *.wrf.gz in
#      /mnt/wfm/waveform_to_use, so the file name does not matter.
#   4. Installs the same file as a stock waveform override under
#      /opt/eink/images/<PANEL_CODE>/ (PANEL_CODE is derived from the panel's
#      XWI, EBAL_3CE for this synthetic panel).  This is the piece that makes a
#      cold boot end with source=stored: the initramfs loads mxc_epdc_fb at
#      ~2.9 s, before the hidden FAT can be mounted, so that first firmware
#      request always falls back to the built-in waveform; the display job on
#      the real rootfs then sees source=built-in, clears the FAT and follows
#      /etc/upstart/display's own override path -- copies this file to
#      waveform_to_use, unloads and reloads mxc_epdc_fb -- and the reload's
#      firmware request is served from the file.
#
# After this, reboot the guest (or restart QEMU): /proc/eink/waveform/source
# reads "stored" and /proc/eink/waveform/data carries the synthetic 2048-byte
# WRF.
set -eu

WFM_GZ=${1:-/tmp/synth.wrf.gz}
WFM_OFFSET=$((4096 * 7233))
WFM_DEV=/dev/loop/1
WFM_MNT=/mnt/wfm
OVERRIDE_ROOT=/opt/eink/images
OVERRIDE_DEFAULT=EBAL_3CE

log() { echo "[prepare-voyage-guest] $*"; }

[ "$(id -u)" = 0 ] || { echo "must run as root in the guest" >&2; exit 1; }
[ -f "$WFM_GZ" ] || { echo "waveform gzip not found: $WFM_GZ" >&2; exit 1; }
log "waveform: $WFM_GZ ($(wc -c < "$WFM_GZ") bytes, sha256 $(sha256sum "$WFM_GZ" | cut -d' ' -f1))"

# --- 1. deviced off (persistent, once) -------------------------------------
if [ -e /etc/upstart/deviced.conf ]; then
    log "remounting / read-write to disable deviced"
    mount -o remount,rw /
    mv /etc/upstart/deviced.conf /etc/upstart/deviced.conf.off
    sync
    mount -o remount,ro /
    log "deviced.conf -> deviced.conf.off"
elif [ -e /etc/upstart/deviced.conf.off ]; then
    log "deviced already disabled (deviced.conf.off)"
else
    log "WARNING: neither deviced.conf nor deviced.conf.off exists"
fi
initctl stop deviced 2>/dev/null || true

# --- 2. WAN services off ----------------------------------------------------
for job in wand mwan; do
    if initctl status "$job" 2>/dev/null | grep -q start/running; then
        log "stopping $job"
        initctl stop "$job" 2>/dev/null || true
    fi
done

# --- 3. hidden waveform FAT -------------------------------------------------
mkdir -p "$WFM_MNT"
if ! losetup -a 2>/dev/null | grep -q "^$WFM_DEV:"; then
    losetup -o "$WFM_OFFSET" "$WFM_DEV" /dev/mmcblk0
fi
if ! grep -q " $WFM_MNT " /proc/self/mounts; then
    mount -t vfat "$WFM_DEV" "$WFM_MNT"
fi
mkdir -p "$WFM_MNT/waveform_to_use"
log "old hidden-FAT contents:"
ls -la "$WFM_MNT/waveform_to_use" || true
rm -f "$WFM_MNT/waveform_to_use"/*.wrf.gz
cp "$WFM_GZ" "$WFM_MNT/waveform_to_use/synth.wrf.gz"
sync
log "new hidden-FAT contents:"
ls -la "$WFM_MNT/waveform_to_use"
sha256sum "$WFM_MNT/waveform_to_use/synth.wrf.gz"
umount "$WFM_MNT"
losetup -d "$WFM_DEV"

# --- 4. stock waveform override under /opt/eink/images/<PANEL_CODE>/ -------
PANEL_CODE=$OVERRIDE_DEFAULT
if [ -r /proc/eink/panel/waveform/human_version ]; then
    HV=$(cat /proc/eink/panel/waveform/human_version)
    BATCH=$(echo "$HV" | cut -d _ -f 2)
    PART=$(echo "$HV" | cut -d _ -f 4)
    [ -n "$BATCH" ] && [ -n "$PART" ] && PANEL_CODE="${BATCH}_${PART}"
fi
OVERRIDE_DIR="$OVERRIDE_ROOT/$PANEL_CODE"
log "installing override: $OVERRIDE_DIR/synth.wrf.gz"
mount -o remount,rw /
mkdir -p "$OVERRIDE_DIR"
cp "$WFM_GZ" "$OVERRIDE_DIR/synth.wrf.gz"
sync
mount -o remount,ro /
ls -la "$OVERRIDE_DIR"
sha256sum "$OVERRIDE_DIR/synth.wrf.gz"
log "done; reboot the guest so /etc/upstart/display follows the override path"
