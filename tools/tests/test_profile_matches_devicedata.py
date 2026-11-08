#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""The two copies of one machine's stock facts, held to each other.

A model's stock firmware is written down twice, because it is needed twice:

  * `boot/profiles/<model>-<firmware>.json` is what the *build* reads -- the
    command line the entry appends to, the model name the E101 screen prints,
    and the facts the boot module pins;
  * `boot/devicedata/devicedata.c` holds one `struct dd_profile` per model, which
    is what the *compiled helper* checks the running system against, twice: once
    before anything is downloaded or written, and again at the handover.

The second one cannot be read from the first at run time: the helper is a static
binary on a Kindle with no interpreter, and the struct is what makes a build for
one firmware refuse to start on another. So the duplication is deliberate, and
this file is what keeps it honest -- the same rule the two kernel-configuration
fragments are held to (tools/tests/test_kconfig_fragments.py).

What goes wrong without it, and did not before the second model existed:

  * a profile whose kernel version was updated and whose struct was not: the
    entry installs, and then every start is refused with E101, which says the
    model is unsupported -- it is the right model on the right firmware;
  * a struct whose command line was updated and whose profile was not: the
    entry builds a command line the handoff module does not pin, and the device
    stops at the handover with no screen that can say why;
  * a profile naming a struct that does not exist, or naming one that another
    profile also names: the build either fails to compile (the good case) or
    checks a machine it was not made for (the one this test exists for).

The check reads the C, not a copy of it: the six strings are taken out of the
initialiser in source order, so a struct that grows a field, or a field whose
order changes, shows up here rather than as a wrong comparison on a device.
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PROFILES = ROOT / 'boot/profiles'
DEVICEDATA = ROOT / 'boot/devicedata/devicedata.c'
HEADER = ROOT / 'boot/devicedata/devicedata.h'
HELPER = ROOT / 'boot/launcher/helper.c'

# The struct's fields, in the order devicedata.h declares them, with the key the
# profile JSON keeps the same fact under. The struct carries more than these six
# strings -- the stock files' identity table and the device archive ceiling --
# and those are checked below from the same initialiser.
FIELDS = [('stock_cmdline', 'cmdline_base'),
          ('user_mount', 'user_mount'),
          ('wifi_board', 'wifi_board'),
          ('wifi_firmware', 'wifi_firmware'),
          ('kernel_release', 'stock_kernel_release'),
          ('kernel_version', 'stock_kernel_version')]

# The four stock files every supported firmware has, in the order the helper
# checks them: the boot the stock system came up with, then the display every
# screen goes through. The paths are the same on every model; only the sizes and
# hashes differ, which is why the profile has to say them.
STOCK_FILES = ('/sbin/init', '/etc/upstart/init.sh', '/sbin/init.exe', '/usr/sbin/eips')

checks = 0
failures = []


def check(ok, what):
    global checks
    checks += 1
    if not ok:
        failures.append(what)


def c_strings(initialiser):
    """The string literals of one C initialiser, in order, unescaped."""
    out = []
    i = 0
    while i < len(initialiser):
        if initialiser[i] != '"':
            i += 1
            continue
        i += 1
        value = ''
        while i < len(initialiser) and initialiser[i] != '"':
            if initialiser[i] == '\\':
                i += 1
                if i == len(initialiser):
                    return None
                escape = initialiser[i]
                value += {'n': '\n', 't': '\t', '0': '\0'}.get(escape, escape)
            else:
                value += initialiser[i]
            i += 1
        i += 1
        out.append(value)
    return out


def structs(text):
    """Every `const struct dd_profile <name> = { ... };`, with its parts.

    The strings are the six facts above; after them the struct names its stock
    file identity array and gives its length and the device archive ceiling,
    which are what the helper runs its content checks from and what it refuses a
    too-large archive with. All of them are read out of the same initialiser in
    source order, so a struct that grows a field shows up here rather than as a
    wrong comparison on a device.
    """
    found = {}
    for match in re.finditer(r'const struct dd_profile (\w+)\s*=\s*\{', text):
        end = text.find('};', match.end())
        if end < 0:
            continue
        initialiser = text[match.end():end]
        strings = c_strings(initialiser)
        if strings is None:
            continue
        parts = {'strings': strings, 'identity': None, 'count': None, 'archive_max': None}
        tail = re.search(r'"\s*,\s*([A-Za-z_]\w*)\s*,\s*(\d+)u?\s*,\s*(\d+)u?\s*,?\s*\Z',
                         initialiser, re.S)
        if tail:
            parts['identity'] = tail.group(1)
            parts['count'] = int(tail.group(2))
            parts['archive_max'] = int(tail.group(3))
        found[match.group(1)] = parts
    return found


