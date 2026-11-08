#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Run INSIDE the Voyage emulator guest (as root, e.g. over SSH or piped through
# `ssh ... bash -s`). Emulator-only: changes nothing persistent, but stops the
# stock UI jobs and unloads two input modules for the rest of this boot.
#
# Why this exists:
#   The stock 5.13.6 kernel's suspend path deadlocks on this QEMU machine in
#   fsr_keypad's suspend callback: `fsr_i2c_suspend -> fsr_i2c_poweroff` waits
#   for a mutex whose holder is stuck on the unimplemented kl05/fsr I2C
#   (ring-buffer evidence: error task `helper` blocked >120 s in dpm_suspend,
#   backtrace through fsr_keypad; see readom-port/voyage-e2e-report.md §5.1).
#   Unloading fsr_keypad (with its drv26xx_haptics dependency) lets the real
#   stock suspend complete: `PM: suspend of devices complete ...`,
#   `suspend debug: Waiting for 5 seconds.`, `Restarting tasks ... done.`,
#   `echo mem > /sys/power/state` returning 0 (~4-5 s round trip in TCG).
#   This is the same family of emulator limitation that makes
#   prepare-voyage-guest.sh disable deviced (deviced blocks in D state on the
#   same I2C device).
#
#   The UI jobs must be stopped first because Xorg holds /dev/keypad and
#   /dev/input/event2 open, which keeps fsr_keypad's refcount non-zero.
#
# Usage:
#   ssh -i /root/kvemu/eink-emulator/build/ssh/id_ed25519 -p 2222 root@127.0.0.1 \
#       bash -s < tools/emulator/prepare-voyage-suspend.sh
#
# Afterwards run the entry from the serial console (ttymxc0), never over SSH.
# A reboot reloads fsr_keypad; re-run this before any suspend-based run.
set -u

log() { echo "[prepare-voyage-suspend] $*"; }

[ "$(id -u)" = 0 ] || { echo "must run as root in the guest" >&2; exit 1; }

log "before: $(grep -oE '^(fsr_keypad|drv26xx_haptics) [0-9]+ [0-9]+' /proc/modules | tr '\n' ' ')"
if [ -e /proc/keypad ]; then
    log "/proc/keypad present; stopping UI jobs that keep it open"
else
    log "/proc/keypad already gone; still stopping UI jobs for a clean suspend"
fi
for job in lab126_gui x lab126; do
    if initctl status "$job" 2>/dev/null | grep -q start/running; then
        initctl stop "$job" && log "stopped $job"
    else
        log "$job not running"
    fi
done
# Xorg takes a moment to exit and close /dev/keypad and event2.
for _ in $(seq 1 50); do
    if ! grep -q 'keypad' /proc/*/maps 2>/dev/null; then break; fi
    sleep 0.1
done
sleep 1

if grep -q '^fsr_keypad ' /proc/modules; then
    log "modprobe -r fsr_keypad (also drops drv26xx_haptics)"
    modprobe -r fsr_keypad 2>&1 && log "fsr_keypad unloaded" || {
        log "unload failed; trying the dependency explicitly"
        modprobe -r drv26xx_haptics 2>&1 || true
        modprobe -r fsr_keypad 2>&1 || true
    }
fi

echo "== verification =="
if grep -qE '^(fsr_keypad|drv26xx_haptics) ' /proc/modules; then
    echo "FAIL: input modules still loaded; suspend will deadlock again" >&2
    grep -E '^(fsr_keypad|drv26xx_haptics) ' /proc/modules >&2
    exit 1
fi
echo "input modules unloaded: fsr_keypad, drv26xx_haptics"
if [ -e /proc/keypad ]; then
    echo "FAIL: /proc/keypad still exists" >&2
    exit 1
fi
echo "/proc/keypad gone"
if grep -q fsr_keypad /proc/bus/input/devices; then
    echo "FAIL: fsr_keypad still an input device" >&2
    exit 1
fi
echo "fsr_keypad not in /proc/bus/input/devices"
echo "deviced: $(ls /etc/upstart/deviced.conf* 2>/dev/null | tr '\n' ' ') (must be the .off file)"
log "ready: run the entry from the serial console; real suspend can now complete"
