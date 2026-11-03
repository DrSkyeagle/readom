#!/usr/bin/env python3
"""Update one pinned Jack server constant from SSLv3 to TLSv1.2.

The server's certificate and client-authentication code is unchanged. This
changes the host build service only, not the Jack compiler or target output.
Run with the build service stopped; source is the immutable AOSP prebuilt.
"""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import socket
import struct
import zipfile

ORIGINAL_SHA = '4f0acef6b8e514357ea45441e89eb3fa6d4b273a4fdce6e4e26d7713eeac46dc'
MEMBER = 'com/android/jack/server/JackHttpServer.class'


def digest(data):
    return hashlib.sha256(data).hexdigest()


def patch_class(data):
    if data[:4] != bytes.fromhex('cafebabe'):
        raise ValueError('Not a Java class')
    count = struct.unpack_from('>H', data, 8)[0]
    at, index, matches = 10, 1, []
    widths = {3: 4, 4: 4, 5: 8, 6: 8, 7: 2, 8: 2, 9: 4,
              10: 4, 11: 4, 12: 4, 15: 3, 16: 2, 18: 4}
    while index < count:
        tag = data[at]
        at += 1
        if tag == 1:
            length = struct.unpack_from('>H', data, at)[0]
            if data[at + 2:at + 2 + length] == b'SSLv3':
                matches.append((at, at + 2 + length))
            at += 2 + length
        elif tag in widths:
            at += widths[tag]
            if tag in (5, 6):
                index += 1
        else:
            raise ValueError('Unexpected class constant type')
        index += 1
    if len(matches) != 1:
        raise ValueError('Expected exactly one SSLv3 constant in pinned class')
    begin, end = matches[0]
    return data[:begin] + struct.pack('>H', 7) + b'TLSv1.2' + data[end:]


def patch(source, installed, receipt):
    original = source.read_bytes()
    if digest(original) != ORIGINAL_SHA:
        raise ValueError('Unrecognized AOSP Jack server; refusing binary edit')
    output = io.BytesIO()
    with zipfile.ZipFile(io.BytesIO(original)) as zin, zipfile.ZipFile(output, 'w') as zout:
        if any(n.startswith('META-INF/') and n.endswith(('.SF', '.RSA', '.DSA', '.EC'))
               for n in zin.namelist()):
            raise ValueError('Signed JAR requires a source rebuild')
        if zin.namelist().count(MEMBER) != 1:
            raise ValueError('Server class missing or duplicated')
        for info in zin.infolist():
            data = zin.read(info)
            zout.writestr(info, patch_class(data) if info.filename == MEMBER else data)
    updated = output.getvalue()
    with zipfile.ZipFile(io.BytesIO(original)) as a, zipfile.ZipFile(io.BytesIO(updated)) as b:
        changed = [n for n in a.namelist() if a.read(n) != b.read(n)]
    if changed != [MEMBER]:
        raise ValueError('Unexpected JAR member change')
    current = installed.read_bytes()
    if digest(current) not in (ORIGINAL_SHA, digest(updated)):
        raise ValueError('Installed server has unexpected changes')
    if current != updated:
        for port in (8076, 8077):
            with socket.socket() as probe:
                probe.settimeout(0.5)
                if probe.connect_ex(('127.0.0.1', port)) == 0:
                    raise ValueError('Stop the running Jack service before changing its JAR')
        metadata = installed.stat()
        temporary = installed.with_suffix('.tls12.tmp')
        with temporary.open('xb') as stream:
            stream.write(updated)
        os.chmod(temporary, metadata.st_mode & 0o777)
        os.chown(temporary, metadata.st_uid, metadata.st_gid)
        os.replace(temporary, installed)
    receipt.write_text(json.dumps({
        'original_sha256': ORIGINAL_SHA,
        'installed_sha256': digest(updated),
        'changed_members': changed,
        'change': 'SSLContext algorithm SSLv3 to TLSv1.2',
        'scope': 'Host Jack server transport only; compiler JAR unchanged',
        'client_authentication_code_changed': False,
        'runtime_tls_verified': False,
    }, indent=2) + '\n')
    print('Pinned Jack server TLS 1.2 constant installed; runtime check still required.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('installed', type=Path)
    parser.add_argument('receipt', type=Path)
    args = parser.parse_args()
    patch(args.source, args.installed, args.receipt)
