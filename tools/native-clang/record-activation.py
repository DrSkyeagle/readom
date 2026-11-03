#!/usr/bin/env python3
"""Record a verified native compiler selection; never start a build or mount."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import tempfile


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prefix-root', type=Path, required=True,
                        help='Frozen keyed directory containing manifest.json and host/')
    parser.add_argument('--comparison', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    manifest_path = args.prefix_root / 'manifest.json'
    manifest = json.loads(manifest_path.read_text())
    proof = json.loads(args.comparison.read_text())
    key = manifest['key']
    if not re.fullmatch(r'[0-9a-f]{20}', key) or args.prefix_root.name != key:
        raise ValueError('Frozen prefix key mismatch')
    if proof['passed'] is not True or proof['compiler_stable'] is not True:
        raise ValueError('Comparison did not pass')
    if len(proof['cases']) != 4 or not all(
        c['code_equal'] is True and c['dependencies_equal'] is True
        for c in proof['cases']
    ):
        raise ValueError('Four matching objects and dependency sets required')
    if sorted(c['variants'][0]['machine'] for c in proof['cases']) != [3, 40, 40, 62]:
        raise ValueError('ARM, i386 and x86_64 samples required')
    if proof['harness_sha256'] != digest(Path(__file__).with_name('compare.py')):
        raise ValueError('Comparison harness changed')
    prefix = args.prefix_root / 'host/linux-x86/clang-4053586/bin'
    compiler_sha = digest(prefix / 'clang.native')
    if not compiler_sha == proof['compiler_sha256'] == manifest['compiler_sha256']:
        raise ValueError('Compiler differs from verified candidate')
    for name in ('clang', 'clang++'):
        if digest(prefix / name) != manifest['wrapper_sha256'][name]:
            raise ValueError('Compiler wrapper changed')
        if digest(prefix / (name + '.real')) != manifest['adapter_sha256']:
            raise ValueError('Compiler adapter changed')
    if args.output.exists() or args.output.is_symlink():
        raise ValueError('Activation already exists; refusing replacement')
    record = {
        'schema': 1,
        'prefix_key': key,
        'prefix_manifest_sha256': digest(manifest_path),
        'comparison_sha256': digest(args.comparison),
        'compiler_sha256': compiler_sha,
        'selection': 'Source-pinned ARM64 host execution; ARM/x86/x86_64 target outputs tested; no Android target ABI change',
        'target_runtime_verified': False,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    fd, temp = tempfile.mkstemp(prefix='.activation-', dir=args.output.parent)
    try:
        with os.fdopen(fd, 'w') as stream:
            json.dump(record, stream, indent=2)
            stream.write('\n')
            stream.flush()
            os.fsync(stream.fileno())
        # Atomic no-replace publication. A concurrent selection is not overwritten.
        os.link(temp, args.output)
    finally:
        os.unlink(temp)
    print('Verified activation recorded; the next locked build provisions it.')


if __name__ == '__main__':
    main()