def identities(text):
    """Every `static const struct dd_identity <name>[] = { ... };`, as entries."""
    found = {}
    for match in re.finditer(r'static const struct dd_identity (\w+)\[\]\s*=\s*\{', text):
        end = text.find('};', match.end())
        if end < 0:
            continue
        entries = []
        for chunk in re.finditer(r'\{([^{}]*)\}', text[match.end():end]):
            part = chunk.group(1)
            strings = c_strings(part)
            size = re.search(r',\s*(\d+)u?\s*,', part)
            if strings is None or len(strings) != 2 or size is None:
                entries = None
                break
            entries.append({'path': strings[0], 'bytes': int(size.group(1)), 'sha256': strings[1]})
        if entries is not None:
            found[match.group(1)] = entries
    return found


def profiles():
    """Every stock (non-virtual) profile on disk, by file name."""
    out = {}
    for path in sorted(PROFILES.glob('*.json')):
        data = json.loads(path.read_text(encoding='utf-8'))
        if data.get('module_profile') == 1:
            out[path.name] = data
    return out


def main():
    check(DEVICEDATA.is_file(), '%s is missing' % DEVICEDATA.name)
    check(HEADER.is_file(), '%s is missing' % HEADER.name)
    check(HELPER.is_file(), '%s is missing' % HELPER.name)
    if failures:
        return report()
    declared = structs(DEVICEDATA.read_text(encoding='utf-8'))
    arrays = identities(DEVICEDATA.read_text(encoding='utf-8'))
    header = HEADER.read_text(encoding='utf-8')
    stock = profiles()
    check(bool(stock), 'no stock profile in boot/profiles; the reader is wrong')
    check(len(declared) >= 2,
          'only %d struct dd_profile in devicedata.c; there are two models now' % len(declared))

    # The header declares what the C defines, or a build for that model cannot
    # name it (-DDD_PROFILE=...).
    for name in sorted(declared):
        check(re.search(r'^extern const struct dd_profile %s;' % name, header, re.M) is not None,
              'devicedata.h does not declare %s, so no build can select it' % name)
    # And the helper reads that selection rather than a struct of its own: a
    # literal name here would make every build check the same machine.
    helper = HELPER.read_text(encoding='utf-8')
    check('&DD_PROFILE' in helper,
          'helper.c does not pass &DD_PROFILE to devicedata; the profile the build '
          'was given would be ignored')
    stray = sorted(set(re.findall(r'&dd_[a-z0-9_]+', helper)))
    check(stray == [],
          'helper.c names a profile struct directly (%s): a build for one firmware '
          'would check another one\'s facts' % ', '.join(stray))
    default = re.search(r'#ifndef DD_PROFILE\s*\n#define DD_PROFILE (\w+)', header)
    check(default is not None, 'devicedata.h has no #ifndef DD_PROFILE default')
    if default:
        check(default.group(1) in declared,
              'the DD_PROFILE default names %s, which devicedata.c does not define' % default.group(1))

    named = {}
    for name, profile in sorted(stock.items()):
        selected = profile.get('device_data_profile')
        check(isinstance(selected, str) and selected != '',
              '%s has no device_data_profile: the helper would be built for whichever '
              'model the default names' % name)
        if not isinstance(selected, str):
            continue
        check(selected in declared,
              '%s names %s, which devicedata.c does not define' % (name, selected))
        named.setdefault(selected, []).append(name)
        if selected not in declared:
            continue
        strings = declared[selected]['strings']
        check(len(strings) == len(FIELDS),
              '%s holds %d strings and the profile has %d facts; the struct and '
              'FIELDS here disagree about what one profile is'
              % (selected, len(strings), len(FIELDS)))
        if len(strings) != len(FIELDS):
            continue
        for (field, key), value in zip(FIELDS, strings):
            check(key in profile,
                  '%s has no %r, so %s.%s cannot be checked against it'
                  % (name, key, selected, field))
            if key not in profile:
                continue
            check(profile[key] == value,
                  '%s says %s is %r and devicedata.c has %r. One of the two was '
                  'changed without the other: the profile is what the boot module and '
                  'the E101 screen are built from, the struct is what the device is '
                  'checked against' % (name, key, profile[key], value))
        # The stock files the helper checks by content, and the archive ceiling it
        # enforces before the handover. The C is the struct the helper was compiled
        # with, the JSON is what the build and the E101 screen read; a build with
        # one changed and not the other either refuses a firmware it was made for
        # or runs code it has not identified.
        identity_name = declared[selected]['identity']
        check(identity_name is not None,
              '%s does not name a stock-file identity array: a build for it would run '
              'the boot and draw screens through programs it never checked' % selected)
        entries = []
        if identity_name is not None:
            check(identity_name in arrays,
                  '%s names %s, which devicedata.c does not define[]' % (selected, identity_name))
            entries = arrays.get(identity_name, [])
            check(declared[selected]['count'] == len(entries),
                  '%s says its identity array has %r entries and it has %d'
                  % (selected, declared[selected]['count'], len(entries)))
            paths = [entry['path'] for entry in entries]
            check(sorted(paths) == sorted(STOCK_FILES),
                  '%s carries stock files %s; the helper checks exactly %s'
                  % (selected, paths, list(STOCK_FILES)))
            check(len(paths) == len(set(paths)),
                  '%s names a stock file twice: %s' % (selected, paths))
            listed = profile.get('stock_files')
            check(isinstance(listed, list),
                  '%s has no stock_files: the build would have no display hash to put '
                  'into the launcher' % name)
            if isinstance(listed, list):
                by_path = {entry.get('path'): entry for entry in listed if isinstance(entry, dict)}
                check(sorted(by_path) == sorted(STOCK_FILES),
                      '%s lists stock files %s and the helper checks %s'
                      % (name, sorted(by_path), list(STOCK_FILES)))
                for entry in entries:
                    got = by_path.get(entry['path'])
                    check(got is not None,
                          '%s has no stock_files entry for %s, which devicedata.c checks'
                          % (name, entry['path']))
                    if got is None:
                        continue
                    check(got.get('bytes') == entry['bytes'] and got.get('sha256') == entry['sha256'],
                          '%s says %s is %r bytes %r and devicedata.c has %r bytes %r. '
                          'One of the two was changed without the other: the helper '
                          'would refuse the firmware this profile is for'
                          % (name, entry['path'], got.get('bytes'), got.get('sha256'),
                             entry['bytes'], entry['sha256']))
            archive = declared[selected]['archive_max']
            check(archive == profile.get('device_archive_max'),
                  '%s says device_archive_max is %r and devicedata.c has %r: the helper '
                  'and the boot module would refuse archives of different sizes'
                  % (name, profile.get('device_archive_max'), archive))
        cpuid = profile.get('stock_cpuid')
        check(isinstance(cpuid, str) and re.fullmatch(r'0x[0-9a-f]{8}', cpuid) is not None,
              '%s has no stock_cpuid (0x followed by eight lower-case hex digits): the '
              'handoff module would have no CPU identity to pin' % name)

    for selected, users in sorted(named.items()):
        check(len(users) == 1,
              'profiles %s all name %s: two firmwares cannot share one set of facts'
              % (', '.join(users), selected))
    orphans = sorted(set(declared) - set(named))
    check(orphans == [],
          'devicedata.c defines %s, which no profile names: a struct nobody can build '
          'for is a machine this project cannot say it supports' % ', '.join(orphans))

    return report()


def report():
    print('%d checks, %d failed (%d stock profiles)'
          % (checks, len(failures), len(profiles())))
    for failure in failures:
        print('  FAIL %s' % failure)
    return 1 if failures else 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Exception:  # noqa: BLE001 - any fault here is this test's answer
        print('0 checks, 1 failed')
        print('  FAIL the check itself stopped: %s' % repr(sys.exc_info()[1]))
        sys.exit(1)
