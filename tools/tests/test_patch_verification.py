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
    rows = set()
    for line in LEDGER.read_text().splitlines():
        if not line.startswith('|'):
            continue
        first = line.split('|')[1].strip()
        found = re.fullmatch(r'`([^`]+\.patch)`', first)
        if found:
            rows.add(found.group(1))

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
