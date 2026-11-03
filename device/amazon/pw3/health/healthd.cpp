// SPDX-License-Identifier: Apache-2.0
#include <healthd/healthd.h>
#include <batteryservice/BatteryService.h>
#include "LowBattery.h"
#include <time.h>
void healthd_board_init(healthd_config* config) {
    // Requires LAB126_ANDROID_POWER_SUPPLY_ABI: both polling and Binder
    // getProperty now observe micro-units at the single kernel boundary.
    config->batteryChargeCounterPath = "/sys/class/power_supply/max77696-battery/charge_now";
    // On battery, wake the system once an hour instead of every ten minutes:
    // each wake-up from sleep costs about 1.3 s of resume work, and while the
    // system is awake the gauge driver reports every change itself.
    config->periodic_chores_interval_slow = 60 * 60;
}
int healthd_board_battery_update(android::BatteryProperties* props) {
    static pw3::LowBattery low;
    const bool powered = props->chargerAcOnline || props->chargerUsbOnline ||
                         props->chargerWirelessOnline;
    timespec now = {};
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (low.empty(powered, props->batteryVoltage, now.tv_sec)) props->batteryLevel = 0;
    return 0;
}
