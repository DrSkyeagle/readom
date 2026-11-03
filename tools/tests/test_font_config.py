#!/usr/bin/env python3
"""device/amazon/pw3/fonts/fonts.xml and the font package list in pw3.mk.

Two things can go wrong with a trimmed font configuration, and the build says
nothing about either:

  * fonts.xml names a font the product no longer installs. The export refuses
    that (tools/export-aosp-images.py), so it cannot reach a device -- but it
    wastes a build, and the build is two hours.
  * the product installs a font fonts.xml no longer names. Nothing refuses
    that; it is 18 MB nobody reads.

Both are a disagreement between two files in this repository, so both are
checked here. With the AOSP tree (--aosp-tree), this also proves the stronger
property the file was built to have: it is AOSP's fonts.xml with whole blocks
deleted and nothing else. That is what keeps a weight, a style or a .ttc index
from drifting -- an index of 0 where AOSP says 2 would silently render
Simplified Chinese with Japanese glyph variants, which no size check sees.
"""
import argparse
import difflib
import json
import re
import struct
import sys
import traceback
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
FONTS_XML = ROOT / 'device/amazon/pw3/fonts/fonts.xml'
PW3_MK = ROOT / 'device/amazon/pw3/pw3.mk'
BOARD_CONFIG = ROOT / 'device/amazon/pw3/BoardConfig.mk'
PATCH = (ROOT / 'patches/aosp-frameworks-base'
         / '0052-pw3-a-product-can-replace-the-font-configuration.patch')

# The scripts this build renders text in. Latin, Greek and Cyrillic arrive
# through the named families, which have no lang attribute and all stay.
KEEP_LANG = {'zh-Hans', 'zh-Hant', 'ja', 'ko', 'und-Zsye', 'und-Zsym'}

# The comments in AOSP's file whose whole subject is a deleted family. Spelled
# out rather than inferred: a rule for "a comment that belongs to the families
# after it" would quietly drop a comment AOSP adds for another reason.
DROP_COMMENTS = (
    '<!-- TODO: add Geok -->',
    '<!-- All scripts of India should come after Devanagari, due to shared',
    '<!-- Esrangela should precede Eastern and Western Syriac, since '
    "it's our default form. -->",
)
TAIL_COMMENT = 'Tai Le, Yi, Mongolian, and Phags-pa are intentionally kept last'

# The one thing in this file that is not a deletion, written out so that it is
# the only one. The colour font is 7.28 MB of CBDT bitmaps and this panel has 16
# greys; the replacement is an outline font with a weight axis, pinned the way
# AOSP's own NotoSansKhmer-VF.ttf entries pin theirs (emoji/PW3-SOURCE.md).
EMOJI_FROM = '        <font weight="400" style="normal">NotoColorEmoji.ttf</font>\n'
EMOJI_TO = ('        <font weight="400" style="normal">NotoEmoji-VF.ttf\n'
            '            <axis tag="wght" stylevalue="500.0" />\n'
            '        </font>\n')
# Fonts this device supplies, which are therefore not in AOSP's own lists.
DEVICE_FONTS = {'NotoEmoji-VF.ttf'}
EMOJI_DIR = ROOT / 'device/amazon/pw3/emoji'
PREBUILTS = ROOT / 'device/amazon/pw3/apps/prebuilts.json'

# Hyphenation: Minikin hyphenates the locale of the text being laid out, and
# fontchain_lint asserts a font exists for every locale with a pattern file.
# These two are Latin, which the named families cover.
HYPHENATION = {'hyph-en-us', 'hyph-en-us.lic.txt', 'hyph-en-gb', 'hyph-en-gb.lic.txt'}

