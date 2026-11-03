#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Two calls in the launcher whose arguments can be transposed and still compile.

`boot/launcher/helper.c` runs on the device, as root, before Android starts. Its
unit tests call the functions in `userlog.c` directly, so they cover what those
functions do and nothing about how the one call site passes its arguments. Both
calls below take adjacent parameters of one type -- two `unsigned`, and two
`const char *` -- so swapping a pair compiles silently and changes what the
person reading the folder on their Kindle drive is told.

Covering them at runtime needs either a QEMU fixture or a host shell that can
run `save_android_report()`, and that function's file includes four headers the
entry build generates (`payload.h`, `release.h`, `screen.h`, `ReportArea.h`), so
such a shell would be a test that only runs after a release has been built. The
cheaper answer is that each of these calls has a second statement next to it
that has to agree with it, and agreement is readable from the source:

  * `pw3_logs_start_word(found, before, now)` chooses the word, and the
    `printf` on the next line prints `previous_boot=` and `this_boot=`. The word
    and the two numbers must describe the same two readings in the same order,
    so the call's 2nd and 3rd arguments must be the printf's last two, in order.
    Transposing either one makes them disagree.
  * `pw3_logs_report_max(dir, current, leaf, boot)` takes the folder being
    written now and the name of the report file inside each folder -- swapped,
    it would look for a folder named `android-report.txt` inside a folder named
    after this run, find nothing, and report "no previous counter", which is
    exactly the reading that decides the word above.

The parse is deliberately narrow: it locates the calls, splits their arguments
at depth zero, and fails if it cannot find them at all, so a rename cannot turn
this into a test that passes over nothing.
"""
import re
import sys
import traceback
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
HELPER = ROOT / 'boot/launcher/helper.c'
HEADER = ROOT / 'boot/launcher/userlog.h'

checks = 0
failures = []


def check(ok, what):
    global checks
    checks += 1
    if not ok:
        failures.append(what)


def arguments(text, name):
    """The arguments of the one call to `name`, split at bracket depth zero."""
    at = text.find(name + '(')
    if at < 0:
        return None
    i = at + len(name) + 1
    depth, start, out = 0, i, []
    while i < len(text):
        char = text[i]
        if char in '([':
            depth += 1
        elif char == ')' and depth == 0:
            out.append(text[start:i].strip())
            return [a for a in out if a]
        elif char in ')]':
            depth -= 1
        elif char == ',' and depth == 0:
            out.append(text[start:i].strip())
            start = i + 1
        i += 1
    return None


def main():
    check(HELPER.is_file(), '%s is missing' % HELPER)
    check(HEADER.is_file(), '%s is missing' % HEADER)
    if not HELPER.is_file() or not HEADER.is_file():
        return report()
    text = HELPER.read_text(encoding='utf-8')

    # The parameter names, read from the header rather than assumed, so that a
    # change to the order there shows up here as a disagreement to look at.
    declaration = HEADER.read_text(encoding='utf-8')
    found = re.search(r'pw3_logs_start_word\(([^)]*)\)', declaration)
    check(found is not None,
          'userlog.h no longer declares pw3_logs_start_word')
    if found:
        names = [p.strip().split()[-1].lstrip('*') for p in found.group(1).split(',')]
        check(names == ['found', 'before', 'now'],
              'pw3_logs_start_word\'s parameters are now %s; this test pins the '
              'order of the last two, so it has to be read again' % names)

    call = arguments(text, 'pw3_logs_start_word')
    check(call is not None and len(call) == 3,
          'helper.c: no three-argument call to pw3_logs_start_word was found '
          '(got %s)' % (call,))

    # The printf that prints both readings, found by its format rather than by
    # position: there are several PW3_ANDROID_START lines and only one shows
    # both numbers.
    shown = None
    at = text.find('previous_boot=%u this_boot=%u')
    check(at > 0, 'helper.c: no line prints previous_boot and this_boot together')
    if at > 0:
        tail = text[at:]
        close = tail.find(');')
        shown = [a.strip() for a in tail[:close].split(',')[1:]] if close > 0 else None
    check(shown is not None and len(shown) == 3,
          'helper.c: could not read the arguments of that printf (got %s)' % (shown,))

    if call and shown and len(call) == 3 and len(shown) == 3:
        check(call[1:] == shown[1:],
              'helper.c: the word is chosen from (%s, %s) and the line prints '
              '(%s, %s). One of the two is transposed, so the word and the '
              'numbers describe different readings'
              % (call[1], call[2], shown[1], shown[2]))

    # The second call: the folder written now, then the name of the file inside
    # each folder. Both are const char *, so a swap compiles.
    maxcall = arguments(text, 'pw3_logs_report_max')
    check(maxcall is not None and len(maxcall) == 4,
          'helper.c: no four-argument call to pw3_logs_report_max was found '
          '(got %s)' % (maxcall,))
    if maxcall and len(maxcall) == 4:
        check(maxcall[2] == 'ANDROID_REPORT_LOG',
              'helper.c: the third argument of pw3_logs_report_max is %r, and '
              'the function takes the name of the report file there. Swapped '
              'with the folder, it would look for a folder named after the '
              'report file and report no previous counter'
              % maxcall[2])
        check(maxcall[1] != 'ANDROID_REPORT_LOG' and maxcall[1] == 'current',
              'helper.c: the second argument of pw3_logs_report_max is %r, and '
              'the function takes the folder being written now there'
              % maxcall[1])

    print('%d checks, %d failed' % (checks, len(failures)))
    for failure in failures:
        print('  FAIL %s' % failure)
    return 1 if failures else 0


def report():
    print('%d checks, %d failed' % (checks, len(failures)))
    for failure in failures:
        print('  FAIL %s' % failure)
    return 1 if failures else 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Exception:  # noqa: BLE001 - any fault here is this test's answer
        print('0 checks, 1 failed')
        print('  FAIL the check itself stopped: %s'
              % traceback.format_exc().strip().replace('\n', ' | '))
        sys.exit(1)
