#!/usr/bin/env python3
"""A gap in scheduling is not a clock change, and the log said which one it was.

On 2026-10-20 `system_server` held this single core for over thirty minutes and
starved the dex2oat that was running to 0.36%. The thread stack pointed at
`UsageStatsService.onStatsReloaded -> postOneTimeCheckIdleStates`, and the log
was a stream of `Time changed in UsageStats by +2/+3/+4/-3 seconds` with
`Flushing usage stats to disk` behind it.

`checkAndGetTimeLocked()` read the two clocks in two adjacent statements -- the
wall clock first, the monotonic one second -- and called the difference a clock
change when it passed two seconds. A thread that loses the CPU between those two
statements reads a monotonic clock that is too large by the gap, so the
difference is about minus the gap. Patch 0050 reads the monotonic clock before
and after, carries the snapshot's own width alongside, and only decides when the
whole interval is past the threshold.

This test is what makes two statements about that checkable rather than
plausible, by running both algorithms over the same gaps:

1. **The patch cannot be fooled by scheduling**, however long the gap, and it
   still catches every real change above the threshold plus the two widths.
2. **All of what was logged was scheduling.** The first firing of the old code is
   minus the first gap; after it resets the snapshot from that skewed pair, every
   later difference is the difference of two consecutive gaps, so the magnitudes
   are bounded by how much the gaps vary and the signs alternate. That is exactly
   the shape in the log: a few seconds, mixed signs, going on and on. A clock
   somebody actually set produces one firing whose size is the jump, and then
   silence -- which is what case 3 below shows.

The algorithm here is a model of the Java, so it is only worth anything while the
two agree: the last part of the test pins the expressions the model stands for.
"""
import pathlib
import random
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
PATCH = ROOT / 'patches/aosp-frameworks-base/0050-pw3-a-scheduling-gap-is-not-a-clock-change.patch'
THRESHOLD = 2000          # TIME_CHANGE_THRESHOLD_MILLIS
WALL0 = 1_700_000_000_000
failures = []


def check(ok, message):
    if not ok:
        failures.append(message)


def run(gaps, offsets, patched):
    """Every call to checkAndGetTimeLocked(), returning the differences it reported.

    `gaps[i]` is how long the thread was off the CPU between the two clock reads
    of call i; `offsets[i]` is how far somebody has moved the wall clock by then.
    """
    snap_mono, snap_wall, spread = 0, WALL0, 0
    true_mono = 0
    fired = []
    for gap, offset in zip(gaps, offsets):
        true_mono += 5_000                      # time between calls; not an error
        wall = WALL0 + true_mono + offset
        if patched:
            before, after = true_mono, true_mono + gap
            half = spread // 2
            at_latest = (wall - half) - ((after - snap_mono) + snap_wall)
            at_earliest = (wall + half) - ((before - snap_mono) + snap_wall)
            hit = at_latest > THRESHOLD or at_earliest < -THRESHOLD
            # What the patch reports and hands to onTimeChanged() is computed from
            # the midpoint, not from the interval endpoint that decided the firing.
            # Taking the endpoint here made the model report a smaller number than
            # the code does, and the bound asserted below was then about the
            # model's number rather than the one the statistics are moved by.
            midpoint = before + (after - before) // 2
            difference = wall - ((midpoint - snap_mono) + snap_wall)
            # One branch for both: a firing and a narrowing re-measure the pair
            # the same way. Only the firing also corrects the statistics already
            # stored, so a narrowing moves the mapping by the error it is taking
            # out of it and leaves that much in the statistics -- bounded by half
            # the widest reading there has been. Carrying the offset across
            # instead keeps the error in the pair, and the next narrow reading
            # then sees it as a difference and fires: checks 2 and 3 below fail
            # on that, which is how it was found.
            if hit or (after - before) < spread:
                snap_mono = before + (after - before) // 2
                snap_wall, spread = wall, after - before
        else:
            mono = true_mono + gap              # read after losing the CPU
            difference = wall - ((mono - snap_mono) + snap_wall)
            hit = abs(difference) > THRESHOLD
            if hit:
                snap_mono, snap_wall = mono, wall
        if hit:
            fired.append(difference)
    # The second value is the mapping the pair stands for -- what
    # convertToSystemTimeLocked() adds to a monotonic event time. Only a firing,
    # which corrects the stored statistics by the same amount, may move it.
    return fired, snap_wall - snap_mono


