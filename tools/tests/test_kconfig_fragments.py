#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""The kernel configuration is split in two, and the split has to stay honest.

`config/android-core.config` says what this port needs of any kernel it runs on;
`config/models/<model>.config` says what is true of one machine. The split exists
because the one list silently produced a kernel for another Kindle that called
itself `-lab126-pw3-android81` and asked for a Wi-Fi chip that board does not
have (docs/design/porting-to-other-models.md section 6).

Nothing in a build notices either half going wrong:

  * an option asked for in both halves takes whichever value the argument order
    gives it, and the reason for it is written in two places. `merge-kconfig.py`
    refuses that now, and the refusal is only as good as the tool being the one
    the build calls, with every fragment passed to it -- which is what this file
    reads `build-kernel.sh` for;
  * an option asked for twice inside one half is the same defect with one file
    instead of two. There were eight such lines when the split was made
    (`CONFIG_PM_SLEEP` three times), all with the same value, so nothing was
    wrong with the result and nothing would have said when one of them changed;
  * a model this repository knows about with no fragment of its own would fall
    back to a missing file, and `--check` would then pass having verified only
    the base. The models the build script names and the fragments on disk have
    to be the same set.

It does not read the kernel tree: whether an option survives Kconfig is what
`merge-kconfig.py --check` answers during a build, on the machine that has the
sources. This is about the two files agreeing before that.
"""
import argparse
import importlib.util
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BASE = ROOT / 'config/android-core.config'
MODELS = ROOT / 'config/models'
BUILD = ROOT / 'tools/build-kernel.sh'
MERGE = ROOT / 'tools/merge-kconfig.py'
SETTING = re.compile(r'(CONFIG_[A-Z0-9_]+)=(.*)|# (CONFIG_[A-Z0-9_]+) is not set')


def names(lines):
    """Every option these lines name, in order, with repeats kept."""
    return [match.group(1) or match.group(3)
            for match in map(SETTING.fullmatch, lines) if match]


def options(path):
    """Every option a fragment asks for, in order, with repeats kept."""
    return names(path.read_text().splitlines())


class KconfigFragments(unittest.TestCase):
    def test_no_option_is_asked_for_twice_in_one_fragment(self):
        for path in [BASE] + sorted(MODELS.glob('*.config')):
            found = options(path)
            repeated = sorted({name for name in found if found.count(name) > 1})
            self.assertEqual(repeated, [], '%s asks for %s more than once'
                             % (path.name, ', '.join(repeated)))

    def test_a_model_never_overrides_the_base(self):
        """The model's half and the base's half are disjoint, not layered."""
        base = set(options(BASE))
        for path in sorted(MODELS.glob('*.config')):
            both = sorted(base.intersection(options(path)))
            self.assertEqual(both, [], '%s and the base both ask for %s'
                             % (path.name, ', '.join(both)))

    def test_every_model_the_build_knows_has_a_fragment(self):
        text = BUILD.read_text()
        case = re.search(r'case "\$MODEL" in(.*?)esac', text, re.S)
        self.assertIsNotNone(case, 'build-kernel.sh no longer chooses a model')
        named = set(re.findall(r'^\s*([a-z0-9]+)\)\s*DEFCONFIG=', case.group(1), re.M))
        self.assertTrue(named, 'no model in build-kernel.sh names a defconfig')
        self.assertEqual(named, {p.stem for p in MODELS.glob('*.config')},
                         'the models in build-kernel.sh and the fragments in '
                         'config/models are not the same set')
        for name in named:
            self.assertIn('config/models/$MODEL.config', text,
                          'the build does not read the model fragment')

    def test_every_caller_passes_both_halves(self):
        """A call that forgets one half checks or merges the wrong thing.

        Every script, not only the kernel build: `prepare-kernel-hardware.sh`
        runs on every AOSP build and checked the base alone, which verifies 144
        of the 147 options and says nothing about the three that are facts about
        the machine -- `CONFIG_LOCALVERSION` among them, which names the
        directory those modules are loaded from.
        """
        checked = 0
        for script in sorted((ROOT / 'tools').glob('*.sh')):
            text = script.read_text().replace('\\\n', ' ')
            for line in text.splitlines():
                if 'merge-kconfig.py' not in line or 'android-core.config' not in line:
                    continue
                checked += 1
                # Either spelling of the model's half: the path, or the variable
                # the kernel build keeps it in.
                self.assertRegex(line, r'config/models/\$?\{?MODEL|MODEL_CONFIG',
                                 '%s asks only for the base fragment: %s'
                                 % (script.name, line.strip()))
        self.assertGreater(checked, 1, 'found almost no calls; the pattern is wrong')

    def test_the_tool_refuses_an_option_asked_for_twice(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            config = work / '.config'
            config.write_text('CONFIG_A=y\n')
            one = work / 'one.config'
            two = work / 'two.config'
            one.write_text('CONFIG_B=y\n')
            two.write_text('CONFIG_B=n\nCONFIG_C=y\n')
            result = subprocess.run([sys.executable, str(MERGE), str(config), str(one), str(two)],
                                    capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0, 'two fragments asking for CONFIG_B passed')
            self.assertIn('CONFIG_B', result.stderr + result.stdout)

    def test_the_tool_merges_and_checks_every_fragment(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            config = work / '.config'
            config.write_text('CONFIG_A=y\n# CONFIG_B is not set\n')
            one = work / 'one.config'
            two = work / 'two.config'
            one.write_text('CONFIG_B=y\n')
            two.write_text('CONFIG_C=m\n')
            for arguments in ([str(config), str(one), str(two)],
                              ['--check', str(config), str(one), str(two)]):
                result = subprocess.run([sys.executable, str(MERGE)] + arguments,
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('CONFIG_B=y', config.read_text())
            self.assertIn('CONFIG_C=m', config.read_text())
            # And a value Kconfig did not keep is reported, not passed over.
            config.write_text('CONFIG_B=y\n')
            result = subprocess.run([sys.executable, str(MERGE), '--check',
                                     str(config), str(one), str(two)],
                                    capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0, 'a dropped option was not reported')
            self.assertIn('CONFIG_C', result.stderr + result.stdout)


GENERATOR = ROOT / 'tools/make-test-kernel-config.py'
TEST_KERNEL = ROOT / 'config/test-kernel'
TEST_KERNEL_SCRIPTS = {'smoke': ROOT / 'tools/build-kernel-smoke.sh',
                       'loader-stage': ROOT / 'tools/build-loader-stage-kernel.sh'}
# The one option two of these files are allowed to disagree about, and which
# way it goes. The network suite's requirement list says the test kernel needs
# no modules; the loader-stage kernel is the one that loads the handoff as a
# module, so it says it does.
EXPECTED_OVERRIDES = {'smoke': set(), 'loader-stage': {'CONFIG_MODULES'}}


def generator():
    spec = importlib.util.spec_from_file_location('pw3_make_test_kernel_config', GENERATOR)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class TestKernelConfigurations(unittest.TestCase):
    """The two QEMU test kernels are assembled from these files, not by hand.

    They were assembled by hand -- a heredoc inside `build-kernel-smoke.sh`
    appending three lists to each other, last one wins -- and the result said
    the test kernel had no network devices while the network suite's own list,
    appended after it, said it had. The kernel had them, nothing reported the
    contradiction, and the loader-stage kernel the handoff tests need was built
    by hand once from a copy of all this and could not be built again.
    """

    def setUp(self):
        self.made = generator()

    def test_each_option_is_asked_for_once(self):
        """What `merge-kconfig.py` is handed has to have one value per option."""
        for profile in sorted(self.made.PROFILES):
            chosen, _, _, _ = self.made.assemble(profile)
            found = names(self.made.render(profile, chosen, {}, {}).splitlines())
            repeated = sorted({name for name in found if found.count(name) > 1})
            self.assertEqual(repeated, [], '%s asks for %s twice' % (profile, repeated))
            self.assertEqual(len(found), len(chosen))

    def test_only_the_declared_overrides_happen(self):
        for profile, expected in EXPECTED_OVERRIDES.items():
            _, _, replaced, _ = self.made.assemble(profile)
            self.assertEqual(set(replaced), expected,
                             '%s: an option is declared twice with different values and '
                             'nobody said it should be' % profile)

    def test_the_profiles_differ_only_where_they_declare_it(self):
        smoke, _, _, _ = self.made.assemble('smoke')
        loader, _, _, _ = self.made.assemble('loader-stage')
        declared = set()
        for profile in self.made.PROFILES.values():
            declared.update(options(ROOT / profile))
        differ = {key for key in set(smoke) | set(loader) if smoke.get(key) != loader.get(key)}
        self.assertEqual(differ - declared, set(),
                         'the two test kernels differ in options neither profile declares')

    def test_no_kindle_hardware_reaches_a_test_kernel(self):
        """A vexpress-a9 has none of it, and Kconfig should answer for the board
        it was given rather than be told `n` for a driver that cannot be set."""
        device = {match.group(1) or match.group(3)
                  for match in filter(None, map(SETTING.fullmatch, BASE.read_text().splitlines()))
                  if self.made.HARDWARE.search(match.group(1) or match.group(3))}
        self.assertTrue(device, 'the hardware pattern matches nothing in the base fragment')
        for profile in sorted(self.made.PROFILES):
            chosen, _, _, _ = self.made.assemble(profile)
            self.assertEqual(sorted(device.intersection(chosen)), [],
                             '%s asks for the device\'s own drivers' % profile)

    def test_every_test_kernel_build_uses_the_generator(self):
        for profile, script in TEST_KERNEL_SCRIPTS.items():
            text = script.read_text()
            self.assertIn('make-test-kernel-config.py --profile %s' % profile, text,
                          '%s does not generate its configuration' % script.name)
            # Naming the base fragment in a comment is how the script explains
            # itself; reading it is how the heredoc came back.
            code = '\n'.join(line for line in text.splitlines()
                             if not line.lstrip().startswith('#'))
            self.assertNotIn('android-core.config', code,
                             '%s assembles a configuration of its own again' % script.name)
            self.assertIn('merge-kconfig.py --check', text,
                          '%s does not check that Kconfig kept what it asked for' % script.name)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    arguments, rest = parser.parse_known_args()
    unittest.main(argv=[sys.argv[0]] + rest, verbosity=2)


if __name__ == '__main__':
    main()
