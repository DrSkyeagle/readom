#!/usr/bin/env python3
"""Check that a source tree is exactly its pinned revision plus its patch series.

apply-patch-series.py records which patches it applied and their hashes, and
export-aosp-images.py refuses an export whose ledger is not the series on disk.
Neither sees the case that matters most for sources other people rebuild: a hunk
taken out of a patch stays in the tree. The ledger entry can be re-recorded,
`git apply --reverse --check` of the smaller patch still passes -- it only looks
at the hunks the patch still has -- and the image then carries code no patch
describes. Whoever takes the published sources builds something else, and the
receipts say the two agree.

The check here is the one that holds: copy out of the tree every file the series
touches, reverse-apply the whole series to the copies in reverse order, and
compare each with the pinned revision. Whatever is left is a change the series
does not describe. Files the tree holds that no patch mentions are reported too,
since they are built into the image just the same. The tree is never written.

collect-copyleft-sources.py makes the same guarantee at release time, from the
other side: it extracts the whole project at its revision, applies the series,
and compares. That one is thorough and costs an archive of every project, which
is what a release wants; this one costs the files the series touches, which is
what every build can afford. Keep both honest.

Usage: verify-tree-matches-series.py SOURCE PATCHES   (both as in the build VM)
"""
import argparse
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

# Text patches in the series carry no "diff --git" line, binary ones carry nothing else.
FILE_LINE = re.compile(r'^(?:(?:---|\+\+\+) [ab]/(\S+)|diff --git a/(\S+) b/\S+)$', re.M)
# The ledger is the applier's own record, not a source file.
NOT_SOURCE = ('.pw3-applied-patches.json', '.pw3-applied-patches.tmp')


def files_of(patches):
    """Every path the series touches, in the order the series first names it."""
    touched = []
    for patch in patches:
        for found in FILE_LINE.findall(patch.read_text(errors='replace')):
            path = found[0] or found[1]
            if path != 'dev/null' and path not in touched:
                touched.append(path)
    return touched


def tree_matches_series(source, patches):
    """Differences between `source` and its pinned revision plus `patches`, as text."""
    if not patches:
        return ['no patches for ' + str(source)]
    # The series is applied from a directory of its own below, where a path
    # relative to the caller's would not be found.
    patches = [patch.resolve() for patch in patches]
    touched = files_of(patches)
    problems = []
    with tempfile.TemporaryDirectory(prefix='pw3-series-check-') as temporary:
        work = Path(temporary)
        for path in touched:
            original = source / path
            if original.is_file():
                copy = work / path
                copy.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(original, copy)
        for patch in reversed(patches):
            undo = subprocess.run(['git', 'apply', '--reverse', '--whitespace=nowarn', str(patch)],
                                  cwd=work, capture_output=True, text=True)
            if undo.returncode:
                return ['%s does not come back out of the tree: %s'
                        % (patch.name, undo.stderr.strip().replace('\n', '; '))]
        for path in touched:
            pinned = subprocess.run(['git', '-C', str(source), 'show', 'HEAD:' + path],
                                    capture_output=True)
            copy = work / path
            here = copy.read_bytes() if copy.is_file() else b''
            if pinned.returncode and here:
                problems.append('%s: added by the series, and something of it is left' % path)
            elif not pinned.returncode and here != pinned.stdout:
                problems.append('%s: not what the series makes of it' % path)
    # Every untracked file by name: the usual listing collapses a new directory
    # to its name, which no patch path can match.
    listing = subprocess.run(['git', '-C', str(source), 'status', '--porcelain', '--untracked-files=all'],
                             capture_output=True, text=True, check=True)
    for line in listing.stdout.splitlines():
        path = line[3:].strip('"')
        if path not in touched and path not in NOT_SOURCE:
            problems.append('%s: changed, and in no patch' % path)
    return problems


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path, help='the AOSP source tree')
    parser.add_argument('patches', type=Path, help='the patches directory, holding projects.txt')
    args = parser.parse_args()
    good = True
    for line in (args.patches / 'projects.txt').read_text().splitlines():
        line = line.split('#')[0].strip()
        if not line:
            continue
        project, series = line.split()
        patches = sorted((args.patches / series).glob('*.patch'))
        problems = tree_matches_series(args.source / project, patches)
        if problems:
            good = False
            print('%s: not its pinned revision plus the series' % project)
            for problem in problems:
                print('  ' + problem)
        else:
            print('%s: %d patches, %d files, the pinned revision plus the series'
                  % (project, len(patches), len(files_of(patches))))
    return 0 if good else 1


if __name__ == '__main__':
    raise SystemExit(main())
