#!/usr/bin/env python3
"""The package name out of an APK, without aapt.

The measurement script has to know which package an APK will install as, before
it is installed, so that it can measure an upgrade -- where the list of packages
does not change and a diff tells you nothing.

aapt is not on the build path here and not on the device, so this reads the
binary AndroidManifest.xml directly: the `package` attribute of the `manifest`
element. The first attempt instead took "the first dotted name in the file that
is not android.*", which looked right and was wrong -- the string pool is not in
document order, so for WeRead it returned `androidx.core.app`. There is no
shortcut: the attribute has to be found where it actually is.

Format (frameworks/base/libs/androidfw/include/androidfw/ResourceTypes.h):
  every chunk is type:u16 headerSize:u16 size:u32
  RES_XML_TYPE            0x0003   the file itself
  RES_STRING_POOL_TYPE    0x0001   stringCount, styleCount, flags, stringsStart
  RES_XML_START_ELEMENT   0x0102   lineNumber, comment, then ns, name,
                                   attributeStart/Size/Count. attributeStart is an
                                   offset from the end of the fixed node header
                                   (headerSize), and each attribute is
                                   ns:u32 name:u32 rawValue:u32 typedValue:8
"""
import struct
import sys
import zipfile

UTF8_FLAG = 1 << 8


def strings(data, at, size):
    count, _styles, flags, start = struct.unpack_from('<IIII', data, at + 8)
    offsets = struct.unpack_from('<%dI' % count, data, at + 28)
    out = []
    for offset in offsets:
        p = at + start + offset
        if flags & UTF8_FLAG:
            # Two lengths, each one or two bytes; the second is the byte length.
            n = data[p]
            p += 2 if n & 0x80 else 1
            n = data[p]
            if n & 0x80:
                n = ((n & 0x7F) << 8) | data[p + 1]
                p += 2
            else:
                p += 1
            out.append(data[p:p + n].decode('utf-8', 'replace'))
        else:
            n = struct.unpack_from('<H', data, p)[0]
            if n & 0x8000:                      # a length over 0x7FFF takes two units
                n = ((n & 0x7FFF) << 16) | struct.unpack_from('<H', data, p + 2)[0]
                p += 4
            else:
                p += 2
            out.append(data[p:p + n * 2].decode('utf-16-le', 'replace'))
    return out


def package_name(manifest):
    if struct.unpack_from('<HH', manifest, 0)[0] != 0x0003:
        raise ValueError('not a binary AndroidManifest.xml')
    pool, at, end = None, 8, struct.unpack_from('<I', manifest, 4)[0]
    while at + 8 <= min(end, len(manifest)):
        kind, _header, size = struct.unpack_from('<HHI', manifest, at)
        if size < 8:
            raise ValueError('invalid chunk size')
        if kind == 0x0001:
            pool = strings(manifest, at, size)
        elif kind == 0x0102:
            if pool is None:
                raise ValueError('an element before the string pool')
            name = struct.unpack_from('<I', manifest, at + 20)[0]
            if pool[name] == 'manifest':
                attr_start, attr_size, attr_count = struct.unpack_from('<HHH', manifest, at + 24)
                # attributeStart counts from the start of the extended header,
                # which is where the fixed node header ends -- not from the
                # chunk. Getting that wrong costs 16 bytes and reads garbage.
                for i in range(attr_count):
                    a = at + _header + attr_start + i * attr_size
                    ns, attr, raw = struct.unpack_from('<III', manifest, a)
                    # The package attribute carries no namespace.
                    if ns == 0xFFFFFFFF and pool[attr] == 'package' and raw != 0xFFFFFFFF:
                        return pool[raw]
                raise ValueError('the manifest element has no package attribute')
        at += size
    raise ValueError('no manifest element found')


def main():
    if len(sys.argv) != 2:
        sys.exit('usage: apk-package-name.py <file.apk>')
    with zipfile.ZipFile(sys.argv[1]) as apk:
        print(package_name(apk.read('AndroidManifest.xml')))


if __name__ == '__main__':
    main()
