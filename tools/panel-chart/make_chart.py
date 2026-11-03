#!/usr/bin/env python3
"""Test chart for the PW3 panel, 1072x1448: a white field, a black field, the
16 grey levels as steps, and lines of text in Android's usual greys.

  make_chart.py OUT_DIR [CONTRAST]

Writes two pictures of the same chart:
  pw3-chart-stock.png    for the stock system (eips -g): a step's grey is its
                         level, value 17 * level;
  pw3-chart-android.png  for Android: a step's grey is a luma the display
                         module's tone table (epdc/Tone.cpp, CONTRAST, default
                         12) shows as that level.
Both reach the panel as the same 16 levels, so a photograph of each, taken in
the same light, compares how the two systems drive the same panel: its white,
its black and the steps between. The text lines are not matched: each system
shows the greys named beside them the way it shows any content.
"""
import sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

W, H = 1072, 1448
def lum(v):
    v /= 255.0
    return v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4
def light(y): return 116 * y ** (1 / 3) - 16 if y > 216 / 24389 else y * 24389 / 27
def table(contrast):
    b = light(1.0 / contrast)
    return [round(15 * min(1, max(0, (light(lum(v)) - b) / (100 - b)))) for v in range(256)]

def chart(grey_of_level, title):
    im = Image.new('L', (W, H), 255)
    d = ImageDraw.Draw(im)
    try:
        font = ImageFont.truetype('/System/Library/Fonts/Supplemental/Arial.ttf', 30)
        small = ImageFont.truetype('/System/Library/Fonts/Supplemental/Arial.ttf', 22)
    except OSError:
        font = small = ImageFont.load_default()
    d.text((40, 30), title, fill=0, font=font)
    # White and black fields, side by side, each with a mark of the other.
    d.rectangle([40, 100, 520, 500], outline=0, width=2)
    d.rectangle([552, 100, 1032, 500], fill=0)
    d.rectangle([250, 270, 310, 330], fill=0)
    d.rectangle([762, 270, 822, 330], fill=255)
    d.text((50, 110), 'white (level 15)', fill=0, font=small)
    d.text((562, 110), 'black (level 0)', fill=255, font=small)
    # The 16 levels, dark to light, two rows of eight.
    for k in range(16):
        x = 40 + (k % 8) * 124; y = 540 + (k // 8) * 190
        g = grey_of_level[k]
        d.rectangle([x, y, x + 123, y + 150], fill=g)
        d.rectangle([x, y, x + 123, y + 150], outline=0, width=1)
        d.text((x + 6, y + 154), str(k), fill=0, font=small)
    # Text in the greys Android uses on white.
    y = 950
    for name, g in (('#000000', 0), ('#212121 primary text', 0x21), ('#757575 secondary text, icons', 0x75),
                    ('#9E9E9E hints, disabled', 0x9e), ('#BDBDBD', 0xbd), ('#E0E0E0 dividers', 0xe0)):
        d.text((40, y), 'The quick brown fox 0123456789  ' + name, fill=g, font=font)
        d.line([40, y + 44, 1032, y + 44], fill=g, width=2)
        y += 70
    return im

out = Path(sys.argv[1]); out.mkdir(parents=True, exist_ok=True)
contrast = float(sys.argv[2]) if len(sys.argv) > 2 else 12
levels = table(contrast)
middle = []
for k in range(16):
    lumas = [v for v in range(256) if levels[v] == k]
    assert lumas, k
    middle.append(0 if k == 0 else 255 if k == 15 else lumas[len(lumas) // 2])
chart([17 * k for k in range(16)], 'PW3 panel chart - stock').save(out / 'pw3-chart-stock.png')
chart(middle, 'PW3 panel chart - Android').save(out / 'pw3-chart-android.png')
print('levels -> luma for Android (contrast %g):' % contrast, middle)