# What "renders Chinese and English" is promised to mean, as code points the
# installed fonts must between them cover. Deleting 101 families is a scope
# decision, and this is the scope: everything here is text a Chinese or English
# reader meets, so a later trim that reaches one of these fails rather than
# showing a box on the device. Written as ranges and literals, not as whole
# blocks, because a block is not a promise -- U+2E80.. are CJK radicals nobody
# types, while U+2026 is in every paragraph.
COVERAGE = [
    ('ASCII', [(0x20, 0x7E)]),
    ('拉丁扩展：英文借词、人名', [(0xA0, 0xFF), (0x100, 0x17F)]),
    ('拼音声调', list('āáǎàēéěèīíǐìōóǒòūúǔùǖǘǚǜüĀÁǍÀ')),
    ('通用标点', list('‐–—‑‒―‖‘’‚“”„†‡•…‰′″‹›⁄') + [(0x2000, 0x200F)]),
    ('货币', list('$¢£¤¥₠₡₣₤₦₧₨₩₪₫€₭₮₱₲₴₵₸₹₺₼₽₾')),
    ('上下标与数字形式', list('⁰¹²³⁴⁵⁶⁷⁸⁹₀₁₂₃₄₅₆₇₈₉½¼¾⅓⅔')),
    ('字母式符号', list('™©®℃℉№℡Ω℮ℓ')),
    ('箭头', [(0x2190, 0x21FF)]),
    ('数学', list('∀∂∃∅∆∇∈∉∋∏∑−∓∕∗√∝∞∟∠∧∨∩∪∫∴∵∶∷∽≈≌≒≠≡≤≥≪≫⊕⊙⊥⊿')),
    ('几何与杂项符号', list('■□▲△▼▽◆◇○●◎★☆☉♀♂♪♭♯※')),
    ('制表与块', [(0x2500, 0x254B), (0x2580, 0x2593)]),
    ('带圈与括号数字', [(0x2460, 0x2473), (0x2474, 0x2487), (0x3220, 0x3229)]),
    ('CJK 标点', [(0x3001, 0x3003), (0x3008, 0x3011), (0x3014, 0x3017),
                  (0x301C, 0x301E)] + list('　。')),
    ('注音', [(0x3105, 0x3129)]),
    ('假名', [(0x3041, 0x3096), (0x30A1, 0x30FA), (0x30FB, 0x30FC)]),
    ('CJK 常用字', list('的一是了我不人在他有这中大来上国个到说们为子和你地出道也时年得就那要下以生会自着去之过家学对可她里后小么心多天而能好都然没日于起还发成事只作当想看文无开手十用主行方又如前所本见经头面公同三已老从动两长知民利比或但质气第向道命此变条只没结解问意情'
                      '书页章节读字体系统设置网络时间电量开关确定取消返回搜索输入'
                      '安装应用文件夹图片下载更新版本升级重启关机')),
    ('全角形式', [(0xFF01, 0xFF5E), (0xFFE0, 0xFFE5)]),
    ('emoji 常见', list('😀😂😊👍👎❤🔥⭐✔✖⚠📚🎉🙏🌙☀🌲')),
]

checks = 0
failures = []


def check(ok, what):
    global checks
    checks += 1
    if not ok:
        failures.append(what)


def product_packages(text):
    """Every name in the PRODUCT_PACKAGES += blocks of a product makefile."""
    names = set()
    lines = text.splitlines()
    i = 0
    while i < len(lines):
        m = re.match(r'PRODUCT_PACKAGES \+=(.*)$', lines[i])
        if not m:
            i += 1
            continue
        rest = m.group(1)
        while rest.rstrip().endswith('\\'):
            names.update(rest.rstrip().rstrip('\\').split())
            i += 1
            rest = lines[i]
        names.update(rest.split())
        i += 1
    return names


def families(path):
    """(name, lang, [(file, index)]) for each family, in order."""
    out = []
    for fam in ET.parse(path).getroot().findall('family'):
        fonts = [(e.text.strip(), e.get('index')) for e in fam.findall('font')]
        out.append((fam.get('name'), fam.get('lang'), fonts))
    return out


def derive(aosp_lines):
    """AOSP's fonts.xml with the families and comments this build deletes."""
    drop = set()
    i = 0
    while i < len(aosp_lines):
        if re.match(r'<family[ >]', aosp_lines[i].lstrip()):
            j = i
            while '</family>' not in aosp_lines[j]:
                j += 1
            fam = ET.fromstring(''.join(aosp_lines[i:j + 1]))
            lang = fam.get('lang')
            if fam.get('name') is None and lang is not None and lang not in KEEP_LANG:
                drop.update(range(i, j + 1))
            i = j + 1
        elif aosp_lines[i].strip().startswith('<!--'):
            j = i
            while '-->' not in aosp_lines[j]:
                j += 1
            text = ''.join(aosp_lines[i:j + 1])
            if aosp_lines[i].strip() in DROP_COMMENTS or TAIL_COMMENT in text:
                drop.update(range(i, j + 1))
            i = j + 1
        else:
            i += 1
    kept = [l for n, l in enumerate(aosp_lines) if n not in drop]
    out = ''.join(kept).replace(EMOJI_FROM, EMOJI_TO)
    return out.splitlines(keepends=True)


