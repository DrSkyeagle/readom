#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""The home screen's five marks: Material Symbols on a tile, as Android resources.

The entries have to read as one set, and the way to get that is not to draw
five marks well but to take five that were already drawn together. These
are Material Symbols Rounded, filled (material-symbols/, Apache-2.0): one
family, one 960-unit viewport, one weight, no strokes.

They sit knocked out of a black rounded tile, which is the shape every
application icon on this screen already has -- WeChat Reading's included --
and it is what makes these marks read as applications rather than as
line drawings floating on the page. On a panel with no colour, a mark is
recognised by its mass; this screen also renders flat black and flat white
better than anything between, so the tile is the darkest ink the panel has and
the mark is the paper.

A VectorDrawable understands only <path>, and a <group> can scale and move
one. So each icon ships as two paths: the tile, written here, and the symbol's
own path data, passed through untouched inside a group that puts it in the
middle of the tile. Untouched matters -- what is on the screen is then
provably Google's drawing and not this script's arithmetic on it.

Nothing is fetched: material-symbols/ holds the five files, and PW3-SOURCE.md
says where they came from.

  --write    write every resource file
  --check    read them back and fail if any differs (tools/run-tests.py)
"""
import argparse
import sys
import xml.etree.ElementTree as ElementTree
from pathlib import Path

SVG = '{http://www.w3.org/2000/svg}'
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]

# The icon as this screen draws it: launcher:iconSize 60dp at ro.sf.lcd_density
# 300, so 60 x 1.875 = 112 px. Authoring in those units means every number
# below is a pixel on the device.
VIEWPORT = 112.0
# The tile does not fill the viewport, and the margin is not taste: Launcher3
# shrinks an application's icon until it is this small, and only an
# application's. The home screen measured 93 px for the browser, the files and
# the settings marks and 113 px for all-applications ("全部应用的 ICON 为什么比
# 其他的都大", user, 2026-10-23), because the first three are drawn through
# IconCache -- LauncherIcons.createBadgedIconBitmap, which scales by
# IconNormalizer.getScale() -- and the fourth is a drawable of the launcher
# itself, which is not.
#
# The margin is what the normaliser is asking for, so authoring it here is
# conforming to the platform's icon geometry rather than compensating for it,
# and once the drawing already satisfies the test the scaling step does
# nothing: all four marks then reach the screen at the size they were drawn.
# IconNormalizer (packages/apps/Launcher3, AOSP 8.1) leaves a drawing alone
# when the area of its convex hull, over the area of the whole canvas, is at
# most
#     MAX_SQUARE_AREA_FACTOR + LINEAR_SCALE_SLOPE * (1 - hull/boundingBox)
#     = 375/576 + ((380-375)/576)/(1 - pi/4) * (1 - 0.9572) = 0.6528
# A rounded square is its own convex hull, so for a tile of side s in a
# viewport of side v that ratio is 0.9572 * (s/v)^2, and the largest tile it
# allows is s = 0.8258 v = 92.5 px. 90 keeps about a twentieth of that in
# hand, which the hull needs: it is counted on a raster (the drawable's
# intrinsic 48dp = 90 px here), and the anti-aliased edge of a tile whose
# sides do not land on pixel boundaries reads a pixel wider on each side than
# the arithmetic above. tools/tests/test_icon_normalizer.py holds this, and
# the screen settles it -- the sizes above were measured off a screenshot.
TILE = 90.0
# 22% of the tile's side, which is the corner every current icon shape uses.
TILE_RADIUS = 20.0
# The mark inside the tile. 57% leaves the margin an icon needs to stay a
# shape rather than a filled square, and matches what the sets that ship with
# a tile do.
GLYPH = 51.0
# The tile is the ink this screen writes text in, and the mark is the paper.
# Nothing in between: a flat mid grey is the one thing this panel does not
# render cleanly, and a 112 px field of it would show every unevenness.
INK = '#FF000000'
PAPER = '#FFFFFFFF'

# Material Symbols draws every icon with these, and the set holds together
# because they are the same for all of them. A source file that disagrees is a
# different drawing language and is refused rather than quietly converted.
SOURCE_VIEWPORT = 960.0
EXPECTED_SVG = {'width': '48', 'height': '48', 'viewBox': '0 -960 960 960'}

ICONS = [
    # the mark, its label on the home screen, the Material Symbols icon it is
    ('all-apps', '全部应用', 'grid_view-fill'),
    ('files', '文件', 'folder-fill'),
    ('settings', '设置', 'settings-fill'),
    ('browser', '浏览器', 'language-fill'),
    # The entry for WeRead before WeRead is there. It was drawn by hand in a
    # 48-unit viewport, outlined, from before there was a set -- an open book
    # with an arrow into it -- and on the screen it was 99 x 94 px beside four
    # tiles of 93 ("微信读书下载的按钮大小也和其他的不一样", the reader,
    # 2026-10-23). The arrow is what this entry says, so the symbol is the
    # download one; the label under it already says whose application it is,
    # and the mark is still not Tencent's, which this project has no business
    # shipping. Once WeRead is installed its own icon takes this place.
    ('weread', '微信读书', 'download-fill'),
]

# Where each mark is installed. all-apps is not here: it is a drawable of
# Launcher3 itself and travels in that project's patch series, so it is written
# with --launcher-drawable and checked against the patch.
TARGETS = {
    'files': ['device/amazon/pw3/overlay/packages/apps/DocumentsUI/res/mipmap-anydpi/ic_app_icon.xml'],
    'settings': ['device/amazon/pw3/overlay/packages/apps/Settings/res/drawable/ic_launcher_settings.xml'],
    # anydpi-v26 exactly, not anydpi: this one is a runtime overlay over a
    # prebuilt whose own icon is anydpi-v26, and ResTable::getEntry() lets an
    # overlay win only where its configuration is better than the target's or
    # equal to it ("!currentTypeIsOverlay || thisConfig.compare(bestConfig) != 0").
    # anydpi alone compiles to anydpi-v21, which is neither, so the overlay
    # would be passed over and the prebuilt's own icon would stay on the screen.
    'browser': ['device/amazon/pw3/apps/BrowserName/res/mipmap-anydpi-v26/ic_launcher.xml',
                'device/amazon/pw3/apps/BrowserName/res/mipmap-anydpi-v26/ic_launcher_round.xml'],
    'weread': ['device/amazon/pw3/apps/WeReadInstaller/res/drawable/ic_weread.xml'],
}

LAUNCHER_PATCH = 'patches/aosp-launcher3/0014-pw3-all-applications-on-the-home-screen.patch'
LAUNCHER_FILE = 'res/drawable/ic_all_apps_pw3.xml'


def number(value):
    """Keep every emitted number short and identical from run to run."""
    return '%g' % float(value)


def rounded_rect_path(x, y, width, height, rx, ry):
    """Path data for <rect>, clockwise from the end of the top left corner."""
    rx = min(rx, width / 2.0)
    ry = min(ry, height / 2.0)
    x1, y1 = x + width, y + height
    if not rx and not ry:
        return 'M%s,%s H%s V%s H%s Z' % (
            number(x), number(y), number(x1), number(y1), number(x))
    return ('M%s,%s H%s A%s,%s 0 0 1 %s,%s V%s A%s,%s 0 0 1 %s,%s '
            'H%s A%s,%s 0 0 1 %s,%s V%s A%s,%s 0 0 1 %s,%s Z' % (
                number(x + rx), number(y), number(x1 - rx),
                number(rx), number(ry), number(x1), number(y + ry),
                number(y1 - ry), number(rx), number(ry), number(x1 - rx), number(y1),
                number(x + rx), number(rx), number(ry), number(x), number(y1 - ry),
                number(y + ry), number(rx), number(ry), number(x + rx), number(y)))


def tile_margin():
    """How far the tile is inset from each edge of the viewport."""
    return (VIEWPORT - TILE) / 2.0


def tile_path():
    margin = tile_margin()
    return rounded_rect_path(margin, margin, TILE, TILE, TILE_RADIUS, TILE_RADIUS)


def placement():
    """Scale and offset that put a 960-unit symbol in the middle of the tile.

    Material Symbols draws in a box whose y runs from -960 to 0, so the offset
    down the page is the margin plus the glyph: the group transform is a
    translation of a scaled point, and (0, -960) has to land on the top left of
    the glyph box.
    """
    # Centred in the viewport, which is centred on the tile: the tile's own
    # margin cancels, and the mark stays in the middle of the mark it is in.
    margin = (VIEWPORT - GLYPH) / 2.0
    return GLYPH / SOURCE_VIEWPORT, margin, margin + GLYPH


def read_icon(name):
    """The path data of one Material Symbols file, exactly as it is written."""
    source = HERE / 'material-symbols' / (name + '.svg')
    root = ElementTree.fromstring(source.read_text())
    for key, want in EXPECTED_SVG.items():
        if root.get(key) != want:
            raise SystemExit('%s: %s is %r, not %r. These are converted on the\n'
                             'understanding that they are drawn alike; look at the new\n'
                             'file before widening this.' % (source.name, key, root.get(key), want))
    paths = []
    for element in root:
        tag = element.tag[len(SVG):] if element.tag.startswith(SVG) else element.tag
        if tag != 'path':
            raise SystemExit('%s: <%s> is not converted here. These files are one path\n'
                             'each; a second kind of element means the family changed, and a\n'
                             'silently missing shape is a wrong icon.' % (source.name, tag))
        paths.append(element.get('d'))
    if len(paths) != 1:
        raise SystemExit('%s: %d paths, expected one.' % (source.name, len(paths)))
    return paths[0]


VECTOR = '''<?xml version="1.0" encoding="utf-8"?>
<!-- {comment} -->
<vector xmlns:android="http://schemas.android.com/apk/res/android"
    android:width="48dp"
    android:height="48dp"
    android:viewportWidth="{viewport}"
    android:viewportHeight="{viewport}">
    <path
        android:fillColor="{ink}"
        android:pathData="{tile}"/>
    <group
        android:scaleX="{scale}"
        android:scaleY="{scale}"
        android:translateX="{x}"
        android:translateY="{y}">
        <path
            android:fillColor="{paper}"
            android:pathData="{glyph}"/>
    </group>
</vector>
'''


def vector_xml(label, symbol):
    comment = ('%s, on the home screen. Material Symbols Rounded %s\n'
               '     (launcher-icons/material-symbols), on a tile, turned into a\n'
               '     VectorDrawable by launcher-icons/make_icons.py. Edit the script or\n'
               '     the drawing it reads, not this file.' % (label, symbol))
    # An XML comment cannot hold a double hyphen, and aapt2 reports one as
    # "not well-formed (invalid token)" against a generated file, which is a
    # poor place to start looking.
    assert '--' not in comment, 'an XML comment may not contain a double hyphen'
    scale, x, y = placement()
    return VECTOR.format(comment=comment, viewport=number(VIEWPORT), ink=INK, paper=PAPER,
                         tile=tile_path(), scale=number(scale), x=number(x), y=number(y),
                         glyph=read_icon(symbol))


def css(colour):
    """#AARRGGBB as the preview's #RRGGBB. Both are opaque here."""
    return '#' + colour[3:]


