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
    "kernel_image_sha256": "sha256 of the stock kernel image the four bodies were read from",
    "stock_functions": {
      "pm_test_show":   {"bytes": 124, "sha256": "..."},
      "l2x0_flush_all": {"bytes": 116, "sha256": "..."},
      "l2x0_disable":   {"bytes": 124, "sha256": "..."},
      "l2x0_inv_all":   {"bytes": 152, "sha256": "..."}
    }
  }

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
FUNCTIONS = ('pm_test_show', 'l2x0_flush_all', 'l2x0_disable', 'l2x0_inv_all')
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


def c_string(value):
    return '"' + value.replace('\\', '\\\\').replace('"', '\\"') + '"'


def rewrite_struct(source, values):
    """The devicedata.c text with this model's struct body replaced."""
    at = source.find('const struct dd_profile %s = {' % STRUCT)
    if at < 0:
        fail('%s does not define %s; this tool fills a struct, it does not create one'
             % (DEVICEDATA.name, STRUCT))
    end = source.find('};', at)
    if end < 0:
        fail('the initialiser of %s is not terminated' % STRUCT)
    body = ''.join('    %s,\n' % c_string(values[field]) for field, _, _ in FIELDS)
    return source[:at] + 'const struct dd_profile %s = {\n' % STRUCT + body + source[end:]


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

    before = dict(profile)
    for field, profile_key, _ in FIELDS:
        profile[profile_key] = values[field]
    profile['stock_functions'] = bodies
    profile['stock_functions_source'] = (
        'function bodies of the stock %s kernel (image sha256 %s), read from a device\'s '
        'own eMMC; only their sizes and hashes are kept'
        % (values['kernel_release'], image))
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
        updated = rewrite_struct(source, values)
        if updated != source:
            a.devicedata.write_text(updated, encoding='utf-8')
            print('%s: %s rewritten to match' % (a.devicedata, STRUCT))
        else:
            print('%s: %s already matches' % (a.devicedata, STRUCT))
    return 0


if __name__ == '__main__':
    sys.exit(main())
