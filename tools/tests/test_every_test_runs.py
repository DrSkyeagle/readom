#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Every test in this tree is run by something, and that something is named.

A test nobody runs is worse than no test: it is a green suite with a hole in
it. This repository had five of them at once on 2026-10-23.

  * device/amazon/pw3/epdc/tests/panel_test.cpp and six others beside it were
    built only as device modules (device/amazon/pw3/tests/paneltest), so they
    ran when somebody asked and otherwise never. panel_test.cpp had been
    asserting the shipped refresh policy's fullEvery == 30 since the reader
    chose 20 on 2026-10-22, and the suite was green the whole time.
  * device/amazon/pw3/health/tests/low_battery_test.cpp says in its own header
    how to build it by hand, and nothing in the repository did.
  * tools/kernel-hardware/tests/gate_test.cpp was built only by that
    directory's own run_tests.py, which needs macOS (`xcrun`) and a kernel
    build directory, so it went unbuilt wherever either was missing.

So this walks the tree for test files and insists each one is either named in
tools/run-tests.py or listed below with what runs it. Adding a test and not
running it then fails here, which is the only moment anybody is looking.
"""
import ast
import sys
import tokenize
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SUITE = ROOT / 'tools/run-tests.py'
# A test file is one named like a test. Build inputs that happen to sit in a
# tests/ directory are not: they are listed in ELSEWHERE if they look like one.
PATTERNS = ('test_*.py', '*_test.py', 'test_*.c', 'test_*.cpp', '*_test.c', '*_test.cpp',
            'test-*.c', 'test-*.cpp')
SKIP_DIRS = {'.git', 'out', 'work', 'private', '__pycache__'}

# Run by something other than the suite, and by what. A fixture that is
# compiled *into* another test's run belongs here, not in the suite: running it
# on the host would prove nothing about the machine it is written for.
ELSEWHERE = {
    'installer/install/test_driver.c':
        'the driver installer/install/test_install.py compiles against the installer, which the '
        'suite runs as installer.',
    'installer/install/test_fatcheck.c':
        'the driver installer/install/test_fatcheck.py compiles against fatcheck.c, which the '
        'suite runs as installer-fatcheck.',
    'installer/install/test_screen.c':
        'the driver installer/install/test_screen.py compiles against the screen renderer, which '
        'the suite runs as installer-screens.',
    'boot/handoff/tests/test-init.c':
        'PID 1 of the QEMU fixture; built and booted by boot/handoff/tests/run_qemu.py '
        'and run_client_test.py, which the suite runs as handoff-in-qemu and handoff-client.',
    'boot/handoff/tests/test-engine-guard.c':
        'a payload for the QEMU fixture; built by boot/handoff/tests/run_qemu.py.',
    'boot/handoff/tests/test-identity-attrs.c':
        'a payload for the QEMU fixture; built by boot/handoff/tests/run_qemu.py.',
}


def test_files():
    for pattern in PATTERNS:
        for path in ROOT.rglob(pattern):
            if any(part in SKIP_DIRS for part in path.relative_to(ROOT).parts):
                continue
            if path.is_file():
                yield path.relative_to(ROOT)


def suite_paths():
    """Every path the suite's source names, taken from its string literals only.

    Not a search of the whole file for a basename, which was two holes at once
    (Codex 2026-10-31): a comment explaining why a test exists counted as
    running it, so deleting the registration and leaving the comment kept this
    green; and a basename matches wherever it appears, so two tests of the same
    name in different directories vouched for each other. A comment cannot
    register anything, and a registration is always a whole path.
    """
    named = set()
    with SUITE.open('rb') as f:
        for token in tokenize.tokenize(f.readline):
            if token.type == tokenize.STRING:
                try:
                    value = ast.literal_eval(token.string)
                except (ValueError, SyntaxError):
                    continue
                if isinstance(value, str) and '/' in value:
                    # Sliced, not lstrip('-I'): lstrip takes a set of
                    # characters, so it would eat a path that itself
                    # began with one of them.
                    named.add(value[2:] if value.startswith('-I') else value)
    return named


def main():
    named = suite_paths()
    missing, accounted, elsewhere_used = [], 0, set()
    for relative in sorted(set(test_files())):
        if str(relative) in named:
            accounted += 1
            continue
        if str(relative) in ELSEWHERE:
            accounted += 1
            elsewhere_used.add(str(relative))
            continue
        missing.append(str(relative))

    stale = sorted(set(ELSEWHERE) - elsewhere_used)
    for entry in stale:
        missing.append('%s is listed as run elsewhere and is not in the tree any more' % entry)
    # This file is itself a test, and the check above counts it: a pass with no
    # files found would mean the walk is broken, not that the tree is clean.
    if accounted < 20:
        missing.append('only %d test files found; the walk is probably wrong' % accounted)

    if missing:
        print('tests nothing runs:')
        for item in missing:
            print('   ', item)
        print('\nEither add it to tools/run-tests.py, or add it to ELSEWHERE in this file '
              'with what runs it.')
        return 1
    print('%d test files, each run by the suite or by something named here' % accounted)
    return 0


if __name__ == '__main__':
    sys.exit(main())
