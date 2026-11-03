#!/usr/bin/env python3
"""tools/add-emoji-variation-sequences.py, against the font it is for.

It rewrites a third-party font, so the questions are: does the result still
say what the original said, does it say the one new thing, and is nothing else
different. Each is checked by reading the output back, not by trusting the
writer: the parser here is written from the OpenType cmap documentation and
shares no code with the generator.

Needs --font (the fetched NotoEmoji-VF.ttf) and --sequences
(external/unicode/emoji-variation-sequences.txt, in the AOSP tree).
"""
import argparse
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
TOOL = ROOT / 'device/amazon/pw3/emoji/add-emoji-variation-sequences.py'
VS16 = 0xFE0F

checks = 0
failures = []


def check(ok, what):
    global checks
    checks += 1
    if not ok:
        failures.append(what)


def directory(data):
    num = struct.unpack('>H', data[4:6])[0]
    out = {}
    for i in range(num):
        tag, _, off, length = struct.unpack('>4sIII', data[12 + 16 * i:28 + 16 * i])
        out[tag.decode('latin-1')] = (off, length)
    return out


def recorded_checksums(data):
    num = struct.unpack('>H', data[4:6])[0]
    out = {}
    for i in range(num):
        tag, chk, _, _ = struct.unpack('>4sIII', data[12 + 16 * i:28 + 16 * i])
        out[tag.decode('latin-1')] = chk
    return out


