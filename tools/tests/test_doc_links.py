#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Every document this repository points at has to be in it.

`test_doc_pairs.py` reads the links between the documents that come in two
languages, and it counts only the names that resolve: a reference to a document
that is not there is not a broken link to it, it is nothing at all. That is the
right rule for telling prose apart from file names ("a bare `pw3.mk` in prose
does not" count), and it means a document that moves takes its incoming links
with it silently.

Which happened: 11 design documents became 7 (2026-10-22, `docs/design/README.md`
says why), and the names of the four that were merged away were written in
thirteen other files, ten of them source comments under `device/`, `boot/` and
`installer/`. A comment that sends the next reader to a file that is not there
is worse than no comment.

So: anything that looks like a path to a document of this repository, written
anywhere this repository keeps text, must exist. The shape it looks for is
deliberately narrow -- a `.md` path with a directory in front of it (`docs/…`,
`design/…`) -- because that is how this repository writes these references and
because a bare `README.md` in prose means whichever one is nearest the reader.
"""
import argparse
import re
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
# A reference with a directory in it: docs/install.md, design/eink-ui.md,
# ../../README.md. Bare names are not paths to anything in particular and are
# left alone. The examples here resolve, because this file is scanned like
# every other -- which is how the first version of it was caught naming a
# document that does not exist.
REFERENCE = re.compile(r'(?<![A-Za-z0-9_./-])((?:\.\./|[A-Za-z0-9_-]+/)+[A-Za-z0-9_.-]+\.md)')
SKIP_SUFFIX = {'.png', '.jpg', '.ttf', '.ttc', '.gz', '.zst', '.xz', '.img', '.bin',
               '.apk', '.jar', '.pyc', '.o', '.so',
               # A record of where evidence was read, not a link to follow:
               # source-lock.json names paths in the build machine's workspace.
               '.json'}
# Paths that belong to something else: AOSP's own tree, and the places a
# reference is a URL rather than a file of this repository.
FOREIGN = ('http://', 'https://', 'external/', 'build/make/', 'frameworks/base/docs/')
# Deliberately outside the repository: the project's research workspace, which a
# clone does not have. Anything written this way says so to its reader, which is
# the point -- `tools/kernel-smoke/network/README.md` used to point at
# `validation/…`, read as a path in the repository and found nowhere.
OUTSIDE = ('work/',
           # The build machine's output directory, where a release's artifacts and
           # its notes are written (`build-share/v72/README.md`). A clone does not
           # have it either, and every document that cites it says what it is.
           'build-share/')


def outside_repository(path, reference):
    """A reference that climbs out of the repository names something a clone has not.

    Notes beside the code point at research folders with `../../../work/…`, and
    on a case-insensitive host those resolved because the workspace is one
    directory up from the clone. They are not links into this repository, and
    whether they resolve says nothing about this repository.
    """
    for candidate in candidates(path, reference):
        try:
            candidate.resolve().relative_to(ROOT.resolve())
        except ValueError:
            continue
        return False
    return True


def candidates(path, reference):
    """Where a reference written in `path` could be pointing.

    A reference that starts with `..` has exactly one reading -- from the file it
    is written in -- and gets only that one. Handing it the prefixes below would
    let `../../../work/x.md` climb out of the repository and back in through one
    of them, which is how this test passed on one host and failed on another.
    """
    if reference.startswith('../'):
        yield ROOT / path.parent / reference
        return
    yield ROOT / reference
    yield ROOT / path.parent / reference
    yield ROOT / 'docs' / reference
    yield ROOT / 'docs/design' / reference
    # How this repository writes a path inside the device directory, which is
    # where most source comments are (`emoji/PW3-SOURCE.md`).
    yield ROOT / 'device/amazon/pw3' / reference


class DocumentLinks(unittest.TestCase):
    def test_every_reference_resolves(self):
        listed = subprocess.run(['git', '-C', str(ROOT), 'ls-files'],
                                capture_output=True, text=True, check=True).stdout.split()
        checked = 0
        broken = []
        for name in listed:
            path = Path(name)
            if path.suffix in SKIP_SUFFIX:
                continue
            full = ROOT / path
            if not full.is_file():
                continue
            try:
                text = full.read_text()
            except (UnicodeDecodeError, OSError):
                continue
            for line_number, line in enumerate(text.splitlines(), 1):
                for reference in REFERENCE.findall(line):
                    if any(mark in line for mark in FOREIGN):
                        continue
                    if reference.startswith(OUTSIDE) or outside_repository(path, reference):
                        continue
                    checked += 1
                    if not any(c.is_file() for c in candidates(path, reference)):
                        broken.append('%s:%d points at %s' % (name, line_number, reference))
        self.assertEqual(broken, [], '\n'.join(broken))
        self.assertGreater(checked, 50, 'found almost no references; the pattern is wrong')
        print('%d references in %d tracked files, 0 broken' % (checked, len(listed)))


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    arguments, rest = parser.parse_known_args()
    unittest.main(argv=[sys.argv[0]] + rest, verbosity=2)


if __name__ == '__main__':
    main()
