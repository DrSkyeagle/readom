#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""What two samples taken around a night of sleep say about the draw.

measure-sleep-power.sh takes the samples; this reads them. It is a separate
file because the arithmetic is the part that can be wrong in a way nobody
notices: a plausible average comes out of a device that never slept, and out of
one that was charging while it was read.

So the three things it refuses to leave implicit:

  * **how much of the interval was actually spent suspended.** /proc/uptime
    counts suspended time and /proc/timer_list's `now at N nsecs` does not, so
    their difference is it. A measurement over a device that stayed awake is
    not a sleep measurement, and the number is printed whether or not it is
    good news;
  * **whether the device was charging at the end.** The fuel gauge's charge
    goes up on USB, and the cable has to be plugged in to read the second
    sample. The reading is taken first thing, but the status is reported;
  * **what the extrapolation assumes.** Days of standby only follows from an
    average draw if the device spends that standby the same way.
"""
import argparse
import re
import sys

# charge in microamp-hours, voltage in microvolts, current in microamps: the
# units the power supply class uses.
NEEDED = ('charge_now', 'uptime_boottime', 'monotonic_ns')

# The second way of reading the same battery, for the release build, where
# /sys/class/power_supply is root's: BatteryService's dump and the line healthd
# writes into the kernel log. Each entry is the sysfs name this file uses, the
# name the device reported it under, and what turns one into the other.
BATTERY_STATUS = {'1': 'Unknown', '2': 'Charging', '3': 'Discharging',
                  '4': 'Not charging', '5': 'Full'}
SECOND_SOURCE = (
    ('charge_now', 'svc_charge_counter_uah', lambda v: v),
    ('charge_full', 'healthd_full_charge_uah', lambda v: v),
    ('voltage_now', 'svc_voltage_mv', lambda v: str(int(v) * 1000)),
    ('capacity', 'svc_level_pct', lambda v: v),
    ('current_now', 'healthd_current_ua', lambda v: v),
    ('status', 'svc_status_code', lambda v: BATTERY_STATUS.get(v, v)),
)


def normalise(sample):
    """One sample in the power supply class's units, whichever way it was read."""
    out = dict(sample)
    filled = []
    for name, reported, convert in SECOND_SOURCE:
        if name in out or reported not in out:
            continue
        try:
            out[name] = convert(out[reported])
        except ValueError:          # the device said something unexpected; drop it
            continue
        filled.append(name)
    # Which fields did not come from sysfs, so the report can say so.
    out['second_source'] = ' '.join(filled)
    return out


def parse(text):
    """The two samples of a file written by measure-sleep-power.sh."""
    parts = text.split('# end')
    if len(parts) < 2:
        raise SystemExit('this file has no end sample yet')
    def grab(block):
        return normalise(dict(re.findall(r'^(\w+)=(\S+)$', block, re.M)))
    return grab(parts[0]), grab('# end'.join(parts[1:]))


def report(begin, end, out=sys.stdout):
    missing = sorted({k for k in NEEDED for d in (begin, end) if k not in d})
    if missing:
        raise SystemExit('缺少 %s：这台机器没有暴露它，或者采样被打断了'
                         % '、'.join(missing))
    seconds = int(end['uptime_boottime'].split('.')[0]) - int(begin['uptime_boottime'].split('.')[0])
    awake = (int(end['monotonic_ns']) - int(begin['monotonic_ns'])) / 1e9
    used = int(begin['charge_now']) - int(end['charge_now'])      # µAh
    if seconds <= 0:
        raise SystemExit('终点比起点早，或者中间重启过：boottime 没有前进')
    if awake < 0 or awake > seconds + 1:
        raise SystemExit('单调时钟走得比 boottime 还多，中间重启过')
    suspended = seconds - awake
    share = 100.0 * suspended / seconds
    lines = ['间隔        %.0f 秒 = %.2f 小时' % (seconds, seconds / 3600),
             '其中挂起    %.0f 秒 = %.1f%%' % (suspended, share),
             '其中醒着    %.0f 秒' % awake,
             '耗电        %d µAh = %.2f mAh' % (used, used / 1000)]
    if used <= 0:
        lines.append('电量没有下降：要么在充电，要么间隔太短，这一轮量不出东西')
    else:
        average = used / (seconds / 3600.0)                        # µA
        lines.append('平均电流    %.0f µA = %.2f mA' % (average, average / 1000))
        full = begin.get('charge_full') or end.get('charge_full')
        if full and full.isdigit() and int(full) > 0:
            lines.append('按本机 charge_full %.0f mAh 推算  待机 %.1f 天'
                         % (int(full) / 1000, int(full) / average / 24))
        else:
            lines.append('没有读到 charge_full，不外推待机天数')
    if share < 90:
        lines.append('这段时间有 %.1f%% 醒着，所以上面的平均值不是"睡着时的耗电"。'
                     '先查什么在拿唤醒锁，再重量一次。' % (100 - share))
    status = (end.get('status') or '').lower()
    if status and status not in ('discharging', 'unknown'):
        lines.append('终点时电池状态是 %s，不是放电：插线到读数之间充进去的电'
                     '会让耗电看起来偏小。' % end['status'])
    if 'charge_now' in begin.get('second_source', ''):
        lines.append('电量计读自 BatteryService 的 Charge counter，不是 sysfs：'
                     '发行版上 /sys/class/power_supply 要 root，而这个数不要。')
    for name, d in (('起点', begin), ('终点', end)):
        lines.append('%s  电压 %s µV  电量 %s%%  状态 %s  瞬时电流 %s µA'
                     % (name, d.get('voltage_now', '?'), d.get('capacity', '?'),
                        d.get('status', '?'), d.get('current_now', '?')))
    print('\n'.join(lines), file=out)
    return {'seconds': seconds, 'suspended': suspended, 'microamp_hours': used}


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('file')
    args = parser.parse_args()
    report(*parse(open(args.file).read()))
    return 0


if __name__ == '__main__':
    sys.exit(main())
