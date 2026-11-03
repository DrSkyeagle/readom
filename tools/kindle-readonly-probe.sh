#!/bin/sh
# Name: PW3 Hardware Probe
# Author: PW3 local port project
# Collect hardware metadata only. Writes only /mnt/us/pw3-port-probe/.

set -eu
if [ ! -d /mnt/us/documents ] || [ ! -r /proc/version ]; then
    echo 'This script must run on the connected Kindle, not on the Mac.' >&2
    exit 1
fi
REPORT_DIR=/mnt/us/pw3-port-probe
mkdir -p "$REPORT_DIR"
if ! mkdir "$REPORT_DIR/.running" 2>/dev/null; then
    echo 'A hardware report is already being collected.'
    exit 0
fi
trap 'rmdir "$REPORT_DIR/.running" 2>/dev/null || true' 0 1 2 15

read_value() {
    value_path=$1
    if [ -r "$value_path" ] && [ ! -d "$value_path" ]; then
        printf '%s: ' "$value_path"
        cat "$value_path" 2>/dev/null || true
        printf '\n'
    fi
}

{
    printf 'PW3 hardware metadata probe v1\n'
    date -u 2>/dev/null || true
    uname -srm
    printf '\n[kernel]\n'
    cat /proc/version
    if [ -r /etc/prettyversion.txt ]; then cat /etc/prettyversion.txt; fi
    printf '\n[cpu_without_serial]\n'
    grep -E '^(Processor|model name|CPU architecture|CPU part|CPU revision|Hardware|Revision|Features)[[:space:]]*:' /proc/cpuinfo || true
    printf '\n[memory]\n'
    grep -E '^(MemTotal|MemFree|Buffers|Cached|SwapTotal|SwapFree):' /proc/meminfo || true
    printf '\n[boot_arguments_allowlist]\n'
    for boot_arg in $(cat /proc/cmdline); do
        case "$boot_arg" in
            root=*|rootfstype=*|rootwait|ro|rw|mem=*|console=*) printf '%s\n' "$boot_arg" ;;
        esac
    done
    read_value /proc/idme/bootmode
    read_value /proc/bootmode
    printf '\n[block_layout]\n'
    cat /proc/partitions
    for block in /sys/class/block/mmcblk0 /sys/class/block/mmcblk0p* /sys/class/block/mmcblk0boot*; do
        [ -d "$block" ] || continue
        for field in size start ro force_ro; do read_value "$block/$field"; done
    done
    printf '\n[mmc_mounts]\n'
    awk '$1 ~ /^\/dev\/mmcblk0/ {print $1, $2, $3}' /proc/mounts
    printf '\n[framebuffer]\n'
    for field in name virtual_size bits_per_pixel rotate modes state; do
        read_value "/sys/class/graphics/fb0/$field"
    done
    if command -v fbset >/dev/null 2>&1; then fbset -i 2>/dev/null || true; fi
    printf '\n[backlight]\n'
    for lamp in /sys/class/backlight/*; do
        [ -d "$lamp" ] || continue
        for field in max_brightness brightness actual_brightness bl_power; do read_value "$lamp/$field"; done
    done
    printf '\n[input_without_identifiers]\n'
    if [ -r /proc/bus/input/devices ]; then
        grep -E '^(N: Name=|H: Handlers=|B: (EV|KEY|ABS|SW)=)' /proc/bus/input/devices || true
    fi
    printf '\n[loaded_modules]\n'
    awk '{print $1, $2, $3}' /proc/modules
    printf '\n[power_supply]\n'
    for supply in /sys/class/power_supply/*; do
        [ -d "$supply" ] || continue
        for field in type status present online capacity voltage_now current_now temp charge_full charge_now cycle_count; do
            read_value "$supply/$field"
        done
    done
    printf '\n[power_management]\n'
    for field in state pm_async wakeup_count; do read_value "/sys/power/$field"; done
    printf '\n[network_driver_without_addresses]\n'
    for netdev in /sys/class/net/wlan* /sys/class/net/eth*; do
        [ -d "$netdev" ] || continue
        read_value "$netdev/type"
        read_value "$netdev/operstate"
        readlink "$netdev/device/driver" 2>/dev/null || true
    done
    printf '\n[usb_gadget]\n'
    for gadget in /sys/class/android_usb/*; do
        [ -d "$gadget" ] || continue
        for field in functions state enable; do read_value "$gadget/$field"; done
    done
    printf '\n[config_availability]\n'
    if [ -r /proc/config.gz ]; then
        printf 'kernel_config=available\n'
        gzip -cd /proc/config.gz > "$REPORT_DIR/kernel.config"
    else
        printf 'kernel_config=unavailable\n'
    fi
    printf '\n[probe_complete]\n'
} > "$REPORT_DIR/report.txt.tmp"
mv "$REPORT_DIR/report.txt.tmp" "$REPORT_DIR/report.txt"
printf 'v1 complete\n' > "$REPORT_DIR/complete.txt"
echo 'Hardware report saved. Return to the library and reconnect USB.'
