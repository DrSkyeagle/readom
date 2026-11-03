#!/usr/bin/env python3
"""The name the entry gives a run folder, and the name its own code will delete.

`ours()` in boot/launcher/userlog.c decides whether a folder on the Kindle drive
was written by this launcher, which is to say whether it may be deleted with
everything inside it. Until 2026-10-21 it decided that by the alphabet of the
name, and `2026` passed: four older folders were enough for rotation to delete a
folder of the reader's and every plain file in it.

It is decided by shape now, and a shape can be wrong in the other direction too.
Too tight and rotation stops recognising its own folders, so the drive fills up
instead of being kept to five runs -- a slower failure and a harder one to see.

So the shape and the thing that produces it are held together by two tests.
boot/launcher/tests/userlog_test.c builds names with strftime and a process id
over dates and pid widths that bracket what the shell can emit, and requires
ours() to count every one of them. This one requires the shell to go on producing
that: the format string here has to be the one the C test builds from, so that
changing either file without the other fails rather than drifting.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
TEMPLATE = ROOT / 'boot/launcher/launcher-template.sh'
C_TEST = ROOT / 'boot/launcher/tests/userlog_test.c'
VALIDATOR = ROOT / 'boot/launcher/userlog.c'


def main():
    problems = []
    template = TEMPLATE.read_text(encoding='utf-8')
    c_test = C_TEST.read_text(encoding='utf-8')
    validator = VALIDATOR.read_text(encoding='utf-8')

    # The default name, from the one line that settles it.
    out = re.search(r'^OUT=\$\{1:-@LOGPREFIX@(.*)\}$', template, re.M)
    if not out:
        return fail(['launcher-template.sh: no OUT=${1:-@LOGPREFIX@...} line, so what the '
                     'launcher names a run folder could not be read'])
    produced = out.group(1)
    found = re.search(r'date \+([^\s)"\']+)', produced)
    shell_format = found.group(1) if found else None
    if not shell_format:
        problems.append('launcher-template.sh: the default name does not come from `date +FORMAT`: '
                        + produced)
    if not produced.endswith('-$$'):
        problems.append('launcher-template.sh: the default name no longer ends with the process '
                        'id after a hyphen, which is half of the shape ours() accepts: ' + produced)

    # The same format, in the test that requires ours() to accept it.
    c_formats = sorted(set(re.findall(r'strftime\([^,]+,[^,]+,\s*"([^"]+)"', c_test)))
    if len(c_formats) != 1:
        problems.append('userlog_test.c: expected exactly one strftime format building a folder '
                        'name, found %d: %s' % (len(c_formats), c_formats))
    elif shell_format and shell_format != c_formats[0]:
        problems.append('the shell names a folder with `date +%s` while userlog_test.c requires '
                        'ours() to accept `strftime("%s")`; one of the two was changed without '
                        'the other' % (shell_format, c_formats[0]))

    # And that the validator is the shape-checking one, not the alphabet again.
    body = re.search(r'static int ours\(const char \*name\)\s*\{(.*?)\n\}', validator, re.S)
    if not body:
        problems.append('userlog.c: ours() could not be found, so nothing here checked it')
    else:
        text = body.group(1)
        if "'T'" not in text or "'-'" not in text:
            problems.append("userlog.c: ours() no longer looks for the timestamp's T and the "
                            'hyphen before the process id, so it is not checking the shape')
        if 'n < 17' not in text:
            problems.append('userlog.c: ours() no longer requires the 17 characters the shortest '
                            'name the shell can produce has (15 of timestamp, a hyphen, one digit)')

    if problems:
        return fail(problems)
    print('PW3_LAUNCHER_NAME_OK format=%s-<pid>, checked against ours() and the C test'
          % shell_format)
    return 0


def fail(problems):
    print('PW3_LAUNCHER_NAME_FAIL')
    for problem in problems:
        print('  ' + problem)
    return 1


if __name__ == '__main__':
    sys.exit(main())
