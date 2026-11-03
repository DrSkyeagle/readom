#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Unpack one file of an installer payload, and check the manifest was right.

The payload's manifest records what each file becomes once unpacked -- its size
and its SHA-256 -- and the installer on the device refuses an image whose
unpacked form does not match. Nothing off the device checks that those recorded
numbers are true of the file in the payload, so this does, as a side effect of
handing the image to whoever asked for it.

The data image is sparse, so unpacking the 1.75 GiB seed takes about a third of
a second and 60 MB of disk. That is why the test that reads it does not have to
be "run elsewhere".

Usage: unpack-payload-file.py --payload DIR --name system|data --output FILE
"""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--payload', type=Path, required=True,
                        help='installer/build_payload.py output (manifest.json)')
    parser.add_argument('--name', required=True, help='which file of the payload')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    manifest = json.loads((args.payload / 'manifest.json').read_text())
    try:
        entry = manifest['files'][args.name]
    except KeyError:
        raise SystemExit('%s holds no %r; it has %s'
                         % (args.payload / 'manifest.json', args.name,
                            ', '.join(sorted(manifest['files']))))
    if entry['format'] != 'zstd':
        raise SystemExit('%s is %s, which this does not unpack' % (args.name, entry['format']))
    source = args.payload / entry['name']
    if source.stat().st_size != entry['bytes'] or \
            hashlib.sha256(source.read_bytes()).hexdigest() != entry['sha256']:
        raise SystemExit('%s is not the file the manifest describes' % source)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(['zstd', '-d', '-q', '-f', '-o', str(args.output), str(source)], check=True)
    digest = hashlib.sha256()
    with args.output.open('rb') as unpacked:
        for block in iter(lambda: unpacked.read(1 << 20), b''):
            digest.update(block)
    size = args.output.stat().st_size
    if size != entry['unpacked_bytes'] or digest.hexdigest() != entry['unpacked_sha256']:
        raise SystemExit('%s unpacks to %d bytes, %s; the manifest says %d bytes, %s'
                         % (entry['name'], size, digest.hexdigest(),
                            entry['unpacked_bytes'], entry['unpacked_sha256']))
    print('%s: %d bytes unpacked, as the manifest says' % (entry['name'], size))


if __name__ == '__main__':
    main()
