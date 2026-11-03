#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""The sleep-power arithmetic, and the four ways it can look right and be wrong.

A draw measured over a device that never suspended, or that was charging while
the second sample was read, comes out as a believable number. Those are the
cases this holds the reporter to -- not the happy one, which is the easy half.
"""
import importlib.util
import io
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('pw3_sleep_power',
                                              ROOT / 'tools/sleep-power-report.py')
sleep_power = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sleep_power)

# One night: 8 hours 10 minutes, 98% of it suspended, 16.4 mAh gone.
GOOD = """# begin
uptime_boottime=1000.00
monotonic_ns=1000000000000
charge_now=1200000
charge_full=1420000
voltage_now=3900000
current_now=-2000
capacity=84
status=Discharging
# end
uptime_boottime=30400.00
monotonic_ns=1600000000000
charge_now=1183600
charge_full=1420000
voltage_now=3880000
current_now=-2100
capacity=83
status=Discharging
"""


# The same night as GOOD, read the way a release build allows: BatteryService
# for the coulomb count, the voltage (millivolts, not micro), the level and a
# numeric status, and healthd's kernel-log line for the full charge.
SERVICE = """# begin
uptime_boottime=1000.00
monotonic_ns=1000000000000
svc_charge_counter_uah=1200000
svc_voltage_mv=3900
svc_level_pct=84
svc_status_code=3
healthd_full_charge_uah=1420000
healthd_current_ua=-2000
# end
uptime_boottime=30400.00
monotonic_ns=1600000000000
svc_charge_counter_uah=1183600
svc_voltage_mv=3880
svc_level_pct=83
svc_status_code=3
healthd_full_charge_uah=1420000
healthd_current_ua=-2100
"""


def run(text):
    out = io.StringIO()
    result = sleep_power.report(*sleep_power.parse(text), out=out)
    return out.getvalue(), result


class SleepPower(unittest.TestCase):
    def test_the_night_that_went_well(self):
        text, result = run(GOOD)
        self.assertEqual(result['seconds'], 29400)
        self.assertEqual(result['suspended'], 28800)
        self.assertEqual(result['microamp_hours'], 16400)
        self.assertIn('98.0%', text)
        # 16400 µAh over 8.1667 h is 2008 µA; 1420 mAh at that rate is 29.5 days.
        self.assertIn('2008 µA', text)
        self.assertIn('29.5 天', text)
        self.assertNotIn('不是"睡着时的耗电"', text)

    def test_a_device_that_never_slept_is_not_reported_as_asleep(self):
        """The failure this whole measurement exists to catch.

        Same interval, same charge used: the average is identical and perfectly
        believable. The only thing that differs is that the monotonic clock ran
        the whole time, and the report has to lead with that.
        """
        text, _ = run(GOOD.replace('monotonic_ns=1600000000000',
                                   'monotonic_ns=30400000000000'))
        self.assertIn('2008 µA', text)          # the number still looks fine
        self.assertIn('0.0%', text)             # and this is why it is not
        self.assertIn('不是"睡着时的耗电"', text)

    def test_charging_at_the_end_is_called_out(self):
        text, _ = run(GOOD.replace('capacity=83\nstatus=Discharging',
                                   'capacity=83\nstatus=Charging'))
        self.assertIn('不是放电', text)

    def test_charge_that_went_up_is_not_reported_as_a_draw(self):
        text, _ = run(GOOD.replace('charge_now=1183600', 'charge_now=1210000'))
        self.assertIn('电量没有下降', text)
        self.assertNotIn('平均电流', text)

    def test_a_reboot_between_the_samples_is_refused(self):
        """boottime going backwards is a restart, not a measurement."""
        with self.assertRaises(SystemExit):
            run(GOOD.replace('uptime_boottime=30400.00', 'uptime_boottime=400.00'))
        # And the subtler half: the monotonic clock cannot have advanced more
        # than boottime did.
        with self.assertRaises(SystemExit):
            run(GOOD.replace('monotonic_ns=1600000000000', 'monotonic_ns=40000000000000'))

    def test_a_missing_field_names_itself(self):
        with self.assertRaises(SystemExit) as caught:
            run(GOOD.replace('charge_now=1200000\n', ''))
        self.assertIn('charge_now', str(caught.exception))

    def test_a_file_with_no_end_sample(self):
        with self.assertRaises(SystemExit):
            sleep_power.parse(GOOD.split('# end')[0])

    def test_the_release_build_reads_the_same_night(self):
        """No sysfs, and the answer has to come out identical.

        This is the build a reader gets: /sys/class/power_supply is root's, so
        every number arrives under another name, in another unit, with the
        status as an integer. If any of that conversion were wrong the report
        would still print something believable, so it is pinned to GOOD's.
        """
        plain, _ = run(GOOD)
        text, result = run(SERVICE)
        self.assertEqual(result, sleep_power.report(*sleep_power.parse(GOOD),
                                                    out=io.StringIO()))
        self.assertIn('2008 µA', text)
        self.assertIn('29.5 天', text)
        self.assertIn('电压 3900000 µV', text)      # millivolts, converted
        self.assertIn('状态 Discharging', text)     # status 3, spelled out
        self.assertNotIn('不是放电', text)
        self.assertIn('读自 BatteryService', text)  # and it says where it read
        self.assertNotIn('读自 BatteryService', plain)

    def test_sysfs_wins_when_both_answered(self):
        both = SERVICE.replace('svc_charge_counter_uah=1200000',
                               'charge_now=1200000\nsvc_charge_counter_uah=999')
        _, result = run(both)
        self.assertEqual(result['microamp_hours'], 16400)

    def test_a_status_code_this_table_does_not_know_is_kept_as_it_came(self):
        text, _ = run(SERVICE.replace('svc_status_code=3\nhealthd_full_charge_uah=1420000\nhealthd_current_ua=-2100',
                                      'svc_status_code=9\nhealthd_full_charge_uah=1420000\nhealthd_current_ua=-2100'))
        self.assertIn('状态 9', text)
        self.assertIn('不是放电', text)             # unknown is not "discharging"

    def test_the_monotonic_clock_comes_from_the_line_that_has_it(self):
        """What the first run on the device caught.

        /proc/timer_list opens with "Timer List Version: v0.6", so `head -1`
        piped through `tr -dc 0-9` reported the monotonic clock as 06 -- in
        both samples, which makes their difference zero and every night look
        perfectly suspended. The unit tests could not see it: they are given
        the sample, not the device.
        """
        script = (ROOT / 'tools/measure-sleep-power.sh').read_text()
        self.assertNotIn('head -1 /proc/timer_list', script)
        self.assertIn("'now at' /proc/timer_list", script)

    def test_the_sampler_reads_what_the_reporter_needs(self):
        """The two halves have to agree on the field names.

        They are in different files and different languages, so nothing else
        would notice if one of them were renamed.
        """
        script = (ROOT / 'tools/measure-sleep-power.sh').read_text()
        for field in sleep_power.NEEDED + ('charge_full', 'status'):
            self.assertIn(field, script, '%s is never sampled' % field)
        # And the names the second source reports them under, which nothing
        # else would catch: they exist only in these two files.
        for _, reported, _ in sleep_power.SECOND_SOURCE:
            self.assertIn(reported, script, '%s is never sampled' % reported)
        # And the paths it reads are the ones this port actually exposes.
        self.assertIn('/sys/class/power_supply/max77696-battery', script)
        healthd = (ROOT / 'device/amazon/pw3/health/healthd.cpp').read_text()
        self.assertIn('/sys/class/power_supply/max77696-battery/charge_now', healthd)


if __name__ == '__main__':
    unittest.main(verbosity=0)
