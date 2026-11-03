#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Copy the files that go to the public repository into a clean folder.

The plan carried this as a checklist of directory names and said so itself:
"这张表是说明，不是工具". A checklist is read once, by somebody doing a thing
that happens once and cannot be taken back -- and the checklist's own warning is
that `git ls-files` loses the files nobody ever committed (56 of them today, 33
when that warning was written). So the list lives here instead, where it can be
run, and where what it refused to copy is printed rather than assumed.

What this does NOT do: create a repository, commit, or push. It produces a
folder and a report; a person reads the report and decides.

    tools/export-open-source.py --output ../readom-export-YYYYMMDD

Checks it runs, each of which can fail:
  * every name on the deny list is absent from the output (the session notes,
    the working directories, the old history, the editor and interpreter
    leavings);
  * no file carries private device data -- tools/check-private-data.py over
    THIS TREE, not over the output, because both its deny list and its
    allow list are paths relative to this tree and neither matches anything
    under another root (see the comment at the call);
  * every file in the output is a file of this tree, byte for byte, which is
    what carries the check above over to the output;
  * every path the include list names exists -- a renamed directory would
    otherwise be silently dropped;
  * every file the check reads that the include list lets through is in the
    output, asked of git twice over rather than of this file's own deny list;
  * the licence files the REUSE manifest refers to are present.
