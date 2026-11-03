#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""The acceptance script asks the device about names the sources have to still use.

`tools/verify-device.sh` is what gets run on the device after an install, and it
works by looking for particular kernel-log tags and properties. When one of
those is renamed in the C and not here, a device that is working is reported as
broken: that happened once already -- the tag `pw3-boot-report` became
`pw3-report-area` and the script kept grepping the old one.

The other direction is worse and is the reason this file exists at all: a check
that looks for something nothing writes any more passes quietly as "not
applicable" and the thing it was watching goes unwatched. On V66 the background
image verification never ran, and nothing in the script asked, so a run of it
reported a clean device.

So every `sys.pw3.*` property and every `pw3-*` tag the script looks for must be
written by something in the tree. Section numbers are checked too, because they
are renumbered by hand whenever a section is inserted.
"""
import re
import sys
import traceback
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
SCRIPT = ROOT / 'tools/verify-device.sh'
# Where a name the script asks about can legitimately come from.
SOURCES = ['boot', 'device/amazon/pw3', 'installer', 'patches']
DIGITS = '一二三四五六七八九'


def chinese(n):
    """1..99 the way the script's headings are written: 十, 十一, 二十, 二十一.

    This used to be a ten-character string indexed by position, which was
    right until the script reached an eleventh section and the check died of
    an IndexError instead of saying anything about the script (2026-10-23).
    """
    if n < 10:
        return DIGITS[n - 1]
    tens, units = divmod(n, 10)
    return ('' if tens == 1 else DIGITS[tens - 1]) + '十' + (DIGITS[units - 1] if units else '')


checks = 0
failures = []


def check(ok, what):
    global checks
    checks += 1
    if not ok:
        failures.append(what)


def tree_text():
    out = []
    for where in SOURCES:
        for path in (ROOT / where).rglob('*'):
            if not path.is_file() or path.suffix.lower() in ('.png', '.ttf', '.ttc',
                                                             '.apk', '.zst', '.img'):
                continue
            try:
                out.append(path.read_text(encoding='utf-8', errors='ignore'))
            except OSError:
                continue
    return '\n'.join(out)


def main():
    check(SCRIPT.is_file(), '%s is missing' % SCRIPT)
    if not SCRIPT.is_file():
        return report()
    text = SCRIPT.read_text(encoding='utf-8')
    sources = tree_text()
    check(len(sources) > 1_000_000,
          'only %d characters of sources were read; the reader is probably wrong'
          % len(sources))

    properties = sorted(set(re.findall(r'\bsys\.pw3\.[a-z_0-9.]+', text)))
    # Anchored on a name that is in the script today rather than on a count:
    # the script asks about one property, so "at least one" would not show that
    # the reader had stopped matching.
    check('sys.pw3.system_verified' in properties,
          'sys.pw3.system_verified is not among the properties read out of the '
          'script (%s); either the check for the background image verification '
          'is gone or this reader is wrong' % properties)
    for name in properties:
        # setprop in an .rc file, SetProperty in C++, or the constant a program
        # sets it through.
        check(name in sources,
              'the script asks about the property %s and nothing in the tree '
              'writes that name' % name)

    # Whole tokens first, then split by whether the script writes a colon after
    # one: a pattern that classified while matching produced the fragment
    # "pw3-report-" out of "pw3-report-area:".
    tokens = set(re.findall(r'\bpw3-[a-z0-9]+(?:-[a-z0-9]+)*', text))
    tags = sorted(t for t in tokens if t + ':' in text)
    names = sorted(tokens - set(tags))
    check('pw3-report-area' in tags,
          'pw3-report-area is not among the tags read out of the script (%s); '
          'either the check for the report area is gone or this reader is wrong'
          % tags)
    for tag in tags:
        check(tag in sources,
              'the script greps for the tag %s: and nothing in the tree prints '
              'it. A working device would be reported as broken' % tag)
    for name in names:
        check(name in sources,
              'the script names %s and nothing in the tree builds, starts or '
              'writes it' % name)

    headings = re.findall(r'echo "=== ([^、]+)、', text)
    check(len(headings) >= 8, 'only %d numbered sections found' % len(headings))
    expected = [chinese(i + 1) for i in range(len(headings))]
    check(headings == expected,
          'the sections are numbered %s; inserting one means renumbering the '
          'rest by hand, and this is the mistake that makes' % headings)

    print('%d checks, %d failed (%d properties, %d tags, %d sections)'
          % (checks, len(failures), len(properties), len(tags) + len(names), len(headings)))
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
