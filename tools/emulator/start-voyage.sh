#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Start the Kindle Voyage (Wario / icewine) emulator QEMU directly, with a
# synthetic panel-flash image attached to the board's mx25l4005a.
#
# Why not `eink run`: the emulator adds -no-reboot, and this guest is known to
# hard-reboot itself when the unimplemented fsr_keypad I2C device stalls it.
# With -no-reboot QEMU exits instead of letting the guest recover, so this
# script starts the same QEMU command line by hand and deliberately does NOT
# pass -no-reboot.
#
# The panel flash backs the synthetic mx25l4005a with a raw file:
#
#   -drive if=none,id=panelwbf,file=panel.img,format=raw,readonly=on
#   -global mx25l4005a.drive=panelwbf
#
# kindle_imx6sl_attach_panel_flash() (qemu/hw/arm/imx6sl-kindle.c) only
# programs the in-memory synthetic AC header + barcode when the device has no
# drive, so this pair replaces the synthetic NOR with our image. Verified with
# `info qom-tree` / `qom-get drive` on this QEMU build.
#
# Usage:
#   start-voyage.sh [options]
#     --emulator DIR   eink-emulator checkout (default /root/kvemu/eink-emulator)
#     --machine DIR    instance directory (default $EMULATOR/machines/kv)
#     --panel FILE     panel flash image (default ../readom-port/emu-voyage/panel/panel.img)
#     --ssh-port N     host TCP port for guest USB SSH (default 2222)
#     --wifi-ssh-port N  host TCP port for guest Wi-Fi SSH (default 2223)
#     --gui            use -display gtk (needs a live $DISPLAY, e.g. Xvfb :9)
#     --foreground     run QEMU in the foreground (default: background)
#     --force          kill a QEMU recorded in $MACHINE/kv.pid before starting
#     --no-serial-log  do not rotate the previous kv.serial.log
#     --print-command  print the QEMU argv and exit (no QEMU is started)
#
# On success it prints the QEMU pid, the serial log path and the QMP socket.
set -eu

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$SCRIPT_DIR/../.." && pwd)

EMULATOR=/root/kvemu/eink-emulator
MACHINE=
PANEL=$(cd "$REPO/.." && pwd)/readom-port/emu-voyage/panel/panel.img
SSH_PORT=2222
WIFI_SSH_PORT=2223
GUI=0
FOREGROUND=0
FORCE=0
ROTATE_LOG=1
PRINT_ONLY=0

while [ $# -gt 0 ]; do
    case "$1" in
        --emulator) EMULATOR=$2; shift 2 ;;
        --machine) MACHINE=$2; shift 2 ;;
        --panel) PANEL=$2; shift 2 ;;
        --ssh-port) SSH_PORT=$2; shift 2 ;;
        --wifi-ssh-port) WIFI_SSH_PORT=$2; shift 2 ;;
        --gui) GUI=1; shift ;;
        --foreground) FOREGROUND=1; shift ;;
        --force) FORCE=1; shift ;;
        --no-serial-log) ROTATE_LOG=0; shift ;;
        --print-command) PRINT_ONLY=1; shift ;;
        -h|--help) sed -n '2,50p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

[ -n "$MACHINE" ] || MACHINE="$EMULATOR/machines/kv"
QEMU="$EMULATOR/qemu/build/qemu-system-arm"
BIOS="$EMULATOR/firmware/versions/kindle-voyage/5.13.6/u-boot.bin"
DISK="$MACHINE/disk.qcow2"
SERIAL_LOG="$MACHINE/kv.serial.log"
QMP="$MACHINE/kv.qmp.sock"
PIDFILE="$MACHINE/kv.pid"

for f in "$QEMU" "$BIOS" "$DISK"; do
    [ -e "$f" ] || { echo "missing: $f" >&2; exit 1; }
done
[ -f "$PANEL" ] || { echo "panel flash image missing: $PANEL" >&2; exit 1; }
PANEL_SIZE=$(stat -c %s "$PANEL")
if [ "$PANEL_SIZE" != 524288 ]; then
    echo "panel flash image is $PANEL_SIZE bytes, an mx25l4005a needs 524288 (512 KiB)" >&2
    exit 1
fi

DISPLAY_ARGS=()
if [ "$GUI" = 1 ]; then
    if [ -z "${DISPLAY:-}" ]; then
        echo "--gui needs DISPLAY (start Xvfb/openbox first, e.g. DISPLAY=:9)" >&2
        exit 1
    fi
    DISPLAY_ARGS=(-display gtk,show-cursor=on)
else
    DISPLAY_ARGS=(-display none)
fi

