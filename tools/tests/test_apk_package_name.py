#!/usr/bin/env python3
"""The package name has to come out of the binary manifest, not out of a guess.

tools/apk-package-name.py exists because the measurement script must know which
package an APK installs as *before* installing it: for an upgrade the list of
packages does not change, so a diff says nothing. aapt is not available here or
on the device, so the binary AndroidManifest.xml is parsed directly.

Two mistakes were made writing it, and this test is what each of them fails:

1. "The first dotted name in the file that is not android.*" -- the string pool
   is not in document order, so for WeRead that returned `androidx.core.app`.
   The manifest fixtures below put the right answer late in the pool and decoys
   early, so that heuristic cannot pass.
2. `attributeStart` counts from the end of the fixed node header (headerSize),
   not from the start of the chunk. Reading it from the chunk is 16 bytes off
   and returns garbage. The fixtures use a real headerSize of 16.

The fixtures are built here rather than shipped, so the test needs no APK and
says exactly which byte means what.
"""
import importlib.util
import pathlib
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('apn', ROOT / 'tools/apk-package-name.py')
apn = importlib.util.module_from_spec(spec)
spec.loader.exec_module(apn)
failures = []


def check(ok, message):
    if not ok:
        failures.append(message)


def string_pool(items, utf8=False):
    """RES_STRING_POOL_TYPE, the way aapt writes it."""
    body, offsets = bytearray(), []
    for item in items:
        offsets.append(len(body))
        if utf8:
            raw = item.encode('utf-8')
            body += bytes([len(item), len(raw)]) + raw + b'\0'
        else:
            body += struct.pack('<H', len(item)) + item.encode('utf-16-le') + b'\0\0'
    while len(body) % 4:
        body += b'\0'
    header = 28 + 4 * len(items)
    chunk = struct.pack('<HHIIIIII', 0x0001, 28, header + len(body), len(items), 0,
                        1 << 8 if utf8 else 0, header, 0)
    return chunk + struct.pack('<%dI' % len(offsets), *offsets) + bytes(body)


def start_element(pool, name, attributes):
    """RES_XML_START_ELEMENT with attributeStart counted from headerSize."""
    attrs = bytearray()
    for attr_ns, attr_name, value in attributes:
        attrs += struct.pack('<IIIIBBH',
                             0xFFFFFFFF if attr_ns is None else pool.index(attr_ns),
                             pool.index(attr_name),
                             0xFFFFFFFF if value is None else pool.index(value),
                             8, 0, 0, 3)
    ext = struct.pack('<IIHHHHHH', 0xFFFFFFFF, pool.index(name),
                      20, 20, len(attributes), 0, 0, 0)
    size = 16 + len(ext) + len(attrs)
    return struct.pack('<HHIII', 0x0102, 16, size, 1, 0xFFFFFFFF) + ext + bytes(attrs)


def manifest(pool, name, attributes, utf8=False):
    body = string_pool(pool, utf8) + start_element(pool, name, attributes)
    return struct.pack('<HHI', 0x0003, 8, 8 + len(body)) + body


# The decoys come first in the pool, as they do in a real APK.
POOL = ['androidx.core.app', 'android.support.v4', 'versionCode', 'versionName',
        'platformBuildVersionCode', 'package', 'manifest', 'com.tencent.weread.eink']
ATTRS = [(None, 'versionCode', None), (None, 'versionName', 'androidx.core.app'),
         (None, 'platformBuildVersionCode', None),
         (None, 'package', 'com.tencent.weread.eink')]

try:
    got = apn.package_name(manifest(POOL, 'manifest', ATTRS))
    check(got == 'com.tencent.weread.eink',
          f'a UTF-16 manifest gave {got!r}, not the package attribute\'s value')
except Exception as error:                                   # noqa: BLE001
    check(False, f'a UTF-16 manifest raised {error!r}')

try:
    got = apn.package_name(manifest(POOL, 'manifest', ATTRS, utf8=True))
    check(got == 'com.tencent.weread.eink', f'a UTF-8 pool gave {got!r}')
except Exception as error:                                   # noqa: BLE001
    check(False, f'a UTF-8 pool raised {error!r}')

# A second package, at a different index in a differently ordered pool. Every
# answer above is the same string, so returning that string as a constant -- and
# never reading the attribute's value at all -- passed all of them.
OTHER_POOL = ['package', 'org.example.other.app', 'manifest', 'versionCode',
              'com.tencent.weread.eink']
OTHER_ATTRS = [(None, 'versionCode', None), (None, 'package', 'org.example.other.app')]
for utf8 in (False, True):
    try:
        got = apn.package_name(manifest(OTHER_POOL, 'manifest', OTHER_ATTRS, utf8=utf8))
        check(got == 'org.example.other.app',
              f'a {"UTF-8" if utf8 else "UTF-16"} manifest naming another package gave {got!r}; '
              f'the value has to come from the attribute, not from anywhere else')
    except Exception as error:                               # noqa: BLE001
        check(False, f'a second package name raised {error!r}')

# A package attribute that is last, and one that is the only attribute.
for attributes in ([(None, 'versionCode', None), (None, 'package', 'com.tencent.weread.eink')],
                   [(None, 'package', 'com.tencent.weread.eink')]):
    try:
        got = apn.package_name(manifest(POOL, 'manifest', attributes))
        check(got == 'com.tencent.weread.eink', f'{len(attributes)} attributes gave {got!r}')
    except Exception as error:                               # noqa: BLE001
        check(False, f'{len(attributes)} attributes raised {error!r}')

# A namespaced attribute called "package" is not the package: android:package
# exists on other elements, and taking it would name the wrong thing.
try:
    apn.package_name(manifest(POOL, 'manifest',
                              [(None, 'versionCode', None),
                               ('android.support.v4', 'package', 'androidx.core.app')]))
    check(False, 'a namespaced package attribute was accepted as the package name')
except ValueError:
    pass

# Refusals, so that a wrong file is an error rather than a plausible answer.
for bad, why in (
        (b'\x00\x00\x08\x00\x08\x00\x00\x00', 'a chunk that is not RES_XML_TYPE'),
        (manifest(POOL, 'manifest', [(None, 'versionCode', None)]), 'no package attribute'),
        (manifest(POOL, 'versionName', ATTRS), 'no manifest element'),
):
    try:
        apn.package_name(bad)
        check(False, f'accepted {why}')
    except ValueError:
        pass
    except Exception as error:                               # noqa: BLE001
        check(False, f'{why} raised {type(error).__name__}, expected ValueError')

for message in failures:
    print('FAIL', message)
if failures:
    sys.exit(1)
print('PW3_APK_PACKAGE_NAME_PASS  the package attribute is read where it is, '
      'in UTF-16 and UTF-8 pools, and a wrong file is refused')
