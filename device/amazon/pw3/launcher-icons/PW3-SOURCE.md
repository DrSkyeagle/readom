# The home screen's five marks

`material-symbols/` holds five unmodified files of the npm package
`@material-symbols/svg-400` 0.37.0
(`https://registry.npmjs.org/@material-symbols/svg-400/-/svg-400-0.37.0.tgz`, sha256
`0498c1f9a0ab1e4cefb7ff36a56cb5ca5df1b04bace2e11846f2287026a4a4a6`, fetched 2026-10-23),
copied byte for byte out of its `rounded/` directory:

| file | the entry it draws | sha256 |
|---|---|---|
| `grid_view-fill.svg` | 全部应用 | `20faffb885e748af48177fb37b2a61c7d4268b41929e0647ee47ef040d8ada6c` |
| `folder-fill.svg` | 文件 | `f97b31e3df56d76fc24f4e47862fa6ba31f3e50f4054c3cd418ae99c25071401` |
| `settings-fill.svg` | 设置 | `760f7c9f04cab2aca4f96f10691024fe75c085520ca34ad46f85b9a0236b6dde` |
| `language-fill.svg` | 浏览器 | `b2443cd77755f593bfd42488bcb8a9934e40887253d20b4122d367970a89caa4` |
| `download-fill.svg` | 微信读书（未安装时的入口） | `5f80208378c696ffbe3eaf828217ee2534132b4185eceb20938fa7a05e22ca61` |

The fifth was taken out of the same package on 2026-10-23, after the archive
above was fetched again and its sha256 compared with the one recorded here:
the entry it draws was the one mark on this screen still drawn by hand, in a
different style and a different size (`make_icons.py`, the `weread` entry).

Nothing else of the package is used, and nothing is fetched at build time:
`make_icons.py` reads these five files and writes the Android resources, and
`tools/run-tests.py` (`launcher-icons`) reads the resources back and fails if any
of them is not what the script makes of the file beside it.

`material-symbols/LICENSE` is the package's own Apache-2.0 text, carried here
because the licence asks that it travel with copies.

## Why an existing set rather than four drawings of our own

The four entries have to read as one set, and four marks drawn to match are
only as alike as the hand that drew them. These were drawn together: one
960-unit viewport, one weight, one optical size, filled.

The first attempt here was a geometry script that drew all four from one set of
numbers -- same stroke, same corner radius, one solid element each. It was
consistent and it was ugly ("你自己画的太丑了", the reader, 2026-10-23), which is
the predictable outcome: consistency is the part a script can get right, and
proportion is the part it cannot.

## Why filled marks on a tile

The set before this one was Lucide's outlines, drawn as they are authored: a
24-unit viewport, two units of stroke, nothing around them. The reader looked
at them on the device and said they are not of this time ("ICON 我认为不符合
现代风格", 2026-10-23), and the reason holds up when the panel is taken into
account:

* every application icon on this screen is already a filled square -- WeChat
  Reading's is -- so four line drawings beside them read as sketches rather
  than as applications;
* the panel has no colour, so a mark is recognised by its mass, and a 9 px
  stroke carries very little of it at 112 px;
* this panel renders flat black and flat white better than anything in
  between. A tile is therefore the darkest ink it has, and the mark is the
  paper, with nothing in the middle. (The same reasoning says no to a grey
  tile, which was the alternative looked at: a 112 px field of flat mid grey
  is where any unevenness in the drive shows first -- the bring-up card used
  to draw two such fields deliberately, for exactly that reason.)

The numbers: the viewport is 112 px, the size this screen draws an icon
(`launcher:iconSize` 60dp at `ro.sf.lcd_density` 300); the tile is 90 px of it
with a 20 px corner (22%, the corner current icon shapes use), and the mark is
51 px (57% of the tile), which leaves the margin an icon needs to stay a shape
rather than a filled square.

The tile does not fill the viewport, and 90 is not taste. Launcher3 shrinks an
application's icon until its convex hull is at most 0.6528 of the canvas
(`IconNormalizer`), and only an application's -- the all-applications mark is
the launcher's own drawable and is not shrunk. Drawing the tile at the size
that rule already accepts is what makes the rule do nothing, and all five
marks then reach the screen at the size they were drawn.
`tools/tests/test_icon_normalizer.py` holds the arithmetic against
IconNormalizer's own constants.

## Why these icons

`grid_view` is the mark the reader gave by example at the start: four rounded
squares with nothing around them. `folder`, `settings` and `language` are that
family's own words for the other three. They were chosen at 112 px, the size
this screen draws them, against Phosphor's filled family and against the same
four as plain filled marks with no tile.

`download` came later and for a different reason. The entry it draws is
WeRead before WeRead is installed, and what it has to say is "this one is not
here yet" -- the label under it already says whose application it is. It
replaced a drawing of this project's own, an outlined open book with an arrow
into it, which was the last mark on the screen not from this family and was
also the wrong size: 99 x 94 px beside four tiles of 93.

## What the conversion does

A VectorDrawable draws only `<path>`, and a `<group>` can scale and move one.
So each icon ships as two paths: the tile, written by the script, and the
symbol's own path data, **passed through untouched** inside a group that scales
it by 51/960 and puts it in the middle of the tile. Untouched is the point --
what reaches the screen is provably Google's drawing rather than this script's
arithmetic on it. (Material Symbols draws in a box whose y runs from -960 to 0,
which is why the group's vertical offset is the margin plus the glyph.)

Anything but a single `<path>` in a source file stops the script instead of
being dropped: a silently missing shape is a wrong icon.

The preview the script writes (`--preview`) is built from the converted path
data rather than from the SVG, so a conversion that went wrong is visible there
instead of on the device.

## Where each one is installed

| mark | installed as | route |
|---|---|---|
| 全部应用 | `res/drawable/ic_all_apps_pw3.xml` of Launcher3 | `patches/aosp-launcher3/0014` |
| 文件 | DocumentsUI's `mipmap/ic_app_icon` | product overlay |
| 设置 | Settings' `drawable/ic_launcher_settings` | product overlay |
| 浏览器 | EinkBro's `mipmap/ic_launcher` and `ic_launcher_round` | runtime overlay (`apps/BrowserName`) |

The browser is the one with a constraint of its own. EinkBro is a prebuilt and
is not rebuilt here, so its icon is replaced by the runtime overlay that
already renames it. Its own icon is `mipmap-anydpi-v26`, and
`ResTable::getEntry()` lets an overlay's entry win only where its configuration
is *better than* the target's or *equal* to it:

```c
if (!thisConfig.isBetterThan(bestConfig, config)) {
    if (!currentTypeIsOverlay || thisConfig.compare(bestConfig) != 0) {
        continue;
    }
}
```

A vector in `res/mipmap-anydpi/` compiles to `anydpi-v21`. Against `anydpi-v26`
that ties on density (both are `DENSITY_ANY`) and then loses on version, so it
is neither better nor equal, and the overlay entry is passed over without a
word: the prebuilt's own adaptive icon stays on the screen and the set is three
marks and a stranger. The directory is therefore `res/mipmap-anydpi-v26/`
exactly, which makes `compare()` zero and the overlay win.
