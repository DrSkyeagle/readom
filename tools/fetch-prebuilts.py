#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Fetch the third-party APKs the PW3 image carries unmodified, from where they
are published, and check them against their recorded size and SHA-256.

They are not kept in the source tree: they are other projects' releases (their
provenance and licences: device/amazon/pw3/apps/*/PW3-SOURCE.md and NOTICE),
and the WebView alone is 87 MB. The list is device/amazon/pw3/apps/prebuilts.json.

Usage: fetch-prebuilts.py [--check] [--dest DIR]
  --check  only verify what is there (no network); exit 1 if anything is
           missing or different, naming the command that fetches it
  --dest   write into DIR/<path> instead of the source tree (for testing)
"""
import argparse
import base64
import hashlib
import json
import os
import sys
import tempfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / 'device/amazon/pw3/apps/prebuilts.json'


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def _umask():
    """The process umask, which can only be read by setting it."""
    value = os.umask(0o022)
    os.umask(value)
    return value


def present(path, item):
    return path.is_file() and path.stat().st_size == item['bytes'] and sha256(path) == item['sha256']


def download(item, target):
    """Into a temporary file next to target, verified, then renamed over it."""
    request = urllib.request.Request(item['url'], headers={'User-Agent': 'pw3-fetch-prebuilts'})
    target.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(prefix='.fetch-', dir=target.parent)
    try:
        with urllib.request.urlopen(request, timeout=120) as response, os.fdopen(fd, 'wb') as out:
            if item.get('encoding') == 'base64':  # gitiles serves a file's bytes as ?format=TEXT
                out.write(base64.b64decode(response.read()))
            else:
                for block in iter(lambda: response.read(1 << 20), b''):
                    out.write(block)
        if not present(Path(name), item):
            raise SystemExit('%s: downloaded file is not the recorded one (size or SHA-256)' % item['path'])
        # mkstemp makes the file private to its owner and os.replace keeps that.
        # These files are built from, by whoever runs the build -- in the build
        # VM that is another account than the one that fetched them.
        os.chmod(name, 0o644 & ~_umask())
        os.replace(name, target)
    finally:
        if os.path.exists(name):
            os.unlink(name)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--dest', type=Path)
    args = parser.parse_args()
    base = args.dest.resolve() if args.dest else ROOT
    missing = []
    for item in json.loads(MANIFEST.read_text())['files']:
        target = base / item['path']
        if present(target, item):
            # A file fetched before this script set the mode is still 0600,
            # and present() only looks at size and hash, so without this the
            # repair never reaches the files that need it.
            want = 0o644 & ~_umask()
            have = target.stat().st_mode & 0o777
            if have != want and not args.check:
                os.chmod(target, want)
                print('ok       %s (mode %o -> %o)' % (item['path'], have, want))
                continue
            if have != want:
                missing.append(item['path'])
                print('mode %o  %s (want %o)' % (have, item['path'], want))
                continue
            print('ok       ' + item['path'])
        elif args.check:
            missing.append(item['path'])
            print('missing  ' + item['path'])
        else:
            print('fetching ' + item['path'] + ' from ' + item['url'], flush=True)
            download(item, target)
            print('ok       ' + item['path'])
    if missing:
        print('Run: python3 ' + str(Path(__file__).relative_to(ROOT)) + ' (from ' + str(ROOT) + ')', file=sys.stderr)
        sys.exit(1)


if __name__ == '__main__':
    main()
