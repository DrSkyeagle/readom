#!/usr/bin/env python3
"""Materialize a pinned compiler once; never rewrite files used by live builds."""
import hashlib
import json
import os
import sys
import tarfile
from pathlib import Path

archive, target=map(Path,sys.argv[1:])
expected=json.loads(Path('/mnt/port/source-lock.json').read_text())['cross_compiler']['archive_sha256']
if hashlib.sha256(archive.read_bytes()).hexdigest()!=expected:
    raise SystemExit('Compiler archive checksum mismatch')
marker=target/'.pw3-toolchain-archive-sha256'
if marker.exists():
    if marker.read_text().strip()!=expected:
        raise SystemExit('Existing compiler pin differs; choose a new toolchain directory.')
    sys.exit(0)
target.mkdir(parents=True,exist_ok=True)
populated=any(target.iterdir())
with tarfile.open(archive) as tar:
    for member in tar.getmembers():
        path=Path(member.name)
        if path.is_absolute() or '..' in path.parts:
            raise SystemExit('Unsafe compiler archive path')
        if member.issym() or member.islnk():
            link=Path(member.linkname)
            resolved=(target/path.parent/link).resolve()
            if target.resolve() not in resolved.parents:
                raise SystemExit('Compiler archive link leaves its tree')
        if member.isdev() or member.isfifo():
            raise SystemExit('Unexpected special compiler archive entry')
    if not populated:
        tar.extractall(target)
    # Adopt an existing project compiler only after verifying actual bytes and
    # symlinks. Verification is read-only even while another build is active.
    for member in tar.getmembers():
        path=target/member.name
        if member.isfile():
            if not path.is_file() or hashlib.sha256(path.read_bytes()).digest()!=hashlib.sha256(tar.extractfile(member).read()).digest():
                raise SystemExit('Existing compiler file differs: '+member.name)
        elif member.issym():
            if not path.is_symlink() or os.readlink(path)!=member.linkname:
                raise SystemExit('Existing compiler symlink differs: '+member.name)
marker.write_text(expected+'\n')
