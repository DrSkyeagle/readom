#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""The four marks are drawn at the size Launcher3 will leave alone.

Three of the four reach the home screen through IconCache, and that path scales
what it is given: LauncherIcons.createBadgedIconBitmap asks
IconNormalizer.getScale() how much smaller the drawing has to be before it
stops reading as too heavy beside other applications' icons. The fourth, the
all-applications mark, is a drawable of the launcher itself and goes nowhere
near that path. So a tile that fills its viewport arrives on the screen at two
different sizes -- measured on 2026-10-23 off a screenshot of the running home
screen: 93 px for the three, 113 px for the fourth.

The fix is not a second scale applied to the fourth. It is to draw all four at
a size the normaliser already agrees with, so that the scaling step does
nothing and the drawing is what the screen shows. That is a property of the
numbers in make_icons.py, and this is where it is held:

  * the formula is IconNormalizer's, with its own constants read out of the
    AOSP tree when the tree is there, so that a change upstream fails here
    rather than on the screen;
  * a rounded square is its own convex hull, which is what makes the hull area
    arithmetic exact rather than an estimate;
  * and the margin below the limit is stated and checked, because the hull is
    counted on an anti-aliased raster and reads a little wider than the
    arithmetic.

What this cannot do is measure the screen. The sizes above came from a
screenshot and the next ones have to as well; this keeps the arithmetic from
drifting between those measurements.
"""
import importlib.util
import math
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ICONS = ROOT / 'device/amazon/pw3/launcher-icons/make_icons.py'
# The same tree the other tests read; absent on a machine that only has this
# repository, and then the constants below stand on their own.
NORMALIZER = Path('/home/builder/android-src/packages/apps/Launcher3/src/com/android/'
                  'launcher3/graphics/IconNormalizer.java')

# IconNormalizer's own numbers, as of AOSP 8.1. Checked against the file when
# it is there.
UPSTREAM = {
    'MAX_SQUARE_AREA_FACTOR': 375.0 / 576,
    'MAX_CIRCLE_AREA_FACTOR': 380.0 / 576,
    'CIRCLE_AREA_BY_RECT': math.pi / 4,
}
# How far below the limit the drawing has to sit. The hull is counted on a
# raster -- the drawable's intrinsic size, 48dp at density 300, so 90 px -- and
# a tile whose edges do not land on pixel boundaries is anti-aliased, which
# adds about a pixel on each side: on a 72 px tile that is 2.8% of the side and
# 5.6% of the area. One twentieth covers it.
MARGIN = 0.05

checks = 0
failures = []


def check(ok, what):
    global checks
    checks += 1
    if not ok:
        failures.append(what)


def upstream_constants():
    """IconNormalizer's constants as the tree spells them, or None without it."""
    if not NORMALIZER.is_file():
        return None
    text = NORMALIZER.read_text()
    out = {}
    for name in UPSTREAM:
        found = re.search(r'\b%s\s*=\s*([^;]+);' % re.escape(name), text)
        if not found:
            return {name: None}
        expression = found.group(1).strip()
        # "375.0f / 576", "(float) Math.PI / 4"
        expression = expression.replace('f /', ' /').replace('(float)', '').replace('Math.PI', str(math.pi))
        expression = re.sub(r'(\d)f\b', r'\1', expression)
        try:
            out[name] = eval(expression, {'__builtins__': {}}, {})   # noqa: S307 - three arithmetic lines
        except Exception:
            out[name] = None
    return out


def scale_of(hull_over_rect, hull_over_canvas, constants):
    """IconNormalizer.getScale(), for a drawing whose hull is known."""
    if hull_over_rect < constants['CIRCLE_AREA_BY_RECT']:
        required = constants['MAX_CIRCLE_AREA_FACTOR']
    else:
        slope = ((constants['MAX_CIRCLE_AREA_FACTOR'] - constants['MAX_SQUARE_AREA_FACTOR'])
                 / (1 - constants['CIRCLE_AREA_BY_RECT']))
        required = constants['MAX_SQUARE_AREA_FACTOR'] + slope * (1 - hull_over_rect)
    return (math.sqrt(required / hull_over_canvas) if hull_over_canvas > required else 1.0), required


def main():
    spec = importlib.util.spec_from_file_location('pw3_make_icons', ICONS)
    icons = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(icons)

    found = upstream_constants()
    if found is None:
        print('     IconNormalizer.java is not here; checking against the constants '
              'written down in this file')
    else:
        for name, want in UPSTREAM.items():
            got = found.get(name)
            check(got is not None and abs(got - want) < 1e-9,
                  '%s is %r in %s, and %r here' % (name, got, NORMALIZER.name, want))

    # A rounded square of side s with corner radius r: the four corners replace
    # a square of side r each with a quarter disc, so the area is
    # s^2 - (4 - pi) r^2. It is convex, so that is its hull too, and its
    # bounding box is the square itself.
    side, radius, viewport = icons.TILE, icons.TILE_RADIUS, icons.VIEWPORT
    check(0 < side <= viewport, 'the tile does not fit the viewport')
    check(0 < radius <= side / 2, 'the corner radius does not fit the tile')
    area = side * side - (4 - math.pi) * radius * radius
    hull_over_rect = area / (side * side)
    hull_over_canvas = area / (viewport * viewport)
    constants = {name: (found or {}).get(name) or want for name, want in UPSTREAM.items()}
    scale, required = scale_of(hull_over_rect, hull_over_canvas, constants)

    check(scale == 1.0,
          'Launcher3 would scale these marks by %.4f, so the three that go through '
          'IconCache would reach the screen at %.1f px and all-applications at %.1f px. '
          'The tile is %.0f of %.0f; the largest the normaliser leaves alone is %.1f.'
          % (scale, scale * side, side, side, viewport,
             viewport * math.sqrt(required / (hull_over_rect))))
    check(hull_over_canvas <= required * (1 - MARGIN),
          'the tile is %.4f of the canvas and the limit is %.4f: inside it, but by '
          'less than the %.0f%% the anti-aliased edge needs, so the raster the '
          'normaliser counts can still come out over the limit'
          % (hull_over_canvas, required, MARGIN * 100))

    # The mark has to stay inside the tile, or the tile is not the shape whose
    # area was just computed.
    check(icons.GLYPH <= side, 'the mark (%g) is wider than the tile (%g)' % (icons.GLYPH, side))

    print('%d checks, %d failed (tile %g of %g, hull %.4f of the canvas, limit %.4f, scale %g)'
          % (checks, len(failures), side, viewport, hull_over_canvas, required, scale))
    for problem in failures:
        print('  FAIL %s' % problem)
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