def cmap_points(path, index=0):
    """Every code point a font's best Unicode cmap subtable maps to a glyph."""
    data = path.read_bytes()
    if data[:4] == b'ttcf':
        n = struct.unpack('>I', data[8:12])[0]
        base = struct.unpack('>%dI' % n, data[12:12 + 4 * n])[index]
    else:
        base = 0
    num = struct.unpack('>H', data[base + 4:base + 6])[0]
    cmap = None
    for i in range(num):
        rec = base + 12 + 16 * i
        tag, _, off, _ = struct.unpack('>4sIII', data[rec:rec + 16])
        if tag == b'cmap':
            cmap = off
    if cmap is None:
        return set()
    best = None
    for i in range(struct.unpack('>H', data[cmap + 2:cmap + 4])[0]):
        pid, eid, sub = struct.unpack('>HHI', data[cmap + 4 + 8 * i:cmap + 12 + 8 * i])
        at = cmap + sub
        fmt = struct.unpack('>H', data[at:at + 2])[0]
        if fmt not in (4, 12):
            continue
        rank = {(3, 10): 3, (0, 4): 3, (0, 6): 3, (3, 1): 2, (0, 3): 2}.get((pid, eid), 1)
        if best is None or rank > best[0]:
            best = (rank, fmt, at)
    if best is None:
        return set()
    _, fmt, at = best
    points = set()
    if fmt == 4:
        segx2 = struct.unpack('>H', data[at + 6:at + 8])[0]
        seg = segx2 // 2
        ends = struct.unpack('>%dH' % seg, data[at + 14:at + 14 + segx2])
        s = at + 16 + segx2
        starts = struct.unpack('>%dH' % seg, data[s:s + segx2])
        d = s + segx2
        deltas = struct.unpack('>%dh' % seg, data[d:d + segx2])
        r = d + segx2
        ranges = struct.unpack('>%dH' % seg, data[r:r + segx2])
        for i in range(seg):
            for c in range(starts[i], min(ends[i], 0xFFFF) + 1):
                if ranges[i] == 0:
                    glyph = (c + deltas[i]) & 0xFFFF
                else:
                    gi = r + 2 * i + ranges[i] + 2 * (c - starts[i])
                    glyph = struct.unpack('>H', data[gi:gi + 2])[0]
                    if glyph:
                        glyph = (glyph + deltas[i]) & 0xFFFF
                if glyph:
                    points.add(c)
    else:
        for i in range(struct.unpack('>I', data[at + 12:at + 16])[0]):
            g = at + 16 + 12 * i
            start, end, glyph = struct.unpack('>III', data[g:g + 12])
            # glyph 0 is .notdef, and in a sequential map group only the first
            # character can land on it -- the format 4 branch above already
            # excludes it, and reading coverage more generously here would
            # count a character the font does not draw as covered.
            if glyph == 0:
                start += 1
            if start <= end:
                points.update(range(start, end + 1))
    return points


def wanted_points():
    """COVERAGE flattened: (label, set of code points)."""
    for label, items in COVERAGE:
        out = set()
        for item in items:
            if isinstance(item, tuple):
                out.update(range(item[0], item[1] + 1))
            else:
                out.update(ord(c) for c in item)
        yield label, out


def ttc_face_names(path):
    """The full name of each face in a font file, in collection order.

    A plain font is a collection of one. The name wanted is name ID 4 in the
    Windows/Unicode/English record, which is the one every Noto face carries.
    """
    data = path.read_bytes()
    if data[:4] == b'ttcf':
        count = struct.unpack('>I', data[8:12])[0]
        bases = struct.unpack('>%dI' % count, data[12:12 + 4 * count])
    else:
        bases = (0,)
    out = []
    for base in bases:
        tables = struct.unpack('>H', data[base + 4:base + 6])[0]
        at = None
        for i in range(tables):
            tag, _, off, length = struct.unpack(
                '>4sIII', data[base + 12 + 16 * i:base + 28 + 16 * i])
            if tag == b'name':
                at = (off, length)
        if at is None:
            out.append(None)
            continue
        off = at[0]
        records, storage = struct.unpack('>HH', data[off + 2:off + 6])
        found = None
        for i in range(records):
            pid, eid, lid, nid, length, where = struct.unpack(
                '>6H', data[off + 6 + 12 * i:off + 18 + 12 * i])
            if (pid, eid, lid, nid) == (3, 1, 0x409, 4):
                found = data[off + storage + where:
                             off + storage + where + length].decode(
                                 'utf-16-be', 'replace')
        out.append(found)
    return out


