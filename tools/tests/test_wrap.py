#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Where the installer's screens break a line.

wrap() in installer/screens/render_screens.py had no test of its own: it ran
only inside a full render, which needs Pillow and the AOSP tree's fonts, so
nothing looked at the rule itself. The rule then produced this, on the E202
screen of V80, in a preview nobody would have opened if the screen had not
just been changed:

    ...然后再点书库里的“Android
    ”。

The rule says a closing mark may not begin a line, and it moved one token down
to avoid that -- but the token it moved was 」, which may not begin a line
either. One step is not enough; it has to keep moving until the line's first
token is one that may start a line.

The widths here come from a stand-in that charges 2 for a character outside
Latin-1 and 1 for everything else, which is what a monospaced CJK font does
and is enough to place a break. What it cannot answer is whether real text in
a real font fits the panel; render_screens.py answers that, by refusing to
write a screen too tall for it.
"""
import importlib.util
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCREENS = ROOT / 'installer/screens/render_screens.py'

checks = 0
failures = []


def check(ok, what):
    global checks
    checks += 1
    if not ok:
        failures.append(what)


class Ruler:
    """Pillow's ImageDraw, as much of it as wrap() uses."""
    @staticmethod
    def textlength(text, font=None):
        return sum(2 if ord(c) > 0xFF else 1 for c in text)


def load():
    """render_screens.py without Pillow, which the host need not have.

    Only the module-level `from PIL import ...` needs it, and nothing wrap()
    touches comes from there.
    """
    for name in ('PIL', 'PIL.Image', 'PIL.ImageDraw', 'PIL.ImageFont'):
        sys.modules.setdefault(name, type(sys)(name))
    pil = sys.modules['PIL']
    for attr in ('Image', 'ImageDraw', 'ImageFont'):
        setattr(pil, attr, sys.modules['PIL.' + attr])
    spec = importlib.util.spec_from_file_location('pw3_render_screens', SCREENS)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    screens = load()

    def lines(text, width, cjk=True):
        return screens.wrap(Ruler, text, None, width, cjk)

    # The one that was wrong: two closing marks at a break.
    broken = lines('请点书库里的“Readom”。', 24)
    check(not any(line and line[0] in screens.NO_LINE_START for line in broken),
          'a line begins with a mark that may not begin one: %r' % broken)
    check(''.join(broken).replace(' ', '') == '请点书库里的“Readom”。'.replace(' ', ''),
          'wrapping changed the text: %r' % broken)

    # One closing mark, which the single step already handled.
    one = lines('这是一句话，后面还有。', 12)
    check(not any(line and line[0] in screens.NO_LINE_START for line in one),
          'a line begins with a mark that may not begin one: %r' % one)

    # An opening mark may not end a line.
    opened = lines('看这里“这是引号里的内容”', 16)
    check(not any(line and line[-1] in screens.NO_LINE_END for line in opened),
          'a line ends with a mark that may not end one: %r' % opened)

    # Three in a row: 」）。 -- the loop has to keep going.
    three = lines('他说（这是一句话）。', 14)
    check(not any(line and line[0] in screens.NO_LINE_START for line in three),
          'a line begins with a mark that may not begin one: %r' % three)

    # A space at the break, which carries no visible character. Both of these
    # came out wrong until 2026-10-30: the first began a line with the space
    # that the break fell on, because backtracking returns it stuck to the mark
    # after it; the second let the space stand in for the line's last character,
    # hiding the opening bracket from the test above, and the rstrip then left
    # the bracket ending the line. A space before punctuation is not something
    # today's screens write, but the text is rewritten often.
    spaced = lines('甲乙 ，丙', 5)
    check(not any(line.startswith(' ') for line in spaced),
          'a line begins with a space: %r' % spaced)
    check(''.join(spaced).replace(' ', '') == '甲乙，丙',
          'wrapping changed the text: %r' % spaced)
    bracket = lines('甲（ 乙', 5)
    check(not any(line and line[-1] in screens.NO_LINE_END for line in bracket),
          'a line ends with a mark that may not end one: %r' % bracket)

    # No empty lines, wherever the spaces are. text_block() gives each line in
    # the list a line's height, so an invisible one pushes the screen down by a
    # line -- and the screens are refused when they are too tall for the panel.
    # Text with nothing visible in it is no lines at all, not one empty one.
    for text, width in [('甲 ', 2), ('     ', 5), ('甲乙 ，丙', 5), ('甲（   乙', 5), ('  甲  乙  ', 3)]:
        out = lines(text, width)
        check(all(line for line in out), 'an empty line came out of %r: %r' % (text, out))
        check(bool(out) == bool(text.strip()), 'lines came out of nothing, or none out of something: %r -> %r' % (text, out))
        check(''.join(out).replace(' ', '') == text.replace(' ', ''),
              'wrapping changed the text of %r: %r' % (text, out))

    # A Latin run is one token and is not split.
    latin = lines('文件叫 readom-system-v80.img.zst 请拷进去', 20)
    check(any('readom-system-v80.img.zst' in line for line in latin),
          'the file name was split across lines: %r' % latin)

    # Nothing is lost or duplicated, on text made only of marks that cannot
    # start a line, where the loop could otherwise eat the whole line.
    marks = lines('啊。。。。。。。。。。。。。。。。', 6)
    check(''.join(marks) == '啊。。。。。。。。。。。。。。。。',
          'wrapping changed the text: %r' % marks)
    check(all(line for line in marks), 'an empty line came out: %r' % marks)

    # English wraps between words and keeps them whole.
    english = screens.wrap(Ruler, 'Connect Wi-Fi, or copy the package again.', None, 20, False)
    check(all(len(word) <= 20 or ' ' not in word for line in english for word in [line]),
          'an English line is over the width: %r' % english)
    check(' '.join(' '.join(english).split()) == 'Connect Wi-Fi, or copy the package again.',
          'wrapping changed the text: %r' % english)

    print('%d checks, %d failed' % (checks, len(failures)))
    for problem in failures:
        print('  FAIL %s' % problem)
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
