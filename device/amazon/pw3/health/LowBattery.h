// SPDX-License-Identifier: Apache-2.0
#ifndef PW3_LOW_BATTERY_H
#define PW3_LOW_BATTERY_H

namespace pw3 {

// At or below this voltage on battery the gauge driver refuses to suspend, "since the
// PMIC's LOBAT/CRITBAT will shut us down soon" (max77696-battery.c,
// SYS_SUSPEND_LOW_VOLT_THRESH): the system can no longer sleep, and the power will be
// cut without a shutdown.
constexpr int kNoSuspendMillivolts = 3450;

// Readings in a row at or below it before the battery counts as empty, so that a dip
// under load (an e-paper refresh) does not end the session. While the system is awake
// the driver reports a new reading every 30 s.
constexpr int kLowReadings = 2;

// How far apart two of them have to be to be two readings at all. The gauge caches a
// sample for 30 s (max77696-battery.c), and reading voltage_now does not refresh it,
// so two callbacks closer together than that carry the same measurement. Unplugging
// makes exactly that happen: the charger and the battery each send their own event,
// healthd handles both, and a single dip under load would have counted twice and
// ended the session with charge left.
constexpr long long kResampleSeconds = 30;

// Whether to report the battery empty, which makes Android shut down cleanly first
// (BatteryService shuts down at 0 % when not powered). `seconds` is a monotonic
// clock: it does not advance while the system sleeps, which is right here, because
// the driver refuses to suspend at this voltage and so is not sampling either.
class LowBattery {
  public:
    bool empty(bool powered, int millivolts, long long seconds) {
        if (powered || millivolts <= 0 || millivolts > kNoSuspendMillivolts) {
            low_ = 0;
            return false;
        }
        if (!low_) {
            low_ = 1;
            at_ = seconds;
        } else if (low_ < kLowReadings && seconds - at_ >= kResampleSeconds) {
            ++low_;
            at_ = seconds;
        }
        return low_ == kLowReadings;
    }

  private:
    int low_ = 0;
    long long at_ = 0;
};

}  // namespace pw3

#endif
