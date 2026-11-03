#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Tests for the installer's screens (screen.c), in the build VM: every screen is put
together by the C code from render_screens.py's output and must equal, pixel for
pixel, the preview render_screens.py drew; the PNG must decode with Pillow and
Python's zlib (CRCs and Adler-32 checked).

Usage: test_screen.py --screens DIR --zstd-archive zstd-1.5.6.tar.gz
  --screens  render_screens.py output
"""
import argparse, hashlib, json, struct, subprocess, sys, tarfile, tempfile, zlib
from pathlib import Path

from PIL import Image

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
ZSTD_FLAGS = ['-DZSTD_LEGACY_SUPPORT=0', '-DZSTD_TRACE=0', '-DZSTD_DISABLE_ASM', '-DDEBUGLEVEL=0']
# The sample values render_screens.py draws in its previews.
SAMPLES = {'DOWNLOAD': (370, 0), 'VERIFY': (820, 0), 'SYSTEM': (450, 0), 'DATA': (90, 0), 'E401': (-1, 812)}


def build(tmp, archive, screens):
    lock = json.loads((ROOT / 'source-lock.json').read_text())['installer_zstd']
    if hashlib.sha256(archive.read_bytes()).hexdigest() != lock['archive_sha256']:
        sys.exit('the zstd archive is not the one in source-lock.json')
    with tarfile.open(archive) as tar:
        tar.extractall(tmp / 'zstd', filter='data')
    lib = tmp / 'zstd' / ('zstd-' + lock['version']) / 'lib'
    objects = []
    for c in sorted(list((lib / 'common').glob('*.c')) + list((lib / 'decompress').glob('*.c'))):
        o = tmp / (c.parent.name + '-' + c.stem + '.o')
        subprocess.run(['gcc', '-std=gnu11', '-O2', '-c', *ZSTD_FLAGS, '-I', str(lib), '-I', str(lib / 'common'), str(c),
                        '-o', str(o)], check=True)
        objects.append(str(o))
    binary = tmp / 'test_screen'
    subprocess.run(['gcc', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-I', str(HERE), '-I', str(screens), '-I', str(lib), str(HERE / 'screen.c'), str(HERE / 'test_screen.c'),
                    *objects, '-o', str(binary)], check=True)
    return binary


def chunks(data):
    """PNG chunks with their CRCs checked."""
    assert data[:8] == b'\x89PNG\r\n\x1a\n'
    pos, out = 8, []
    while pos < len(data):
        length, kind = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        crc = struct.unpack('>I', data[pos + 8 + length:pos + 12 + length])[0]
        assert zlib.crc32(kind + body) == crc, 'CRC of ' + kind.decode()
        out.append((kind, body))
        pos += 12 + length
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--screens', type=Path, required=True)
    ap.add_argument('--zstd-archive', type=Path, required=True)
    a = ap.parse_args()
    result = json.loads((a.screens / 'build-result.json').read_text())
    x, y, w, h = result['region']
    # The receipt says which screens were built, and used to be the only word on
    # it: a receipt listing none ran no screen at all and printed "0 of 0
    # passed" (Codex 2026-10-31). The previews on disk are the second witness --
    # one file per screen, written in the same run -- and SAMPLES below is this
    # file's own list of the screens that carry a sample value, so every name in
    # it has to be there.
    built = set(result['screens'])
    drawn = {p.stem for p in sorted((a.screens / 'preview').glob('*.png'))}
    if not built:
        sys.exit('build-result.json lists no screen: there is nothing here to test')
    if built != drawn:
        sys.exit('build-result.json and the previews disagree about which screens exist: %s'
                 % ', '.join(sorted(built ^ drawn)))
    if not set(SAMPLES) <= built:
        sys.exit('screens with a sample value that were not built: %s'
                 % ', '.join(sorted(set(SAMPLES) - built)))
    failures = 0
    with tempfile.TemporaryDirectory(prefix='pw3-screen-test-') as t:
        tmp = Path(t)
        binary = build(tmp, a.zstd_archive, a.screens)
        for name in result['screens']:
            permille, number = SAMPLES.get(name, (-1, 0))
            p = subprocess.run([str(binary), str(a.screens / 'screens.bin'), str(tmp), name, str(permille), str(number)],
                               capture_output=True, text=True)
            ok, why = p.returncode == 0, p.stderr.strip()
            if ok:
                data = (tmp / 'screen.png').read_bytes()
                try:
                    parts = chunks(data)
                    raw = zlib.decompress(b''.join(body for kind, body in parts if kind == b'IDAT'))
                    ok = len(raw) == h * (w + 1) and all(raw[r * (w + 1)] == 0 for r in range(h))
                    got = Image.open(tmp / 'screen.png')
                    got.load()
                    want = Image.open(a.screens / 'preview' / (name + '.png')).crop((x, y, x + w, y + h))
                    ok = ok and got.mode == 'L' and got.size == (w, h) and got.tobytes() == want.tobytes()
                    why = '' if ok else 'differs from the preview'
                except (AssertionError, zlib.error, OSError) as e:
                    ok, why = False, str(e)
            failures += not ok
            print('%-4s %s %s' % ('ok' if ok else 'FAIL', name, why))
    count = len(result['screens'])
    print('%d of %d passed' % (count - failures, count))
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
