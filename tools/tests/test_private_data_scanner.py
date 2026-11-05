#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""The scanner must read a file's own bytes, whatever its tail looks like.

`tools/check-private-data.py` decodes the base64 helper that `Readom.sh`
carries, because a serial number inside that binary survives base64 in a form
none of the patterns match. Twice now it has done that by deciding *not* to
read the raw bytes, and both times the decision was the hole:

  1. It replaced them, so everything after the marker was read only as whatever
     the base64 decoded to -- and `b64decode(validate=False)` discards every
     character outside the alphabet, so whether a run of ordinary text "decoded"
     turned on nothing but whether its length divides by four. Measured
     2026-11-03: the same secret found at a tail of 19, 21 and 22 characters and
     gone at 20.
  2. The repair kept the raw bytes unless the tail was one well-formed base64
     blob. A Kindle serial number is sixteen alphanumeric characters and every
     one of them is in the base64 alphabet, so a serial behind the marker
     satisfied that test and disappeared -- and so did the exact comparison
     against this developer's own denylisted values. The length coin flip was
     still live inside it: hidden at 16 and 20, found at 17, 18 and 19.

The first version of this file asserted the property the second fix did not
have, and passed anyway: the only secret it carried was a PEM private-key
header, whose dashes are outside the base64 alphabet -- so all four of its
paddings kept the raw view and all four found the secret. A test guarding a
predicate with input that cannot satisfy the predicate.

