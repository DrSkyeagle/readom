#!/usr/bin/env python3
"""The screens the device shows, and what is supposed to explain them.

The person this is built for has a cable and no way to ask anyone. What they
can read is whatever the Kindle puts on its own panel, so the panel is the
product and the install guide is the copy of it. Three things have to hold:

  * every screen says its piece in both languages -- an English reader who
    meets a screen that lost its English half is as stuck as one who meets a
    blank;
  * every screen constant the code names has a screen behind it, and every
    screen built into the release is shown by some code path;
  * every error code that can appear is in all three documents. This one has
    already failed once: E104, the code for "unplug the cable", which is the
    likeliest thing a first-time installer sees, was in the code, on the panel
    and in installer/README.md, and in neither install guide.

The screen definitions are read with ast, not a regular expression: the lists
carry %-formatting and string concatenation, and a pattern that quietly stops
matching would leave this reporting a pass over nothing.
"""
import ast
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
SCREENS = ROOT / 'installer/screens/render_screens.py'
DOCS = (ROOT / 'docs/install.md', ROOT / 'docs/install.en.md',
        ROOT / 'installer/README.md')
# Where a screen is chosen. Not a wildcard: a new place that shows screens
# should be added here deliberately, with its own look at what it shows.
CODE = (ROOT / 'boot/launcher/helper.c',      # before Android starts
        ROOT / 'installer/install/kindle.c')  # during installation
# PW3_SCREEN_* names that are not screens: the layout constants the generator
# writes into the same header. They have to be named, because the alternative
# -- keeping only the names that are screens -- is what made the check below
# unable to fail.
NOT_SCREENS = {'W', 'H', 'COUNT', 'BAR_GAP', 'BAR_H', 'BAR_LINE',
               'NUMBER_GAP', 'REGION_H', 'REGION_W', 'REGION_X', 'REGION_Y'}

checks = 0
failures = []


def check(ok, what):
    global checks
    checks += 1
    if not ok:
        failures.append(what)


def cjk(text):
    return any('㐀' <= c <= '鿿' or '＀' <= c <= '￯' for c in text)


def strings(node, known=None):
    """Every string under a node, in source order, names followed through.

    A screen's words may be written in the tuple or in a name above it: the
    BOOT screen's are in constants because the handover renders that same
    screen from them (boot/handoff-init/make-start-picture.py). Reading only
    the literals inside the tuple would find none of them there and report a
    pass over nothing, which is the failure this file is built to avoid.
    """
    out = []
    for n in ast.walk(node):
        if isinstance(n, ast.Constant) and isinstance(n.value, str):
            out.append(n.value)
        elif known and isinstance(n, ast.Name) and n.id in known:
            out += known[n.id]
    return out


def assigned_strings(tree):
    """Each name made of strings, with its strings, in the order they are written."""
    found = {}
    for node in sorted((n for n in ast.walk(tree) if isinstance(n, ast.Assign)),
                       key=lambda n: n.lineno):
        for target in node.targets:
            if isinstance(target, ast.Name):
                texts = strings(node.value, found)
                if texts:
                    found[target.id] = texts
            elif (isinstance(target, ast.Tuple) and isinstance(node.value, ast.Tuple)
                  and len(target.elts) == len(node.value.elts)):
                for name, value in zip(target.elts, node.value.elts):
                    texts = strings(value, found)
                    if isinstance(name, ast.Name) and texts:
                        found[name.id] = texts
    return found


def screen_defs(path):
    """(name, [strings]) for each tuple in the two screen lists."""
    tree = ast.parse(path.read_text())
    known = assigned_strings(tree)
    out = []
    for node in ast.walk(tree):
        if not isinstance(node, ast.Assign) or not isinstance(node.value, ast.List):
            continue
        if not any(isinstance(t, ast.Name) and t.id in ('s', 'errors')
                   for t in node.targets):
            continue
        for item in node.value.elts:
            if not isinstance(item, ast.Tuple) or not item.elts:
                continue
            head = item.elts[0]
            if isinstance(head, ast.Constant) and isinstance(head.value, str):
                out.append((head.value, strings(item, known)[1:]))
    return out


