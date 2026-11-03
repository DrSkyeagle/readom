#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""What the repository says about the licence of every file it would publish.

The README says `reuse lint` passes. That tool is installed on neither machine
this project builds on, so the claim was last true whenever someone ran it
somewhere, and nothing noticed when a file was added. One had already gone
wrong: `device/amazon/pw3/fonts/fonts.xml` is AOSP's file with 101 families
deleted, and with no entry of its own it fell to the catch-all at the top of
REUSE.toml -- so the repository would have published Google's file under this
project's copyright.

So the properties are checked here, against the files git would publish
(tracked, plus untracked and not ignored: the fetched fonts and APKs are
ignored and are nobody's to license here):

  * REUSE.toml is read, and where tomllib exists the simple reader below is
    held to it field by field. That is the point of the comparison: the reader
    has to run on Python 3.10 in the build VM, and a hand-written reader that
    quietly stops understanding the file would make every check below pass over
    nothing;
  * the first annotation covers `**`, so no file can be left unlicensed;
  * every annotation carries both a copyright and a licence, and its paths
    match something -- a stale path after a rename says it covers a file that
    is not there;
  * every licence named, in REUSE.toml or in a file's own SPDX header, has its
    text in LICENSES/, and every text in LICENSES/ is named by something. The
    first of those is what a publisher is obliged to ship; the second is how a
    licence that no longer applies gets noticed.
"""
import re
import subprocess
import sys
import traceback
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
TOML = ROOT / 'REUSE.toml'
LICENSES = ROOT / 'LICENSES'
# A whole line that is nothing but an SPDX statement, with or without a comment
# marker. Prose that mentions an identifier does not match, which is why this
# is anchored rather than searched.
# REUSE-IgnoreStart
HEADER = re.compile(r'^\s*(?:#|//|/\*|\*|;|--|<!--|rem\s)?\s*'
                    r'SPDX-License-Identifier:\s*([A-Za-z0-9.\-+]+)'
                    r'\s*(?:\*/|-->)?\s*$')
# REUSE-IgnoreEnd
# Anchored to the end of the line on purpose: `reuse lint` reads everything
# after the tag as the expression, so `MIT; and then a sentence` is an invalid
# expression to it even though this pattern used to accept it. Three files in
# tools/kernel-smoke were written that way and were found by the official tool
# on 2026-10-29, not by this test.

checks = 0
failures = []


def check(ok, what):
    global checks
    checks += 1
    if not ok:
        failures.append(what)


def strip_comment(line):
    """The line without a trailing comment. Exact for this file: no string in
    it contains a '#', so the first '#' outside quotes ends the content."""
    out, quoted = [], False
    for char in line:
        if char == '"':
            quoted = not quoted
        if char == '#' and not quoted:
            break
        out.append(char)
    return ''.join(out)


def read_annotations(text):
    """[[annotations]] blocks as {path, copyright, licence}, simply."""
    blocks, current, key, buffer = [], None, None, ''

    def finish():
        nonlocal key, buffer
        if key is not None:
            items = re.findall(r'"([^"]*)"', buffer)
            if key == 'path':
                current['path'] = items
            elif key == 'SPDX-FileCopyrightText':
                current['copyright'] = items
            elif key == 'SPDX-License-Identifier':
                current['licence'] = items[0] if items else None
        key, buffer = None, ''

    for raw in text.splitlines():
        line = strip_comment(raw)
        if line.strip() == '[[annotations]]':
            finish()
            current = {'path': [], 'copyright': [], 'licence': None}
            blocks.append(current)
            continue
        if current is None:
            continue
        match = re.match(r'\s*([A-Za-z0-9-]+)\s*=\s*(.*)$', line)
        if match:
            finish()
            key, buffer = match.group(1), match.group(2)
        elif key is not None:
            buffer += ' ' + line
        if key is not None and buffer.count('[') == buffer.count(']'):
            finish()
    finish()
    return blocks


def with_tomllib(text):
    try:
        import tomllib
    except ImportError:
        return None
    data = tomllib.loads(text)
    out = []
    for item in data.get('annotations', []):
        paths = item.get('path', [])
        rights = item.get('SPDX-FileCopyrightText', [])
        out.append({
            'path': [paths] if isinstance(paths, str) else list(paths),
            'copyright': [rights] if isinstance(rights, str) else list(rights),
            'licence': item.get('SPDX-License-Identifier'),
        })
    return out


def ids(expression):
    """The licence identifiers in an SPDX expression.

    REUSE.toml holds compound ones -- "Apache-2.0 AND GPL-3.0-only", and one
    with five terms -- and a publisher owes the text of every term, not of the
    sentence. WITH is not handled: no expression here uses it, and guessing
    whether the name after it is a licence or an exception would be the kind of
    silent wrong answer this file exists to prevent, so it is reported instead.
    """
    if not expression:
        return set()
    cleaned = expression.replace('(', ' ').replace(')', ' ')
    parts = [w for w in cleaned.split() if w.upper() not in ('AND', 'OR')]
    if any(w.upper() == 'WITH' for w in cleaned.split()):
        return {expression}          # names no text; the check below says so
    return set(parts)


def as_regex(glob):
    """A REUSE path glob as a regular expression over repository paths."""
    out = ''
    i = 0
    while i < len(glob):
        if glob.startswith('**', i):
            out += '.*'
            i += 2
        elif glob[i] == '*':
            out += '[^/]*'
            i += 1
        elif glob[i] == '?':
            out += '[^/]'
            i += 1
        else:
            out += re.escape(glob[i])
            i += 1
    return re.compile('^' + out + '$')


def published_files():
    """What git would publish: tracked, plus untracked and not ignored."""
    names = set()
    for args in (['ls-files'], ['ls-files', '--others', '--exclude-standard']):
        done = subprocess.run(['git', '-C', str(ROOT)] + args,
                              capture_output=True, text=True)
        if done.returncode:
            return None, done.stderr.strip()[:200]
        names.update(n for n in done.stdout.splitlines() if n)
    return sorted(names), None


def main():
    check(TOML.is_file(), 'REUSE.toml is missing')
    check(LICENSES.is_dir(), 'LICENSES/ is missing')
    if not TOML.is_file() or not LICENSES.is_dir():
        return report()
    text = TOML.read_text(encoding='utf-8')

    simple = read_annotations(text)
    check(len(simple) >= 10,
          'only %d annotations were read out of REUSE.toml; the reader is '
          'probably wrong' % len(simple))
    real = with_tomllib(text)
    if real is None:
        print('no tomllib here: the simple reader is not cross-checked '
              '(it is on the machine that has one)')
    else:
        check(simple == real,
              'the simple reader and tomllib do not agree on REUSE.toml. '
              'Simple read %d annotations, tomllib %d; first difference: %s'
              % (len(simple), len(real),
                 next((('%r against %r' % (a, b))
                       for a, b in zip(simple, real) if a != b), 'length only')))

    check(bool(simple) and simple[0]['path'] == ['**'],
          'the first annotation is not the catch-all "**", so a file with no '
          'header and no entry of its own would carry no licence at all '
          '(first paths: %s)' % (simple[0]['path'] if simple else None))

    names, problem = published_files()
    check(names is not None, 'git could not list the files: %s' % problem)
    if names is None:
        return report()
    check(len(names) > 300, 'only %d files listed; that is not this repository'
          % len(names))

    used = set()
    for i, block in enumerate(simple):
        where = 'annotation %d (%s)' % (i + 1, ', '.join(block['path'])[:70])
        check(bool(block['copyright']), '%s has no SPDX-FileCopyrightText' % where)
        check(bool(block['licence']), '%s has no SPDX-License-Identifier' % where)
        if block['licence']:
            used |= ids(block['licence'])
            check('WITH' not in block['licence'].upper(),
                  '%s uses WITH in its licence expression, which this check '
                  'does not read' % where)
        for glob in block['path']:
            if glob == '**':
                continue
            pattern = as_regex(glob)
            check(any(pattern.match(n) for n in names),
                  '%s: the path %r matches no file that would be published' %
                  (where, glob))

    headers = {}
    for name in names:
        path = ROOT / name
        if not path.is_file() or path.stat().st_size > 2_000_000:
            continue
        try:
            with path.open('r', encoding='utf-8', errors='strict') as stream:
                for _ in range(20):
                    line = stream.readline()
                    if not line:
                        break
                    found = HEADER.match(line)
                    if found:
                        headers[name] = found.group(1)
                        break
        except (UnicodeDecodeError, OSError):
            continue
    for name, expression in headers.items():
        used |= ids(expression)
    # The count is of files carrying a header, not of distinct licences: most
    # files here are covered by REUSE.toml instead, so a handful of identifiers
    # is normal and would not show that the reader had stopped matching.
    check(len(headers) >= 40,
          'only %d files were found to carry an SPDX header; the reader is '
          'probably wrong' % len(headers))

    have = {p.stem for p in LICENSES.glob('*.txt')}
    missing = sorted(used - have)
    check(not missing,
          'these licences are named but their text is not in LICENSES/: %s '
          '(a publisher has to ship the text of every licence it names)'
          % missing)
    unused = sorted(have - used)
    check(not unused,
          'these texts are in LICENSES/ and nothing names them any more: %s'
          % unused)

    print('%d checks, %d failed (%d annotations, %d files, %d licences)'
          % (checks, len(failures), len(simple), len(names), len(used)))
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
