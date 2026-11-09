#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build pw3-unpack (unpack/: README.md "解包工具"), a static ARM program, in the build VM.

It links zstd's decompression library (source-lock.json "installer_zstd", BSD-3-Clause
OR GPL-2.0-only) and the SHA-256 of boot/handoff/.

Usage: build_unpack.py --zstd-archive zstd-1.5.6.tar.gz --output DIR
  --zstd-archive  the release archive named in source-lock.json; checked against its SHA-256
"""
import argparse, hashlib, json, os, platform, subprocess, sys, tarfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
MUSL = '/home/builder/toolchains/musl-arm/bin/musl-gcc'
READELF = '/home/builder/toolchains/aosp-arm-gcc49/bin/arm-linux-androideabi-readelf'
# Decompression only; no tracing hooks, no legacy (pre-1.0) formats, no assembly.
ZSTD_FLAGS = ['-DZSTD_LEGACY_SUPPORT=0', '-DZSTD_TRACE=0', '-DZSTD_DISABLE_ASM', '-DDEBUGLEVEL=0']


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--zstd-archive', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    a = ap.parse_args()
    if platform.node() != (os.environ.get('READOM_BUILD_HOST') or 'lima-pw3') or os.geteuid() == 0:
        sys.exit('Run as the builder in the build VM.')
    lock = json.loads((ROOT / 'source-lock.json').read_text())['installer_zstd']
    if sha(a.zstd_archive) != lock['archive_sha256']:
        sys.exit('the zstd archive is not the one in source-lock.json')
    out = a.output
    out.mkdir(parents=True, exist_ok=False)
    with tarfile.open(a.zstd_archive) as tar:
        tar.extractall(out / 'src', filter='data')
    zstd = out / 'src' / ('zstd-' + lock['version']) / 'lib'
    objects = []
    for c in sorted(list((zstd / 'common').glob('*.c')) + list((zstd / 'decompress').glob('*.c'))):
        o = out / 'obj' / (c.parent.name + '-' + c.stem + '.o')
        o.parent.mkdir(exist_ok=True)
        subprocess.run([MUSL, '-std=gnu11', '-O2', '-c', *ZSTD_FLAGS, '-I', str(zstd), '-I', str(zstd / 'common'),
                        str(c), '-o', str(o)], check=True)
        objects.append(str(o))
    binary = out / 'pw3-unpack'
    subprocess.run([MUSL, '-std=gnu11', '-static', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-I', str(zstd), '-I', str(ROOT / 'boot/handoff'),
                    str(HERE / 'unpack/unpack.c'), str(HERE / 'unpack/main.c'), str(ROOT / 'boot/handoff/sha256.c'),
                    *objects, '-o', str(binary)], check=True)
    elf = subprocess.check_output([READELF, '-h', '-lW', str(binary)], text=True)
    if 'INTERP' in elf or 'ARM' not in elf:
        sys.exit('pw3-unpack is not a static ARM executable')
    result = {'schema': 1, 'binary': binary.name, 'bytes': binary.stat().st_size, 'sha256': sha(binary),
              'zstd_version': lock['version'], 'zstd_archive_sha256': lock['archive_sha256'],
              'sources_sha256': {p: sha(ROOT / p) for p in
                                 ['installer/unpack/unpack.c', 'installer/unpack/unpack.h', 'installer/unpack/main.c',
                                  'boot/handoff/sha256.c', 'boot/handoff/sha256.h']}}
    (out / 'build-result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=1))


if __name__ == '__main__':
    main()
