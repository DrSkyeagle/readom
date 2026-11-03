#!/bin/bash
# How much the device draws while it is asleep, measured without anything
# running on it.
#
# The rule this is built around: a script that wakes up on the device to take a
# sample costs 6%-22% of a single core on this machine, which is more than the
# thing being measured. So nothing runs during the interval. Two samples are
# taken -- one before the device is left alone, one after -- and everything the
# answer needs is already being counted by hardware and by the kernel:
#
#   charge_now      the fuel gauge's coulomb count, in microamp-hours
#   /proc/uptime    time since boot INCLUDING the time spent suspended
#                   (3.0.35's fs/proc/uptime.c calls monotonic_to_bootbased)
#   /proc/timer_list  "now at N nsecs" is ktime_get(), CLOCK_MONOTONIC, which
#                   does NOT advance while suspended
#
# The difference between those two clocks over the interval is how long the
# device was actually suspended. Without it the number means nothing: a device
# that stayed awake all night draws a believable-looking average too.
#
# Usage:
#   tools/measure-sleep-power.sh begin [FILE]   # then unplug and leave it alone
#   tools/measure-sleep-power.sh end   [FILE]   # plug in and run this first
#
# FILE defaults to ./sleep-power-sample.txt. `end` prints the result.
#
#   ADB=...       path to adb (default: the Homebrew command-line-tools one)
#   ADB_PORT=...  adb server port, if another server has the device
set -u
ADB_BIN=${ADB:-/opt/homebrew/share/android-commandlinetools/platform-tools/adb}
ADB_PORT=${ADB_PORT:-}
adb_() { if [ -n "$ADB_PORT" ]; then "$ADB_BIN" -P "$ADB_PORT" "$@"; else "$ADB_BIN" "$@"; fi; }

BATTERY=/sys/class/power_supply/max77696-battery
FILE=${2:-sleep-power-sample.txt}
what=${1:-}

sample() {
  # One shell on the device, one round trip: the cost of the measurement
  # itself should not show up in it.
  adb_ shell "
    echo host_unix=\$(date +%s 2>/dev/null)
    echo uptime_boottime=\$(cut -d' ' -f1 /proc/uptime)
    # The line that has the clock, not the first line: /proc/timer_list opens
    # with \"Timer List Version: v0.6\", and head -1 piped through tr made the
    # monotonic clock read 06 on every sample. Written here from the start and
    # never run against the device until 2026-10-24.
    echo monotonic_ns=\$(grep -m1 'now at' /proc/timer_list | tr -dc 0-9)
    for f in charge_now charge_full voltage_now current_now capacity status; do
      v=\$(cat $BATTERY/\$f 2>/dev/null) && echo \$f=\$v
    done
    # On the build a reader gets, every one of those files is root's
    # (\`Permission denied\` even for ls), so the same numbers are read a second
    # way that needs no root -- the same reason the kernel log is read with
    # \`logcat -b kernel\` there, where \`dmesg\` is refused. BatteryService has the coulomb count,
    # and healthd writes the full charge and the instantaneous current into the
    # kernel log on every poll. Raw as the device reports them: the units and
    # the status spelling differ from sysfs, and sleep-power-report.py converts.
    dumpsys battery 2>/dev/null | sed -n \
      's/^ *Charge counter: */svc_charge_counter_uah=/p
       s/^ *voltage: */svc_voltage_mv=/p
       s/^ *level: */svc_level_pct=/p
       s/^ *status: */svc_status_code=/p'
    line=\$(logcat -b kernel -d 2>/dev/null | grep 'battery l=' | tail -1)
    case \"\$line\" in *' fc='*) echo healthd_full_charge_uah=\$(echo \"\$line\" | sed 's/.* fc=\([0-9]*\).*/\1/');; esac
    case \"\$line\" in *' c='*) echo healthd_current_ua=\$(echo \"\$line\" | sed 's/.* c=\(-\{0,1\}[0-9]*\).*/\1/');; esac
    echo screen=\$(dumpsys power 2>/dev/null | grep -c 'mWakefulness=Awake')
  " 2>/dev/null | tr -d '\r'
}

case "$what" in
begin)
  adb_ get-state >/dev/null 2>&1 || { echo "adb 看不到设备。线插着吗？Android 起来了吗？" >&2; exit 1; }
  { echo "# begin $(date -u +%Y-%m-%dT%H:%M:%SZ)"; echo "host_begin_unix=$(date +%s)"; sample; } > "$FILE"
  grep -qE 'charge_now=|svc_charge_counter_uah=' "$FILE" || {
    echo "既没读到 $BATTERY/charge_now，也没从 BatteryService 读到电量计数——没有起点可记" >&2; exit 1; }
  echo "起点已记在 $FILE："
  grep -E 'charge_now|svc_|healthd_|uptime_boottime|monotonic_ns' "$FILE" | sed 's/^/  /'
  cat <<'NOTE'

现在：拔掉数据线，按一下电源键熄屏，把设备放着别动，至少 4 小时。
不要充电——插着线测的是充电电流，不是耗电。
到时间后插上线，第一件事就跑：tools/measure-sleep-power.sh end
NOTE
  ;;
end)
  [ -f "$FILE" ] || { echo "没有找到起点文件 $FILE，先跑 begin" >&2; exit 1; }
  # adb not answering here is a result, not an error. Android holds the device
  # for as long as it is running, so if it is gone the start did not survive
  # the sleep: either it panicked (docs/open-items.md, 已知故障) or something
  # else ended it. The next tap on the Kindle entry collects the previous
  # start's kernel ring, which is where the reason is.
  if ! adb_ get-state >/dev/null 2>&1; then
    cat <<'GONE' >&2
adb 看不到设备。如果线插着、屏幕又停在 Kindle 原厂界面，那么这一觉它没睡过来——
这一程是自己结束的，不是被人结束的。这不是测量失败，是测到了另一件事。

接下来：在书库里点一次「Readom」。启动器会在那一下把上一程的内核缓冲
（连同调用栈）收进 Kindle 盘的 android/logs/<时间>/previous-kernel.txt。
GONE
    exit 3
  fi
  tmp=$(mktemp); { echo "# end $(date -u +%Y-%m-%dT%H:%M:%SZ)"; sample; } > "$tmp"
  cat "$tmp" >> "$FILE"
  python3 "$(dirname "$0")/sleep-power-report.py" "$FILE"
  rm -f "$tmp"
  ;;
*)
  sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
  exit 2 ;;
esac
