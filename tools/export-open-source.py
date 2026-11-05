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
import stat
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

    # A link named at the top of the include list never reaches the output as a
    # link: copy2() follows one by default, and copytree() given one starts
    # enumerating at its target, so what lands in the output is an ordinary file
    # or an ordinary directory and the walk below has nothing left to find. So
    # the source is asked before anything is copied (2026-11-03, found by
    # review: the first version of the guard looked only at the output and this
    # went straight past it).
    for name in INCLUDE:
        source = ROOT / name
        if source.is_symlink():
            sys.exit('the include list names %s and it is a symbolic link to %s; copying it '
                     'would dereference it, so what gets published is that target\'s content '
                     'under this name' % (name, os.readlink(source)))

    output.mkdir(parents=True, exist_ok=True)
    skipped = []
    for name in INCLUDE:
        if name in IGNORED:
            sys.exit('the include list names %s and git ignores it' % name)
        skipped += copy_tree(ROOT / name, output / name)

    # One walk, and it has to say that it finished.
    #
    # Everything below asks its question about `files`, so a subtree that was
    # never enumerated is a subtree no check reads. Both of the ways this was
    # written before answer an unreadable directory the same way they answer an
    # empty one -- measured on this host, 2026-11-03: with one subdirectory
    # chmod 000, `os.walk(output, followlinks=False)` yields it with an empty
    # file list and `rglob('*')` names the directory and nothing inside it,
    # neither raising. "No symbolic links here" and "this subtree was not read"
    # were the same empty list, which is the defect this guard was added to
    # prevent, in the guard itself. `onerror` turns it into an exception.
    #
    # The links matter because copytree(symlinks=True) copies a link as a link,
    # and a link is not a file: it enters none of the checks below -- not the
    # byte-for-byte comparison, not the private data scan, not the "inside what
    # git publishes" set -- while what would be published is its text, which can
    # name a path outside this tree. There is no symlink inside the include list
    # today; this is here so that the day somebody adds one is not the day it
    # leaves quietly.
    def stop(error):
        raise error

    # The same goes for each entry's type. is_symlink() and is_file() answer
    # False for an entry they cannot stat, so an entry the walk named but whose
    # type could not be read was neither, and entered no check (2026-11-03
    # Codex review, item 9). lstat raises instead, and anything that is not a
    # directory, a file or a link -- a fifo, a socket -- is refused by name.
    entries = []
    files = []
    links = []
    others = []
    for directory, subdirs, names in os.walk(output, followlinks=False, onerror=stop):
        for name in subdirs + names:
            path = Path(directory) / name
            entries.append(path)
            mode = os.lstat(path).st_mode
            if stat.S_ISLNK(mode):
                links.append('%s -> %s' % (path.relative_to(output), os.readlink(path)))
            elif stat.S_ISREG(mode):
                files.append(path)
            elif not stat.S_ISDIR(mode):
                others.append(str(path.relative_to(output)))
    print('%d files copied into %s' % (len(files), output))

    if links:
        sys.exit('the output has %d symbolic link(s), which none of the checks below reads '
                 'and whose target may be outside this tree:\n  %s'
                 % (len(links), '\n  '.join(sorted(links))))
    if others:
        sys.exit('the output has %d entr(ies) that are neither a file, a directory nor a link, '
                 'which none of the checks below reads:\n  %s'
                 % (len(others), '\n  '.join(sorted(others))))
    # Nor is an empty output this tool passing: with no files, every check
    # below is comparing empty sets and says so cheerfully.
    if not files:
        sys.exit('the output has no files at all, so none of the checks below asks anything')
    if skipped:
        print('left out (%d):' % len(skipped))
        for name in sorted(set(skipped)):
            print('  ' + name)

    failures = []
    # Asked over the one enumeration above rather than with a fresh rglob, so
    # that this cannot answer "not found" out of a subtree that was not read --
    # and over every entry, not just the regular files, because most of the deny
    # list is directory names (private, work, .git, node_modules) and a check
    # that only looked at files would have stopped seeing all of them.
    for bad in sorted(DENY_NAMES):
        found = [str(p.relative_to(output)) for p in entries if p.name == bad]
        if found:
            failures.append('%s is in the output: %s' % (bad, ', '.join(found[:3])))
    # The suffixes were filtered during the copy and never looked at again, so
    # nothing said whether that filter had run (2026-11-03, found by review).
    for bad in sorted(DENY_SUFFIXES):
        found = [str(p.relative_to(output)) for p in entries if p.suffix == bad]
        if found:
            failures.append('%d file(s) with the denied suffix %s are in the output: %s'
                            % (len(found), bad, ', '.join(sorted(found)[:3])))

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
    # The manifest's absence is not this check passing. Written as `if
    # manifest.is_file():` with nothing on the other side, deleting REUSE.toml
    # -- or exporting a tree that has not got one -- switched the whole check
    # off and the export still said ok (2026-11-03).
    manifest = (output / 'REUSE.toml')
    if not manifest.is_file():
        failures.append('REUSE.toml is not in the output, so this check asked nothing; '
                        'it is what names the licences that have to be there')
    else:
        import re
        import tomllib
        # Parsed, not matched. Every pattern written for this missed a spelling
        # that TOML reads the same way, and each time the miss was silent: the
        # first one stopped at a space, so none of the five ids inside
        # "Apache-2.0 AND ISC" were asked for at all; the second read all four
        # of TOML's string forms and still missed a quoted key --
        # `"SPDX-License-Identifier" = "Apache-2.0 AND ISC"`, which
        # tomllib.loads proves is the same document (measured on this host,
        # 2026-11-03) -- and the form-agnostic count added to catch exactly that
        # shares the blind spot, so the two agreed with each other and said
        # nothing. A parser has no spellings left to miss. It also stops the
        # opposite mistake, reading a commented-out line as a real value, which
        # the patterns did.
        def spdx(node):
            if isinstance(node, dict):
                for key, value in node.items():
                    if key == 'SPDX-License-Identifier':
                        yield from ([value] if isinstance(value, str) else value)
                    else:
                        yield from spdx(value)
            elif isinstance(node, list):
                for item in node:
                    yield from spdx(item)

        values = []
        try:
            values = list(spdx(tomllib.loads(manifest.read_text())))
        except (tomllib.TOMLDecodeError, TypeError) as problem:
            failures.append('REUSE.toml could not be read as TOML, so this check does not '
                            'know what licences it names: %s' % problem)
        wanted = set()
        for expression in values:
            for word in re.split(r'[()\s]+', expression):
                if word and word.upper() not in {'AND', 'OR', 'WITH'}:
                    wanted.add(word.rstrip('+'))
        # A name is not a licence. `{p.stem for p in ...glob('*.txt')}` counted
        # anything with the right name, so an empty file -- or a directory
        # called ISC.txt -- answered for the licence text that has to ship with
        # the code (2026-11-03, found by review). What is wanted is a readable,
        # non-empty regular file.
        have = set()
        for path in sorted((output / 'LICENSES').glob('*.txt')):
            if path.is_file() and not path.is_symlink() and path.stat().st_size > 0:
                have.add(path.stem)
        if not wanted:
            failures.append('REUSE.toml named no licence at all, so this check asked for '
                            'nothing; the manifest or the way it is read has changed')
        for name in sorted(wanted - have):
            failures.append('REUSE.toml names %s and LICENSES/%s.txt is not there as a '
                            'readable, non-empty file' % (name, name))
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
    #
    # And the scanner's absence is not a pass either. The counts above are read
    # back because "0 files checked, 0 denylist entries" is this check's
    # ordinary way of being switched off -- and then the whole thing sat behind
    # `if check.is_file():` with nothing on the other side, so renaming or
    # losing the scanner skipped every one of those guards and the export said
    # ok (2026-11-03, found by review; the same shape as the REUSE check above).
    check = ROOT / 'tools' / 'check-private-data.py'
    if not check.is_file():
        failures.append('tools/check-private-data.py is not in this tree, so nothing looked '
                        "for this developer's own values in what is about to be published")
    else:
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
    # An empty answer from both queries is not this check passing: it is this
    # check comparing the output against nothing, which every path is outside
    # of -- except that the comparison is the other way round, so every path
    # passes. git exiting non-zero is handled above; succeeding and naming no
    # file at all is a tree this tool cannot have exported from (2026-11-03,
    # found by review).
    if not seen and not failures:
        failures.append('git names no file at all in this tree, so this check compared the '
                        'output against an empty set')
    if seen:
        unseen = sorted(str(p.relative_to(output)) for p in files
                        if str(p.relative_to(output)) not in seen)
        if unseen:
            failures.append('%d exported file(s) are outside what git tracks or would add, so '
                            'the private data check never read them: %s'
                            % (len(unseen), ', '.join(unseen[:5])))
        else:
            print('all %d are inside what the private data check reads' % len(files))

        # The other direction, which nothing above asks: everything that should
        # be in the output is. "Every exported file is this tree's and was read"
        # holds just as well for an export that silently left half the tree
        # behind -- a copy that skipped what it could not stat, an INCLUDE entry
        # that matched less than it used to. RELEASE-PLAN said the only proof
        # was to diff the two file lists after exporting, and nothing did
        # (2026-11-03 Codex review, item 9; added 2026-11-04). Expected is what
        # git tracks or would add, under the included names, minus the deny
        # list -- the same rules the copy applies -- and only files that exist,
        # because a tracked file deleted in the work tree is not one to export.
        #
        # "Exists" is asked with lstat and only "no such file" counts as no:
        # Path.is_file() on this host's Python 3.15 answers False for a file it
        # may not stat, which would drop from the expected set exactly the file
        # a copy that could not read it left behind.
        exported = {str(p.relative_to(output)) for p in files}
        expected = set()
        unreadable = []
        for name in seen:
            parts = Path(name).parts
            if not parts or parts[0] not in INCLUDE:
                continue
            # At every level, as the copy applies it: its ignore() is asked for
            # each name in each directory, so a directory called `old.orig` is
            # left out with everything under it (11-04 review, item 12).
            if any(part in DENY_NAMES or Path(part).suffix in DENY_SUFFIXES for part in parts):
                continue
            try:
                mode = os.lstat(ROOT / name).st_mode
            except FileNotFoundError:
                continue
            except OSError as error:
                unreadable.append('%s (%s)' % (name, error.strerror))
                continue
            if stat.S_ISREG(mode) or stat.S_ISLNK(mode):
                expected.add(name)
        if unreadable:
            failures.append('cannot tell whether %d file(s) git tracks or would add exist: %s'
                            % (len(unreadable), ', '.join(sorted(unreadable)[:5])))
        left_behind = sorted(expected - exported)
        if left_behind:
            failures.append('%d file(s) git tracks or would add under the included names are not '
                            'in the output: %s' % (len(left_behind), ', '.join(left_behind[:5])))
        else:
            print('and all %d files that should be there are' % len(expected))

    if failures:
        print('\nFAILED')
        for line in failures:
            print('  ' + line)
        return 1
    print('\nok: nothing on the deny list reached the output')
    return 0


if __name__ == '__main__':
    sys.exit(main())