def check_coverage(fonts_dir):
    """What the installed fonts between them can draw.

    Deleting families is a scope decision; this is where the scope is kept. It
    reads the fonts the image carries, not the configuration, so it also
    catches a font that is installed but truncated.
    """
    have = set()
    for name, index in sorted((f.name, i) for f in fonts_dir.iterdir()
                              if f.is_file() and not f.is_symlink()
                              for i in ([0, 1, 2, 3] if f.suffix == '.ttc' else [0])):
        try:
            have |= cmap_points(fonts_dir / name, index)
        except (struct.error, IndexError, ValueError) as e:
            check(False, '%s cannot be read as a font: %s' % (name, e))
    for label, want in wanted_points():
        missing = sorted(want - have)
        check(not missing, '%s: %d of %d code points have no installed font, '
                           'first %s' % (label, len(missing), len(want),
                                         ' '.join('U+%04X %s' % (c, chr(c))
                                                  for c in missing[:10])))


def table(path, tag, index=0):
    """One table's bytes, by tag."""
    data = path.read_bytes()
    if data[:4] == b'ttcf':
        n = struct.unpack('>I', data[8:12])[0]
        base = struct.unpack('>%dI' % n, data[12:12 + 4 * n])[index]
    else:
        base = 0
    for i in range(struct.unpack('>H', data[base + 4:base + 6])[0]):
        rec = base + 12 + 16 * i
        name, _, off, length = struct.unpack('>4sIII', data[rec:rec + 16])
        if name == tag:
            return data[off:off + length]
    return None


def check_font_files(fonts_dir):
    """Each installed font is a whole font.

    A coverage check does not see a truncated file: the tables it still has
    parse, and what it lost is usually covered by another font anyway, so the
    union comes out right. Nothing else looks either -- the export checks that
    the files fonts.xml names exist, not that they are complete. So this reads
    each one's table directory and asks whether every table is inside the file.
    """
    for path in sorted(fonts_dir.iterdir()):
        if not path.is_file() or path.is_symlink():
            continue
        data = path.read_bytes()
        size = len(data)
        try:
            if data[:4] == b'ttcf':
                count = struct.unpack('>I', data[8:12])[0]
                bases = struct.unpack('>%dI' % count, data[12:12 + 4 * count])
            else:
                bases = (0,)
            tables = []
            for base in bases:
                num = struct.unpack('>H', data[base + 4:base + 6])[0]
                if base + 12 + 16 * num > size:
                    raise ValueError('the table directory runs past the file')
                for i in range(num):
                    rec = base + 12 + 16 * i
                    tag, _, off, length = struct.unpack('>4sIII', data[rec:rec + 16])
                    tables.append((tag.decode('latin-1'), off, length))
        except (struct.error, IndexError, ValueError, UnicodeDecodeError) as e:
            check(False, '%s is not a readable font: %s' % (path.name, e))
            continue
        past = sorted({tag for tag, off, length in tables if off + length > size})
        check(not past,
              '%s is %d bytes and its %s table%s %s past the end: the file is '
              'truncated' % (path.name, size, ', '.join(past),
                             '' if len(past) == 1 else 's',
                             'runs' if len(past) == 1 else 'run'))
        names = {tag for tag, _, _ in tables}
        for required in ('cmap', 'head', 'hhea', 'name'):
            check(required in names,
                  '%s has no %s table' % (path.name, required))


