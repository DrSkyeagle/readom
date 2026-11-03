#!/usr/bin/env python3
"""Inspect a built raw system image, read-only, inside the isolated build VM."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import re
import stat
import subprocess
import sys
import tempfile
import zipfile


def require(ok, message):
    if not ok:
        raise RuntimeError(message)


def run(*args):
    return subprocess.run(args, check=True, text=True, capture_output=True).stdout.strip()


def platform_signer(root):
    """The certificate plat_mac_permissions.xml grants seinfo=platform, as DER."""
    text = (root / 'etc/selinux/plat_mac_permissions.xml').read_text()
    for signature, seinfo in re.findall(
            r'<signer signature="([0-9a-fA-F]+)"\s*>\s*<seinfo value="([^"]+)"', text):
        if seinfo == 'platform':
            return bytes.fromhex(signature)
    return None


def apk_signer(path):
    """The certificate an APK is signed with, as DER (the v1 signature block)."""
    with zipfile.ZipFile(path) as archive:
        blocks = [name for name in archive.namelist()
                  if name.upper().startswith('META-INF/')
                  and name.upper().endswith(('.RSA', '.DSA', '.EC'))]
        require(len(blocks) == 1, '%s carries %d signature blocks' % (path, len(blocks)))
        block = archive.read(blocks[0])
    certs = subprocess.run(['openssl', 'pkcs7', '-inform', 'DER', '-print_certs'],
                           input=block, check=True, capture_output=True).stdout
    return subprocess.run(['openssl', 'x509', '-outform', 'DER'],
                          input=certs, check=True, capture_output=True).stdout


def subject(der):
    out = subprocess.run(['openssl', 'x509', '-inform', 'DER', '-noout', '-subject'],
                         input=der, check=True, capture_output=True).stdout
    return out.decode('utf-8', 'replace').strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    require(platform.node() == 'lima-pw3' and os.geteuid() == 0,
            'Run only as root in the project build VM')
    require(args.image.is_file() and not args.image.is_symlink(), 'Expected a regular raw image')
    require(not args.output.exists(), 'Refusing to overwrite an inspection')
    module = Path(__file__).parent / 'preinit-pack/pack.py'
    spec = importlib.util.spec_from_file_location('pw3_pack', module)
    pack = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = pack
    spec.loader.exec_module(pack)
    info = pack.ext4_info(args.image)
    check = subprocess.run(['e2fsck', '-f', '-n', str(args.image)], text=True, capture_output=True)
    require(check.returncode == 0, 'Read-only e2fsck failed: ' + check.stdout + check.stderr)
    records = []
    loop = None
    mounted = False
    with tempfile.TemporaryDirectory(prefix='pw3-image-audit-', dir='/home/builder/pw3-build') as temp:
        root = Path(temp)
        try:
            loop = run('losetup', '--find', '--show', '--read-only', str(args.image.resolve()))
            require(re.fullmatch(r'/dev/loop[0-9]+', loop), 'Unexpected loop-device name')
            run('mount', '-t', 'ext4', '-o', 'ro,noload,nosuid,nodev,noexec', loop, str(root))
            mounted = True
            options = run('findmnt', '--noheadings', '--output', 'OPTIONS', '--mountpoint', str(root)).split(',')
            require({'ro', 'nosuid', 'nodev', 'noexec', 'norecovery'} <= set(options),
                    'Read-only non-replaying mount options not effective')
            for directory, dirs, files in os.walk(root, followlinks=False):
                for name in sorted(dirs + files):
                    path = Path(directory) / name
                    st = path.lstat()
                    attrs = {key: os.getxattr(path, key, follow_symlinks=False)
                             for key in os.listxattr(path, follow_symlinks=False)}
                    label = attrs.get('security.selinux', b'').rstrip(b'\0').decode('ascii')
                    caps = attrs.get('security.capability', b'')
                    entry = {'path': '/system/' + str(path.relative_to(root)),
                             'mode': oct(st.st_mode), 'uid': st.st_uid, 'gid': st.st_gid,
                             'bytes': st.st_size, 'selinux': label, 'capabilities_hex': caps.hex()}
                    if stat.S_ISREG(st.st_mode):
                        entry['sha256'] = pack.sha(path)
                    elif stat.S_ISLNK(st.st_mode):
                        entry['link'] = os.readlink(path)
                    records.append(entry)
            # The certificate that mac_permissions.xml grants seinfo=platform has to be
            # the one the platform applications are actually signed with. When it is not,
            # every one of them gets seinfo=default, seapp_contexts has no rule pairing
            # uid 1000 with default, and zygote aborts the fork: SystemUI, Settings and
            # anything else that runs as system crash in a loop, with nothing in the image
            # looking wrong on its own. V83 shipped exactly that -- a file left over from
            # the previous keys (patches/aosp-sepolicy/0001) -- and the only place the two
            # halves meet is here, in the finished image.
            signer = platform_signer(root)
            require(signer is not None, 'plat_mac_permissions.xml grants no seinfo=platform')
            witness = root / 'priv-app/SystemUI/SystemUI.apk'
            require(witness.is_file(), 'no %s to check the platform signature against' % witness.name)
            signed = apk_signer(witness)
            require(signer == signed,
                    'the image is signed by one key and labelled for another: '
                    'plat_mac_permissions.xml grants seinfo=platform to %s, while '
                    'SystemUI.apk is signed by %s. Every system application would get '
                    'seinfo=default and zygote would abort.' % (subject(signer), subject(signed)))
            props = {}
            for line in (root / 'build.prop').read_text().splitlines():
                if line.strip() and not line.lstrip().startswith('#') and '=' in line:
                    key, value = line.split('=', 1)
                    if key in ('ro.build.type', 'ro.build.tags', 'ro.debuggable', 'ro.secure',
                               'ro.adb.secure', 'ro.product.device', 'ro.build.version.release'):
                        props.setdefault(key, []).append(value)
            result = {'schema': 1, 'scope': 'Offline filesystem inspection, not a boot or release approval',
                      'image': info, 'e2fsck_exit': check.returncode,
                      'e2fsck_output': check.stdout + check.stderr, 'mount_options': options,
                      'properties': props, 'entries': records,
                      'setid_entries': [r for r in records if int(r['mode'], 8) & 0o6000],
                      'world_writable_entries': [r for r in records if
                          int(r['mode'], 8) & 0o002 and not stat.S_ISLNK(int(r['mode'], 8))],
                      'capability_entries': [r for r in records if r['capabilities_hex']],
                      'unlabeled_entries': [r['path'] for r in records if not r['selinux']],
                      'private_firmware_paths': [r['path'] for r in records if
                          '/firmware/ath6k/' in r['path'] or '/firmware/epdc/' in r['path']],
                      'platform_signature': {'subject': subject(signer),
                                             'sha256': hashlib.sha256(signer).hexdigest(),
                                             'matches_signed_apk': True},
                      'device_access': False, 'inspection_completed': True}
        finally:
            if mounted:
                run('umount', str(root))
            if loop:
                run('losetup', '--detach', loop)
    require(pack.sha(args.image) == info['sha256'], 'Image changed during inspection')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open('x') as file:
        json.dump(result, file, indent=2)
        file.write('\n')
    print(json.dumps({key: value for key, value in result.items()
                      if key not in ('entries', 'e2fsck_output')}, indent=2))


if __name__ == '__main__':
    main()
