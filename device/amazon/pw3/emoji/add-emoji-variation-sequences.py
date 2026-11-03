#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Give an emoji font the cmap format 14 subtable Android asks it about.

Android answers "can this be drawn?" for an emoji written with the emoji
variation selector U+FE0F through Paint.hasGlyph(), which reaches
FontFamily::hasGlyph(codepoint, variationSelector) in Minikin. With a
variation selector that path reads mCmapFmt14Coverage and nothing else:

    if (mCmapFmt14Coverage.empty()) {
        return false;
    }

(frameworks/minikin/libs/minikin/FontFamily.cpp). A font with no format 14
subtable therefore answers "no" for every sequence, however well it draws it,
and the drawing path does not care: FontCollection::calcCoverageScore scores a
family that covers the base character whether or not it carries the table. So
the font renders the emoji and the system says it cannot. An application that
asks first -- EmojiCompat does, through PaintCompat.hasGlyph -- then supplies
its own font or hides the character.

AOSP's NotoColorEmoji.ttf carries 351 of these mappings, which is every emoji
style sequence in external/unicode/emoji-variation-sequences.txt. The
monochrome Noto Emoji carries none. This builds them from that same Unicode
file, which is the authority AOSP's own font follows, keeping only the ones
whose base character the font actually covers.

The mappings go in a default UVS table: "this sequence is supported and uses
the glyph the ordinary cmap gives for the base character", which is what a
monochrome font means. Minikin merges the default and non-default tables into
one coverage set (CmapCoverage.cpp, getCoverageFromUVSTable), so a default-only
table is read the same way.

Everything outside the cmap table is copied byte for byte.
"""
import argparse
import re
import struct
import sys
from pathlib import Path

VS16 = 0xFE0F
# The platform and encoding a format 14 subtable is registered under; there is
# no other valid pair for it.
UVS_PLATFORM, UVS_ENCODING = 0, 5


def read_tables(data):
    """tag -> (offset, length), in the order the directory lists them."""
    if data[:4] == b'ttcf':
        raise SystemExit('a font collection has no single table directory')
    num = struct.unpack('>H', data[4:6])[0]
    out = {}
    for i in range(num):
        rec = 12 + 16 * i
        tag, checksum, off, length = struct.unpack('>4sIII', data[rec:rec + 16])
        out[tag.decode('latin-1')] = (off, length)
    return out


def cmap_subtables(data, cmap_off):
    """(platformID, encodingID, format, offset, length) for each subtable."""
    num = struct.unpack('>H', data[cmap_off + 2:cmap_off + 4])[0]
    records = []
    for i in range(num):
        at = cmap_off + 4 + 8 * i
        pid, eid, sub = struct.unpack('>HHI', data[at:at + 8])
        records.append((pid, eid, cmap_off + sub))
    # A subtable's length is in its own header, at a different place per format.
    out = []
    for pid, eid, at in records:
        fmt = struct.unpack('>H', data[at:at + 2])[0]
        if fmt in (0, 2, 4, 6):
            length = struct.unpack('>H', data[at + 2:at + 4])[0]
        elif fmt in (8, 10, 12, 13, 14):
            length = struct.unpack('>I', data[at + 4:at + 8])[0] if fmt != 14 \
                else struct.unpack('>I', data[at + 2:at + 6])[0]
        else:
            raise SystemExit('cmap subtable format %d not handled' % fmt)
        out.append((pid, eid, fmt, at, length))
    return out


def covered(data, subtables):
    """Every code point the font's ordinary cmap maps to a glyph."""
    best = None
    for pid, eid, fmt, at, _ in subtables:
        if fmt not in (4, 12):
            continue
        rank = {(3, 10): 3, (0, 4): 3, (0, 6): 3, (3, 1): 2, (0, 3): 2}.get((pid, eid), 1)
        if best is None or rank > best[0]:
            best = (rank, fmt, at)
    if best is None:
        raise SystemExit('the font has no format 4 or 12 cmap subtable')
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
        groups = struct.unpack('>I', data[at + 12:at + 16])[0]
        for i in range(groups):
            g = at + 16 + 12 * i
            start, end, glyph = struct.unpack('>III', data[g:g + 12])
            # A sequential map group gives c the glyph startGlyphID + (c -
            # startCharCode), so the only character in the group that can land
            # on glyph 0 is its first, and only when startGlyphID is 0. Glyph 0
            # is .notdef, which is not coverage: taking the whole group would
            # have this tool record "this base supports the sequence" for a
            # character the font does not draw. The fixed font has no such
            # group; a future one may.
            if glyph == 0:
                start += 1
            if start <= end:
                points.update(range(start, end + 1))
    return points


def emoji_bases(path):
    """The base character of every emoji style sequence Unicode defines."""
    out = set()
    for line in Path(path).read_text().splitlines():
        line = line.split('#', 1)[0]
        if 'emoji style' not in line:
            continue
        m = re.match(r'\s*([0-9A-Fa-f]{4,6})\s+FE0F\s*;', line)
        if m:
            out.add(int(m.group(1), 16))
    return out


