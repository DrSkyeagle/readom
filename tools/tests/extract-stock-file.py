#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Copy one file out of the pinned kernel archive, as Amazon shipped it.

Two tests here compare a file before and after a patch of this port's
(tools/tests/test_kbuild_cmd_cache.py, tools/tests/test_pmic_i2c.py), and the
"before" side has to be the stock file rather than a reconstruction of it. Both
sat in the skipped list for weeks because supplying that side meant keeping a
second kernel tree by hand, while the archive those trees are made from is
pinned (source-lock.json) and holds every stock file exactly once.

It reads the one member and writes it; nothing is unpacked, so a member name
cannot reach outside the output path given.

Usage: extract-stock-file.py --archive ARCHIVE --path scripts/Kbuild.include --output FILE
"""
import argparse
import tarfile
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--archive', type=Path, required=True)
    parser.add_argument('--path', required=True, help="the member's path inside the archive")
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    with tarfile.open(args.archive, mode='r:gz') as archive:
        try:
            member = archive.getmember(args.path)
        except KeyError:
            raise SystemExit('%s holds no %s' % (args.archive, args.path))
        if not member.isfile():
            raise SystemExit('%s is not a file in %s' % (args.path, args.archive))
        source = archive.extractfile(member)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(source.read())
    print('%s: %s, %d bytes' % (args.archive.name, args.path, args.output.stat().st_size))


if __name__ == '__main__':
    main()