def checksum(block):
    block = block + b'\0' * ((-len(block)) % 4)
    return sum(struct.unpack('>%dI' % (len(block) // 4), block)) & 0xFFFFFFFF


def subtable(data, at):
    """One cmap subtable's bytes.

    Its length is in its own header, in a place that depends on the format:
    the long formats carry a uint32 at +4, the short ones a uint16 at +2.
    """
    fmt = struct.unpack('>H', data[at:at + 2])[0]
    if fmt in (8, 10, 12, 13, 14):
        length = struct.unpack('>I', data[at + 4:at + 8])[0]
    else:
        length = struct.unpack('>H', data[at + 2:at + 4])[0]
    return data[at:at + length]


def encodings(data):
    off = directory(data)['cmap'][0]
    num = struct.unpack('>H', data[off + 2:off + 4])[0]
    out = []
    for i in range(num):
        pid, eid, sub = struct.unpack('>HHI', data[off + 4 + 8 * i:off + 12 + 8 * i])
        fmt = struct.unpack('>H', data[off + sub:off + sub + 2])[0]
        out.append((pid, eid, fmt, off + sub))
    return out


def uvs_points(data):
    """Code points the format 14 subtable records for U+FE0F, read fresh."""
    found = [at for _, _, fmt, at in encodings(data) if fmt == 14]
    if len(found) != 1:
        return None, len(found)
    at = found[0]
    _, _, records = struct.unpack('>HII', data[at:at + 10])
    points = set()
    selectors = []
    for i in range(records):
        r = at + 10 + 11 * i
        selector = struct.unpack('>I', b'\0' + data[r:r + 3])[0]
        default_off, nondefault_off = struct.unpack('>II', data[r + 3:r + 11])
        selectors.append(selector)
        if selector != VS16 or not default_off:
            continue
        d = at + default_off
        ranges = struct.unpack('>I', data[d:d + 4])[0]
        for j in range(ranges):
            e = d + 4 + 4 * j
            start = struct.unpack('>I', b'\0' + data[e:e + 3])[0]
            extra = data[e + 3]
            points.update(range(start, start + extra + 1))
    return points, selectors


def expected(path):
    out = set()
    for line in Path(path).read_text().splitlines():
        line = line.split('#', 1)[0]
        if 'emoji style' not in line:
            continue
        m = re.match(r'\s*([0-9A-Fa-f]{4,6})\s+FE0F\s*;', line)
        if m:
            out.add(int(m.group(1), 16))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--font', required=True, type=Path)
    ap.add_argument('--sequences', required=True, type=Path)
    args = ap.parse_args()

    original = args.font.read_bytes()
    with tempfile.TemporaryDirectory() as work:
        out = Path(work) / 'out.ttf'
        run = subprocess.run([sys.executable, '-I', str(TOOL), '--font', str(args.font),
                              '--sequences', str(args.sequences), '--output', str(out)],
                             capture_output=True, text=True)
        check(run.returncode == 0, 'the generator failed: %s' % run.stderr.strip()[:200])
        if run.returncode:
            return report()
        built = out.read_bytes()

        # Running it again on its own output must be refused, not silently add
        # a second subtable -- that is what an incremental build would do.
        again = subprocess.run([sys.executable, '-I', str(TOOL), '--font', str(out),
                                '--sequences', str(args.sequences),
                                '--output', str(Path(work) / 'twice.ttf')],
                               capture_output=True, text=True)
        check(again.returncode != 0 and 'already has a format 14' in again.stderr,
              'the generator does not refuse a font that already has the table')

    want = expected(args.sequences)
    check(len(want) == 351, 'the Unicode file no longer lists 351 emoji style '
                            'sequences but %d; AOSP\'s colour font carried 351' % len(want))

    before, _ = uvs_points(original)
    check(before is None or before == set(),
          'the upstream font already carries variation sequences; this tool '
          'would be adding what is there')

    got, selectors = uvs_points(built)
    check(got is not None, 'the result does not have exactly one format 14 '
                           'subtable (%s)' % selectors)
    if got is not None:
        check(selectors == [VS16],
              'the format 14 subtable records selectors %s, not only U+FE0F'
              % ['U+%04X' % s for s in selectors])
        check(got == want,
              'the recorded sequences are not Unicode\'s emoji style set: '
              'missing %s, extra %s'
              % (sorted(want - got)[:8], sorted(got - want)[:8]))

    # (0, 5) is the only pair a format 14 subtable is valid under, and the
    # records must stay sorted or a reader may stop early.
    enc = encodings(built)
    check(any(p == 0 and e == 5 and f == 14 for p, e, f, _ in enc),
          'the new subtable is not registered under platform 0 encoding 5: %s'
          % [(p, e, f) for p, e, f, _ in enc])
    check([(p, e) for p, e, _, _ in enc] == sorted((p, e) for p, e, _, _ in enc),
          'the cmap encoding records are no longer sorted: %s'
          % [(p, e) for p, e, _, _ in enc])

    old_enc = {(p, e, f) for p, e, f, _ in encodings(original)}
    new_enc = {(p, e, f) for p, e, f, _ in enc}
    # The symmetric difference, not just what was gained: a record that was
    # dropped left the other form of this check satisfied, while its message
    # claimed to be reporting both directions.
    check(new_enc ^ old_enc == {(0, 5, 14)},
          'the cmap gained or lost a subtable other than the new one: %s'
          % sorted(new_enc ^ old_enc))

    # Every subtable the font already had, byte for byte. Without this, cmap is
    # unchecked apart from the new subtable, because the table-by-table
    # comparison below has to skip the whole of cmap: emptying both format 12
    # subtables (nGroups = 0), which removes every mapping above the BMP and so
    # every emoji this font is for, passed all 30 checks.
    old_subs = {(p, e, f): subtable(original, at)
                for p, e, f, at in encodings(original)}
    new_subs = {(p, e, f): subtable(built, at) for p, e, f, at in enc}
    for key in sorted(old_subs):
        check(old_subs[key] == new_subs.get(key),
              'the cmap subtable for platform %d encoding %d format %d is not '
              'the one the upstream font had (%d bytes against %d)'
              % (key + (len(old_subs[key]), len(new_subs.get(key) or b''))))

    # Nothing outside cmap may differ: this rewrites someone else's font.
    old_dir, new_dir = directory(original), directory(built)
    check(set(old_dir) == set(new_dir),
          'the table directory gained or lost tables: %s'
          % sorted(set(old_dir) ^ set(new_dir)))
    for tag in sorted(set(old_dir) & set(new_dir) - {'cmap', 'head'}):
        o, ol = old_dir[tag]
        n, nl = new_dir[tag]
        check(original[o:o + ol] == built[n:n + nl],
              'the %s table is not the one the upstream font had' % tag)
    if 'head' in old_dir:
        # head differs only in checkSumAdjustment, bytes 8..12.
        o, ol = old_dir['head']
        n, nl = new_dir['head']
        check(original[o:o + 8] == built[n:n + 8]
              and original[o + 12:o + ol] == built[n + 12:n + nl],
              'the head table differs outside checkSumAdjustment')

    # Every checksum in the directory, and the whole-file one. head's own entry
    # is included rather than skipped: it is computed with checkSumAdjustment
    # zeroed, and the version of the writer that zeroed the field only after
    # the directory had been written produced a correct whole-file checksum and
    # a wrong head entry, which a whole-file check alone does not see.
    chk = recorded_checksums(built)
    for tag, (off, length) in sorted(new_dir.items()):
        block = built[off:off + length]
        if tag == 'head':
            block = block[:8] + b'\0\0\0\0' + block[12:]
        check(checksum(block) == chk.get(tag),
              'the directory checksum for %s is 0x%08x and the table sums to '
              '0x%08x' % (tag, chk.get(tag) or 0, checksum(block)))
    if 'head' in new_dir:
        at = new_dir['head'][0] + 8
        zeroed = bytearray(built)
        zeroed[at:at + 4] = b'\0\0\0\0'
        stored = struct.unpack('>I', built[at:at + 4])[0]
        check((checksum(bytes(zeroed)) + stored) & 0xFFFFFFFF == 0xB1B0AFBA,
              'the whole-file checksum does not come to 0xB1B0AFBA: the file '
              'sums to 0x%08x with the field zeroed and the field holds 0x%08x'
              % (checksum(bytes(zeroed)), stored))

    check(len(built) > len(original),
          'the result is not larger than the font it added a table to')
    return report()


def report():
    print('%d checks, %d failed' % (checks, len(failures)))
    for f in failures:
        print('  FAIL %s' % f)
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