def default_uvs_ranges(points):
    """(start, additionalCount) runs; additionalCount is one byte, so <= 255."""
    ranges = []
    for cp in sorted(points):
        if ranges and cp == ranges[-1][0] + ranges[-1][1] + 1 and ranges[-1][1] < 255:
            ranges[-1][1] += 1
        else:
            ranges.append([cp, 0])
    return ranges


def format14(ranges):
    header = 10          # format, length, numVarSelectorRecords
    record = 11          # varSelector, defaultUVSOffset, nonDefaultUVSOffset
    table = 4 + 4 * len(ranges)
    length = header + record + table
    out = struct.pack('>HII', 14, length, 1)
    out += struct.pack('>I', VS16)[1:]          # 24-bit variation selector
    out += struct.pack('>II', header + record, 0)   # default table, no non-default
    out += struct.pack('>I', len(ranges))
    for start, extra in ranges:
        out += struct.pack('>I', start)[1:] + bytes([extra])
    assert len(out) == length
    return out


def checksum(block):
    if len(block) % 4:
        block = block + b'\0' * (4 - len(block) % 4)
    return sum(struct.unpack('>%dI' % (len(block) // 4), block)) & 0xFFFFFFFF


def build(data, new_cmap):
    """The font with its cmap replaced, directory and checksums rebuilt."""
    tables = read_tables(data)
    order = sorted(tables, key=lambda t: tables[t][0])
    num = len(order)
    offset = 12 + 16 * num
    body, records = b'', {}
    for tag in order:
        off, length = tables[tag]
        block = new_cmap if tag == 'cmap' else data[off:off + length]
        if tag == 'head':
            # head.checkSumAdjustment is zeroed here, before anything is
            # summed, because the spec's order is: zero the field, enter every
            # table's checksum in the directory (head's included), sum the
            # whole file, store 0xB1B0AFBA minus that sum in the field. Zeroing
            # it after the directory was written -- which is what this did --
            # leaves the directory holding head's checksum over four bytes the
            # file no longer has. The whole-file checksum came out right, so
            # checking only that one does not see it.
            block = block[:8] + b'\0\0\0\0' + block[12:]
        pad = (-len(body)) % 4
        body += b'\0' * pad
        offset_here = 12 + 16 * num + len(body)
        records[tag] = (checksum(block), offset_here, len(block))
        body += block
    entry = max(i for i in range(17) if (1 << i) <= num)
    head = struct.pack('>IHHHH', struct.unpack('>I', data[:4])[0], num,
                       (1 << entry) * 16, entry, num * 16 - (1 << entry) * 16)
    directory = b''.join(struct.pack('>4sIII', tag.encode('latin-1'), *records[tag])
                         for tag in sorted(records))
    font = bytearray(head + directory + body)
    # The field is already zero in the assembled file (above), so this sum is
    # the one the spec asks for and the value goes straight in.
    if 'head' in records:
        at = records['head'][1] + 8
        font[at:at + 4] = struct.pack('>I', (0xB1B0AFBA - checksum(bytes(font)))
                                      & 0xFFFFFFFF)
    return bytes(font)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--font', required=True, type=Path)
    ap.add_argument('--sequences', required=True, type=Path,
                    help='external/unicode/emoji-variation-sequences.txt')
    ap.add_argument('--output', required=True, type=Path)
    args = ap.parse_args()

    data = args.font.read_bytes()
    tables = read_tables(data)
    if 'cmap' not in tables:
        raise SystemExit('%s has no cmap table' % args.font)
    cmap_off, cmap_len = tables['cmap']
    subs = cmap_subtables(data, cmap_off)
    if any(fmt == 14 for _, _, fmt, _, _ in subs):
        raise SystemExit('%s already has a format 14 subtable; this would add a '
                         'second one' % args.font)

    wanted = emoji_bases(args.sequences)
    have = covered(data, subs)
    bases = wanted & have
    if not bases:
        raise SystemExit('none of the %d emoji style sequences in %s have a base '
                         'character this font covers' % (len(wanted), args.sequences))
    ranges = default_uvs_ranges(bases)
    new_sub = format14(ranges)

    # Rebuild the cmap: the records are sorted by (platform, encoding), and the
    # subtable data follows them, each kept where it was relative to the others.
    keep = sorted(subs, key=lambda s: (s[0], s[1]))
    entries = [(pid, eid, data[at:at + length]) for pid, eid, _, at, length in keep]
    entries.append((UVS_PLATFORM, UVS_ENCODING, new_sub))
    entries.sort(key=lambda e: (e[0], e[1]))
    head = struct.pack('>HH', 0, len(entries))
    body, offsets = b'', []
    base = 4 + 8 * len(entries)
    for _, _, block in entries:
        offsets.append(base + len(body))
        body += block
    cmap = head + b''.join(struct.pack('>HHI', pid, eid, off)
                           for (pid, eid, _), off in zip(entries, offsets)) + body

    args.output.write_bytes(build(data, cmap))
    print('%s: %d emoji style sequences in %d ranges (%d of Unicode\'s %d have a '
          'base this font covers); cmap %d -> %d bytes'
          % (args.output.name, len(bases), len(ranges), len(bases), len(wanted),
             cmap_len, len(cmap)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
