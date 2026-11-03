#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Finds private data in what the repository would publish: the files git tracks or
would add (not those .gitignore leaves out), or the files given.

Two kinds of check:
  patterns   what is private wherever it comes from: MAC addresses, private keys, adb
             keys, Kindle serial numbers, home folders, e-mail and LAN addresses, Wi-Fi
             passphrases. Known harmless hits are listed in tools/private-data-allow.txt.
  denylist   this developer's own values (a device's serial number and MAC, a network
             name, ...), kept outside the repository in private/denylist.json as SHA-256
             only: the values themselves are never written down. Words of the files are
             hashed and compared (MAC addresses in any notation).

Usage: check-private-data.py [FILE...]          check (exit 1 on a finding)
       check-private-data.py --staged           check what is staged for the next commit
                                                (tools/git-hooks/pre-commit)
       check-private-data.py --history          check every version of every file in the
                                                history, which is what making the
                                                repository public publishes
       check-private-data.py --add KIND [--words N]  read one value from standard input
                                                    and add its hash (KIND: mac, text)

--history is the check that matters once, and once is the day the repository is made
public. The working tree is what the other modes see, and a value removed from a file
is gone from the working tree and still in the history: `git log -p` hands it to
anybody, and so does a clone. There is no fixing that afterwards -- a published commit
has been fetched -- so this walks every (path, blob) pair in every commit reachable
from any ref and applies the same two checks to each.
"""
import argparse, base64, binascii, fnmatch, hashlib, json, re, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DENYLIST = ROOT / 'private/denylist.json'
ALLOW = ROOT / 'tools/private-data-allow.txt'
MAX_BYTES = 64 << 20

MAC = re.compile(rb'(?<![0-9A-Fa-f:-])((?:[0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2})(?![0-9A-Fa-f:-])')
PATTERNS = [
    ('MAC address', MAC),
    ('private key', re.compile(rb'-----BEGIN (?:RSA |EC |DSA |OPENSSH |ENCRYPTED )?PRIVATE KEY-----')),
    ('adb public key', re.compile(rb'QAAAA[A-Za-z0-9+/]{200,}')),
    ('Kindle serial number', re.compile(rb'(?<![0-9A-Za-z])(?:G0|90)[0-9A-Z]{14}(?![0-9A-Za-z])')),
    ('home folder', re.compile(rb'/Users/[^/\s"\'`)]+|/home/(?!builder\b)[a-z_][a-z0-9_-]*|[Cc]:\\\\Users\\\\[^\\\s]+')),
    # The domain starts with a letter: HIDL names (android.hardware.keymaster@3.0-service) are not addresses.
    ('e-mail address', re.compile(rb'[A-Za-z0-9._%+-]+@[A-Za-z][A-Za-z0-9-]*(?:\.[A-Za-z0-9-]+)*\.[A-Za-z]{2,}')),
    ('LAN address', re.compile(rb'(?<![\d.])(?:192\.168|10\.\d{1,3}|172\.(?:1[6-9]|2\d|3[01]))\.\d{1,3}\.\d{1,3}(?![\d.])')),
    ('Wi-Fi passphrase', re.compile(rb'\b(?:psk|wpa_passphrase|PreSharedKey)\s*[=:]\s*"?[^"\s]{8,}')),
]
# Values that are nobody's: all-zero and broadcast MACs, Android's placeholder MAC.
HARMLESS_MACS = {b'000000000000', b'ffffffffffff', b'020000000000'}
WORD = re.compile(rb'[^\s"\'`<>()\[\]{},;=|]+')


def files_to_check(given):
    if given:
        return [Path(g) for g in given]
    out = subprocess.run(['git', '-C', str(ROOT), 'ls-files', '-z', '--cached', '--others', '--exclude-standard'],
                         capture_output=True, check=True).stdout
    return [ROOT / p.decode() for p in out.split(b'\0') if p]


def load_allow():
    """Lines "PATH-GLOB<TAB>TEXT": a hit of TEXT in a file matching PATH-GLOB is harmless."""
    # This list holds a fragment of every hit it forgives, so it matches the
    # patterns itself. It forgives itself rather than growing an entry for
    # every entry.
    rules = [(str(ALLOW.relative_to(ROOT)), b'')]
    if ALLOW.exists():
        for line in ALLOW.read_text().splitlines():
            if line.strip() and not line.startswith('#'):
                glob, text = line.split('\t', 1)
                rules.append((glob, text.encode()))
    return rules


def load_denylist():
    """This machine's real values, or nothing where there are none to have.

    Two cases that must not be confused, because one of them is this check
    switched off while still reporting green (seen 2026-10-31, in the dry run of
    the open-source export):

      private/ is not there at all -- the published tree, where the denylist
        cannot exist and is not wanted: it holds the owner's own serials and
        network names, and a contributor's machine has different ones. The
        pattern rules are the whole of what this can do there, and the line it
        prints says so rather than counting zero entries and leaving the reader
        to notice.
      private/ is there and the file is not -- the machine that is supposed to
        have one. That is a failure, not a lighter check: the denylist is what
        keeps exact values out of a repository about to be made public, and a
        green line with it missing is the one answer that cannot be allowed.
    """
    if DENYLIST.exists():
        return {'entries': checked_entries(json.loads(DENYLIST.read_text()))}
    if DENYLIST.parent.is_dir():
        sys.exit('%s is missing, and %s is here: this tree is supposed to have a denylist. '
                 'Running the patterns alone would report a pass for a check that did not run.'
                 % (DENYLIST, DENYLIST.parent))
    # Derived from the file not being there, never read out of the file: an
    # earlier version of add() wrote this key into the denylist it created, and
    # from then on every load believed the tree had no denylist -- which skipped
    # the "git must be ignoring it" check and printed "patterns only" over a file
    # full of real values (Codex 2026-10-31).
    return {'entries': [], 'absent': True}


def checked_entries(deny):
    """The rules in a denylist, or an exit saying why there are none to use.

    A file that is there and carries no rule this can use is the same failure as
    a file that is gone: the exact values stay unguarded while the line still
    reads green. Only `--add` writes this file, so an empty list means someone
    emptied it. Three ways an entry can be there and do nothing, all of them
    silent before this (Codex 2026-10-31):

      an unknown `kind` -- dropped by the two set comprehensions in main();
      a `sha256` that is not a string -- 64 decimal digits are a JSON *integer*,
        which matches the pattern and can never equal what sha() returns;
      a text rule whose `words` is not a positive count -- word_counts decides
        how many words are joined before hashing, so a 0 or a -1 means the value
        is never assembled and never compared.
    """
    entries = deny.get('entries')
    if not isinstance(entries, list) or not entries:
        sys.exit('%s holds no entries: the private-value check would be off while reporting a pass. '
                 'Add the values with --add, or delete the file if this tree has none.' % DENYLIST)
    for e in entries:
        why = None
        if not isinstance(e, dict):
            why = 'not an object'
        elif e.get('kind') not in ('text', 'mac'):
            why = 'kind is not "text" or "mac"'
        elif not isinstance(e.get('sha256'), str) or not re.fullmatch(r'[0-9a-f]{64}', e['sha256']):
            why = 'sha256 is not 64 lowercase hex characters in a string'
        elif e['kind'] == 'text' and not (isinstance(e.get('words', 1), int)
                                          and not isinstance(e.get('words', 1), bool)
                                          and e.get('words', 1) >= 1):
            why = 'words is not a count of one or more'
        if why:
            sys.exit('%s has an entry this cannot use (%s): %r' % (DENYLIST, why, e))
    return entries


def sha(data):
    return hashlib.sha256(data).hexdigest()


def mac_key(text):
    return re.sub(rb'[^0-9a-f]', b'', text.lower())


def staged_files():
    out = subprocess.run(['git', '-C', str(ROOT), 'diff', '--cached', '--name-only', '--diff-filter=ACMR', '-z'],
                         capture_output=True, check=True).stdout
    return [p.decode() for p in out.split(b'\0') if p]


def staged_bytes(rel):
    return subprocess.run(['git', '-C', str(ROOT), 'show', ':' + rel], capture_output=True, check=True).stdout


def history_pairs():
    """Every (path, blob) the history holds, once each, and the number of commits.

    Per commit and not `rev-list --objects`: that lists each object once, under
    whichever path it was first reached by, and the path decides which allow-list
    rules apply. A blob that lives at two paths has to be checked under both, or
    a rule written for one of them forgives it under the other.
    """
    commits = subprocess.run(['git', '-C', str(ROOT), 'rev-list', '--all'],
                             capture_output=True, check=True, text=True).stdout.split()
    pairs = {}
    for commit in commits:
        out = subprocess.run(['git', '-C', str(ROOT), 'ls-tree', '-r', '-z', '--full-tree', commit],
                             capture_output=True, check=True).stdout
        for record in out.split(b'\0'):
            if not record:
                continue
            # "<mode> <type> <sha>\t<path>"
            meta, _, path = record.partition(b'\t')
            fields = meta.split(b' ')
            if len(fields) != 3 or fields[1] != b'blob':
                continue        # a submodule's commit, a subtree
            pairs[(path.decode('utf-8', 'surrogateescape'), fields[2].decode())] = None
    return sorted(pairs), len(commits)


def history_items(pairs):
    """(path, rel, data, blob) for each pair, read through one cat-file process.

    One process for every blob in a 169-commit history is 2,000 processes and
    most of a minute; `--batch` is one.
    """
    if not pairs:
        return
    proc = subprocess.Popen(['git', '-C', str(ROOT), 'cat-file', '--batch'],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    try:
        for rel, blob in pairs:
            proc.stdin.write((blob + '\n').encode())
            proc.stdin.flush()
            header = proc.stdout.readline().split()
            if len(header) != 3 or header[1] != b'blob':
                raise SystemExit('git cat-file %s: %s' % (blob, b' '.join(header).decode()))
            size = int(header[2])
            data = proc.stdout.read(size)
            proc.stdout.read(1)                 # the newline after the object
            yield ROOT / rel, rel, data, blob
    finally:
        proc.stdin.close()
        proc.stdout.close()
        proc.wait()


HELPER_MARKER = b'\n__PW3_FIRST_KERNEL_HELPER_BASE64__\n'


def decoded_helper(data):
    """The launcher script with its helper decoded, so that the helper is read as
    the binary it is.

    Readom.sh is a shell script followed by the base64 of a 6 MB program
    (boot/build_launcher.py). Base64 is upper case, lower case and digits, so a
    run of sixteen of its characters is indistinguishable from a Kindle serial
    number, and whether a build produces such a run is chance: V82's entry had
    none and V83's had one, from the same source. Reading the encoding is also
    not the check anyone wants -- what has to carry no private data is the
    program, and the encoding hides it: a serial number inside the binary
    survives base64 in a form no pattern here matches.

    So the tail is decoded and checked in its decoded form. A file without the
    marker, and one whose tail does not decode, are returned unchanged.
    """
    cut = data.find(HELPER_MARKER)
    if cut < 0:
        return data
    head = data[:cut + len(HELPER_MARKER)]
    try:
        return head + base64.b64decode(data[cut + len(HELPER_MARKER):], validate=False)
    except (ValueError, binascii.Error):
        return data


def check_file(path, rel, allow, deny_text, deny_mac, word_counts, data=None):
    if data is None:
        data = path.read_bytes()
    # Too big to scan is reported, not scanned to its first MAX_BYTES and
    # counted among the files checked: the values this looks for are short and
    # can sit anywhere in a file, so a truncated scan is this check running on
    # part of the tree while its last line says it ran on all of it
    # (Codex 2026-10-31, who put a synthesized key past the cut and got 0
    # findings). The largest thing this repository has ever held is 3.6 MB, so
    # this guards against something new arriving rather than limiting anything
    # that is here.
    if len(data) > MAX_BYTES:
        return [(0, 'not scanned', 'longer than %d bytes' % MAX_BYTES)]
    data = decoded_helper(data)
    findings = []
    for name, pattern in PATTERNS:
        for m in pattern.finditer(data):
            hit = m.group(0)
            if name == 'MAC address' and mac_key(hit) in HARMLESS_MACS:
                continue
            if any(fnmatch.fnmatch(rel, glob) and text in hit for glob, text in allow):
                continue
            line = data.count(b'\n', 0, m.start()) + 1
            findings.append((line, name, hit[:80].decode('utf-8', 'replace')))
    if deny_text or deny_mac:
        words = [(m.start(), m.group(0)) for m in WORD.finditer(data)]
        for i, (start, word) in enumerate(words):
            for n in word_counts:
                if i + n > len(words):
                    continue
                joined = b' '.join(w for _, w in words[i:i + n])
                for candidate in (joined, joined.rstrip(b'.:')):
                    if sha(candidate) in deny_text:
                        findings.append((data.count(b'\n', 0, start) + 1, 'denylisted value', '(hidden)'))
            key = mac_key(word)
            if len(key) == 12 and sha(key) in deny_mac:
                findings.append((data.count(b'\n', 0, start) + 1, 'denylisted MAC', '(hidden)'))
    return sorted(set(findings))


def add(kind, words=None):
    """Put one value's hash in the denylist, creating or repairing the file.

    Not load_denylist(): that one is for a scan, and it is strict on purpose --
    it exits when the file holds no usable rule. Adding is how that state gets
    fixed, so going through the strict loader made the repair impossible and the
    message that suggested it wrong (Codex 2026-10-31). What this does take from
    the strict side is the per-entry check, applied to what is already in the
    file: an existing entry this cannot use is still a reason to stop, because
    writing beside it would leave a file that no scan will accept.
    """
    value = sys.stdin.buffer.read().rstrip(b'\r\n')
    if not value:
        sys.exit('no value on standard input')
    if kind == 'mac':
        key = mac_key(value)
        if len(key) != 12:
            sys.exit('not a MAC address')
        entry = {'kind': 'mac', 'sha256': sha(key)}
    else:
        count = words if words is not None else len(value.split())
        if count < 1:
            sys.exit('--words has to be one or more; a text rule joins that many words before hashing')
        entry = {'kind': 'text', 'sha256': sha(value), 'words': count}
    entries = []
    if DENYLIST.exists():
        held = json.loads(DENYLIST.read_text())
        entries = held.get('entries') or []
        if entries:
            entries = checked_entries(held)
    if entry not in entries:
        entries.append(entry)
    DENYLIST.parent.mkdir(exist_ok=True)
    # `entries` and nothing else. The loader's own "absent" marker is derived
    # from the file not being there, so persisting it would make every later
    # load believe this tree has no denylist.
    DENYLIST.write_text(json.dumps({'entries': entries}, indent=1) + '\n')
    print('added a %s entry (%d in all)' % (kind, len(entries)))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('files', nargs='*')
    ap.add_argument('--staged', action='store_true')
    ap.add_argument('--history', action='store_true')
    ap.add_argument('--add', choices=['mac', 'text'])
    ap.add_argument('--words', type=int)
    a = ap.parse_args()
    if a.add:
        return add(a.add, a.words)
    allow = load_allow()
    denylist = load_denylist()
    deny = denylist['entries']
    # Where there is a denylist, git must be ignoring it: a file holding this
    # machine's real values, tracked, is the accident this whole check exists to
    # prevent. Where there is none (the published tree) there is nothing to
    # ignore, and asking git about it in a repository that may not exist yet
    # answered with a fatal error and failed this check for the wrong reason.
    if not denylist.get('absent') and \
            subprocess.run(['git', '-C', str(ROOT), 'check-ignore', '-q', str(DENYLIST)]).returncode != 0:
        sys.exit('private/denylist.json is not ignored by git')
    deny_text = {e['sha256'] for e in deny if e['kind'] == 'text'}
    deny_mac = {e['sha256'] for e in deny if e['kind'] == 'mac'}
    word_counts = sorted({e.get('words', 1) for e in deny if e['kind'] == 'text'}) or [1]
    total = 0
    if a.staged:
        items = [(ROOT / rel, rel, staged_bytes(rel), None) for rel in staged_files()]
        counted, what = len(items), 'files'
    elif a.history:
        pairs, commits = history_pairs()
        items = history_items(pairs)
        counted, what = len(pairs), 'versions of files in %d commits' % commits
    else:
        items, missing = [], []
        for path in files_to_check(a.files):
            if path.is_file():
                rel = str(path.resolve().relative_to(ROOT)) if path.resolve().is_relative_to(ROOT) else str(path)
                items.append((path, rel, None, None))
            elif a.files:
                missing.append(str(path))
        # A path named on the command line that is not a file used to be dropped
        # here, so asking about a file that is not there answered "0 files
        # checked, 0 findings" and exited 0 -- a typo in a hook or a script
        # reads as a pass (Codex 2026-10-31). Tree mode has no such case: git
        # lists what is there.
        if missing:
            sys.exit('not a file: %s' % ', '.join(missing))
        counted, what = len(items), 'files'
    for path, rel, data, blob in items:
        for line, name, hit in check_file(path, rel, allow, deny_text, deny_mac, word_counts, data):
            # In history mode the blob as well as the path: one path has as many
            # versions as it has commits, and `git log --find-object=<blob>` is
            # how the commits holding this one are found.
            where = '%s@%s:%d' % (rel, blob[:12], line) if blob else '%s:%d' % (rel, line)
            print('%s: %s: %s' % (where, name, hit))
            total += 1
    how = ('patterns only, no denylist in this tree' if denylist.get('absent')
           else '%d denylist entries' % len(deny))
    print('%d %s checked, %s, %d findings' % (counted, what, how, total), file=sys.stderr)
    if total and a.history:
        print('A finding here is published by making the repository public, and stays published: '
              'find the commits with `git log --all --find-object=<blob>`, and decide between '
              'rewriting the history before the first publish and leaving the value out there.',
              file=sys.stderr)
    sys.exit(1 if total else 0)


if __name__ == '__main__':
    main()
