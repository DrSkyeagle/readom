# Noto Emoji (monochrome)

`NotoEmoji-VF.ttf` is the unmodified file `ofl/notoemoji/NotoEmoji[wght].ttf` of
`github.com/google/fonts` at commit `8b0a1d0f5983c89bc2b93f1b5fb55f9e252744b5` (1,982,596 bytes,
sha256 `de6c18832938afc99caf132b39d6a30a19bac7f2e812e28db2535b4608d27551`), renamed on the way in
for the reason AOSP names `NotoSansKhmer-VF.ttf` that way: it is a variable font, weight axis
300-700, default 400. Copyright 2013, 2022 Google Inc.; SIL Open Font License 1.1 (`NOTICE`).

It replaces AOSP's `NotoColorEmoji.ttf` as the `und-Zsye` family of `fonts/fonts.xml`.

Why: `NotoColorEmoji.ttf` is 7.28 MB of CBDT colour bitmaps, and this panel has 16 grey levels.
Rendered at this device's own text sizes (`ro.sf.lcd_density=300`, so 14 sp is 26 px, 18 sp 34 px,
22 sp 41 px) and quantised to those 16 levels, every colour emoji lands as a mid-grey shape: the
star, the sun and the check mark are pale blurs, the books a dark smudge, and at 14 sp they are
hard to tell from each other. The outline font keeps full black-on-white contrast at every size.

Why the variable font and not the static `NotoEmoji-Regular.ttf` (418,804 bytes) that
`googlefonts/noto-emoji` carried until tag v2.034 and dropped afterwards: the static one draws
faces as filled shapes, which at 14 sp are black blobs with no expression left -- and faces are
most of the emoji in a reading application's comments and notes. The variable font draws them as
outlines whose expression survives. It costs 1.56 MB more than the static file and still saves
5.30 MB against the colour font. The renders both judgements come from are in
`docs/design/performance.md`.

`fonts.xml` pins the weight axis at 500 rather than the default 400: at 26 px the 400 strokes thin
out, and 700 begins to fill in the tree and the sun. AOSP's own `NotoSansKhmer-VF.ttf` entries set
`<axis>` the same way.

Not kept in the source tree: `tools/fetch-prebuilts.py` downloads it from the address above and
checks its size and SHA-256 (list: `../apps/prebuilts.json`); the AOSP build refuses to start
without it.

## What is installed is not this file byte for byte

`Android.mk` runs `add-emoji-variation-sequences.py` over it on the way into the image, adding one
cmap format 14 subtable. The upstream font has none, and AOSP's `NotoColorEmoji.ttf` has 351
mappings; without them `FontFamily::hasGlyph(codepoint, variationSelector)` returns false for every
emoji written with U+FE0F -- it reads `mCmapFmt14Coverage` and nothing else -- while the drawing
path renders those emoji perfectly well. An application that asks before drawing, which
`EmojiCompat` does through `PaintCompat.hasGlyph`, would then substitute its own font for a
character this one draws.

The mappings are built from `external/unicode/emoji-variation-sequences.txt`, the file AOSP's own
font follows, keeping the sequences whose base character this font covers -- which is all 351 of
them. They go in a default UVS table, meaning "supported, using the glyph the ordinary cmap
already gives", which is what a monochrome font means by an emoji-style sequence; Minikin merges
the default and non-default tables into one coverage set (`CmapCoverage.cpp`,
`getCoverageFromUVSTable`).

So the installed font is the fetched bytes plus 4,088 bytes of table built from in-tree Unicode
data, and nothing else. `tools/tests/test_emoji_vs.py` reads the result back with a parser that
shares no code with the writer: every other table byte-identical, the recorded sequences exactly
Unicode's set, the subtable registered under platform 0 encoding 5, the encoding records still
sorted, and running the tool over its own output refused. FreeType renders the rewritten font
pixel-identically to the original.
