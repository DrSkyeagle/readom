#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""No check in the acceptance script may pass on an answer it never got.

`tools/verify-device.sh` asks the device sixty-odd questions. Every one of them
can fail -- the file is root-only, the log has been dropped, the service is not
running, the cable moved -- and for most of the run the script was written as if
the only two outcomes were "the answer is right" and "the answer is wrong". A
failed read came back as empty output, a `grep -c` over empty output came back
as zero, and zero was the passing value for every negative check in the file. So
the checks that mattered most reported ok precisely when they had learned
nothing. Two of them, run against the original lines on 2026-11-03:

    /system/build.prop unreadable  ->  "build.prop 没有覆盖它"       ok
    the kernel log unreadable      ->  "除 su 外没有 permissive=1"   ok

Those were found by reading the script, not by running it, and that is the
reason this file exists. The whole test suite -- here, on the build machine and
in CI -- fed every tool good input and checked that the answer was right.
Nothing fed a tool a question that fails, so a check that cannot fail was
invisible to all of it.

Three properties are asserted, and the first one is here because the first
version of this file had only the other two. A script that printed the one
exempt line and the closing count and then exited non-zero satisfied both of
them -- that is, deleting every check in the file passed this test (found by
review the same day). So:

  1. The set of check names is the same whether the device answers or not.
     A check that disappears, or is renamed, or stops being reached, changes
     that set. The baseline comes from a run where the stub answers everything.
  2. With every question failing, every check reports `--`. Not ok, because
     nothing was learned. And not FAIL either -- a device nobody could ask is
     not a device that is wrong, and reporting it as wrong is what sent people
     after the device on 2026-10-21 when the problem was that six files were
     root-only.
  3. With one question failing and the rest answered, the checks that rest on
     that question do not report ok. This is separate from 2 because several
     branches are only reached when an earlier probe succeeded: when everything
     fails, root detection fails too and those branches take the "needs root"
     exit instead, so property 2 never looks at them. Both of the defects
     quoted above live there.

The one exception throughout is the check that the device is present, which is
answered by `adb devices` rather than by a question put to the device.

This needs no device. The script takes the adb to use from $ADB, which is the
seam; a stub stands in for it.
"""
import os
import re
import subprocess
import sys
import tempfile
import traceback
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
SCRIPT = ROOT / 'tools/verify-device.sh'

# The one check that does not put a question to the device: section one reads
# `adb devices`, and the stub answers it, so ok there is the right answer.
ALLOWED_OK = 'adb 看得到设备'

# Names anchored so that a rename cannot quietly shrink what this file covers.
# Not a count -- the sections grow.
MUST_BE_CHECKED = [
    '没有留下任何 debug.pw3.* 覆盖',
    '除 su（userdebug 构建里唯一的 permissive 域）外没有 permissive=1',
    'default.prop 里写的就是 quicken，而 build.prop 没有覆盖它',
    '/system 里没有一份是非 speed 的',
    'SELinux 是 Enforcing',
]

# The readings a release build cannot make, because it has no su. Anchored by
# name: these have to come back as "skipped by design", never as "could not
# look", or the acceptance gate is one no shipped image can pass.
NEEDS_ROOT = [
    'default.prop 里写的就是 quicken，而 build.prop 没有覆盖它',
    'dm-0 的设备级 ro = 0',
    'init 的 0/1/2 都指向 selinux/null（没有悬空给后来的 open 占）',
    # These two were left reporting `--` when the six above were fixed, so a
    # healthy release image still could not reach exit 0 and the gate asking
    # for it was a gate nothing passes (2026-11-03, found by review).
    '安装器记下了它处理到哪一份报告',
    '报告写下了这个系统的版本号',
]

# One question failing, the rest answered. Each entry is (what to break, the
# substring of the check that rests on it). The command the script sends is
# matched against the pattern.
PARTIAL = [
    ('/system/build.prop', 'build.prop 没有覆盖它'),
    ('logcat -b kernel -d', 'permissive=1'),
    ('getenforce', 'SELinux 是 Enforcing'),
    ('dumpsys package dexopt', '非 speed'),
    ('getprop$', 'debug.pw3.* 覆盖'),
]

# Section seven's answer, as the device gives it: the first four records of
# `dumpsys package dexopt` on the f image, 2026-11-04, byte for byte, including
# the line dumpsys broke at 120 columns in the middle of `speed`. Every package
# has a `path:` line above its record, which is what the check for orphan
# records counted as orphans -- on every healthy device -- until a real dump was
# put through it. Everything this file asserted before was asked with empty
# answers, so nothing here had ever seen what a healthy device says.
DEXOPT_HEALTHY = """Dexopt state:
  [com.android.providers.telephony]
    path: /system/priv-app/TelephonyProvider/TelephonyProvider.apk
      arm: /system/priv-app/TelephonyProvider/oat/arm/TelephonyProvider.odex[status=kOatUpToDate, compilation_filter=spe
      ed]
  [com.android.providers.media]
    path: /system/priv-app/MediaProvider/MediaProvider.apk
      arm: /system/priv-app/MediaProvider/oat/arm/MediaProvider.odex[status=kOatUpToDate, compilation_filter=speed]
      used by other apps: [com.android.providers.downloads]
  [com.android.documentsui]
    path: /system/priv-app/DocumentsUI/DocumentsUI.apk
      arm: /system/priv-app/DocumentsUI/oat/arm/DocumentsUI.odex[status=kOatUpToDate, compilation_filter=speed]
  [com.android.externalstorage]
    path: /system/priv-app/ExternalStorageProvider/ExternalStorageProvider.apk
      arm: /system/priv-app/ExternalStorageProvider/oat/arm/ExternalStorageProvider.odex[status=kOatUpToDate, compilatio
      n_filter=speed]
