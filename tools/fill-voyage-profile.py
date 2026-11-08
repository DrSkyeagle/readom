#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Fill a model's boot profile from facts read off the device, and keep the two
copies of those facts in step.

A stock firmware's facts are written down twice -- `boot/profiles/<model>-
<firmware>.json` for the build, and one `struct dd_profile` in
`boot/devicedata/devicedata.c` for the compiled helper that checks the running
system (tools/tests/test_profile_matches_devicedata.py is what holds them
together). The values cannot be derived from the sources: they are what that
firmware's kernel and root file system happen to contain, so they are read off
the machine and typed in. This tool is the typing, with the checks that a typo
would otherwise pass: a command line with a newline, a hash that is not 64 hex
digits, a function body of zero bytes, a path that is not absolute.

Usage: fill-voyage-profile.py --facts FACTS.json [--profile PATH] [--devicedata PATH]
                              [--output PATH] [--check]
  --facts       what was read off the device (below)
  --profile     the profile to fill; default boot/profiles/voyage-5.13.6.json
  --devicedata  the C file whose struct is rewritten too; default
                boot/devicedata/devicedata.c. Pass an empty string to leave it alone
  --output      where to write the profile; default: in place
  --check       change nothing: fail if the profile still carries a TODO

The facts file is JSON, written by hand from what the device says:

  {
    "cmdline":            "/proc/cmdline, without root= and rootwait",
    "kernel_release":     "uname -r",
    "kernel_version":     "uname -v",
    "user_mount":         "the stock mount of the user FAT, e.g. /mnt/base-us",
    "wifi_board":         "the stock path of the Wi-Fi calibration file",
    "wifi_firmware":      "the stock path of the Wi-Fi firmware",
    "stock_cpuid":        "the CPU identity register the handoff's gate reads, e.g. 0x412fc09a",
    "stock_files": [
      {"path": "/sbin/init", "bytes": 1345, "sha256": "..."},
      {"path": "/etc/upstart/init.sh", "bytes": 1345, "sha256": "..."},
      {"path": "/sbin/init.exe", "bytes": 111824, "sha256": "..."},
      {"path": "/usr/sbin/eips", "bytes": 61104, "sha256": "..."}
    ],
    "device_archive_max": 12582912,
    "kernel_image_sha256": "sha256 of the stock kernel image the four bodies were read from",
    "stock_functions": {
      "pm_test_show":   {"bytes": 124, "sha256": "..."},
      "l2x0_flush_all": {"bytes": 116, "sha256": "..."},
      "l2x0_disable":   {"bytes": 124, "sha256": "..."},
      "l2x0_inv_all":   {"bytes": 152, "sha256": "..."}
    }
  }

`stock_files` is all four of the files the helper checks by content -- the boot
(/sbin/init, /etc/upstart/init.sh, /sbin/init.exe) and the display
(/usr/sbin/eips) -- and only those four: the paths are the same on every model,
so a list missing one is a profile the entry cannot start, and an extra path is
a fact nothing checks. `stock_cpuid` is the MIDR the handoff's hardware gate
compares (boot/handoff/native-handoff.h). `device_archive_max` is the profile's
ceiling on the device archive a start may hand the kernel; it is written into
both copies because the helper refuses a bigger one and the boot module pins
the same number from the JSON.

`cmdline` is compared token for token with the running system's /proc/cmdline
after devicedata.c has removed `root=` and `rootwait`, so it has to be written
the way that check will see it: one line, single spaces, no root selection.
"""
import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PROFILE = ROOT / 'boot/profiles/voyage-5.13.6.json'
DEVICEDATA = ROOT / 'boot/devicedata/devicedata.c'
STRUCT = 'dd_voyage_5_13_6'
# The identity array is named after the struct: dd_voyage_5_13_6 ->
# dd_identity_voyage_5_13_6. One rule, written once, so a struct and its array
# cannot be renamed apart by this tool.
IDENTITY = 'dd_identity_' + STRUCT[len('dd_'):]
FUNCTIONS = ('pm_test_show', 'l2x0_flush_all', 'l2x0_disable', 'l2x0_inv_all')
# The four stock files every Kindle this project supports has, in the order the
# helper checks them: the boot the stock system came up with, then the display
# every screen goes through. The paths are the same on every model; the sizes
# and hashes are this firmware's.
STOCK_FILES = ('/sbin/init', '/etc/upstart/init.sh', '/sbin/init.exe', '/usr/sbin/eips')
# The fields of struct dd_profile in devicedata.c, in the order devicedata.h
# declares them; the key the profile JSON keeps the same fact under; and the key
# the facts file carries it under.
FIELDS = [('stock_cmdline', 'cmdline_base', 'cmdline'),
          ('user_mount', 'user_mount', 'user_mount'),
          ('wifi_board', 'wifi_board', 'wifi_board'),
          ('wifi_firmware', 'wifi_firmware', 'wifi_firmware'),
          ('kernel_release', 'stock_kernel_release', 'kernel_release'),
          ('kernel_version', 'stock_kernel_version', 'kernel_version')]
HEX64 = re.compile(r'[0-9a-f]{64}')


def fail(message):
    sys.exit('fill-voyage-profile: ' + message)


def text(facts, key):
    """One printable single-line string from the facts."""
    value = facts.get(key)
    if not isinstance(value, str) or not value:
        fail('%r is missing from the facts file' % key)
    if not all(0x20 <= ord(c) < 0x7f for c in value):
        fail('%r has a character a C string cannot carry unescaped: %r' % (key, value))
    if '\n' in value or value != value.strip() or '  ' in value:
        fail('%r is not one line of single-spaced text: %r' % (key, value))
    return value


def hex64(facts, key, where):
    value = facts.get(key)
    if not isinstance(value, str) or not HEX64.fullmatch(value):
        fail('%s: %r is not a sha256 (64 lower-case hex digits)' % (where, value))
    return value


def functions(facts):
    """The four function bodies the handoff module pins."""
    got = facts.get('stock_functions')
    if not isinstance(got, dict):
        fail('stock_functions is missing from the facts file')
    out = {}
    for name in FUNCTIONS:
        entry = got.get(name)
        if not isinstance(entry, dict):
            fail('stock_functions.%s is missing' % name)
        size = entry.get('bytes')
        if not isinstance(size, int) or not 1 <= size <= 4096:
            fail('stock_functions.%s.bytes is not 1 to 4096: %r' % (name, size))
        out[name] = {'bytes': size, 'sha256': hex64(entry, 'sha256', 'stock_functions.' + name)}
    return out


def stock_files(facts):
    """The four stock files the helper checks, path, size and hash."""
    got = facts.get('stock_files')
    if not isinstance(got, list):
        fail('stock_files is missing from the facts file')
    found = {}
    for entry in got:
        if not isinstance(entry, dict):
            fail('stock_files entries must be objects: %r' % (entry,))
        path = text(entry, 'path')
        if not path.startswith('/'):
            fail('stock_files path is not absolute: %r' % (path,))
        size = entry.get('bytes')
        if not isinstance(size, int) or size < 1:
            fail('stock_files[%r].bytes is not a positive size: %r' % (path, size))
        found[path] = {'bytes': size, 'sha256': hex64(entry, 'sha256', 'stock_files' + path)}
    missing = [path for path in STOCK_FILES if path not in found]
    if missing:
        fail('stock_files is missing %s: those are the files the helper checks, and a '
             'profile without one cannot start a device' % ', '.join(missing))
    extra = [path for path in found if path not in STOCK_FILES]
    if extra:
        fail('stock_files carries %s, which the helper does not check: a fact nothing '
             'reads is a typo nothing catches' % ', '.join(extra))
    return found


def stock_cpuid(facts):
    """The MIDR the handoff's hardware gate compares, as 0x%08x."""
    value = facts.get('stock_cpuid')
    if isinstance(value, int):
        value = '0x%08x' % value
    if not isinstance(value, str) or not re.fullmatch(r'0x[0-9a-f]{8}', value):
        fail('stock_cpuid is not 0x followed by eight lower-case hex digits: %r' % (value,))
    return value

def archive_max(facts, profile):
    """The device archive ceiling, from the facts or the profile it is filling."""
    value = facts.get('device_archive_max', profile.get('device_archive_max'))
    if not isinstance(value, int) or not 1 << 20 <= value <= 16 << 20:
        fail('device_archive_max is not 1 MiB to 16 MiB: %r' % (value,))
    return value


def c_string(value):
    return '"' + value.replace('\\', '\\\\').replace('"', '\\"') + '"'


def rewrite_struct(source, values, count, max_bytes):
    """The devicedata.c text with this model's struct body replaced."""
    at = source.find('const struct dd_profile %s = {' % STRUCT)
    if at < 0:
        fail('%s does not define %s; this tool fills a struct, it does not create one'
             % (DEVICEDATA.name, STRUCT))
    end = source.find('};', at)
    if end < 0:
        fail('the initialiser of %s is not terminated' % STRUCT)
    body = ''.join('    %s,\n' % c_string(values[field]) for field, _, _ in FIELDS)
    body += '    %s,\n    %du,\n    %du,\n' % (IDENTITY, count, max_bytes)
    return source[:at] + 'const struct dd_profile %s = {\n' % STRUCT + body + source[end:]


def rewrite_identity(source, files):
    """The devicedata.c text with this model's stock-file identity array replaced."""
    at = source.find('static const struct dd_identity %s[] = {' % IDENTITY)
    if at < 0:
        fail('%s does not define %s[]; this tool fills an array, it does not create one'
             % (DEVICEDATA.name, IDENTITY))
    end = source.find('};', at)
    if end < 0:
        fail('the initialiser of %s is not terminated' % IDENTITY)
    body = ''.join('    {%s, %du, %s},\n' % (c_string(path), files[path]['bytes'],
                                             c_string(files[path]['sha256']))
                   for path in STOCK_FILES)
    return source[:at] + 'static const struct dd_identity %s[] = {\n' % IDENTITY + body + source[end:]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--facts', type=Path)
    ap.add_argument('--profile', type=Path, default=PROFILE)
    ap.add_argument('--devicedata', type=Path, default=DEVICEDATA)
    ap.add_argument('--output', type=Path)
    ap.add_argument('--check', action='store_true',
                    help='change nothing; fail if the profile still carries a TODO')
    a = ap.parse_args()
    profile = json.loads(a.profile.read_text(encoding='utf-8'))
    if a.check:
        unfinished = sorted(set(re.findall(r'TODO\([a-z-]+\)', json.dumps(profile))))
        if unfinished:
            print('%s still has %s' % (a.profile, ', '.join(unfinished)))
            return 1
        print('%s carries no TODO' % a.profile)
        return 0
    if not a.facts:
        fail('--facts is required unless --check is given')
    facts = json.loads(a.facts.read_text(encoding='utf-8'))
    values = {field: text(facts, facts_key) for field, _, facts_key in FIELDS}
    if 'root=' in values['stock_cmdline'] or 'rootwait' in values['stock_cmdline'].split():
        fail('cmdline still selects the stock root: devicedata.c strips root= and '
             'rootwait before comparing, so this string would never match')
    bodies = functions(facts)
    image = hex64(facts, 'kernel_image_sha256', 'kernel_image_sha256')
    files = stock_files(facts)
    cpuid = stock_cpuid(facts)
    max_bytes = archive_max(facts, profile)

    before = dict(profile)
    for field, profile_key, _ in FIELDS:
        profile[profile_key] = values[field]
    profile['stock_functions'] = bodies
    profile['stock_functions_source'] = (
        'function bodies of the stock %s kernel (image sha256 %s), read from a device\'s '
        'own eMMC; only their sizes and hashes are kept'
        % (values['kernel_release'], image))
    profile['stock_cpuid'] = cpuid
    profile['stock_files'] = [{'path': path, **files[path]} for path in STOCK_FILES]
    profile['device_archive_max'] = max_bytes
    for key in ('module_kernel_source', 'module_kernel_output'):
        if key in facts:
            profile[key] = text(facts, key)
    profile['todo'] = [item for item in profile.get('todo', [])
                       if not any(item.startswith(prefix) for prefix in
                                  ('cmdline_base', 'stock_kernel_version', 'stock_functions',
                                   'user_mount', 'wifi_board', 'wifi_firmware',
                                   'module_kernel_source', 'module_kernel_output'))]
    if not profile['todo']:
        del profile['todo']

    out = a.output or a.profile
    out.write_text(json.dumps(profile, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
    changed = sorted(k for k in set(before) | set(profile) if before.get(k) != profile.get(k))
    print('%s: %d field(s) written: %s' % (out, len(changed), ', '.join(changed)))
    if a.devicedata and str(a.devicedata) not in ('', '.'):
        source = a.devicedata.read_text(encoding='utf-8')
        updated = rewrite_identity(source, files)
        updated = rewrite_struct(updated, values, len(STOCK_FILES), max_bytes)
        if updated != source:
            a.devicedata.write_text(updated, encoding='utf-8')
            print('%s: %s and %s[] rewritten to match' % (a.devicedata, STRUCT, IDENTITY))
        else:
            print('%s: %s and %s[] already match' % (a.devicedata, STRUCT, IDENTITY))
    return 0


if __name__ == '__main__':
    sys.exit(main())
