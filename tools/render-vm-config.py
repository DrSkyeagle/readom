#!/usr/bin/env python3
"""Render this checkout's paths into the pinned Lima template."""
import argparse
import json
import os
from pathlib import Path
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
port = Path(__file__).resolve().parents[1]
project = port.parent
values = {
    '@PW3_PORT_ROOT@': port,
    '@PW3_KERNEL_INPUTS@': project / 'work/android-port/kernel-research',
    '@PW3_BUILD_SHARE@': project / 'work/android-port/build-share',
}
result = (port / 'config/build-vm.template.yaml').read_text()
for token, path in values.items():
    if result.count(token) != 1:
        raise SystemExit('Invalid VM template path placeholder count')
    result = result.replace(token, json.dumps(str(path), ensure_ascii=False))
args.output.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
if args.output.is_symlink():
    raise SystemExit('Refusing a symlink configuration output')
with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', dir=args.output.parent,
                                 prefix='.pw3-vm-', delete=False) as f:
    f.write(result)
    temporary = Path(f.name)
try:
    os.replace(temporary, args.output)
finally:
    temporary.unlink(missing_ok=True)