So the secrets here are alphanumeric, which is the shape that reaches the
branch, and what is asserted is the *kind* of each finding rather than a count:
"at least one finding" is satisfied by a scanner that reports one finding for
everything, which is how a scan that had stopped looking would read.
"""
import base64
import hashlib
import importlib.util
import sys
import tempfile
import traceback
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
SCANNER = ROOT / 'tools/check-private-data.py'

# Assembled rather than written out, so that this file does not have to be an
# exception in tools/private-data-allow.txt to say what it is about. Both are
# synthesized: SERIAL has a real serial's shape and nobody's digits.
KEY = b'-----' + b'BEGIN ' + b'PRIVATE KEY' + b'-----\n'
SERIAL = b'G0' + b'123456789' + b'ABCDE'
WIFI = b'MySecretWifi' + b'123X'

checks = 0
failures = []


def check(ok, what):
    global checks
    checks += 1
    if not ok:
        failures.append(what)


def main():
    check(SCANNER.is_file(), '%s is missing' % SCANNER)
    if not SCANNER.is_file():
        return report()
    spec = importlib.util.spec_from_file_location('scanner_under_test', SCANNER)
    scanner = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(scanner)

    allow = scanner.load_allow()
    marker = scanner.HELPER_MARKER

    def findings(data, deny_text=None):
        return scanner.check_file(Path('x'), 'docs/x', allow, deny_text or {}, {},
                                  [len(SERIAL.split())], data=data)

    def kinds(data, deny_text=None):
        return {name for _, name, _, _ in findings(data, deny_text)}

    def encoded_flags(data):
        return {enc for _, _, _, enc in findings(data)}

    check(len(SERIAL) == 16 and len(SERIAL) % 4 == 0,
          'SERIAL has to be sixteen characters for this to be the shape under test')
    check(not set(SERIAL) - set(b'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz'
                               b'0123456789+/='),
          'SERIAL has to be inside the base64 alphabet, or it cannot reach the branch '
          'this file is about -- which is how the first version of it passed')

    # The controls first. Every assertion below names the kind it expects, and a
    # scanner that answers one constant finding to everything would still match
    # some of them; one that never reads the file at all must not.
    check(not findings(b'nothing private here\n'),
          '一份干净的内容也报出了发现，所以下面每一条都不说明它真的在扫')
    check(kinds(KEY) == {'private key'},
          '一个普通文件里的明文私钥标记没有被认成私钥（认成了 %s）' % (kinds(KEY) or '什么都没有'))
    check(kinds(SERIAL + b'\n') == {'Kindle serial number'},
          '一个普通文件里的序列号没有被认成序列号（认成了 %s）'
          % (kinds(SERIAL + b'\n') or '什么都没有'))
    # And through a real file, not only through the data= shortcut, so that a
    # scanner which has stopped reading files cannot pass this.
    with tempfile.TemporaryDirectory() as box:
        onto = Path(box) / 'carrier.txt'
        onto.write_bytes(KEY)
        check(len(scanner.check_file(onto, 'docs/carrier.txt', allow, {}, {}, {},
                                     data=None)) == 1,
              '从真实文件读的时候认不出来，只有直接喂 data= 才认——读文件那条路没在走')

    # The branch itself: a secret whose own characters are all in the base64
    # alphabet, behind the marker, at every length remainder mod 4.
    for pad in ('', 'A', 'AA', 'AAA'):
        data = marker + SERIAL + b'\n' + pad.encode()
        check(kinds(data) == {'Kindle serial number'},
              '序列号在标记之后、尾部补了 %d 个 base64 字符时，报的是 %s 而不是序列号——'
              '能不能发现它又取决于尾巴长什么样' % (len(pad), kinds(data) or '什么都没有'))

    # Whole-tail variants of the same thing: nothing but alphabet characters,
    # which is what the removed predicate accepted as "one base64 blob".
    check(kinds(marker + SERIAL) == {'Kindle serial number'},
          '标记之后只有一个序列号、没有别的字符时，它没有被当成序列号报出来')
    check(kinds(marker + base64.b64encode(b'abc') + b'\n' + SERIAL) ==
          {'Kindle serial number'},
          '标记之后是「一段 base64 加一个序列号」时，序列号没有被报出来')

    # The denylist comparison is a separate path through the same views, and the
    # removed predicate cancelled it too -- which is worse, because a denylist
    # entry is one of this machine's real values rather than a shape.
    deny = {hashlib.sha256(WIFI).hexdigest(): 'synthesized'}
    check(kinds(WIFI + b'\n', deny) == {'denylisted value'},
          '普通文件里的 denylist 值没有被比出来（报的是 %s），所以下一条不作数'
          % (kinds(WIFI + b'\n', deny) or '什么都没有'))
    check('denylisted value' in kinds(marker + WIFI + b'\n', deny),
          'denylist 值放到标记之后就比不出来了——漏的是这台机器真实的值，不是一个形状')

    # And the decoded side still works: a secret that exists only inside the
    # base64 has to be found, which is the whole reason the decoding is there.
    hidden = marker + base64.b64encode(b'\x7fELF\x00\x00\x00\x00' + KEY)
    check(KEY not in hidden, '这个用例构造错了：明文不该出现在编码之外')
    check(kinds(hidden) == {'private key'},
          '只存在于 base64 里的明文没有被发现，解码那一侧坏了——'
          'Readom.sh 带的那个 6 MB 程序就是靠它扫的')

    # A tail that is not base64 at all must not stop the scan of either form.
    check(kinds(marker + KEY + b'\xff\xfe not base64 at all\n') == {'private key'},
          '标记之后是根本不能解码的内容时，明文私钥没有被报出来')

    # The fourth field says whether a hit sits in the machine-generated part, and
    # the release checklist reads it. A hit before the marker must not claim to.
    check(encoded_flags(KEY) == {False},
          '标记都没有的文件里，发现却被标成「在编码区里」')
    check(encoded_flags(marker + SERIAL + b'\n') == {True},
          '标记之后的发现没有被标成「在编码区里」，第 10 步就分不出噪声和真值')
    before = KEY + marker + base64.b64encode(b'harmless')
    check(False in encoded_flags(before),
          '标记之前的明文私钥被标成「在编码区里」，等于把它算进可以放过的那一堆')

    print('%d checks, %d failed' % (checks, len(failures)))
    for failure in failures:
        print('  FAIL %s' % failure)
    return 1 if failures else 0


def report():
    print('%d checks, %d failed' % (checks, len(failures)))
    for failure in failures:
        print('  FAIL %s' % failure)
    return 1 if failures else 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Exception:  # noqa: BLE001 - any fault here is this test's answer
        print('0 checks, 1 failed')
        print('  FAIL the check itself stopped: %s'
              % traceback.format_exc().strip().replace('\n', ' | '))
        sys.exit(1)
