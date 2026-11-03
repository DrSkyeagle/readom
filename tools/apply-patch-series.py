#!/usr/bin/env python3
"""Apply an append-only patch series without GNU patch's automatic reversal."""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path


def apply_series(source, patches):
    ledger_path = source / '.pw3-applied-patches.json'
    expected = [{'name': p.name, 'sha256': hashlib.sha256(p.read_bytes()).hexdigest()} for p in patches]
    recorded = json.loads(ledger_path.read_text()) if ledger_path.exists() else []
    if recorded != expected[:len(recorded)]:
        # Which patch, and which tree. A new patch in the middle of a series is
        # the ordinary way to meet this -- a tree that applies two directories
        # in turn records them in that order, so a patch added to the first one
        # lands before the second one's entries and no longer appends. The
        # answer is always the same (start from a fresh tree), but the message
        # used to say neither where the tree is nor what changed, and in a
        # build script that runs on its own the path is the part the reader
        # does not have (PW3 Android, the loader-stage kernel, 2026-10-23).
        differs = next((i for i, item in enumerate(recorded)
                        if i >= len(expected) or expected[i] != item), len(recorded))
        here = recorded[differs]['name'] if differs < len(recorded) else '(nothing)'
        wanted = expected[differs]['name'] if differs < len(expected) else '(nothing)'
        raise RuntimeError(
            'The recorded patch series differs at position %d: the tree has %s where the series '
            'now has %s. No files changed. Start from a fresh source tree: %s'
            % (differs + 1, here, wanted, source))
    for item, patch in zip(expected[len(recorded):], patches[len(recorded):]):
        command = ['git', '-C', str(source), 'apply', '--whitespace=nowarn']
        check = subprocess.run(command + ['--check', str(patch)], capture_output=True, text=True)
        if check.returncode == 0:
            subprocess.run(command + [str(patch)], check=True)
            print('Applied ' + patch.name, flush=True)
        else:
            reverse = subprocess.run(command + ['--reverse', '--check', str(patch)], capture_output=True, text=True)
            if reverse.returncode:
                raise RuntimeError('Patch does not match pinned source: ' + patch.name + '\n' + check.stderr)
            print('Recorded already-applied ' + patch.name, flush=True)
        recorded.append(item)
        temporary = ledger_path.with_suffix('.tmp')
        temporary.write_text(json.dumps(recorded, indent=2) + '\n')
        temporary.replace(ledger_path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('patches', type=Path, nargs='+')
    args = parser.parse_args()
    apply_series(args.source.resolve(), [p.resolve() for p in args.patches])


if __name__ == '__main__':
    main()
