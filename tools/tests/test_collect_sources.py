#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""collect-copyleft-sources.py: which files need their source, where it is, and that it is what was built."""
import gzip
import importlib.util
import json
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('collect', Path(__file__).parents[1] / 'collect-copyleft-sources.py')
collect = importlib.util.module_from_spec(spec)
spec.loader.exec_module(collect)


def notice(*blocks):
    parts = ['<html><body>']
    for files, text in blocks:
        # As build/make/tools/generate-notice-files.py writes it.
        parts.append('<div class="label">Notices for file(s):</div>\n<div class="file-list">\n%s</div>'
                     '<!-- file-list -->\n\n<pre class="license-text">\n%s</pre><!-- license-text -->'
                     % (''.join(f + ' <br/>\n' for f in files), text))
    return ''.join(parts).encode()


GPL = 'GNU GENERAL PUBLIC LICENSE\n Version 2, June 1991\n'
LGPL = '\n  GNU LESSER GENERAL PUBLIC LICENSE\n Version 2.1\n'
APACHE = 'Apache License\n Version 2.0, January 2004\n'

MODULES = {
    'iptables': {'path': ['external/iptables/iptables'], 'installed': ['out/target/product/pw3/system/bin/iptables']},
    'libnl': {'path': ['external/libnl'], 'installed': []},
    'pw3-kmod-g-ffs': {'path': ['vendor/amazon/pw3-hardware'],
                       'installed': ['out/target/product/pw3/system/vendor/lib/modules/g_ffs.ko']},
    'EinkBro': {'path': ['device/amazon/pw3/apps/EinkBro'],
                'installed': ['out/target/product/pw3/system/app/EinkBro/EinkBro.apk']},
    'gdbserver': {'path': ['prebuilts/misc/android-arm/gdbserver'], 'installed': ['out/target/product/pw3/system/bin/gdbserver']},
    'toybox': {'path': ['external/toybox'], 'installed': ['out/target/product/pw3/system/bin/toybox']},
}
PROJECTS = {'external/iptables': 'a' * 40, 'external/libnl': 'b' * 40, 'external/toybox': 'c' * 40}
UPSTREAMS = {'EinkBro': {'attach': []}}


class Blocks(unittest.TestCase):
    def test_only_copyleft_blocks_with_their_files(self):
        text = notice((['/system/bin/toybox'], APACHE), (['/system/bin/iptables', '/kernel'], GPL),
                      (['/system/lib/libnl.a'], LGPL))
        for data in (text, gzip.compress(text)):
            found = collect.copyleft_blocks(data)
            self.assertEqual(found, [(['/system/bin/iptables', '/kernel'], 'GNU GENERAL PUBLIC LICENSE'),
                                     (['/system/lib/libnl.a'], 'GNU LESSER GENERAL PUBLIC LICENSE')])

    def test_a_notice_without_blocks_is_an_error(self):
        with self.assertRaises(RuntimeError):
            collect.copyleft_blocks(b'<html></html>')


class Trace(unittest.TestCase):
    def test_project_kernel_static_library_and_repository_module(self):
        blocks = [(['/system/bin/iptables', '/kernel', '/system/vendor/lib/modules/g_ffs.ko',
                    '/system/lib/libnl.a', '/system/app/EinkBro/EinkBro.apk'], 'GPL')]
        found = collect.trace(blocks, MODULES, PROJECTS, UPSTREAMS)
        self.assertEqual({k: v['sources'] for k, v in found.items()}, {
            '/system/bin/iptables': [('aosp', 'external/iptables')],
            '/kernel': [('kernel',)],
            '/system/vendor/lib/modules/g_ffs.ko': [('kernel',)],
            '/system/lib/libnl.a': [('aosp', 'external/libnl')],
            '/system/app/EinkBro/EinkBro.apk': [('repository', 'EinkBro')],
        })

    def test_untraceable_files_are_all_named(self):
        blocks = [(['/system/bin/gdbserver', '/system/bin/unknown', '/system/app/EinkBro/EinkBro.apk'], 'GPL')]
        with self.assertRaises(collect.Untraceable) as caught:
            collect.trace(blocks, MODULES, PROJECTS, {})
        message = str(caught.exception)
        self.assertIn('/system/bin/gdbserver: prebuilt', message)
        self.assertIn('/system/bin/unknown: no module installs it', message)
        self.assertIn('module EinkBro of this repository has no entry', message)

    def test_a_directory_outside_every_project(self):
        modules = {'x': {'path': ['vendor/other'], 'installed': ['out/target/product/pw3/system/bin/x']}}
        with self.assertRaises(collect.Untraceable):
            collect.trace([(['/system/bin/x'], 'GPL')], modules, PROJECTS, {})

    def test_the_longest_project_path_owns_a_directory(self):
        projects = {'external/a': '1', 'external/a/b': '2'}
        self.assertEqual(collect.owner('external/a/b/c', projects), 'external/a/b')
        self.assertEqual(collect.owner('external/a/c', projects), 'external/a')
        self.assertIsNone(collect.owner('external/ab', projects))


