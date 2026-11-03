#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Tests for pw3-unpack (build_unpack.py output), in the build VM: the ARM binary runs
under qemu-arm-static on inputs made here, and optionally on a release payload.

Usage: test_unpack.py BINARY [--payload DIR --scratch DIR]
  --payload  build_payload.py output (manifest.json): both files are unpacked into
             --scratch (about 2.5 GB) and timed; the outputs are deleted afterwards
"""
import argparse, hashlib, json, random, struct, subprocess, sys, tempfile, time
from pathlib import Path

QEMU = '/usr/bin/qemu-arm-static'


def sha(data):
    return hashlib.sha256(data).hexdigest()


class Runner:
    def __init__(self, binary, tmp):
        self.binary, self.tmp, self.failures, self.count = binary, tmp, 0, 0

    def run(self, packed, out, size, digest, in_size=None, in_digest=None):
        data = packed.read_bytes()
        args = [QEMU, str(self.binary), str(packed), str(len(data) if in_size is None else in_size),
                in_digest or sha(data), str(out), str(size), digest]
        p = subprocess.run(args, capture_output=True, text=True)
        return p.returncode, p.stdout, p.stderr

    def expect(self, name, code, result, check=None):
        self.count += 1
        got, out, err = result
        ok = got == code and (check is None or check())
        print('%-4s %s (exit %d%s)' % ('ok' if ok else 'FAIL', name, got, ', ' + err.strip() if err.strip() else ''))
        if not ok:
            self.failures += 1
            print(out[-400:])


def zstd(data, *opts):
    return subprocess.run(['zstd', '-q', '-c', *opts], input=data, capture_output=True, check=True).stdout


def unit_tests(r, tmp):
    rnd = random.Random(7)
    image = rnd.randbytes(3 << 20) + bytes(20 << 20) + rnd.randbytes(1 << 20) + b'end'
    digest, size = sha(image), len(image)
    out = tmp / 'out.img'

    packed = tmp / 'a.zst'
    packed.write_bytes(zstd(image, '-3'))
    r.expect('zstd: unpacks to the image', 0, r.run(packed, out, size, digest),
             lambda: out.read_bytes() == image)
    out.write_bytes(bytes(size + (5 << 20)))
    r.expect('zstd: a larger existing output is overwritten and cut', 0, r.run(packed, out, size, digest),
             lambda: out.stat().st_size == size and sha(out.read_bytes()) == digest)

    half = len(image) // 2
    skippable = struct.pack('<II', 0x184D2A50, 5) + b'skip!'
    packed.write_bytes(zstd(image[:half], '-1') + skippable + zstd(image[half:], '-1'))
    r.expect('zstd: several frames and a skippable frame', 0, r.run(packed, out, size, digest),
             lambda: out.read_bytes() == image)

    whole = bytes(20 << 20)   # whole output buffers (1 MiB), the frame ending as one fills
    packed.write_bytes(zstd(whole, '-19'))
    r.expect('zstd: a frame ending exactly at a full output buffer', 0, r.run(packed, out, len(whole), sha(whole)),
             lambda: out.stat().st_size == len(whole) and sha(out.read_bytes()) == sha(whole))

    far = rnd.randbytes(1 << 20)
    wide = far + bytes(12 << 20) + far   # a match 13 MiB back: needs a window over 8 MiB
    packed.write_bytes(zstd(wide, '-3', '--long=27'))
    r.expect('zstd: a window over 8 MiB is refused', 3, r.run(packed, out, len(wide), sha(wide)))

    good = zstd(image, '-3')
    packed.write_bytes(good[:-10])
    r.expect('zstd: a short input is incomplete', 1,
             r.run(packed, out, size, digest, in_size=len(good), in_digest=sha(good)))
    bad = bytearray(good)
    bad[len(bad) // 2] ^= 0xFF
    packed.write_bytes(bad)
    r.expect('zstd: a changed byte is corrupt', 2, r.run(packed, out, size, digest, in_digest=sha(good)))
    packed.write_bytes(good)
    r.expect('zstd: a wrong output SHA-256 is a payload defect', 3, r.run(packed, out, size, sha(b'x')))
    r.expect('zstd: a larger manifest size is a payload defect', 3, r.run(packed, out, size + 1, digest))
    r.expect('zstd: a smaller manifest size is a payload defect', 3, r.run(packed, out, size - 1, digest))
    packed.write_bytes(good[:len(good) // 3])
    r.expect('zstd: a stream ending inside a frame is a payload defect', 3, r.run(packed, out, size, digest))
    packed.write_bytes(good)
    # The screen shows this number ("delete some books to free this much"), so a
    # run that stops for space has to say how much: every path out of the writer
    # used to leave it at zero, and the screen then asked for 0 MB.
    res = r.run(packed, Path('/dev/full'), size, digest)
    r.expect('zstd: no space, and it says how much is missing', 4, res,
             lambda: 'more bytes needed' in res[2])
    p = subprocess.run([QEMU, str(r.binary), str(tmp / 'none'), '1', '0' * 64, str(out), '1', '0' * 64],
                       capture_output=True, text=True)
    r.expect('zstd: a missing input is incomplete', 1, (p.returncode, p.stdout, p.stderr))


def payload_test(r, payload, scratch):
    manifest = json.loads((payload / 'manifest.json').read_text())
    for key in ('system', 'data'):
        f = manifest['files'][key]
        out = scratch / ('payload-' + key + '.img')
        start = time.monotonic()
        args = [QEMU, str(r.binary), str(payload / f['name']), str(f['bytes']), f['sha256'], str(out),
                str(f['unpacked_bytes']), f['unpacked_sha256']]
        p = subprocess.run(args, capture_output=True, text=True)
        seconds = time.monotonic() - start
        r.expect('payload %s: %s -> %d bytes in %.0f s under QEMU' % (key, f['name'], f['unpacked_bytes'], seconds),
                 0, (p.returncode, p.stdout, p.stderr))
        out.unlink(missing_ok=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('binary', type=Path)
    ap.add_argument('--payload', type=Path)
    ap.add_argument('--scratch', type=Path)
    a = ap.parse_args()
    with tempfile.TemporaryDirectory(prefix='pw3-unpack-test-') as t:
        r = Runner(a.binary.resolve(), Path(t))
        unit_tests(r, Path(t))
        if a.payload:
            payload_test(r, a.payload, a.scratch)
    print('%d of %d passed' % (r.count - r.failures, r.count))
    sys.exit(1 if r.failures else 0)


if __name__ == '__main__':
    main()
