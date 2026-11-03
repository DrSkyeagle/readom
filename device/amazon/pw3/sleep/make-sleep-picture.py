#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""The sleep pictures and the pieces of the line written on them (README.md).

  make-sleep-picture.py pictures ART.png TITLE_FONT.ttf OUTDIR
      sleep-en.pgm: the art as it is (its own title, "Reading knows no forbidden zones");
      sleep-zh.pgm: the same with that title replaced by 读书无禁区.
  make-sleep-picture.py glyphs FONT.ttf OUT.pgm OUT.txt
      the pieces of the line "休眠于 21:47 · 10月15日 · 电量 63%" (epdc/SleepText.h).

Needs Pillow and numpy. Output: 8-bit binary PGM, the screen's size (1072 x 1448, PW3).
"""
import math, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont

WIDTH, HEIGHT = 1072, 1448
# The art's own title, in the art's pixels (1086 x 1448), with a margin: the
# box whose background is filled in for the Chinese picture.
ART_TITLE = (285, 187, 810, 318)


def pgm(picture, path):
    with open(path, 'wb') as f:
        f.write(b'P5\n%d %d\n255\n' % picture.size)
        f.write(picture.tobytes())


def pictures(art_path, font, out):
    art = Image.open(art_path).convert('L')
    if art.size[1] != HEIGHT or art.size[0] < WIDTH:
        raise SystemExit('the art must be %d high and at least %d wide' % (HEIGHT, WIDTH))
    left = (art.size[0] - WIDTH) // 2
    en = art.crop((left, 0, left + WIDTH, HEIGHT))
    pgm(en, out + '/sleep-en.pgm')
    v = np.asarray(en).astype(np.float64).copy()
    x0, y0, x1, y1 = ART_TITLE[0] - left, ART_TITLE[1], ART_TITLE[2] - left, ART_TITLE[3]
    # The paper around the title is one flat tone: fill the box with it.
    ring = np.concatenate([v[y0 - 6:y0 - 2, x0:x1].ravel(), v[y1 + 2:y1 + 6, x0:x1].ravel()])
    v[y0:y1, x0:x1] = np.median(ring)
    zh = Image.fromarray(np.round(v).astype(np.uint8))
    face = ImageFont.truetype(font, 78)
    ImageDraw.Draw(zh).text(((x0 + x1) / 2, (y0 + y1) / 2), '读书无禁区', font=face, fill=45, anchor='mm')
    pgm(zh, out + '/sleep-zh.pgm')


# Every piece of the line, drawn once, side by side on one strip.
TOKENS = (list('0123456789') + [':', '%', ' ', ' · ', '休眠于', '月', '日', '电量', 'Asleep at', 'Battery'] +
          ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'])


def glyphs(font, path, index):
    small = ImageFont.truetype(font, 32)
    ascent, descent = small.getmetrics()
    widths = [max(1, math.ceil(small.getlength(t))) for t in TOKENS]
    strip = Image.new('L', (sum(widths), ascent + descent), 0)
    draw = ImageDraw.Draw(strip)
    x, rows = 0, []
    for token, w in zip(TOKENS, widths):
        draw.text((x, ascent), token, font=small, fill=255, anchor='ls')   # coverage: 255 is ink
        rows.append('%d %d %s' % (x, w, token))
        x += w
    pgm(strip, path)
    with open(index, 'w', encoding='utf-8') as f:
        f.write('pw3-sleep-glyphs 1\n' + '\n'.join(rows) + '\n')


def main():
    args = sys.argv[1:]
    if len(args) == 4 and args[0] == 'pictures':
        pictures(*args[1:])
    elif len(args) == 4 and args[0] == 'glyphs':
        glyphs(*args[1:])
    else:
        raise SystemExit(__doc__)


if __name__ == '__main__':
    main()