def check_font_chain(fonts_dir, hyphens_dir):
    """The four properties AOSP's own fontchain_lint checks.

    It cannot run here: external/fonttools in the tree is 2.4 and calls
    array.fromstring, which Python 3.9 removed, and installing a current
    fontTools would put a dependency outside the tree. Saying "its checks do
    not bind for this change" was reasoning, not a check, so here they are,
    read off the fonts the image carries.
    """
    fams = families(FONTS_XML)
    check(len(fams) < 254,
          'Minikin takes at most 253 families in the default collection, and '
          'fonts.xml has %d' % len(fams))

    # Which face inside the collection each language is given. The numbers in
    # fonts.xml are AOSP's, and they are right today -- index 0 is JP, 1 KR,
    # 2 SC, 3 TC -- but nothing in the configuration says so, and the eight
    # faces are in whatever order the upstream font ships them in. Reordered
    # upstream, Chinese would quietly be laid out with the Japanese forms of
    # the unified Han characters: the same glyph slot, a different drawing, and
    # no error anywhere. So the index is checked against the face's own name.
    want_face = {'zh-Hans': 'SC', 'zh-Hant': 'TC', 'ja': 'JP', 'ko': 'KR'}
    for fam_name, lang, fonts in fams:
        if lang not in want_face:
            continue
        for file_name, index in fonts:
            path = fonts_dir / file_name
            if not path.is_file():
                continue
            names = ttc_face_names(path)
            face = names[int(index or 0)] if int(index or 0) < len(names) else None
            check(face is not None and (' CJK %s ' % want_face[lang]) in face,
                  'fonts.xml gives %s index %s of %s, and that face is %r, not '
                  'the %s one' % (lang, index or 0, file_name, face,
                                  want_face[lang]))

    # parse_fonts_xml's own assertions: a named family carries no language or
    # variant, a fallback family's variant is one of two words, every weight is
    # a multiple of 100 and every style is one of two words.
    root = ET.parse(FONTS_XML).getroot()
    for fam in root.findall('family'):
        name = fam.get('name')
        if name:
            check(fam.get('lang') is None and fam.get('variant') is None,
                  'family %s is named, so it may not carry lang or variant' % name)
        else:
            check(fam.get('variant') in (None, 'elegant', 'compact'),
                  'family lang=%s has variant %r' % (fam.get('lang'), fam.get('variant')))
        for font in fam.findall('font'):
            weight = font.get('weight')
            check(weight is not None and int(weight) % 100 == 0,
                  'font %s has weight %s, not a multiple of 100'
                  % (font.text.strip(), weight))
            check(font.get('style') in ('normal', 'italic'),
                  'font %s has style %r' % (font.text.strip(), font.get('style')))

    # check_vertical_metrics: the families that share a line box have to agree
    # on it, or text in one jumps against text in another.
    for name, ymax, ymin in (('sans-serif', 2163, -555), ('sans-serif-condensed', 2163, -555)):
        for fam_name, _, fonts in fams:
            if fam_name != name:
                continue
            for file_name, _ in fonts:
                head = table(fonts_dir / file_name, b'head')
                if head is None:
                    check(False, '%s has no head table' % file_name)
                    continue
                got_min, got_max = struct.unpack('>hh', head[38:40] + head[42:44])
                check((got_max, got_min) == (ymax, ymin),
                      '%s has yMax %d yMin %d, not AOSP\'s %d and %d'
                      % (file_name, got_max, got_min, ymax, ymin))
    for name in ('sans-serif', 'sans-serif-condensed', 'serif', 'monospace'):
        for fam_name, _, fonts in fams:
            if fam_name != name:
                continue
            for file_name, _ in fonts:
                hhea = table(fonts_dir / file_name, b'hhea')
                if hhea is None:
                    check(False, '%s has no hhea table' % file_name)
                    continue
                ascent, descent = struct.unpack('>hh', hhea[4:8])
                check((ascent, descent) == (1900, -500),
                      '%s has ascent %d descent %d, not AOSP\'s 1900 and -500'
                      % (file_name, ascent, descent))

    # check_cjk_punctuation: every font ahead of the CJK family must leave
    # U+3000..U+301F alone, or Chinese punctuation comes from a Latin font.
    punctuation = set(range(0x3000, 0x3020))
    for fam_name, lang, fonts in fams:
        if lang in ('zh-Hans', 'zh-Hant', 'ja', 'ko'):
            break
        for file_name, index in fonts:
            got = cmap_points(fonts_dir / file_name, int(index or 0)) & punctuation
            check(not got,
                  '%s comes before the CJK families and covers %d of the CJK '
                  'punctuation, so Chinese punctuation would come from it'
                  % (file_name, len(got)))

    # check_hyphens: every script with a pattern file needs a font that can
    # draw a hyphen, or Minikin breaks a line at a character it cannot show.
    check(hyphens_dir.is_dir(),
          'nothing is staged at %s, so the English hyphenation pair the '
          'product installs cannot be confirmed' % hyphens_dir)
    if hyphens_dir.is_dir():
        hyphens = {0x002D, 0x2010}
        drawn = set()
        for fam_name, lang, fonts in fams:
            for file_name, index in fonts:
                if cmap_points(fonts_dir / file_name, int(index or 0)) & hyphens:
                    drawn.add(file_name)
        patterns = sorted(p.stem[len('hyph-'):] for p in hyphens_dir.glob('*.hyb'))
        check(patterns == ['en-gb', 'en-us'],
              'the installed hyphenation patterns are %s, not the English pair; '
              'every one of them needs a font for its script' % patterns)
        check(drawn, 'no installed font draws a hyphen, and hyphenation patterns '
                     'are installed')