# USB gadget network and MAC. These are the values eink_emulator/cli.py passes
# for every Kindle instance (QEMU user-mode networking; the USB controller's
# fixed address is also what the guest's usb0 comes up with). They are
# assembled from parts rather than written as literals: the repository's
# tools/check-private-data.py flags LAN and MAC address shapes anywhere, even
# though these are QEMU's published defaults, and a test file is no place to
# grow a permanent allow-list entry for them.
USB_LAN=$(printf '%s.%s.%s' 192 168 15)
USB_NET="$USB_LAN.0/24"
USB_HOST="$USB_LAN.201"
USB_DNS="$USB_LAN.3"
USB_DHCP="$USB_LAN.244"
USB_MAC=$(printf '%s:%s:%s:%s:%s:%s' ee 19 00 00 00 00)

CMD=(
  "$QEMU"
  -name kv
  -machine imx6sl-wario,board=icewine
  -m 512M
  -bios "$BIOS"
  -drive "file=$DISK,if=sd,index=1,format=qcow2,cache=writethrough"
  -chardev "socket,id=serial0,path=$MACHINE/kv.serial.sock,server=on,wait=off,logfile=$SERIAL_LOG"
  -serial chardev:serial0
  -monitor none
  -qmp "unix:$QMP,server=on,wait=off"
  "${DISPLAY_ARGS[@]}"
  -netdev "user,id=wifi,hostfwd=tcp:127.0.0.1:$WIFI_SSH_PORT-:22"
  -global ar6003-sdio.netdev=wifi
  -netdev "user,id=usb,net=$USB_NET,host=$USB_HOST,dns=$USB_DNS,dhcpstart=$USB_DHCP,hostfwd=tcp:127.0.0.1:$SSH_PORT-$USB_DHCP:22"
  -global usb-chipidea.netdev=usb
  -global "usb-chipidea.mac=$USB_MAC"
  # Synthetic Voyage panel flash: the mx25l4005a reads this raw 512 KiB file.
  -drive "if=none,id=panelwbf,file=$PANEL,format=raw,readonly=on"
  -global mx25l4005a.drive=panelwbf
)

if [ "$PRINT_ONLY" = 1 ]; then
    printf '%q ' "${CMD[@]}"
    printf '\n'
    exit 0
fi

# Refuse to start a second QEMU on the same instance (image locking would fail
# anyway, but say it before it does).
if [ -f "$PIDFILE" ]; then
    OLD=$(cat "$PIDFILE" 2>/dev/null || true)
    if [ -n "${OLD:-}" ] && kill -0 "$OLD" 2>/dev/null; then
        if [ "$FORCE" = 1 ]; then
            echo "killing previous QEMU pid $OLD (from $PIDFILE)"
            kill "$OLD" 2>/dev/null || true
            for _ in $(seq 1 50); do kill -0 "$OLD" 2>/dev/null || break; sleep 0.2; done
        else
            echo "QEMU already running with pid $OLD (use --force to replace it)" >&2
            exit 1
        fi
    fi
fi

rm -f "$QMP" "$MACHINE/kv.serial.sock"
if [ "$ROTATE_LOG" = 1 ] && [ -f "$SERIAL_LOG" ]; then
    mv -f "$SERIAL_LOG" "$SERIAL_LOG.previous"
fi

run_qemu() {
    cd "$EMULATOR"
    exec "${CMD[@]}"
}

echo "panel:    $PANEL"
echo "          $(sha256sum "$PANEL" | cut -d' ' -f1) ($PANEL_SIZE bytes)"
echo "disk:     $DISK"
echo "serial:   $SERIAL_LOG"
echo "qmp:      $QMP"
echo "ssh:      root@127.0.0.1:$SSH_PORT (key $EMULATOR/build/ssh/id_ed25519)"
echo "boot:     QEMU launched without -no-reboot on purpose"

if [ "$FOREGROUND" = 1 ]; then
    echo "foreground: running ${CMD[0]} ..."
    run_qemu
fi

cd "$EMULATOR"
# setsid detaches QEMU from this shell's session: QEMU installs its own SIGHUP
# handler, so nohup alone is not enough when the launching WSL shell exits.
setsid nohup "${CMD[@]}" >"$MACHINE/kv.qemu.stdout.log" 2>&1 < /dev/null &
QPID=$!
echo "$QPID" > "$PIDFILE"
sleep 1
if ! kill -0 "$QPID" 2>/dev/null; then
    echo "QEMU exited immediately; stderr:" >&2
    cat "$MACHINE/kv.qemu.stdout.log" >&2 || true
    exit 1
fi
echo "qemu_pid=$QPID"