# 1. Scheduling alone, over the range of gaps a starved core actually produces
#    and then well past it. Nothing here moves the clock.
random.seed(20261021)
for low, high in ((0, 50), (2_200, 6_000), (0, 30_000), (0, 600_000)):
    gaps = [random.randint(low, high) for _ in range(2_000)]
    quiet = [0] * len(gaps)
    false_positives, mapping = run(gaps, quiet, patched=True)
    # With no clock change, the mapping may only drift by the error a reading
    # takes out of the pair, which is bounded by half the widest gap. It is not
    # zero and must not be: see the branch above.
    check(abs(mapping - WALL0) <= high // 2 + 1,
          f'patch 0050: the mapping drifted {mapping - WALL0} ms over gaps of '
          f'{low}-{high} ms with no clock change, past the half-width that is all a '
          f're-measurement can be taking out of the pair')
    check(not false_positives,
          f'patch 0050: {len(false_positives)} false positives for gaps of '
          f'{low}-{high} ms, first {false_positives[:3]}')

# 2. A real change, once, with scheduling noise on top. The threshold plus both
#    widths is the most the pair of intervals can hide, so anything past it has
#    to be seen -- and seen once, because the snapshot is reset to it.
for jump in (10_000, -10_000, 60_000, -3_600_000, 86_400_000):
    for high in (50, 6_000):
        gaps = [random.randint(0, high) for _ in range(60)]
        offsets = [0] * 6 + [jump] * 54
        fired, _ = run(gaps, offsets, patched=True)
        check(len(fired) == 1,
              f'patch 0050: a {jump} ms change with gaps up to {high} ms fired '
              f'{len(fired)} times, expected once ({fired[:4]})')
        if fired:
            # Within half the sampling width of the real change, on either side of
            # it. Not "never more than the real change", which is what this said
            # until 2026-10-21: the figure reported -- and handed to
            # onTimeChanged(), so the amount the stored statistics are moved by --
            # comes from the midpoint of the pair of monotonic reads, and the wall
            # clock was read somewhere inside that pair. A -10,000 ms change over
            # gaps up to 6,000 ms is reported as -11,348 ms here. The old code is
            # worse on the same input, being off by the whole gap rather than half
            # of it, which is the comparison that matters; bounding it tightly is a
            # separate change and is not one this patch makes.
            check(abs(abs(fired[0]) - abs(jump)) <= high // 2 + 1,
                  f'patch 0050: a {jump} ms change with gaps up to {high} ms was '
                  f'reported as {fired[0]} ms, more than half a sampling width away')

# 2b. The worst case for the snapshot's own width, which is why it is carried at
#     all. A real change is noticed during a wide sample, so the snapshot stores a
#     midpoint that is off by up to half that width; if the next call compared
#     against it as if it were exact, that error alone would be read as a second
#     change. The first draft of patch 0050 did exactly that and this is the case
#     that catches it: wide gap at the moment of detection, no gap afterwards.
for jump in (10_000, -10_000, 86_400_000):
    for wide in (6_000, 20_000, 120_000):
        gaps = [0] * 6 + [wide] + [0] * 40
        offsets = [0] * 6 + [jump] * 41
        fired, _ = run(gaps, offsets, patched=True)
        check(len(fired) == 1,
              f'patch 0050: a {jump} ms change seen during a {wide} ms gap fired '
              f'{len(fired)} times, expected once ({fired[:4]}) -- the snapshot\'s '
              f'own width is being treated as exact')

# 3. Below the threshold nothing fires, so the small drift between a 1-second RTC
#    and the monotonic clock across a resume stays quiet.
for jump in (-1_900, -1_000, 0, 1_000, 1_900):
    gaps = [random.randint(0, 50) for _ in range(60)]
    fired, _ = run(gaps, [0] * 6 + [jump] * 54, patched=True)
    check(not fired, f'patch 0050: a {jump} ms change fired {len(fired)} times, expected none')

# 4. The old code, on the gaps that were actually happening: this is the evidence
#    that the log was scheduling and nothing else.
gaps = [random.randint(2_200, 6_000) for _ in range(300)]
old, _ = run(gaps, [0] * len(gaps), patched=False)
check(len(old) > 20, f'the old code fired only {len(old)} times; the model no longer reproduces it')
if old:
    check(round(old[0] / -1000) == round(gaps[0] / 1000),
          f'the old code\'s first firing should be minus the first gap, was {old[0]} vs {gaps[0]}')
    later = old[1:]
    check(all(abs(d) <= 6_000 for d in later),
          f'the old code\'s later firings should be bounded by the gap spread, '
          f'largest was {max(abs(d) for d in later)}')
    check(max(abs(round(d / 1000)) for d in later) <= 4,
          'the later firings should be the few seconds the log showed')
    alternating = sum(1 for i in range(1, len(later)) if (later[i] > 0) != (later[i - 1] > 0))
    check(alternating >= 0.9 * (len(later) - 1),
          f'the old code\'s firings should alternate in sign, only '
          f'{alternating}/{len(later)-1} did')

