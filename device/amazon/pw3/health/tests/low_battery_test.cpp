// SPDX-License-Identifier: Apache-2.0
// Host test of the low-battery rule: g++ -std=c++14 -I.. low_battery_test.cpp
#include <cassert>
#include <cstdio>
#include "LowBattery.h"

int main() {
    // A reading a sample apart from the one before it counts as the next one.
    const long long s = pw3::kResampleSeconds;
    pw3::LowBattery low;
    assert(!low.empty(false, 3700, 0));
    assert(!low.empty(false, 3450, s));        // one reading at the limit: a dip
    assert(low.empty(false, 3449, 2 * s));     // the second in a row: empty
    assert(low.empty(false, 3400, 3 * s));     // stays empty
    assert(!low.empty(true, 3400, 4 * s));     // plugged in: never
    assert(!low.empty(false, 3440, 5 * s));    // the count starts again
    assert(low.empty(false, 3440, 6 * s));

    pw3::LowBattery dip;
    assert(!dip.empty(false, 3420, 0));
    assert(!dip.empty(false, 3600, s));        // recovered: not two in a row
    assert(!dip.empty(false, 3420, 2 * s));
    assert(!dip.empty(false, 0, 3 * s));       // no reading: unknown, not low
    assert(!dip.empty(false, 3420, 4 * s));
    assert(dip.empty(false, 3420, 5 * s));

    // Unplugging sends a charger event and a battery event, and the gauge holds
    // its sample for 30 s, so both carry the same measurement. One dip under
    // load must not end the session for having been reported twice.
    pw3::LowBattery unplug;
    assert(!unplug.empty(false, 3400, 100));
    assert(!unplug.empty(false, 3400, 100));   // the same sample, same instant
    assert(!unplug.empty(false, 3400, 100 + s - 1));
    assert(unplug.empty(false, 3400, 100 + s));

    // A sample that arrives late still counts, and a recovery in between still
    // starts the count again however long the gap was.
    pw3::LowBattery slow;
    assert(!slow.empty(false, 3400, 0));
    assert(!slow.empty(false, 3600, 1000));
    assert(!slow.empty(false, 3400, 2000));
    assert(slow.empty(false, 3400, 2000 + s));
    std::puts("LOW_BATTERY_TEST_PASS");
    return 0;
}