"""
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Whole directories and single files, copied as they are. Directories are
# copied entire: that is the point, see the module docstring.
INCLUDE = [
    '.github', '.gitignore',
    'CHANGELOG.md', 'CHANGELOG.en.md',
    'CODE_OF_CONDUCT.md', 'CODE_OF_CONDUCT.en.md',
    'CONTRIBUTING.md', 'CONTRIBUTING.en.md',
    'LICENSE', 'LICENSES',
    'README.md', 'README.en.md',
    'REUSE.toml',
    'SECURITY.md', 'SECURITY.en.md',
    'THIRD_PARTY.md', 'THIRD_PARTY.en.md',
    'boot', 'config', 'device', 'docs', 'installer', 'patches',
    'source-lock.json', 'tools',
]

# Never, at any depth. The first four are this project's own notes and
# scratch space; the rest is what an editor, an interpreter or a previous
# clone leaves behind.
DENY_NAMES = {
    'private', 'work', 'HANDOVER.md', 'RELEASE-PLAN.md',
    '.git', '.DS_Store', '__pycache__', 'node_modules', '.wrangler',
}
DENY_SUFFIXES = {'.pyc', '.orig', '.rej'}


def git_ignored():
    """What git leaves out of this tree, as paths relative to it.

    Copying whole directories is what keeps the files nobody ever committed --
    that is the point of this script. It also picks up the ones git is told to
    ignore, and the first run found two: the EinkBro and SystemWebView APKs,
    binaries that tools/fetch-prebuilts.py downloads at build time. They would
    have gone to the public repository, and -- this is the part that matters --
    tools/check-private-data.py would never have looked at them, because what it
    checks is "the files git tracks or would add". So the ignore list is read
    and applied here (2026-11-03).
    """
    run = subprocess.run(['git', '-C', str(ROOT), 'ls-files', '--others', '--ignored',
                          '--exclude-standard', '-z'], capture_output=True, text=True)
    if run.returncode:
        sys.exit('cannot ask git what it ignores: ' + run.stderr)
    return {name for name in run.stdout.split('\0') if name}


IGNORED = None


def copy_tree(source, target):
    """Copy, leaving out the deny list and what git ignores; say what was left."""
    skipped = []

    def ignore(directory, names):
        out = []
        here = Path(directory)
        for name in names:
            relative = str((here / name).relative_to(ROOT))
            if (name in DENY_NAMES or Path(name).suffix in DENY_SUFFIXES
                    or relative in IGNORED):
                out.append(name)
                skipped.append(relative)
        return out

    if source.is_dir():
        shutil.copytree(source, target, ignore=ignore, symlinks=True)
    else:
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
    return skipped


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--output', required=True, help='an empty or non-existent directory')
    args = parser.parse_args()

    if os.geteuid() == 0:
        sys.exit('Refusing to run as root: the files it writes are the ones a person commits.')

    output = Path(args.output).resolve()
    if output.exists() and any(output.iterdir()):
        sys.exit('%s exists and is not empty. Give a fresh directory.' % output)
    if ROOT in output.parents or output == ROOT:
        sys.exit('The output would be inside the tree being exported: %s' % output)

    missing = [name for name in INCLUDE if not (ROOT / name).exists()]
    if missing:
        sys.exit('The include list names things this tree does not have, so they would be '
                 'dropped without a word: ' + ', '.join(missing))

    global IGNORED
    IGNORED = git_ignored()

    output.mkdir(parents=True, exist_ok=True)
    skipped = []
    for name in INCLUDE:
        if name in IGNORED:
            sys.exit('the include list names %s and git ignores it' % name)
        skipped += copy_tree(ROOT / name, output / name)

    files = [p for p in output.rglob('*') if p.is_file()]
    print('%d files copied into %s' % (len(files), output))
    if skipped:
        print('left out (%d):' % len(skipped))
        for name in sorted(set(skipped)):
            print('  ' + name)

    failures = []
    for bad in sorted(DENY_NAMES):
        found = [str(p.relative_to(output)) for p in output.rglob(bad)]
        if found:
            failures.append('%s is in the output: %s' % (bad, ', '.join(found[:3])))

    # REUSE names its licence files; a missing one turns `reuse lint` red on the
    # day, which is the wrong day to find out.
    #
    # Read every SPDX-License-Identifier value and take the expression apart,
    # because the ones that matter here are compound: "Apache-2.0 AND ISC",
    # "BSD-2-Clause AND BSD-3-Clause AND BSD-4-Clause-UC AND SunPro AND
    # Apache-2.0". The first version of this matched `"([A-Za-z0-9.+-]+)"`,
    # which a space ends, so it asked for none of the five ids inside those two
    # and then compared what was left against a hardcoded list of seven --
    # five of the fifteen licences in LICENSES/ could have gone missing without
    # a word (2026-11-03).
    manifest = (output / 'REUSE.toml')
    if manifest.is_file():
        import re
        wanted = set()
        for expression in re.findall(r'SPDX-License-Identifier *= *"([^"]+)"',
                                     manifest.read_text()):
            for word in re.split(r'[()\s]+', expression):
                if word and word.upper() not in {'AND', 'OR', 'WITH'}:
                    wanted.add(word.rstrip('+'))
        have = {p.stem for p in (output / 'LICENSES').glob('*.txt')}
        if not wanted:
            failures.append('REUSE.toml named no licence at all, so this check asked for '
                            'nothing; the manifest or this pattern has changed')
        for name in sorted(wanted - have):
            failures.append('REUSE.toml names %s and LICENSES/%s.txt is missing'
                            % (name, name))
        if wanted and not wanted - have:
            print('all %d licence(s) REUSE.toml names are in LICENSES/' % len(wanted))

    # The private data check runs on THIS tree, not on the output, and the
    # output is then shown to be a faithful copy of it. Two reasons, both found
    # by running it the other way round first (2026-11-03):
    #
    #   * its allow list, tools/private-data-allow.txt, is written in paths
    #     relative to this tree -- an upstream kernel patch's author address,
    #     the made-up hardware addresses a launcher test uses. Handed the same
    #     files under another path it matches none of them and reports 65
    #     findings that are all known. (Those addresses are not written out
    #     here either: this file is one of the files that check scans, and a
    #     comment naming one is a finding of its own -- 2026-11-03.)
    #   * its denylist -- this developer's own serial number, MAC and network
    #     name -- is private/denylist.json, which is on the deny list above and
    #     so is not in the output. A check started inside the export runs the
    #     generic patterns only and comes back green without ever having looked
    #     for the values it exists to catch.
    #
    # So: check the tree (where both lists apply), then prove the output adds
    # nothing to it. The counts are read back and a zero in either is a failure,
    # because "0 files checked, 0 denylist entries, 0 findings" is this check's
    # ordinary way of being switched off.
    check = ROOT / 'tools' / 'check-private-data.py'
    if check.is_file():
        run = subprocess.run([sys.executable, str(check)],
                             capture_output=True, text=True, cwd=str(ROOT))
        text = run.stdout + run.stderr
        import re as _re
        summary = next((line for line in text.splitlines() if 'files checked' in line), '(no summary)')
        print('private data check on the tree: ' + summary.strip())
        if run.returncode:
            failures.append('the private data check failed on this tree:\n' + text)
        entries = _re.search(r'(\d+) denylist entries', summary)
        checked = _re.search(r'(\d+) files checked', summary)
        if not entries or entries.group(1) == '0':
            failures.append('the private data check ran with no denylist entries, so it never '
                            "looked for this developer's own values: " + summary.strip())
        if not checked or checked.group(1) == '0':
            failures.append('the private data check looked at no files: ' + summary.strip())

    # The output adds nothing to the tree: every file in it is a file of this
    # tree, byte for byte. Without this the check above says nothing about what
    # was copied.
    import filecmp
    differs = []
    for path in files:
        original = ROOT / path.relative_to(output)
        if not original.is_file() or not filecmp.cmp(str(original), str(path), shallow=False):
            differs.append(str(path.relative_to(output)))
    if differs:
        failures.append('the output has %d file(s) that are not this tree\'s: %s'
                        % (len(differs), ', '.join(differs[:5])))
    else:
        print('every one of the %d files is this tree\'s, byte for byte' % len(files))

    # And every file in it is one the check above actually looked at.
    #
    # Asked the obvious way -- "is this exported path in IGNORED?" -- this
    # check is dead, because IGNORED being empty or wrong is the failure it
    # exists to catch, and an empty IGNORED makes it answer no for everything.
    # A mutation run proved exactly that on 2026-11-03: with git_ignored()
    # stubbed to return nothing, both APKs were copied and this said nothing.
    #
    # So it is asked from the other side, and from git rather than from this
    # script's own idea of things: the set the private data check reads is "the
    # files git tracks or would add", which is `git ls-files` plus
    # `git ls-files --others --exclude-standard`. An exported file outside that
    # set was never read, whatever IGNORED happens to contain.
    seen = set()
    for argv in (['ls-files', '-z'], ['ls-files', '--others', '--exclude-standard', '-z']):
        run = subprocess.run(['git', '-C', str(ROOT)] + argv, capture_output=True, text=True)
        if run.returncode:
            failures.append('cannot ask git what it would publish: ' + run.stderr)
            break
        seen |= {name for name in run.stdout.split('\0') if name}
    if seen:
        unseen = sorted(str(p.relative_to(output)) for p in files
                        if str(p.relative_to(output)) not in seen)
        if unseen:
            failures.append('%d exported file(s) are outside what git tracks or would add, so '
                            'the private data check never read them: %s'
                            % (len(unseen), ', '.join(unseen[:5])))
        else:
            print('all %d are inside what the private data check reads' % len(files))

    if failures:
        print('\nFAILED')
        for line in failures:
            print('  ' + line)
        return 1
    print('\nok: nothing on the deny list reached the output')
    return 0


if __name__ == '__main__':
    sys.exit(main())