def main():
    if not SCREENS.is_file():
        print('no %s' % SCREENS)
        return 1
    defs = screen_defs(SCREENS)
    names = [n for n, _ in defs]
    check(len(names) >= 20,
          'only %d screens parsed out of %s; the reader is probably wrong'
          % (len(names), SCREENS.name))
    check(len(names) == len(set(names)),
          'a screen is defined twice: %s'
          % sorted({n for n in names if names.count(n) > 1}))

    # Both languages, on every screen. A screen with no words at all (a title
    # only) still has its title in both.
    for name, texts in defs:
        zh = [t for t in texts if cjk(t)]
        en = [t for t in texts if t.strip() and not cjk(t)
              and re.search('[A-Za-z]{3}', t)]
        check(bool(zh), 'screen %s has no Chinese text' % name)
        check(bool(en), 'screen %s has no English text' % name)

    shown = {n for n in names if re.fullmatch(r'E[1-4][0-9]{2}', n)}
    check(len(shown) >= 10, 'only %d error screens found' % len(shown))

    for doc in DOCS:
        if not doc.is_file():
            check(False, '%s is missing' % doc)
            continue
        listed = set(re.findall(r'\bE[1-4][0-9]{2}\b', doc.read_text()))
        check(listed == shown,
              '%s does not explain the same codes the device shows: '
              'on screen only %s, in the document only %s'
              % (doc.relative_to(ROOT), sorted(shown - listed) or 'none',
                 sorted(listed - shown) or 'none'))

    # The size of the download, as the three documents state it. The screen
    # computes it from the payload manifest and is therefore always right; the
    # documents are prose, and the person compares what they downloaded against
    # whichever one they are reading. It was already wrong in one of the three:
    # 196 MB where the others and the screen said 195.
    sizes = {}
    for doc in DOCS:
        if not doc.is_file():
            continue
        # Only the lines that name the payload: these documents give other
        # sizes too, and a check that collected all of them would compare the
        # download against the entry script.
        found = [m for line in doc.read_text().splitlines()
                 if 'readom-system-' in line
                 for m in re.findall(r'(?:约|about)\s*([0-9]{2,4})\s*MB', line)]
        if found:
            sizes[str(doc.relative_to(ROOT))] = sorted(set(found))
    check(len(sizes) == len(DOCS),
          'the download size is stated in %d of the %d documents, so the one '
          'that does not state it cannot be compared: %s'
          % (len(sizes), len(DOCS), sorted(sizes)))
    stated = {size for found in sizes.values() for size in found}
    check(len(stated) <= 1,
          'the documents do not agree on the size of the download: %s. The '
          'person compares the file they downloaded against the document they '
          'are reading' % sizes)

    # The screens and the code that shows them. There is no third list to
    # compare with: the generator writes the enum and the bitmaps from this
    # same list, so the names in the generated header agree with the drawings
    # by construction and checking them against each other settles nothing.
    used = set()
    for path in CODE:
        if not path.is_file():
            check(False, '%s is missing' % path)
            continue
        used.update(re.findall(r'PW3_SCREEN_([A-Z][A-Z0-9_]*)\b', path.read_text()))
    # Only the layout constants are dropped. Intersecting with the screens
    # that are drawn, which is what this did, removed exactly the names the
    # next check is looking for, so it could not fail: PW3_SCREEN_E999 in
    # kindle.c passed all 52 checks.
    used -= NOT_SCREENS
    check(used <= set(names),
          'the code shows screens that are not drawn: %s' % sorted(used - set(names)))
    check(set(names) <= used,
          'these screens are built into every release and nothing shows them: %s'
          % sorted(set(names) - used))

    print('%d checks, %d failed (%d screens, %d error codes)'
          % (checks, len(failures), len(names), len(shown)))
    for f in failures:
        print('  FAIL %s' % f)
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
