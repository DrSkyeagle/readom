#!/usr/bin/env python3
"""Extend the PinyinIME word list with words from rime-ice.

  make_rawdict.py AOSP_RAWDICT VALID_HANZI RIME_ICE_DIR OUT_RAWDICT
                  [--base-min-weight N] [--ext] [--max-lemmas N]

The AOSP list is kept line for line: its words and its frequencies are what the
engine was tuned with. Added are (word, reading) pairs it does not have, 2 to 8
characters, written with characters and syllables the engine already knows.

rime-ice weights are not on the AOSP scale. The AOSP list is the head of a
corpus count: every word in it has a frequency of 105 or more, so a word it
lacks was either rarer than that or did not exist yet. Hence two tiers:

  * weight >= --low-tier-below: taken as a word in common use today; it gets
    the AOSP frequency that words of similar rime-ice weight have in the AOSP
    list (median per weight band over the shared words, made monotone);
  * below: kept under the AOSP cut-off (2..100 by weight), so that it never
    outranks an AOSP word with the same letters but is there as a word of its
    own (names, titles) instead of being pieced together from fragments.

When there are more candidates than --max-lemmas allows, the heaviest stay.
"""
import argparse, bisect, math, statistics
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('aosp'); p.add_argument('valid'); p.add_argument('rime'); p.add_argument('out')
p.add_argument('--base-min-weight', type=int, default=1)
p.add_argument('--ext', action='store_true')
p.add_argument('--max-lemmas', type=int, default=499000)
p.add_argument('--low-tier-below', type=int, default=5000)
a = p.parse_args()

aosp_text = Path(a.aosp).read_bytes().decode('utf-16')
aosp = [l.split() for l in aosp_text.splitlines() if l.strip()]
valid = set(Path(a.valid).read_bytes().decode('utf-16'))
syllables = {s for l in aosp for s in l[3:]}
have = {(l[0], ' '.join(l[3:])) for l in aosp}
aosp_freq = {(l[0], ' '.join(l[3:])): float(l[1]) for l in aosp}
respell = {'lve': 'lue', 'nve': 'nue'}

def rows(name):
    body = False
    for line in open(Path(a.rime) / (name + '.dict.yaml'), encoding='utf-8'):
        line = line.rstrip('\n')
        if not body:
            body = line == '...'
            continue
        if not line or line.startswith('#'): continue
        f = line.split('\t')
        if len(f) < 2: continue
        weight = int(f[2]) if len(f) > 2 and f[2].strip().isdigit() else 0
        yield f[0], ' '.join(respell.get(s, s) for s in f[1].split()), weight

base = list(rows('base'))
# Calibration: rime-ice weight -> AOSP frequency, from the words in both lists.
pairs = sorted((w, aosp_freq[(t, py)]) for t, py, w in base if w > 0 and (t, py) in aosp_freq)
bands, xs, ys = {}, [], []
for w, f in pairs: bands.setdefault(round(math.log10(w) * 4), []).append(f)
level = 0.0
for band in sorted(bands):
    if len(bands[band]) < 20: continue
    level = max(level, math.log10(statistics.median(bands[band])))
    xs.append(band / 4); ys.append(level)
def calibrated(weight):
    x = math.log10(max(weight, 1))
    if x <= xs[0]: return 10 ** ys[0]
    if x >= xs[-1]: return 10 ** ys[-1]
    i = bisect.bisect_right(xs, x)
    return 10 ** (ys[i - 1] + (ys[i] - ys[i - 1]) * (x - xs[i - 1]) / (xs[i] - xs[i - 1]))

stats = {'aosp': len(aosp), 'shared_for_calibration': len(pairs), 'bands': len(xs)}
added, seen, dropped = [], set(have), {'length': 0, 'character': 0, 'syllable': 0, 'weight': 0, 'duplicate': 0}
def consider(source, minimum):
    for text, py, weight in source:
        if weight < minimum: dropped['weight'] += 1; continue
        if not 2 <= len(text) <= 8 or len(py.split()) != len(text): dropped['length'] += 1; continue
        if any(c not in valid for c in text): dropped['character'] += 1; continue
        if any(s not in syllables for s in py.split()): dropped['syllable'] += 1; continue
        if (text, py) in seen: dropped['duplicate'] += 1; continue
        seen.add((text, py)); added.append((weight, text, py))
consider(base, a.base_min_weight)
stats['from_base'] = len(added)
if a.ext:
    consider(rows('ext'), 1)
stats['from_ext'] = len(added) - stats['from_base']
room = a.max_lemmas - len(aosp)
if len(added) > room:
    added.sort(key=lambda e: -e[0]); stats['cut_for_limit'] = len(added) - room; del added[room:]
lines = [l for l in aosp_text.splitlines() if l.strip()]
def frequency(weight):
    if weight >= a.low_tier_below: return calibrated(weight)
    return min(100.0, max(2.0, 100.0 * (weight / a.low_tier_below) ** 0.5))
lines += ['%s %.6f 0 %s' % (text, frequency(weight), py) for weight, text, py in added]
stats['low_tier'] = sum(w < a.low_tier_below for w, _, _ in added)
Path(a.out).write_bytes(b'\xff\xfe' + ('\n'.join(lines) + '\n').encode('utf-16-le'))
stats.update(added=len(added), total=len(lines), dropped=dropped,
             calibration={('%g' % 10 ** x): round(10 ** y, 2) for x, y in zip(xs, ys)})
print(stats)