def git(tree, *args):
    return subprocess.run(['git', '-C', str(tree)] + list(args), check=True, capture_output=True, text=True).stdout


class Stage(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        base = Path(self.temp.name)
        self.aosp, self.stage = base / 'aosp', base / 'stage'
        self.tree = self.aosp / 'external/demo'
        self.tree.mkdir(parents=True)
        self.stage.mkdir()
        git(self.tree, 'init', '-q')
        (self.tree / 'value').write_text('original\n')
        (self.tree / 'tool').write_text('#!/bin/sh\n')
        (self.tree / 'tool').chmod(0o755)
        git(self.tree, 'add', '.')
        git(self.tree, '-c', 'user.name=t', '-c', 'user.email=t@t', 'commit', '-q', '-m', 'base')
        self.revision = git(self.tree, 'rev-parse', 'HEAD').strip()
        self.patch = base / '0001-demo.patch'
        self.patch.write_text('--- a/value\n+++ b/value\n@@ -1 +1 @@\n-original\n+patched\n'
                              '--- /dev/null\n+++ b/added\n@@ -0,0 +1 @@\n+new\n')
        collect.load_patch_tool().apply_series(self.tree, [self.patch])  # the tree as built
        self.built = {'external/demo': [{'name': self.patch.name, 'sha256': collect.sha256(self.patch)}]}

    def tearDown(self):
        self.temp.cleanup()

    def run_stage(self):
        return collect.stage_project(self.aosp, 'external/demo', self.revision, [self.patch], self.built, self.stage)

    def test_revision_plus_patches_matches_the_built_tree(self):
        _, changed = self.run_stage()
        target = self.stage / 'external/demo'
        self.assertEqual(changed, 2)
        self.assertEqual((target / 'value').read_text(), 'patched\n')
        self.assertEqual((target / 'added').read_text(), 'new\n')
        self.assertFalse((target / '.pw3-applied-patches.json').exists())

    def test_a_stray_file_in_the_built_tree(self):
        (self.tree / 'stray.c').write_text('int x;\n')
        with self.assertRaisesRegex(RuntimeError, 'stray.c'):
            self.run_stage()

    def test_a_stray_edit_in_the_built_tree(self):
        (self.tree / 'tool').write_text('#!/bin/sh\necho changed\n')
        with self.assertRaisesRegex(RuntimeError, 'tool'):
            self.run_stage()

    def test_other_patches_than_the_image_was_built_with(self):
        self.built = {'external/demo': []}
        with self.assertRaisesRegex(RuntimeError, 'other patches'):
            self.run_stage()

    def test_another_revision_than_the_manifest(self):
        self.revision = '0' * 40
        with self.assertRaisesRegex(RuntimeError, 'manifest says'):
            self.run_stage()

    def test_reproducible_archive(self):
        mtime, _ = self.run_stage()
        first, second = Path(self.temp.name) / 'a.tar.xz', Path(self.temp.name) / 'b.tar.xz'
        collect.write_tar(first, self.stage, {'external/demo': mtime})
        collect.write_tar(second, self.stage, {'external/demo': mtime})
        self.assertEqual(first.read_bytes(), second.read_bytes())
        with tarfile.open(first) as tar:
            members = {m.name: m for m in tar.getmembers()}
        self.assertEqual(sorted(members), ['external/demo', 'external/demo/added', 'external/demo/tool',
                                           'external/demo/value'])
        self.assertEqual(members['external/demo/tool'].mode, 0o755)
        self.assertEqual(members['external/demo/value'].mode, 0o644)
        self.assertEqual({m.uid for m in members.values()}, {0})
        self.assertEqual({m.mtime for m in members.values()}, {mtime})


if __name__ == '__main__':
    unittest.main()
