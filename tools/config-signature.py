#!/usr/bin/env python3
"""Fingerprint Kconfig inputs and effective config, excluding unrelated C edits."""
import hashlib
import sys
from pathlib import Path

source=Path(sys.argv[1])
digest=hashlib.sha256()
for path in sorted(p for p in source.rglob('Kconfig*') if p.is_file()):
    digest.update(str(path.relative_to(source)).encode()+b'\0')
    digest.update(path.read_bytes()+b'\0')
for arg in sys.argv[2:]:
    path=Path(arg)
    digest.update(str(path).encode()+b'\0'+path.read_bytes()+b'\0')
print(digest.hexdigest())