"""

# The section seven checks, by name.
DEXOPT_CHECKS = [
    '/system 里有编译记录的代码文件数 > 0',
    '/system 里每一份的编译记录都是最新的',
    '/system 里没有一份是非 speed 的',
    '没有 ART 读不出状态的记录',
    'dumpsys 的每一条 /system 记录都是完整的',
]

# One defect each, made in the healthy dump, and the check that has to say so.
DEXOPT_BROKEN = [
    ('一条记录的右括号没到（输出被截断）',
     lambda d: d.replace('compilation_filter=speed]\n  [com.android.externalstorage]',
                         'compilation_filter=speed\n  [com.android.externalstorage]'),
     'dumpsys 的每一条 /system 记录都是完整的'),
    ('一行没有自己记录的 /system 路径排在一条完整记录前面',
     lambda d: d.replace('      arm: /system/priv-app/DocumentsUI/oat',
                         '      arm: /system/priv-app/Orphan/oat/arm/Orphan.odex\n'
                         '      arm: /system/priv-app/DocumentsUI/oat'),
     'dumpsys 的每一条 /system 记录都是完整的'),
    ('一份是 speed-profile',
     lambda d: d.replace('DocumentsUI.odex[status=kOatUpToDate, compilation_filter=speed]',
                         'DocumentsUI.odex[status=kOatUpToDate, compilation_filter=speed-profile]'),
     '/system 里没有一份是非 speed 的'),
    ('一份不是最新的',
     lambda d: d.replace('DocumentsUI.odex[status=kOatUpToDate, compilation_filter=speed]',
                         'DocumentsUI.odex[status=kOatDexOutOfDate, vdex-only]'),
     '/system 里每一份的编译记录都是最新的'),
    ('一个 /system 包只有 path 没有记录',
     lambda d: d + '  [example.broken]\n    path: /system/app/Broken/Broken.apk\n',
     'dumpsys 的每一条 /system 记录都是完整的'),
    ('一个 /system 包的记录是 [Exception]',
     lambda d: d + '  [example.broken]\n    path: /system/app/Broken/Broken.apk\n'
                   '      arm: [Exception]: query failed\n',
     'dumpsys 的每一条 /system 记录都是完整的'),
    ('输出在第一条完整记录之后就断了（前六行）',
     lambda d: '\n'.join(d.split('\n')[:6]) + '\n',
     'dumpsys 的每一条 /system 记录都是完整的'),
    ('输出断在下一个包的包头上（前七行）',
     lambda d: '\n'.join(d.split('\n')[:7]) + '\n',
     'dumpsys 的每一条 /system 记录都是完整的'),
    ('ART 两个文件都打不开（invalid[]）',
     lambda d: d + '  [example.broken]\n    path: /system/app/Broken/Broken.apk\n'
                   '      arm: invalid[]\n',
     '没有 ART 读不出状态的记录'),
]

STUB = r'''#!/bin/sh
# Stand-in for adb, selected through $ADB. It answers `devices` so the script
# gets past section one -- the state being tested is a device that is there and
# does not answer questions, not a missing device, which the script already
# handles by exiting.
while [ $# -gt 0 ]; do
  case "$1" in
    -P) shift 2 ;;
    devices) printf 'List of devices attached\nstubdevice\tdevice\n'; exit 0 ;;
    shell) shift; break ;;
    *) shift ;;
  esac
done
asked="$*"
# The script's marker is random per run, and it is in the command it sent:
# `( ... ) ; echo "<marker>$?"`. Taking it from there is the only way a stub
# cannot go out of step with it -- a hard-coded `__rc=` turned every answer
# into "could not reach the device" the moment the marker became random, and
# the run still looked like a script defect (2026-11-03).
mark=$(printf '%s' "$asked" | sed -n 's/.*echo "\(__rc[_a-z0-9]*=\)\$?".*/\1/p')
[ -n "$mark" ] || mark='__rc='
# STUB_FAIL names the one question that fails; everything else is answered.
if [ -n "${STUB_FAIL:-}" ] && printf '%s' "$asked" | grep -q "$STUB_FAIL"; then
  case "${STUB_FAIL_MODE:-rc}" in
    rc) printf '%s1\n' "$mark"; exit 0 ;;
    *) exit 1 ;;
  esac
fi
case "${STUB_MODE:-rc}" in
  # Every question fails. The shell ran and the command inside it failed: the
  # sentinel is printed because the remote shell would have printed it, the
  # command's own output is not, because it failed.
  rc) printf '%s1\n' "$mark"; exit 0 ;;
  # Nothing came back at all.
  unreachable) exit 1 ;;
esac
# STUB_MODE=answer: every question succeeds. The answers are empty except the
# two that decide which branches are reached at all -- the script asks for root
# before the readings that need it, and asks how far back the kernel log goes
# before the questions it answers out of that log. With those empty, the
# branches that hold two of the defects this file is about are never entered.
case "$asked" in
  # STUB_NOROOT: an unprivileged shell, and no su at all, which is what a
  # release build looks like.
  *"su 0 id -u"*) if [ -n "${STUB_NOROOT:-}" ]; then printf '%s127\n' "$mark"; exit 0; fi
                  printf '0\n' ;;
  *"id -u"*) if [ -n "${STUB_NOROOT:-}" ]; then printf '2000\n'; else printf '0\n'; fi ;;
  # What a release image says about itself, which is the only thing that makes
  # "no su" a design rather than a fault.
  # A fresh image: WeRead is not installed until the owner taps for it. The
  # probe asks for `android` first, as its control, then for WeRead; `pm path`
  # says "not installed" with status 1 and no output (measured on f).
  *"pm path android"*) printf 'package:/system/framework/framework-res.apk\nctl=0\nwr=1\n' ;;
  *"getprop ro.build.type"*) if [ -n "${STUB_NOROOT:-}" ]; then printf '%s\n' "${STUB_BUILD_TYPE:-user}"; fi ;;
  *"getprop ro.debuggable"*) if [ -n "${STUB_NOROOT:-}" ]; then printf '%s\n' "${STUB_DEBUGGABLE:-0}"; fi ;;
  # Enough of an install session that the write and the abandon are reached:
  # with these empty, the run takes the "no session" arm and the check that the
  # session was abandoned is never asked at all (2026-11-03).
  *"install-create"*) printf 'Success: created install session [41]\n' ;;
  *"install-write"*) printf 'Success: streamed 4096 bytes\n' ;;
  *"logcat -b kernel -d -v epoch"*) printf '0\n' ;;
  # The recheck probe is run, not answered: its verdict is the logic under
  # test, so the device-side command goes through a real shell with an `ls`
  # that says what STUB_LS_OUT says (STUB_FAKEBIN, made by the test). Answering
  # "no" here directly would pass whatever the probe did with ls's words
  # (11-04 review, item 14). The command prints the script's own marker.
  *"ls -d /pw3-system-recheck"*) PATH="$STUB_FAKEBIN:$PATH" sh -c "$asked"; exit 0 ;;
  # STUB_DEXOPT: a file holding the dexopt dump to answer with, for the cases
  # that feed section seven a real answer rather than an empty one.
  # The odex files /system holds, which section seven holds the dump to: the
  # four records of the healthy fixture, when a dexopt fixture is in use.
  *"ls /system/app/*/oat/*/*.odex"*)
    if [ -n "${STUB_DEXOPT:-}" ]; then
      printf '%s\n' /system/priv-app/TelephonyProvider/oat/arm/TelephonyProvider.odex \
        /system/priv-app/MediaProvider/oat/arm/MediaProvider.odex \
        /system/priv-app/DocumentsUI/oat/arm/DocumentsUI.odex \
        /system/priv-app/ExternalStorageProvider/oat/arm/ExternalStorageProvider.odex
    fi ;;
  # Only for the exact command: a query cut down on the device side (a `| head`
  # added, say) is a regression the parsing cases below cannot see if any
  # command naming dexopt gets the whole file (11-04 review, item 13).
  *"dumpsys package dexopt"*)
    if [ -n "${STUB_DEXOPT:-}" ]; then
      if [ "$(printf '%s' "$asked" | sed -n '1s/^( //p')" = 'dumpsys package dexopt' ]; then
        cat "$STUB_DEXOPT"
      else
        printf '%s1\n' "$mark"; exit 0
      fi
    fi ;;
esac
printf '%s0\n' "$mark"
exit 0
'''

checks = 0
failures = []


def check(ok, what):
    global checks
    checks += 1
    if not ok:
        failures.append(what)


def run(stub, mode, fail=None, fail_mode='rc', noroot=False, dexopt=None, extra=None):
    environment = dict(os.environ, ADB=str(stub), STUB_MODE=mode)
    for name in ('STUB_BUILD_TYPE', 'STUB_DEBUGGABLE', 'STUB_LS_OUT', 'STUB_LS_RC'):
        environment.pop(name, None)
    environment['STUB_FAKEBIN'] = str(Path(stub).parent / 'fakebin')
    environment.update(extra or {})
    if dexopt is None:
        environment.pop('STUB_DEXOPT', None)
    else:
        environment['STUB_DEXOPT'] = str(dexopt)
    if noroot:
        environment['STUB_NOROOT'] = '1'
    else:
        environment.pop('STUB_NOROOT', None)
    environment.pop('ADB_PORT', None)
    if fail is None:
        environment.pop('STUB_FAIL', None)
    else:
        environment['STUB_FAIL'] = fail
        environment['STUB_FAIL_MODE'] = fail_mode
    return subprocess.run(['bash', str(SCRIPT)], capture_output=True, text=True,
                          env=environment, cwd=str(ROOT), timeout=900)


def declared_names():
    """The check names the script writes out, as opposed to builds from a variable."""
    source = SCRIPT.read_text()
    return {name for name in re.findall(r'^\s*(?:ck|ckq|na|skip) "([^"]*)"',
                                        source, re.M) if '$' not in name}


def conflicts(output):
    """Names reported twice with different outcomes, and lines that fold together.

    The reader below keeps the first line for a name, so a second line saying
    something else was silently dropped -- `-- X` followed by `ok X` read as
    `--` and nothing said the two disagreed. And the name is cut at the two
    spaces before its detail, so two checks whose names differ only after such
    a break become one (2026-11-03, both found by review).
    """
    seen = {}
    bad = []
    for line in output.splitlines():
        matched = re.match(r'^(ok|FAIL|--|~~)\s{1,3}(.*)$', line)
        if not matched:
            continue
        rest = matched.group(2).strip()
        name = re.split(r'\s{2,}[(（]', rest)[0].strip()
        if name in seen and seen[name][0] != matched.group(1):
            bad.append('%s：先报 %s（%s），又报 %s（%s）'
                       % (name, seen[name][0], seen[name][1], matched.group(1), rest))
        elif name in seen and seen[name][1] != rest:
            bad.append('%s：两行不同的检查折叠成了同一个名字（%s / %s）'
                       % (name, seen[name][1], rest))
        seen.setdefault(name, (matched.group(1), rest))
    return bad


def outcomes(output):
    """{check name: 'ok' | 'FAIL' | '--' | '~~'}, keyed by the name without its detail."""
    found = {}
    for line in output.splitlines():
        matched = re.match(r'^(ok|FAIL|--|~~)\s{1,3}(.*)$', line)
        if not matched:
            continue
        how, rest = matched.group(1), matched.group(2).strip()
        # The detail is a trailing "  (...)", and some of them use full-width
        # parentheses, so it is cut at the two spaces rather than matched as a
        # balanced ASCII pair (2026-11-03: two checks looked as if they had
        # stopped being reached, and it was this).
        name = re.split(r'\s{2,}[(（]', rest)[0].strip()
        found.setdefault(name, how)
    return found


def main():
    check(SCRIPT.is_file(), '%s is missing' % SCRIPT)
    if not SCRIPT.is_file():
        return report()

    with tempfile.TemporaryDirectory() as workspace:
        stub = Path(workspace) / 'adb'
        stub.write_text(STUB)
        stub.chmod(0o755)
        # The device's `ls`, for the probe that is run rather than answered.
        # By default it says what f says for this path: there is no such file.
        fakebin = Path(workspace) / 'fakebin'
        fakebin.mkdir()
        (fakebin / 'ls').write_text(
            '#!/bin/sh\n'
            'printf "%s\\n" "${STUB_LS_OUT:-ls: /pw3-system-recheck: No such file or directory}"\n'
            'exit "${STUB_LS_RC:-1}"\n')
        (fakebin / 'ls').chmod(0o755)

        # The baseline: the device answers everything. What this run is for is
        # the set of names, not the verdicts -- the answers are empty, so most
        # of them are FAIL here, which is correct and not what is asserted.
        base = run(stub, 'answer')
        expected = outcomes(base.stdout)
        check(len(expected) >= 20,
              '基线那一轮只认出 %d 项检查，这个解析器大概已经和脚本的输出格式脱节了。'
              'stderr：%s' % (len(expected), base.stderr.strip()[-300:] or '(空)'))
        # Every check the script spells out must be reached in the baseline. A
        # floor of twenty said nothing about which checks: the baseline and the
        # failing runs could lose the same ones together and stay green
        # (2026-11-03, found by review). Names built from variables are not in
        # this set; the set-equality below covers those.
        declared = declared_names()
        check(declared,
              '从 %s 里一个字面量检查名都没抽出来，这个抽取规则已经和脚本脱节了'
              % SCRIPT.name)
        unreached = sorted(declared - set(expected))
        check(not unreached,
              '脚本里写着这些检查，基线那一轮却一次都没走到——要么它们走不到了，'
              '要么这个假 adb 答得不够让它们被问出来，两种都会让下面的断言盖不住'
              '它们：\n      %s' % '\n      '.join(unreached))
        for name in MUST_BE_CHECKED:
            check(name in expected,
                  '基线里没有「%s」这项检查。要么它被删了或改名了，要么这个测试覆盖的'
                  '东西已经悄悄变少了' % name)

        for mode, described in (('rc', '设备回答了、命令失败'),
                                ('unreachable', '问不到设备')):
            done = run(stub, mode)
            got = outcomes(done.stdout)

            # Property 1: the same checks happened.
            missing = sorted(set(expected) - set(got))
            check(not missing,
                  '%s（STUB_MODE=%s）时这些检查一项都没出现，所以它们没有被问到——'
                  '把检查删掉、改名或让它走不到，下面两条断言都看不见：\n      %s'
                  % (described, mode, '\n      '.join(missing)))

            # Property 2: and every one of them said "could not look".
            wrong = sorted('%s（%s）' % (name, how) for name, how in got.items()
                           if name != ALLOWED_OK and how != '--')
            check(not wrong,
                  '%s（STUB_MODE=%s）时这些检查没有报 `--`。报 ok 的是在没有答案的情况下'
                  '通过了；报 FAIL 的是把"问不出来"说成了"看过了、是坏的"，会让人去查'
                  '设备而问题在这里；报 `~~` 的是在连"有没有 root"都没问出来的时候'
                  '声称这一项是按设计跳过的：\n      %s'
                  % (described, mode, '\n      '.join(wrong)))

            clashes = conflicts(done.stdout)
            check(not clashes,
                  '%s（STUB_MODE=%s）时这些检查报了两次而且说法不同，或者两项检查的'
                  '名字折叠成了一个——这两种都会让上面的断言读到错的那一条：'
                  '\n      %s' % (described, mode, '\n      '.join(clashes)))
            check('通过' in done.stdout,
                  '%s（STUB_MODE=%s）时脚本没有跑到末尾的计数行。stderr：%s'
                  % (described, mode, done.stderr.strip()[-400:] or '(空)'))
            check(done.returncode != 0,
                  '%s（STUB_MODE=%s）时脚本以 0 退出，所以自动流程会把一轮什么都没问出来的'
                  '运行当成验收通过' % (described, mode))

        # Property 3: one question fails, the rest are answered.
        for pattern, about in PARTIAL:
            for fail_mode, how_failed in (('rc', '命令失败'), ('dead', '没有回答')):
                done = run(stub, 'answer', fail=pattern, fail_mode=fail_mode)
                got = outcomes(done.stdout)
                related = {name: how for name, how in got.items() if about in name}
                # Present, first. An empty run satisfies "none of them is ok",
                # which is how this case would pass while the script died on
                # its second line (2026-11-03, found by review).
                check(related,
                      '只让匹配 %r 的那次查询%s 时，名字里含 %r 的检查一项都没出现，'
                      '所以这个用例什么也没测到。stderr：%s'
                      % (pattern, how_failed, about,
                         done.stderr.strip()[-200:] or '(空)'))
                passed = sorted(name for name, how in related.items() if how == 'ok')
                check(not passed,
                      '只让匹配 %r 的那次查询%s、其余全部正常回答时，这些检查仍然报 ok：'
                      '\n      %s' % (pattern, how_failed, '\n      '.join(passed)))

        # A release build has no su, and that is not a failure to look: the
        # readings that need root are skipped by design and must not stop the
        # run. Reported as unknown they made exit 0 unreachable on every image
        # anyone actually ships, which is the regression the exit status change
        # introduced (2026-11-03).
        done = run(stub, 'answer', noroot=True)
        got = outcomes(done.stdout)
        for name in NEEDS_ROOT:
            check(got.get(name) == '~~',
                  '镜像上没有 su（发行构建就是这样）而其余问题都答上了的时候，'
                  '「%s」报的是 %r，应该是 `~~`（按设计查不了、不挡发行）——'
                  '报 `--` 会让任何发行镜像都到不了退出 0'
                  % (name, got.get(name, '什么都没报')))
        # And the thing those `~~` exist for: no check may be left reporting
        # `--`. Anchoring only the names above is what let two of them stay
        # `--` through a round that claimed to have fixed exactly this; this
        # asserts the property instead of a list.
        stranded = sorted(name for name, how in got.items() if how == '--')
        check(not stranded,
              '镜像上没有 su 而其余问题都答上了的时候，这些检查报的是 `--`（没能查），'
              '于是退出码是 2，任何发行镜像都过不了验收门。它们要么该是 `~~`（按设计'
              '查不了），要么该真的被问出来：\n      %s' % '\n      '.join(stranded))

        # Section seven fed what a healthy device says. Without this the file
        # proves that no check passes on an answer it never got, and nothing
        # proves that a check passes on the answer it should: the record count
        # failed every healthy device for a day and every run here was green.
        healthy = Path(workspace) / 'dexopt-healthy.txt'
        healthy.write_text(DEXOPT_HEALTHY)
        got = outcomes(run(stub, 'answer', dexopt=healthy).stdout)
        for name in DEXOPT_CHECKS:
            check(got.get(name) == 'ok',
                  '把一台正常设备的 dexopt 输出原样喂给第七节，「%s」报的是 %r，'
                  '应该是 ok——这一项在健康的设备上也会失败' % (name, got.get(name, '什么都没报')))
        for described, mutate, name in DEXOPT_BROKEN:
            broken = Path(workspace) / 'dexopt-broken.txt'
            text = mutate(DEXOPT_HEALTHY)
            # The mutation has to have happened, or this case asks nothing.
            check(text != DEXOPT_HEALTHY, '「%s」这个变异没有改到输出，用例什么都没测' % described)
            broken.write_text(text)
            got = outcomes(run(stub, 'answer', dexopt=broken).stdout)
            check(got.get(name) == 'FAIL',
                  '第七节的输出里%s时，「%s」报的是 %r，应该是 FAIL'
                  % (described, name, got.get(name, '什么都没报')))

        # The recheck probe, run against what ls really says in each case.
        for ls_out, ls_rc, want, what in (
                ('ls: /pw3-system-recheck: No such file or directory', '1', '~~', '没有这个文件'),
                ('/pw3-system-recheck', '0', 'FAIL', '文件在、属性没设、也没有日志'),
                ('ls: /pw3-system-recheck: Permission denied', '1', '--', '不许看')):
            got = outcomes(run(stub, 'answer',
                               extra={'STUB_LS_OUT': ls_out, 'STUB_LS_RC': ls_rc}).stdout)
            check(got.get('后台整镜像校验的结论') == want,
                  '设备上 ls 回答 %r（%s）时，「后台整镜像校验的结论」报的是 %r，应该是 %r'
                  % (ls_out, what, got.get('后台整镜像校验的结论', '什么都没报'), want))

        # And "no su" is a design only on a release build. The same missing su
        # on a debuggable build is an image that should have root and does not,
        # and the readings that need it must not be waved through as skipped.
        done = run(stub, 'answer', noroot=True,
                   extra={'STUB_BUILD_TYPE': 'userdebug', 'STUB_DEBUGGABLE': '1'})
        got = outcomes(done.stdout)
        for name in NEEDS_ROOT:
            check(got.get(name) == '--',
                  'userdebug 镜像上没有 su（坏了）时，「%s」报的是 %r，应该是 `--`——'
                  '按发行构建的设计豁免放过去，会让要 root 的检查一项都不挡'
                  % (name, got.get(name, '什么都没报')))

        # The exemption is an exemption only because `adb devices` answered it.
        # Anchored so that it cannot start covering something else.
        done = run(stub, 'rc')
        check(outcomes(done.stdout).get(ALLOWED_OK) == 'ok',
              '%r 没有报 ok，这个测试的唯一豁免项因此可能已经在豁免别的东西了' % ALLOWED_OK)

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
