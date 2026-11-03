#!/usr/bin/env python3
"""Regression: overlapping incremental patches must never reverse on rebuild."""
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('patches', Path(__file__).parents[1] / 'apply-patch-series.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

spec = importlib.util.spec_from_file_location('series', Path(__file__).parents[1] / 'verify-tree-matches-series.py')
series = importlib.util.module_from_spec(spec)
spec.loader.exec_module(series)

class PatchSeriesTests(unittest.TestCase):
    def test_overlapping_rebuild_and_changed_input(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            source = base / 'source'; source.mkdir()
            (source/'value').write_text('original\n')
            first = base/'0001.patch'
            second = base/'0002.patch'
            first.write_text('--- a/value\n+++ b/value\n@@ -1 +1 @@\n-original\n+first\n')
            second.write_text('--- a/value\n+++ b/value\n@@ -1 +1 @@\n-first\n+second\n')
            module.apply_series(source, [first])
            module.apply_series(source, [first,second])
            module.apply_series(source, [first,second])
            self.assertEqual((source/'value').read_text(), 'second\n')
            first.write_text(first.read_text().replace('+first', '+changed'))
            with self.assertRaises(RuntimeError):
                module.apply_series(source, [first,second])
            self.assertEqual((source/'value').read_text(), 'second\n')

    def test_reordering_is_rejected_before_writes(self):
        with tempfile.TemporaryDirectory() as directory:
            base=Path(directory); source=base/'source'; source.mkdir()
            (source/'value').write_text('original\n')
            first=base/'0001.patch'; second=base/'0002.patch'
            first.write_text('--- a/value\n+++ b/value\n@@ -1 +1 @@\n-original\n+first\n')
            second.write_text('--- a/value\n+++ b/value\n@@ -1 +1 @@\n-first\n+second\n')
            module.apply_series(source,[first,second])
            with self.assertRaises(RuntimeError):
                module.apply_series(source,[second,first])
            self.assertEqual((source/'value').read_text(),'second\n')

PINNED = 'one\ntwo\nthree\n'
WHOLE = ('--- a/file\n+++ b/file\n@@ -1,3 +1,5 @@\n one\n+first addition\n two\n'
         '+second addition\n three\n')
HALF = '--- a/file\n+++ b/file\n@@ -1,3 +1,4 @@\n one\n+first addition\n two\n three\n'
APPLIED = 'one\nfirst addition\ntwo\nsecond addition\nthree\n'
HALF_APPLIED = 'one\nfirst addition\ntwo\nthree\n'


class TreeMatchesSeriesTests(unittest.TestCase):
    """The ledger cannot see a hunk dropped from a patch but left in the tree."""

    def world(self, directory, patch, tree, extra=None):
        base = Path(directory)
        source = base / 'source'
        source.mkdir()
        for command in (['init', '--quiet'], ['config', 'user.email', 'test@example.com'],
                        ['config', 'user.name', 'test']):
            subprocess.run(['git', '-C', str(source)] + command, check=True, capture_output=True)
        (source / 'file').write_text(PINNED)
        subprocess.run(['git', '-C', str(source), 'add', 'file'], check=True, capture_output=True)
        subprocess.run(['git', '-C', str(source), 'commit', '--quiet', '-m', 'pinned'],
                       check=True, capture_output=True)
        (source / 'file').write_text(tree)
        if extra:
            (source / extra).write_text('built, and in no patch\n')
        first = base / '0001.patch'
        first.write_text(patch)
        return series.tree_matches_series(source, [first])

    def test_the_tree_is_the_series(self):
        with tempfile.TemporaryDirectory() as directory:
            self.assertEqual(self.world(directory, WHOLE, APPLIED), [])

    def test_a_hunk_of_the_patch_is_missing_from_the_tree(self):
        with tempfile.TemporaryDirectory() as directory:
            self.assertTrue(self.world(directory, WHOLE, HALF_APPLIED))

    def test_the_patch_lost_a_hunk_the_tree_still_has(self):
        # The applier accepts this: reversing the smaller patch checks only its own
        # hunks. The second addition would be built from no patch at all.
        with tempfile.TemporaryDirectory() as directory:
            self.assertTrue(self.world(directory, HALF, APPLIED))

    def test_a_file_no_patch_mentions(self):
        with tempfile.TemporaryDirectory() as directory:
            self.assertTrue(self.world(directory, WHOLE, APPLIED, extra='extra.c'))

    def test_the_ledger_is_not_a_source_file(self):
        with tempfile.TemporaryDirectory() as directory:
            self.assertEqual(self.world(directory, WHOLE, APPLIED,
                                        extra='.pw3-applied-patches.json'), [])

    def test_a_patch_named_relative_to_the_caller(self):
        # The series is applied in a directory of the check's own.
        with tempfile.TemporaryDirectory() as directory:
            here = os.getcwd()
            os.chdir(directory)
            try:
                self.assertEqual(self.world('.', WHOLE, APPLIED), [])
            finally:
                os.chdir(here)


if __name__ == '__main__': unittest.main()
