#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build the installer check (check-template.sh) in the build VM: one scriptlet for
the Kindle library that carries pw3-unpack, a 64 MiB test image, the installer's CA
bundle and two installer screens drawn by screen.c, each checked by SHA-256 on the
device before use.

Usage: build_check.py --unpack DIR --screens DIR --ca-bundle FILE --zstd-archive FILE --output DIR
  --unpack   installer/build_unpack.py output
  --screens  installer/screens/render_screens.py output
"""
import argparse, base64, hashlib, importlib.util, json, os, platform, random, re, subprocess, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
# A public release file of known content (EinkBro, GitHub Releases), as in the probe.
DOWNLOAD_URL = 'https://github.com/plateaukao/einkbro/releases/download/v16.7.1/app-armeabi-v7a-release.apk'
DOWNLOAD_SHA = '4bf5a7245141022c0bc29434afc81e7665ea3d55af718aefe3e9e12417d673b6'
NAME = 'Android-Installer-Check.sh'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ['unpack', 'screens', 'ca-bundle', 'zstd-archive', 'output']:
        ap.add_argument('--' + name, type=Path, required=True)
    a = ap.parse_args()
    if platform.node() != 'lima-pw3' or os.geteuid() == 0:
        sys.exit('Run as the builder in the build VM.')
    lock = json.loads((ROOT / 'source-lock.json').read_text())
    ca = a.ca_bundle.read_bytes()
    if sha(ca) != lock['installer_ca_bundle']['sha256']:
        sys.exit('the CA bundle is not the one in source-lock.json')
    unpack_receipt = json.loads((a.unpack / 'build-result.json').read_text())
    unpack = (a.unpack / 'pw3-unpack').read_bytes()
    if sha(unpack) != unpack_receipt['sha256']:
        sys.exit('pw3-unpack is not its build receipt\'s')
    screens = json.loads((a.screens / 'build-result.json').read_text())

    rnd = random.Random(23)
    image = rnd.randbytes(2 << 20) + bytes(62 << 20)
    packed = subprocess.run(['zstd', '-q', '-19', '-c'], input=image, capture_output=True, check=True).stdout

    spec = importlib.util.spec_from_file_location('test_screen', ROOT / 'installer/install/test_screen.py')
    ts = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(ts)
    pngs = {}
    with tempfile.TemporaryDirectory(prefix='pw3-check-') as t:
        tmp = Path(t)
        binary = ts.build(tmp, a.zstd_archive, a.screens)
        for key, (screen, permille, number) in {'a': ('DOWNLOAD', 370, 0), 'b': ('E401', -1, 812)}.items():
            subprocess.run([str(binary), str(a.screens / 'screens.bin'), str(tmp), screen, str(permille), str(number)], check=True)
            pngs[key] = (tmp / 'screen.png').read_bytes()

    files = {'UNPACK': unpack, 'TESTIMG': packed, 'CA': ca, 'SCREENA': pngs['a'], 'SCREENB': pngs['b']}
    script = (HERE / 'check-template.sh').read_text()
    values = {'@UNPACK_SHA@': sha(unpack), '@TESTIMG_SHA@': sha(packed), '@CA_SHA@': sha(ca),
              '@SCREENA_SHA@': sha(pngs['a']), '@SCREENB_SHA@': sha(pngs['b']), '@TESTIMG_BYTES@': str(len(packed)),
              '@IMG_BYTES@': str(len(image)), '@IMG_SHA@': sha(image), '@DOWNLOAD_URL@': DOWNLOAD_URL,
              '@DOWNLOAD_SHA@': DOWNLOAD_SHA, '@REGION_X@': str(screens['region'][0]), '@REGION_Y@': str(screens['region'][1])}
    for k, v in values.items():
        if k not in script:
            sys.exit('the template has no ' + k)
        script = script.replace(k, v)
    leftover = re.findall(r'@[A-Z_]+@', script)
    if leftover:
        sys.exit('placeholders left: %s' % leftover)
    for name, data in files.items():
        encoded = base64.b64encode(data).decode()
        script += '__%s__\n%s\n__END_%s__\n' % (name, '\n'.join(encoded[i:i + 76] for i in range(0, len(encoded), 76)), name)
    out = a.output
    out.mkdir(parents=True, exist_ok=False)
    (out / NAME).write_text(script)
    subprocess.run(['/bin/sh', '-n', str(out / NAME)], check=True)
    result = {'schema': 1, 'script': NAME, 'bytes': len(script.encode()), 'sha256': sha(script.encode()),
              'files_sha256': {k: sha(v) for k, v in files.items()}, 'test_image': {'bytes': len(image), 'packed': len(packed)},
              'download_url': DOWNLOAD_URL, 'pw3_unpack_sha256': unpack_receipt['sha256'],
              'screens_bin_sha256': screens['screens_bin']['sha256'], 'contains_device_data': False}
    (out / 'build-result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=1))


if __name__ == '__main__':
    main()