# 5. The model is only worth something while it is the same arithmetic as the
#    patch. These are the expressions it stands for.
text = PATCH.read_text(encoding='utf-8')
# Whole expressions, and how many times each appears. Fragments were not enough:
# with only the left-hand sides pinned, replacing every
# `mRealTimeSnapshotSpread = afterRealtime - beforeRealtime;` with `= 0` -- which
# takes out the whole mechanism this patch is -- left the suite passing, because
# the Python above is the model's arithmetic and not the patch's. Found on
# 2026-10-21 by running exactly that mutation.
#
# `+` stripped, so the added lines read as the Java they become.
# From the diff body only. The patch's own prose quotes the original code with
# the same indentation, so taking the whole file counted those lines too.
body = text.splitlines()
body = body[next(i for i, l in enumerate(body) if l.startswith('--- a/')):]
java = '\n'.join(line[1:] for line in body
                 if line.startswith(('+', ' ')) and not line.startswith('+++'))
for needle, times in (
        ('long mRealTimeSnapshotSpread;', 1),
        ('final long snapshotHalf = mRealTimeSnapshotSpread / 2;', 1),
        ('final long beforeRealtime = SystemClock.elapsedRealtime();', 2),
        ('final long afterRealtime = SystemClock.elapsedRealtime();', 2),
        # the three places the width is taken: the first snapshot, a firing, a narrowing
        ('mRealTimeSnapshotSpread = afterRealtime - beforeRealtime;', 3),
        # and the two places the pair is anchored to the midpoint of a pair of reads
        ('mRealTimeSnapshot = beforeRealtime + (afterRealtime - beforeRealtime) / 2;', 2),
        ('final long diffAtLatest = (actualSystemTime - snapshotHalf)\n'
         '                - ((afterRealtime - mRealTimeSnapshot) + mSystemTimeSnapshot);', 1),
        ('final long diffAtEarliest = (actualSystemTime + snapshotHalf)\n'
         '                - ((beforeRealtime - mRealTimeSnapshot) + mSystemTimeSnapshot);', 1),
        ('if ((diffAtLatest > TIME_CHANGE_THRESHOLD_MILLIS\n'
         '                        || diffAtEarliest < -TIME_CHANGE_THRESHOLD_MILLIS)\n'
         '                && ENABLE_TIME_CHANGE_CORRECTION) {', 1),
        # the wall clock written into the pair: once by the firing, once by the
        # narrowing, and once when the first snapshot is taken at boot
        ('mSystemTimeSnapshot = System.currentTimeMillis();', 1),
        ('} else if (afterRealtime - beforeRealtime < mRealTimeSnapshotSpread) {', 1),
        ('final long actualRealtime =\n'
         '                    beforeRealtime + (afterRealtime - beforeRealtime) / 2;', 1),
        ('final long diffSystemTime = actualSystemTime - expectedSystemTime;', 1),
        ('mSystemTimeSnapshot = actualSystemTime;', 2),
):
    got = java.count(needle)
    check(got == times,
          f'patch 0050: expected {needle.splitlines()[0]!r} {times} time(s), found {got}; '
          f'the model above stands for expressions the patch no longer has')
# The wall clock has to stay between the two monotonic reads, or the interval
# this test models is not the interval the code computes. The wall-clock read is
# a context line -- it was already the first statement -- so the lines the patch
# leaves in place count here too, and only the removed ones are dropped.
kept = '\n'.join(line for line in text.splitlines()
                 if line.startswith(('+', ' ')) and not line.startswith('+++'))
hunk = kept[kept.index('private long checkAndGetTimeLocked()'):]
check(hunk.index('final long beforeRealtime = SystemClock.elapsedRealtime();')
      < hunk.index('final long actualSystemTime = System.currentTimeMillis();')
      < hunk.index('final long afterRealtime = SystemClock.elapsedRealtime();'),
      'patch 0050 must read the wall clock between the two monotonic reads')

for message in failures:
    print('FAIL', message)
if failures:
    sys.exit(1)
print('PW3_USAGE_TIME_CHECK_PASS  8000 scheduling gaps 0 false positives, '
      '10 real changes all caught once, 5 sub-threshold changes quiet, '
      'and the old code reproduces the logged signature')
