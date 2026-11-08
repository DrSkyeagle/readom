#!/usr/bin/env python3
"""Every kernel-log tag this project prints is classified by userlog.c.

The installer copies the previous Android kernel log into a folder on the Kindle
drive that the person is expected to hand on, filtered by an allow-list in
`boot/launcher/userlog.c`. That list keeps `pw3-`, the prefix this project's own
patches and programs print under, so that a diagnostic added later is in the log
that gets read rather than dropped by a list nobody extended.

The price of a prefix is this test. A tag of ours that prints what somebody
else wrote -- an application's log line, a file name, a path from the user
store -- must be in DENY, and a new tag must be classified here before it is
printed, which is what makes "keeps everything of ours" a statement somebody
checked instead of a hope.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
USERLOG = ROOT / 'boot/launcher/userlog.c'

# Why each tag is safe to hand on, or why it is not. The reason is the point of
# the entry: a tag arrives here when somebody adds a printk, and the question to
# answer then is whether its text can only ever come from this project.
KEEP = {
    'pw3-usb': 'the cable, VBUS and the pull-up decision: flags and numbers (patch 1046)',
    'pw3-usb-diag': 'bounded UDC software state: counters and register values (patch 1022)',
    'pw3-preinit': "the pre-init stage's own failure reasons, fixed strings",
    'pw3-init-check': 'storage, USB and interface readiness, fixed strings',
    'pw3-handoff': "the handover's own init: rails, temperature, the display driver"
                   ' and the start screen, fixed strings with errno and sizes'
                   ' (boot/handoff-init/pw3-handoff-init.c, P())',
    'pw3-report-area': 'whether the report area was written and, when it was not, why:'
                       ' fixed strings, a path of ours and strerror()'
                       ' (BootReport.cpp, KernelLine())',
    'pw3-first-kernel': "the handoff module's own line before an emulator-only direct commit:"
                        ' fixed text and the two physical destinations, in an emulator build'
                        ' only (native-handoff.h, PWS_EMULATOR)',
}
FREE_TEXT = {
    'pw3-boot-report': 'not named in the whitelist, and since 2026-10-29 it reaches no kernel log '
                       'at all: its sink went with androidboot.pw3.diag (BootReport.cpp, Errors() '
                       'now counts per tag from a fixed list)',
}
# The two tags above are deliberately not one, and the second is deliberately not
# a name beginning with the first: DENY is matched by prefix, so a tag called
# `pw3-boot-report-area` would be dropped with the logcat relay. The check below
# that a KEEP tag is not denied is what holds them apart.

SOURCES = ('*.c', '*.cpp', '*.h', '*.patch')
TAG = re.compile(r'pw3-[a-z0-9]+(?:-[a-z0-9]+)*:')


def arrays(text):
    """ALLOW and DENY as they are written in userlog.c."""
    found = {}
    for name in ('ALLOW', 'DENY'):
        match = re.search(r'static const char \*const ' + name + r'\[\] = \{(.*?)\n\};', text, re.S)
        if not match:
            sys.exit('userlog.c: no ' + name + ' array')
        found[name] = re.findall(r'"((?:[^"\\]|\\.)*)"', match.group(1))
    return found['ALLOW'], found['DENY']


def tags():
    """The tags named anywhere in this tree, with where they came from.

    Whole text, not string literals: pairing quotes across a C file goes wrong
    at the first `'"'` and then silently misses the literals after it, which is
    how this test first passed while a tag was printed two lines below what it
    had found. A tag named in a comment counts too -- it is one
    somebody has to classify either way.
    """
    where = {}
    for pattern in SOURCES:
        for path in ROOT.rglob(pattern):
            if any(part in ('out', '.git') for part in path.parts):
                continue
            try:
                text = path.read_text(encoding='utf-8', errors='replace')
            except OSError:
                continue
            for tag in TAG.findall(text):
                where.setdefault(tag[:-1], set()).add(str(path.relative_to(ROOT)))
    return where


def main():
    allow, deny = arrays(USERLOG.read_text(encoding='utf-8'))
    found = tags()
    problems = []

    for tag, places in sorted(found.items()):
        listed = tag in KEEP or tag in FREE_TEXT
        if not listed:
            problems.append(
                tag + ' is printed in ' + ', '.join(sorted(places)) +
                ' and is not classified in this test. Say what its text can contain:'
                ' this project\'s own fixed text and numbers (KEEP), or something'
                ' somebody else wrote (FREE_TEXT, and add it to DENY in userlog.c).')
            continue
        denied = any(tag.startswith(entry) for entry in deny)
        if tag in FREE_TEXT and not denied:
            problems.append(tag + ' carries free text (' + FREE_TEXT[tag] +
                            ') and is not in DENY in userlog.c, so the installer'
                            ' would copy it into a folder meant to be handed on.')
        if tag in KEEP and denied:
            problems.append(tag + ' is in DENY in userlog.c but classified as safe here;'
                            ' one of the two is wrong.')
        if tag in KEEP and not any(tag.startswith(entry) for entry in allow):
            problems.append(tag + ' is classified as safe but no ALLOW entry matches it,'
                            ' so the one log that answers a question would drop it.')

    for tag in sorted(set(KEEP) | set(FREE_TEXT)):
        if tag not in found:
            problems.append(tag + ' is classified here but printed nowhere in the tree;'
                            ' remove the entry or the code that was meant to print it.')

    if problems:
        print('PW3_KLOG_TAGS_FAIL')
        for problem in problems:
            print('  ' + problem)
        return 1
    print('PW3_KLOG_TAGS_OK tags=%d kept=%d denied=%d' % (len(found), len(KEEP), len(FREE_TEXT)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
