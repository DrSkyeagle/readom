#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Every patch that says it has not been run on the device has a row saying so.

A patch header is written the day the patch is, and the series is append-only
(apply-patch-series.py records each file's SHA256), so a header cannot be
corrected later. Ten of this project's patches end with "Not run on the device
yet when this patch was written", and until 2026-11-03 nothing went back to
them: frameworks-base 0008 had stood for twelve days, and the premise it stated
without measuring -- that the previous screen stays until the new application
draws -- was false, which is the defect the owner reported four times.

So the list lives outside the patches, in patches/VERIFICATION.md, and this
checks that the two agree in both directions:

  * a patch that says the sentence and has no row -- a new patch, or one nobody
    wrote down;
  * a row for a patch that does not say it -- a row left behind by a rename, or
    one written for a patch that never claimed to be unverified.

What a row says is not checked here. Whether the device has actually been asked
is a question for a person; this only makes sure the question is on the list.

The patches are looked for one level down, because that is how the series are
laid out (patches/<series>/NNNN-*.patch). A patch dropped straight into
patches/ would be outside every check here, and there is one: the bare
timeconst.pl diff the kernel build needs on a host with a current perl, which
has no header to say anything and is documented in patches/README.md. It is
named below rather than left to the glob, so that the next patch put at the top
level fails this instead of quietly escaping it.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
PATCHES = ROOT / 'patches'
LEDGER = PATCHES / 'VERIFICATION.md'
SENTENCE = 'Not run on the device yet when this patch was written'
# The only patch that is allowed to sit in patches/ itself: see the docstring.
TOP_LEVEL_ALLOWED = {'0001-timeconst-current-perl.patch'}

failures = []


def main():
    if not LEDGER.is_file():
        sys.exit('no %s' % LEDGER.relative_to(ROOT))

    # A patch file is a header followed by the diff. The sentence belongs to the
    # header, and a diff can carry anything, including this file's own text: so
    # only what comes before the first "--- a/" counts.
    says = set()
    patches = sorted(PATCHES.glob('*/*.patch'))
    for path in patches:
        header = path.read_text(errors='replace').split('\n--- a/')[0]
        if SENTENCE in header:
            says.add('%s/%s' % (path.parent.name, path.name))

    # One row per patch, the patch named in backticks in the first column.
    # Collected as a list first: dropping them straight into a set folded two
    # rows for the same patch into one, so a duplicate -- including two rows
    # that say opposite things about the same patch -- read as a tidy ledger
    # (2026-11-03, found by review, reproduced by appending a real row again).
    #
    # The table is read as Markdown shows it, and strictly. Two earlier
    # readings each let rows out of the check: one only saw rows starting in
    # column one with a bare `name` (an indented or linked row sat in the
    # rendered table unread, 2026-11-04, item I239); the next took any line
    # that looked like a row, so rows hidden in an HTML comment or a code
    # block could stand in for the whole ledger, while a row written without
    # its leading bar or with a reference link was still skipped (a Codex
    # review, 2026-11-04). Tracking what Markdown hides is a parser of its
    # own -- a processing instruction, a fence closed by a shorter one, and a
    # second table whose rows need no bar at all each got past the first
    # attempt (the next review, same day). So the ledger is held to a form
    # with nothing to track: no HTML block, no comment and no fence anywhere
    # in it, and exactly one table, ours. Every line of that table up to the
    # blank line that ends it must be a row whose first cell is one
    # backticked series/NNNN-name.patch, or the check fails rather than skip
    # it; any other table delimiter row in the file fails too.
    lines = LEDGER.read_text().splitlines()
    for n, line in enumerate(lines):
        if re.match(r' {0,3}(<|```|~~~)', line) or '<!--' in line:
            failures.append('%s line %d starts an HTML block, a comment or a code fence; the '
                            'ledger is plain Markdown, because those can hide rows from the '
                            'reader while this check still counts them'
                            % (LEDGER.relative_to(ROOT), n + 1))
    heads = [n for n, line in enumerate(lines) if re.match(r' {0,3}\|\s*Patch\s*\|', line)]
    table = set()
    listed = []
    if len(heads) != 1:
        failures.append('%s should have exactly one table headed "| Patch |"; it has %d'
                        % (LEDGER.relative_to(ROOT), len(heads)))
    else:
        n = heads[0]
        table.update((n, n + 1))
        if n + 1 >= len(lines) or not re.match(r' {0,3}\|?\s*:?-{3,}', lines[n + 1]):
            failures.append('the "| Patch |" line in %s is not followed by a table delimiter row'
                            % LEDGER.relative_to(ROOT))
        n += 2
        while n < len(lines) and lines[n].strip():
            table.add(n)
            row = re.fullmatch(r' {0,3}\|\s*`([a-z0-9-]+/[0-9]{4}-[A-Za-z0-9._-]+\.patch)`\s*\|.*',
                               lines[n])
            if row:
                listed.append(row.group(1))
            else:
                failures.append('%s line %d is in the table but is not a row of the form '
                                '"| `series/NNNN-name.patch` | ..."; it would be shown and '
                                'not checked' % (LEDGER.relative_to(ROOT), n + 1))
            n += 1
    for n, line in enumerate(lines):
        if n not in table and '|' in line and '-' in line and re.fullmatch(r'[\s|:-]+', line):
            failures.append('%s line %d is the delimiter row of a second table; rows of a table '
                            'need no bar at all, so anything under it would be shown and not '
                            'checked' % (LEDGER.relative_to(ROOT), n + 1))
    rows = set(listed)
    for name in sorted({n for n in listed if listed.count(n) > 1}):
        failures.append('%s has %d rows in %s; one patch, one row -- two rows can disagree '
                        'and nothing would say so'
                        % (name, listed.count(name), LEDGER.relative_to(ROOT)))

    # Neither side may be empty. With the patch directories gone and the ledger
    # down to its heading, this printed "0 patches checked … 0 rows" and exited
    # 0 -- the shape this whole file exists to catch, in the file itself
    # (2026-11-03, found by review).
    if not patches:
        failures.append('no patch files were found under %s at all, so this check compared '
                        'two empty sets' % PATCHES.relative_to(ROOT))
    if not listed:
        failures.append('%s has no row naming a patch, so this check compared two empty sets'
                        % LEDGER.relative_to(ROOT))
    # And the top-level allowance is for a file that is there. The count was
    # printed from the size of the set, which says nothing about the tree.
    for name in sorted(TOP_LEVEL_ALLOWED):
        if not (PATCHES / name).is_file():
            failures.append('TOP_LEVEL_ALLOWED names patches/%s and it is not there; the '
                            'allowance is covering nothing' % name)

    stray = sorted(p.name for p in PATCHES.glob('*.patch')
                   if p.name not in TOP_LEVEL_ALLOWED)
    for name in stray:
        failures.append('patches/%s is at the top level, where this check does not look for '
                        'the sentence or for a row; move it into a series, or add it to '
                        'TOP_LEVEL_ALLOWED and say why' % name)

    for name in sorted(says - rows):
        failures.append('%s says it was not run on the device and has no row in %s'
                        % (name, LEDGER.relative_to(ROOT)))
    for name in sorted(rows - says):
        failures.append('%s has a row in %s but does not say it was not run on the device'
                        % (name, LEDGER.relative_to(ROOT)))

    if failures:
        for line in failures:
            print('FAILED ' + line)
        print('%d patch(es) checked, %d failed' % (len(patches), len(failures)))
        return 1
    print('%d patches checked (+%d named at the top level), %d say they were not run on the '
          'device, %d rows' % (len(patches), len(TOP_LEVEL_ALLOWED), len(says), len(rows)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