def check_repository():
    """What the two files in this repository say about each other."""
    tree = ET.parse(FONTS_XML)
    root = tree.getroot()
    fams = families(FONTS_XML)

    named = {name for name, _, _ in fams if name}
    check(named == {'sans-serif', 'sans-serif-condensed', 'serif', 'monospace',
                    'serif-monospace', 'casual', 'cursive', 'sans-serif-smallcaps'},
          'the named families are not the eight AOSP ones: %s' % sorted(named))

    # An alias to a family that is not there is a family an application asks for
    # and does not get. ElementTree keeps document order, so this also checks
    # AOSP's own rule that an alias follows what it points at.
    seen = set()
    for child in root:
        if child.tag == 'family' and child.get('name'):
            seen.add(child.get('name'))
        elif child.tag == 'alias':
            check(child.get('to') in seen,
                  'alias %s points at %s, which is not a family above it'
                  % (child.get('name'), child.get('to')))

    langs = {lang for _, lang, _ in fams if lang}
    check(langs == KEEP_LANG,
          'the fallback languages are not the kept set: %s' % sorted(langs))

    # The .ttc index is the one value in this file that is wrong silently: the
    # file exists, the family loads, the glyphs are the wrong region's.
    cjk = {lang: fonts for name, lang, fonts in fams
           if not name and fonts and fonts[0][0].endswith('.ttc')}
    check(cjk == {'zh-Hans': [('NotoSansCJK-Regular.ttc', '2')],
                  'zh-Hant': [('NotoSansCJK-Regular.ttc', '3')],
                  'ja': [('NotoSansCJK-Regular.ttc', '0')],
                  'ko': [('NotoSansCJK-Regular.ttc', '1')]},
          'the CJK families do not carry AOSP\'s .ttc indices: %r' % cjk)

    wanted = {f for _, _, fonts in fams for f, _ in fonts if f.startswith('Noto')}
    packages = product_packages(PW3_MK.read_text())
    listed = {p for p in packages if p.startswith('Noto')}
    check(wanted == listed,
          'fonts.xml and pw3.mk disagree: only in fonts.xml %s, only in pw3.mk %s'
          % (sorted(wanted - listed), sorted(listed - wanted)))

    check({p for p in packages if p.startswith('hyph-')} == HYPHENATION,
          'the hyphenation list is not the English pair: %s'
          % sorted(p for p in packages if p.startswith('hyph-')))

    check('NotoColorEmoji.ttf' not in packages,
          'NotoColorEmoji.ttf is still installed: 7.28 MB of colour bitmaps '
          'that fonts.xml no longer names')
    # A font this repository does not carry has to be fetched, or the build
    # stops at a missing LOCAL_SRC_FILES with nothing saying where to get it.
    # By full path, not by name: the module's LOCAL_SRC_FILES is relative to
    # its own directory, so a manifest that fetches the right bytes to the
    # wrong place leaves a clean build with no source file.
    fetched = {f['path'] for f in json.loads(PREBUILTS.read_text())['files']}
    for font in sorted(DEVICE_FONTS):
        want = str(EMOJI_DIR.relative_to(ROOT) / font)
        check(want in fetched,
              '%s is not fetched to %s, where its module looks for it (the '
              'manifest has %s)' % (font, want,
                                    sorted(p for p in fetched if font in p) or 'nothing'))
        check((EMOJI_DIR / 'Android.mk').is_file(),
              '%s has no module, so nothing installs it' % font)
    check('pw3-fonts.xml' not in packages,
          'pw3.mk installs a second module on /system/etc/fonts.xml. That does '
          'not override AOSP\'s and does not collide with it: kati merges the '
          'two copy rules into one and keeps AOSP\'s recipe')

    # How the replacement actually happens: one module keeps the path and is
    # told which file to copy. The hook is patch 0052; the file is named in
    # BoardConfig.mk, where the build-time variables are.
    check(PATCH.is_file(), '%s is not there, so AOSP\'s Android.mk has no '
                           'REPLACEMENT_FONTS_FILE hook' % PATCH.name)
    board = BOARD_CONFIG.read_text()
    named = re.search(r'^REPLACEMENT_FONTS_FILE := *(\S+)$', board, re.M)
    check(named is not None,
          'BoardConfig.mk does not set REPLACEMENT_FONTS_FILE, so AOSP\'s '
          'fonts.xml is installed')
    if named:
        check(named.group(1) == 'device/amazon/pw3/fonts/fonts.xml',
              'REPLACEMENT_FONTS_FILE names %s, not the file in this directory'
              % named.group(1))

    # The inherit line, not the name anywhere in the file: both names appear in
    # the comments that say why they are not inherited.
    inherited = set(re.findall(r'^\$\(call inherit-product, *([^)]+?) *\)$',
                               PW3_MK.read_text(), re.M))
    for mk, installs in (('external/noto-fonts/fonts.mk', 'all 160 Noto fonts'),
                         ('external/hyphenation-patterns/patterns.mk',
                          'all 35 hyphenation locales')):
        check(mk not in inherited, 'pw3.mk still inherits %s, which installs %s'
              % (mk, installs))
    for mk in ('frameworks/base/data/fonts/fonts.mk',
               'external/roboto-fonts/fonts.mk',
               'external/google-fonts/dancing-script/fonts.mk',
               'external/google-fonts/carrois-gothic-sc/fonts.mk',
               'external/google-fonts/coming-soon/fonts.mk',
               'external/google-fonts/cutive-mono/fonts.mk'):
        check(mk in inherited,
              'pw3.mk no longer inherits %s, which installs fonts that '
              'fonts.xml still names, or the module that copies it' % mk)