def inline_svg(symbol, px):
    """The same two paths as an SVG, for the preview."""
    scale, x, y = placement()
    return ('<svg width="%d" height="%d" viewBox="0 0 %s %s">'
            '<path fill="%s" d="%s"/>'
            '<g transform="translate(%s,%s) scale(%s)"><path fill="%s" d="%s"/></g></svg>'
            % (px, px, number(VIEWPORT), number(VIEWPORT), css(INK), tile_path(),
               number(x), number(y), number(scale), css(PAPER), read_icon(symbol)))


def preview_html():
    """The four marks at the size the device draws them, and at twice it.

    112 px is this screen's icon: launcher:iconSize 60dp at ro.sf.lcd_density
    300, so 60 x 1.875. Built from the converted path data, so a conversion
    that went wrong shows up here instead of on the device.
    """
    cells = []
    for _, label, symbol in ICONS:
        cells.append('<div class=c><div class=b>%s</div><div class=r>%s</div>'
                     '<div class=l>%s</div><div class=n>%s</div></div>'
                     % (inline_svg(symbol, 224), inline_svg(symbol, 112), label, symbol))
    return ('<!doctype html><meta charset="utf-8"><title>PW3 home screen marks</title>'
            '<style>body{background:#fff;color:#000;margin:0;padding:28px;'
            'font:14px/1.5 -apple-system,"PingFang SC",sans-serif}'
            'h1{font-size:16px;margin:0 0 16px}.row{display:flex;gap:36px}'
            '.c{text-align:center}.b{height:224px}'
            '.r{height:112px;margin-top:16px;display:flex;align-items:center;justify-content:center}'
            '.l{font-size:15px;margin-top:8px}'
            '.n{font-size:11px;color:#999;font-family:ui-monospace,monospace}</style>'
            '<h1>首页的四个标记：上面 x2，下面是设备上的真实大小 112 px</h1>'
            '<div class=row>%s</div>' % ''.join(cells))


