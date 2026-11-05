#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""The open-source export has to refuse, not shrug, when a check cannot be made.

`tools/export-open-source.py` decides which files go onto the public internet.
Its checks were written as `if <the thing to check> exists: check it`, with
nothing on the other side, so losing the thing switched the check off and the
export still said ok. Four of those were found by reading the file on
2026-11-03, and then four more in the fixes themselves -- including a guard
against symbolic links that `os.walk` without `onerror` made fail-open, which
is the same defect inside the repair.

None of that was visible to the test suite, here or in CI, because every test
fed these tools good input and checked that the answer was right. So the
property this file asserts is the other half: for each thing a check depends
on, take it away and the export must fail.

It runs the exporter against a synthetic tree rather than this repository, so a
mutation cannot leave the real tree in a half-broken state, and so the private
data scanner can be a stub whose answer each case controls.
"""
import importlib.util
import io
import os
import shutil
import subprocess
import sys
import tempfile
import traceback
from contextlib import redirect_stdout
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
EXPORTER = ROOT / 'tools/export-open-source.py'

# A scanner that answers the way the real one does on a clean tree. Each case
# can replace it.
GOOD_SCANNER = ('import sys\n'
                'print("3 files checked, 6 denylist entries, 0 findings")\n'
                'sys.exit(0)\n')

checks = 0
failures = []


def check(ok, what):
    global checks
    checks += 1
    if not ok:
        failures.append(what)


def git(root, *args):
    subprocess.run(['git', '-C', str(root)] + list(args), check=True,
                   capture_output=True, text=True)


def build(workspace, scanner=GOOD_SCANNER):
    """A tree the exporter is happy with, so that a case changes one thing."""
    root = workspace / 'tree'
    (root / 'docs').mkdir(parents=True)
    (root / 'tools').mkdir()
    (root / 'LICENSES').mkdir()
    # Joined, not spelled out: a literal `docs/<name>.md` in this file reads as
    # a document reference to the doc-links test, and the fixture is not one.
    (root / 'docs' / 'guide.md').write_text('nothing private here\n')
    (root / 'README').write_text('a tree\n')
    (root / 'tools/check-private-data.py').write_text(scanner)
    (root / 'LICENSES/Apache-2.0.txt').write_text('the licence text\n')
    (root / 'LICENSES/ISC.txt').write_text('the other licence text\n')
    (root / 'REUSE.toml').write_text(
        'version = 1\n\n'
        '[[annotations]]\n'
        'path = "docs/**"\n'
        'SPDX-License-Identifier = "Apache-2.0 AND ISC"\n')
    git(root, 'init', '-q')
    git(root, 'config', 'user.email', 'test@example.invalid')
    git(root, 'config', 'user.name', 'test')
    git(root, 'add', '-A')
    git(root, 'commit', '-q', '-m', 'the fixture')
    return root


def export(root, output, include=('docs', 'tools', 'LICENSES', 'REUSE.toml', 'README'),
           after_copy=None):
    """Run the real exporter over `root`. Returns (status, output text)."""
    spec = importlib.util.spec_from_file_location('exporter_under_test', EXPORTER)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.ROOT = root
    module.INCLUDE = list(include)
    injected = []
    if after_copy is not None:
        original = module.copy_tree

        def wrapped(source, target):
            result = original(source, target)
            # A fault in the injection is the test's, not a refusal by the
            # exporter: caught here it would be counted as one, and a mkfifo
            # the filesystem does not support would pass the FIFO case with no
            # FIFO ever made (11-04 review, item 15).
            try:
                after_copy(target)
            except Exception:  # noqa: BLE001 - reported as the test's own fault
                injected.append(traceback.format_exc())
            return result

        module.copy_tree = wrapped
    argv = sys.argv
    sys.argv = ['export-open-source.py', '--output', str(output)]
    text = io.StringIO()
    try:
        with redirect_stdout(text):
            status = module.main()
    except SystemExit as stop:
        status = 1 if stop.code else 0
        text.write(str(stop.code or ''))
    except Exception:  # noqa: BLE001 - a crash is a refusal, and says why
        status = 1
        text.write(traceback.format_exc())
    finally:
        sys.argv = argv
    if injected:
        raise RuntimeError('the injection itself failed, so this case tested nothing:\n'
                           + injected[0])
    return status, text.getvalue()


def case(name, mutate, scanner=GOOD_SCANNER, include=None, after_copy=None):
    """Mutate the fixture one way; the export must not succeed."""
    with tempfile.TemporaryDirectory() as workspace:
        space = Path(workspace)
        root = build(space, scanner=scanner)
        if mutate is not None:
            mutate(root)
        kwargs = {} if include is None else {'include': include}
        status, said = export(root, space / 'out', after_copy=after_copy, **kwargs)
        check(status != 0,
              '把「%s」之后导出仍然成功了，所以这道检查在被关掉的情况下会报通过。'
              '它说的是：\n      %s' % (name, said.strip()[-300:] or '(什么都没说)'))


def main():
    check(EXPORTER.is_file(), '%s is missing' % EXPORTER)
    if not EXPORTER.is_file():
        return report()

    # First: the fixture itself must pass. Without this every case below could
    # be failing for a reason that has nothing to do with its mutation, and the
    # whole file would be green for the wrong reason.
    with tempfile.TemporaryDirectory() as workspace:
        space = Path(workspace)
        status, said = export(build(space), space / 'out')
        check(status == 0,
              '没有动过的那棵树都导不出来，所以下面每一条都可能是因为别的原因失败的。'
              '它说的是：\n      %s' % said.strip()[-400:])

    def unlink(relative):
        return lambda root: (root / relative).unlink()

    case('拿掉私有数据扫描器', unlink('tools/check-private-data.py'))
    case('拿掉 REUSE.toml', unlink('REUSE.toml'))
    case('拿掉一份被 REUSE.toml 点名的许可证', unlink('LICENSES/ISC.txt'))

    def quote_the_key(root):
        # Valid TOML, same parse, and the pattern-based versions of this check
        # read one licence fewer without saying so.
        path = root / 'REUSE.toml'
        path.write_text(path.read_text().replace(
            'SPDX-License-Identifier =', '"SPDX-License-Identifier" ='))
        (root / 'LICENSES/ISC.txt').unlink()

    case('把 SPDX 的键加上引号（合法 TOML、解析相同）并拿掉那份许可证', quote_the_key)

    def licence_as_directory(root):
        (root / 'LICENSES/ISC.txt').unlink()
        (root / 'LICENSES/ISC.txt').mkdir()

    case('把一份许可证换成同名的目录', licence_as_directory)
    case('把一份许可证清空', lambda root: (root / 'LICENSES/ISC.txt').write_text(''))

    def inner_symlink(root):
        (root / 'docs/outside').symlink_to(root.parent)

    case('在导出目录里放一个指向树外的符号链接', inner_symlink)

    def top_level_symlink(root):
        real = root.parent / 'elsewhere'
        real.mkdir()
        (real / 'secret.md').write_text('not meant to be published\n')
        shutil.rmtree(root / 'docs')
        (root / 'docs').symlink_to(real)

    case('把 INCLUDE 点名的顶层目录换成指向树外的符号链接', top_level_symlink)

    # Injected after the copy on purpose. Putting `private/` in the source tests
    # the copy filter, which removes it correctly and reports it under "left
    # out" -- so the export succeeds, and should. What has to be tested is the
    # other thing: that the recheck over the output would catch it if the filter
    # ever stopped working. (This test asserted the wrong one first, and the
    # run said so.)
    # The file is put in the tree as well, untracked and not ignored, and the
    # copy in the output is the tree's own bytes. Otherwise two other checks
    # refuse it first -- "not this tree's, byte for byte" and "outside what git
    # would add" -- and these cases pass with the deny-list recheck deleted
    # (11-04 review, round two, item 10).
    def denied_name_in_tree(root):
        (root / 'docs/private').mkdir()
        (root / 'docs/private' / 'notes.md').write_text('internal\n')

    def denied_name_in_output(target):
        if target.name == 'docs':
            shutil.copytree(target.parent.parent / 'tree/docs/private', target / 'private')

    case('过滤器失效、输出里留下一个叫 private 的目录', denied_name_in_tree,
         after_copy=denied_name_in_output)

    def denied_suffix_in_tree(root):
        (root / 'docs/stale.pyc').write_bytes(b'\x00')

    def denied_suffix_in_output(target):
        if target.name == 'docs':
            shutil.copyfile(target.parent.parent / 'tree/docs/stale.pyc', target / 'stale.pyc')

    case('过滤器失效、输出里留下一个 .pyc', denied_suffix_in_tree,
         after_copy=denied_suffix_in_output)

    # The one that the first version of the symlink guard could not see: a
    # subtree the walk cannot read answers exactly like a subtree with nothing
    # in it. Injected after the copy, because a source directory that cannot be
    # read fails earlier, during the copy.
    def make_unreadable(target):
        if target.name == 'docs':
            hidden = target / 'hidden'
            hidden.mkdir()
            (hidden / 'file.md').write_text('x\n')
            hidden.chmod(0o000)

    case('让导出目录里有一个读不进去的子目录', None, after_copy=make_unreadable)

    # The other direction: a copy that quietly left a file behind. Everything
    # above asks whether what is in the output belongs there; this one asks
    # whether what belongs there is in the output (added 2026-11-04).
    def left_behind(target):
        if target.name == 'docs':
            (target / 'guide.md').unlink()

    case('复制完少了一个 git 跟踪的文件', None, after_copy=left_behind)

    # And the reverse check must leave out what the copy leaves out, at every
    # level: a tracked file under `docs/old.orig/` is not copied, and must not
    # then be reported as left behind (11-04 review, item 12). The fixture
    # passes with it, or the case fails.
    with tempfile.TemporaryDirectory() as workspace:
        space = Path(workspace)
        root = build(space)
        (root / 'docs/old.orig').mkdir()
        (root / 'docs/old.orig/guide.md').write_text('a stale copy\n')
        git(root, 'add', '-A')
        git(root, 'commit', '-q', '-m', 'a tracked file under a denied directory name')
        status, said = export(root, space / 'out')
        check(status == 0,
              '被跟踪的 docs/old.orig/guide.md（目录名带被拒的后缀）让导出失败了：复制按每一级'
              '名字排除它，反向检查却要它在输出里。它说的是：\n      %s' % said.strip()[-300:])

    def fifo_in_output(target):
        if target.name == 'docs':
            os.mkfifo(target / 'pipe')

    case('输出里有一个既不是文件也不是目录的条目（命名管道）', None, after_copy=fifo_in_output)

    case('让扫描器报 0 files checked',
         None, scanner='print("0 files checked, 6 denylist entries, 0 findings")\n')
    case('让扫描器报 0 denylist entries',
         None, scanner='print("3 files checked, 0 denylist entries, 0 findings")\n')
    case('让扫描器以非零退出',
         None, scanner='import sys\nprint("3 files checked, 6 denylist entries, 1 findings")\n'
                       'sys.exit(1)\n')
    case('让 INCLUDE 只点名一个空目录', lambda root: (root / 'empty').mkdir(),
         include=('empty',))

    print('%d checks, %d failed' % (checks, len(failures)))
    for failure in failures:
        print('  FAIL %s' % failure)
    return 1 if failures else 0


def report():
    print('%d checks, %d failed' % (checks, len(failures)))
    for failure in failures:
        print('  FAIL %s' % failure)
    return 1 if failures else 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Exception:  # noqa: BLE001 - any fault here is this test's answer
        print('0 checks, 1 failed')
        print('  FAIL the check itself stopped: %s'
              % traceback.format_exc().strip().replace('\n', ' | '))
        sys.exit(1)