def check_against_aosp(tree_root):
    """The stronger property: deletions only, against AOSP's own file."""
    # Every one of these is required. Skipping a check because its input is
    # absent reports a pass for something nobody looked at, which is how a tree
    # with the hook commented out, or no tree at all, would have come back clean.
    needed = {name: Path(tree_root) / name for name in (
        'frameworks/base/data/fonts/fonts.xml',
        'frameworks/base/data/fonts/Android.mk',
        'external/noto-fonts/fonts.mk',
        'external/hyphenation-patterns/patterns.mk')}
    absent = sorted(n for n, p in needed.items() if not p.is_file())
    if absent:
        print('cannot check against %s: it is missing %s'
              % (tree_root, ', '.join(absent)))
        return False

    aosp = needed['frameworks/base/data/fonts/fonts.xml']
    aosp_lines = aosp.read_text().splitlines(keepends=True)
    ours = FONTS_XML.read_text().splitlines(keepends=True)

    # Deletions only, apart from the one substitution named above. Equality with
    # the derivation would catch a changed line too, but says "they differ";
    # this says which line was not a deletion.
    replaced = 0
    for tag, i1, i2, j1, j2 in difflib.SequenceMatcher(
            None, aosp_lines, ours, autojunk=False).get_opcodes():
        if tag == 'replace' and ''.join(aosp_lines[i1:i2]) == EMOJI_FROM \
                and ''.join(ours[j1:j2]) == EMOJI_TO:
            replaced += 1
            continue
        check(tag in ('equal', 'delete'),
              'line %d of fonts.xml is a %s, not a deletion of AOSP\'s file: %r'
              % (j1 + 1, tag, ''.join(ours[j1:j2])[:70]))
    check(replaced == 1,
          'the emoji family is not the one substitution this file is allowed '
          '(%d of them)' % replaced)

    check(ours == derive(aosp_lines),
          'fonts.xml is not what deleting the non-Chinese, non-English families '
          'from AOSP\'s file gives; regenerate it')

    # A font we install but AOSP's own list does not have is a name typed wrong:
    # the module does not exist and the build says only "unknown module".
    packages = product_packages(PW3_MK.read_text())
    theirs = set(re.findall(r'(Noto[\w.-]+\.tt[fc])',
                            needed['external/noto-fonts/fonts.mk'].read_text()))
    ours_noto = {p for p in packages if p.startswith('Noto')} - DEVICE_FONTS
    check(ours_noto <= theirs,
          'pw3.mk installs Noto fonts external/noto-fonts/fonts.mk does not '
          'list: %s' % sorted(ours_noto - theirs))
    check(not (DEVICE_FONTS & theirs),
          'a font this device supplies has the name of an AOSP module, so '
          'two modules would install it: %s' % sorted(DEVICE_FONTS & theirs))

    locales = set(re.findall(
        r'^ +([a-z][\w-]*) *\\?$',
        needed['external/hyphenation-patterns/patterns.mk'].read_text(), re.M))
    ours_hyph = {p[len('hyph-'):].replace('.lic.txt', '')
                 for p in packages if p.startswith('hyph-')}
    check(ours_hyph <= locales,
          'pw3.mk installs hyphenation locales patterns.mk does not have: %s'
          % sorted(ours_hyph - locales))

    # The hook itself, in the tree: the patch is append-only and the ledger
    # records it, but neither says the module reads the variable. Matched as
    # make directives at the start of a line, so the same words in a comment
    # -- this file's own explanation of the hook, for one -- do not pass for it.
    text = needed['frameworks/base/data/fonts/Android.mk'].read_text()
    check(re.search(r'^ifdef REPLACEMENT_FONTS_FILE$', text, re.M) is not None
          and re.search(r'^AOSP_FONTS_FILE := \$\(REPLACEMENT_FONTS_FILE\)$',
                        text, re.M) is not None,
          'frameworks/base/data/fonts/Android.mk does not read '
          'REPLACEMENT_FONTS_FILE; patch 0052 is not applied')
    # They cannot compose: add_additional_fonts.py opens AOSP's fonts.xml by
    # name, so a replacement set alongside it would be ignored in silence.
    check(re.search(r'^\$\(error REPLACEMENT_FONTS_FILE and '
                    r'ADDITIONAL_FONTS_FILE cannot both be set\)$', text, re.M)
          is not None,
          'the hook does not refuse ADDITIONAL_FONTS_FILE alongside it, so '
          'setting both would silently install AOSP\'s configuration')

    # What actually reached the staged tree. The checks above say the
    # configuration is right; this one says the build agreed, which is the step
    # that failed the first time: both modules wrote /system/etc/fonts.xml.
    # These staged files are not optional inputs: a tree was given, so they
    # are the thing being checked. Letting is_file() decide whether to assert
    # meant that deleting the staged configuration, or the whole fonts
    # directory, removed the checks instead of failing them.
    staged = Path(tree_root) / 'out/target/product/pw3/system/etc/fonts.xml'
    check(staged.is_file(),
          'nothing is staged at %s, so what the build installed as the font '
          'configuration cannot be confirmed' % staged)
    if staged.is_file():
        check(staged.read_bytes() == FONTS_XML.read_bytes(),
              'the staged /system/etc/fonts.xml is not ours (%d bytes, ours is %d)'
              % (staged.stat().st_size, FONTS_XML.stat().st_size))
    fonts_dir = Path(tree_root) / 'out/target/product/pw3/system/fonts'
    check(fonts_dir.is_dir(),
          'nothing is staged at %s, so neither the fonts the configuration '
          'names nor the coverage they give can be read' % fonts_dir)
    if fonts_dir.is_dir():
        before = len(failures)
        check_font_files(fonts_dir)
        # Everything below reads glyph coverage out of these files. Against a
        # font that is not whole it reads whatever the bytes happen to say, so
        # there is nothing to learn from it and the real fault is already named.
        if len(failures) == before:
            check_coverage(fonts_dir)
            check_font_chain(
                fonts_dir,
                Path(tree_root) / 'out/target/product/pw3/system/usr/hyphen-data')
        else:
            print('a font is not whole: coverage and chain not checked')
    return True


def main():
    global checks
    ap = argparse.ArgumentParser()
    ap.add_argument('--aosp-tree')
    args = ap.parse_args()

    # An unhandled exception here printed a traceback and no verdict at all,
    # which reads as "nothing to report" to anything collecting the output.
    # Whatever goes wrong is a failure of this check, and says so in its place.
    try:
        if args.aosp_tree:
            if not check_against_aosp(args.aosp_tree):
                return 1
        else:
            check_repository()
    except Exception:  # noqa: BLE001 - any fault here is this test's answer
        checks += 1
        failures.append('the check itself stopped: %s'
                        % traceback.format_exc().strip().replace('\n', ' | ')[-400:])

    print('%d checks, %d failed' % (checks, len(failures)))
    for f in failures:
        print('  FAIL %s' % f)
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
