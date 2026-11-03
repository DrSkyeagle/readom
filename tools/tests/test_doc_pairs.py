#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""The documents that exist in two languages, held to each other.

Four documents are written twice, once in Chinese and once in English, and
nothing held the two halves together. The English half drifted four times in
one sitting (2026-10-21), each time silently:

  * `README.en.md` said the device-side installer had "not yet run on the
    device" months after it had, with all twelve checks passing;
  * it pointed at `docs/known-issues.md`, the Chinese file, although
    `docs/known-issues.en.md` exists -- an English reader following the link
    lands in Chinese;
  * it dropped the pointer to the porting research and to `performance.md`,
    so the two documents most useful to a contributor were reachable only
    from the Chinese side;
  * its licence list lost the monochrome Noto Emoji entry (OFL-1.1) -- a font
    that ships in the image, in a repository meant to be published.

None of those is a typo. Each is a sentence that was changed on one side only,
and each would have been caught by comparing the two halves rather than
reading them. So:

  * the three documents that are translations of each other are held to the
    same shape -- the same heading levels in the same order, the same number of
    table rows, bullets and code fences. Prose length differs between the
    languages and is not compared;
  * both halves of those three point at the same documents, each in its own
    language: a pointer added on one side has to be added on the other;
  * no half points at the other language's copy of a document that exists in
    its own, except the deliberate pointer to its own counterpart;

Bare file names in prose (`pw3.mk`, `userlog.c`) and paths into the AOSP tree
are not checked: they are shorthand, not links, and 603 of them are written
that way across 34 documents.
"""
import re
import sys
import traceback
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
# Chinese, English. The first three are translations of each other; open-items
# is the declared exception, handled on its own below.
STRICT = [('README.md', 'README.en.md'),
          # The per-release record. It is the one place in the repository that
          # says what a version changed and how far it was verified, and the
          # website's log and the Release notes are written from it, so the two
          # halves cannot be allowed to say different things.
          ('CHANGELOG.md', 'CHANGELOG.en.md'),
          ('docs/install.md', 'docs/install.en.md'),
          ('docs/known-issues.md', 'docs/known-issues.en.md'),
          # "What is still missing" was held loosely while it also carried the
          # engineering log and the two halves could not be the same length.
          # The log is its own document now, so this one is held in full.
          ('docs/open-items.md', 'docs/open-items.en.md'),
          ('docs/README.md', 'docs/README.en.md'),
          ('SECURITY.md', 'SECURITY.en.md'),
          ('CONTRIBUTING.md', 'CONTRIBUTING.en.md'),
          # The Contributor Covenant and its own Chinese translation, with the
          # same means of reporting filled into both.
          ('CODE_OF_CONDUCT.md', 'CODE_OF_CONDUCT.en.md'),
          # The licence inventory. A published repository whose licence list
          # exists in one language only is a gap for the reader who most needs
          # it, so this one is held to the same shape as the rest.
          ('THIRD_PARTY.md', 'THIRD_PARTY.en.md'),
          # The design records. They are for somebody changing the system
          # rather than installing it, so they were Chinese only for a long
          # time; each one that gets an English half joins this list, so the
          # two cannot drift apart afterwards.
          ('docs/design/README.md', 'docs/design/README.en.md'),
          ('docs/design/eink-ui.md', 'docs/design/eink-ui.en.md'),
          ('docs/design/usb-mtp.md', 'docs/design/usb-mtp.en.md'),
          ('docs/design/diagnostics.md', 'docs/design/diagnostics.en.md'),
          ('docs/design/app-install.md', 'docs/design/app-install.en.md'),
          ('docs/design/release-build.md', 'docs/design/release-build.en.md'),
          ('docs/design/performance.md', 'docs/design/performance.en.md'),
          ('docs/design/porting-to-other-models.md',
           'docs/design/porting-to-other-models.en.md')]
PAIRS = STRICT
PAIRED = {name for pair in PAIRS for name in pair}

checks = 0
failures = []


def check(ok, what):
    global checks
    checks += 1
    if not ok:
        failures.append(what)


def shape(text):
    """What a translation has to keep: structure, not wording."""
    heads, rows, bullets, fences = [], 0, 0, 0
    for line in text.splitlines():
        if line.startswith('#'):
            heads.append(len(line) - len(line.lstrip('#')))
        elif line.startswith('|'):
            rows += 1
        elif re.match(r'(  )*- ', line):
            bullets += 1
        elif line.startswith('```'):
            fences += 1
    return heads, rows, bullets, fences


def doc_refs(path, text):
    """The paired documents this one points at, as repository paths.

    Only names that resolve to a document in this repository count; a bare
    `pw3.mk` in prose does not.
    """
    out = set()
    for match in re.finditer(r'`([A-Za-z0-9_][A-Za-z0-9_./-]*\.md)`', text):
        ref = match.group(1)
        for candidate in (ref, 'docs/' + ref, 'docs/design/' + ref,
                          str(Path(path).parent / ref)):
            if candidate in PAIRED or (ROOT / candidate).is_file():
                out.add(candidate)
                break
    return out


def canonical(ref):
    """A document's name, whichever language it is in."""
    return ref[:-len('.en.md')] + '.md' if ref.endswith('.en.md') else ref


def counterpart(name):
    return name[:-len('.md')] + '.en.md' if not name.endswith('.en.md') \
        else name[:-len('.en.md')] + '.md'


def main():
    text = {}
    for name in sorted(PAIRED):
        path = ROOT / name
        check(path.is_file(), '%s is missing' % name)
        text[name] = path.read_text(encoding='utf-8') if path.is_file() else ''
        check(len(text[name]) > 500, '%s is empty or a stub' % name)

    for zh, en in STRICT:
        a, b = shape(text[zh]), shape(text[en])
        for value, label in zip(range(4),
                                ('heading levels', 'table rows', 'bullets',
                                 'code fences')):
            check(a[value] == b[value],
                  '%s and %s differ in %s: %s against %s. One side was '
                  'changed without the other'
                  % (zh, en, label, a[value] if value else len(a[0]),
                     b[value] if value else len(b[0])))

        # The same documents pointed at from both halves, each in its own
        # language. A reference to the pair's own counterpart is deliberate
        # ("English: README.en.md") and does not count as a document pointed at.
        refs = {}
        for name in (zh, en):
            refs[name] = {canonical(r) for r in doc_refs(name, text[name])
                          if canonical(r) != canonical(name)}
        only_zh = sorted(refs[zh] - refs[en])
        only_en = sorted(refs[en] - refs[zh])
        check(not only_zh and not only_en,
              '%s and %s do not point at the same documents: only the Chinese '
              'points at %s, only the English at %s'
              % (zh, en, only_zh or 'nothing', only_en or 'nothing'))

    # Nobody sends a reader into the other language when their own exists.
    for name in sorted(PAIRED):
        own = counterpart(name)
        for ref in sorted(doc_refs(name, text[name])):
            if ref in PAIRED and ref != own and ref != name:
                check(ref.endswith('.en.md') == name.endswith('.en.md'),
                      '%s points at %s, the other language, although %s exists'
                      % (name, ref, counterpart(ref)))

    print('%d checks, %d failed (%d documents in %d pairs)'
          % (checks, len(failures), len(PAIRED), len(PAIRS)))
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