def drawable_in_patch():
    """The all-applications drawable as the Launcher3 patch carries it."""
    text = (ROOT / LAUNCHER_PATCH).read_text()
    start = text.index('diff --git a/' + LAUNCHER_FILE)
    body = text[text.index('@@', start):]
    body = body[body.index('\n') + 1:]
    lines = []
    for line in body.split('\n'):
        if not line.startswith('+'):
            break
        lines.append(line[1:])
    return '\n'.join(lines) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--write', action='store_true')
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--launcher-drawable', type=Path,
                        help='also write the all-applications drawable here, to be put '
                             'into ' + LAUNCHER_PATCH)
    parser.add_argument('--preview', type=Path, help='write the preview page here')
    args = parser.parse_args()

    made = {name: vector_xml(label, symbol) for name, label, symbol in ICONS}
    total = sum(len(p) for p in TARGETS.values()) + 1
    if args.preview:
        args.preview.write_text(preview_html())
    if args.launcher_drawable:
        args.launcher_drawable.write_text(made['all-apps'])
    if args.write:
        for name, paths in TARGETS.items():
            for relative in paths:
                target = ROOT / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(made[name])
        print('wrote %d files' % (total - 1))
    if args.check:
        wrong = [relative for name, paths in TARGETS.items() for relative in paths
                 if (ROOT / relative).read_text() != made[name]]
        if drawable_in_patch() != made['all-apps']:
            wrong.append(LAUNCHER_PATCH)
        if wrong:
            print('not what this script makes:\n  ' + '\n  '.join(wrong))
            return 1
        print('%d of %d marks are what this script makes' % (total, total))
    return 0


if __name__ == '__main__':
    sys.exit(main())
