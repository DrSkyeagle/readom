#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""The home screen's marks, read back out of the packages the build made.

launcher-icons/make_icons.py --check proves the resource files in this
repository are what the script makes of Material Symbols' drawings. It cannot prove that
those files are what the device will draw, and between the two there is one
step that fails silently: the browser's icon is replaced by a runtime overlay,
and ResTable::getEntry() passes over an overlay entry whose configuration is
neither better than the target's nor equal to it, without a word in any log.
The icon that survived that is the prebuilt's own, and the only way to see it
is to look.

So this reads the built APKs:

  * each of the four resources exists and is a compiled vector, not a bitmap;
  * its path data is the path data make_icons.py wrote, so the drawing in the
    package is the drawing in the tree;
  * the browser overlay's configuration is exactly the one its target's own
    icon is defined in, which is what makes the overlay win.
"""
import argparse
import re
import subprocess
import sys
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_xml_named_classes import strings_of_string_pool   # noqa: E402

# The device runs API 27, which is what decides whether a -vNN qualifier
# applies at all.
DEVICE_SDK = 27

# mark, the package it is drawn from, the resource that names it. The
# all-applications mark is a drawable of the launcher itself; the other three
# are the application icons the home screen loads through PackageManager.
ENTRIES = [
    ('all-apps', 'system/priv-app/Launcher3Go/Launcher3Go.apk', 'drawable/ic_all_apps_pw3', None),
    ('files', 'system/priv-app/DocumentsUI/DocumentsUI.apk', 'mipmap/ic_app_icon', None),
    ('settings', 'system/priv-app/Settings/Settings.apk', 'drawable/ic_launcher_settings', None),
    # The browser is a prebuilt; its icon comes from the overlay, and the
    # overlay has to beat what the prebuilt itself defines.
    ('browser', 'system/vendor/overlay/PW3BrowserName.apk', 'mipmap/ic_launcher',
     'system/app/EinkBro/EinkBro.apk'),
    ('browser', 'system/vendor/overlay/PW3BrowserName.apk', 'mipmap/ic_launcher_round',
     'system/app/EinkBro/EinkBro.apk'),
]


def dump(aapt2, apk):
    out = subprocess.run([str(aapt2), 'dump', str(apk)],
                         capture_output=True, text=True)
    if out.returncode:
        raise SystemExit('aapt2 dump %s: %s' % (apk.name, out.stderr.strip()))
    return out.stdout


def entry_of(table, resource):
    """The configurations one resource is defined in, and the file of each.

    None where the package has no such resource at all; the default
    configuration prints as "()" and comes back as the empty string.
    """
    kind, name = resource.split('/')
    found = re.search(r'^\s+spec resource 0x[0-9a-f]+ [\w.]+:%s/%s$(.*?)(?=^\s+spec resource |\Z)'
                      % (re.escape(kind), re.escape(name)), table, re.M | re.S)
    if not found:
        return None
    return re.findall(r'^\s+\(([\w-]*)\) \(file\) (\S+)$', found.group(1), re.M)


def version_of(config):
    """The -vNN of a configuration, 0 when it has none."""
    found = re.search(r'-v(\d+)$', config)
    return int(found.group(1)) if found else 0


def density_any(configs):
    """The anydpi entries: the only ones in the running for a DENSITY_ANY win.

    ResTable_config::isBetterThan returns DENSITY_ANY over every density
    bucket before it ever looks at the version, so a bucket cannot win against
    an anydpi entry and the contest is among these alone.
    """
    return [(config, path) for config, path in configs if config.split('-')[0] == 'anydpi']


def path_data(apk, member):
    """The android:pathData strings of a compiled VectorDrawable, in order.

    A compiled resource is binary XML: a RES_XML_TYPE chunk (0x0003) holding a
    string pool. The path data is in that pool, and reading the strings out of
    it is enough to say whether the drawing is the one the tree holds, without
    a parser for the rest of the format.
    """
    with zipfile.ZipFile(apk) as archive:
        blob = archive.read(member)
    if blob[:2] != b'\x03\x00':
        raise SystemExit('%s!%s is not compiled XML (%r): a bitmap here means the '
                         'override did not take' % (apk.name, member, blob[:4]))
    # The string pool is the first chunk after the 8-byte file header, and it
    # is read by the one reader this tree has. There used to be a second copy
    # of that reader here, wrong in exactly the way the first one was: a UTF-8
    # string carries two lengths, and reading only the first works until a
    # string needs two bytes for it -- which the icons' own path data does.
    try:
        pool = strings_of_string_pool(blob[8:])
    except ValueError as problem:
        raise SystemExit('%s!%s: %s' % (apk.name, member, problem))
    return [s for s in pool if re.match(r'^[Mm][\d\s.,-]', s)]


def authored(root, mark):
    """The path data make_icons.py writes for one mark."""
    import importlib.util
    script = root / 'device/amazon/pw3/launcher-icons/make_icons.py'
    spec = importlib.util.spec_from_file_location('pw3_make_icons', script)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    symbol = {name: icon for name, _, icon in module.ICONS}[mark]
    # Two paths now: the tile the script writes, and the symbol's own drawing
    # inside it.
    return [module.tile_path(), module.read_icon(symbol)]


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--product', type=Path, required=True,
                        help='out/target/product/pw3')
    parser.add_argument('--aapt2', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]

    failures = []
    checked = 0
    for mark, relative, resource, target_relative in ENTRIES:
        apk = args.product / relative
        if not apk.is_file():
            failures.append('%s: %s is not in the image' % (mark, relative))
            continue
        configs = entry_of(dump(args.aapt2, apk), resource)
        if configs is None:
            failures.append('%s: %s has no %s' % (mark, apk.name, resource))
            continue
        if not configs:
            failures.append('%s: %s defines %s, but not as a file; these are vectors'
                            % (mark, apk.name, resource))
            continue
        if len(configs) != 1:
            failures.append('%s: %s defines %s in %d configurations (%s); one mark is one '
                            'drawing' % (mark, apk.name, resource, len(configs),
                                         ', '.join(c for c, _ in configs)))
            continue
        config, member = configs[0]

        if target_relative:
            target = args.product / target_relative
            if not target.is_file():
                failures.append('%s: %s is not in the image' % (mark, target_relative))
                continue
            target_configs = entry_of(dump(args.aapt2, target), resource)
            applicable = [(c, p) for c, p in density_any(target_configs or [])
                          if version_of(c) <= DEVICE_SDK]
            if not applicable:
                failures.append('%s: %s defines %s in no anydpi configuration, so an anydpi '
                                'overlay would win on density alone; this check no longer '
                                'describes the contest' % (mark, target.name, resource))
                continue
            wins = max(applicable, key=lambda item: version_of(item[0]))[0]
            if config != wins:
                failures.append(
                    '%s: the overlay defines %s in (%s) and %s defines it in (%s). An overlay '
                    'entry is taken only where its configuration is better than the target\'s '
                    'or equal to it, so this one is passed over without a word and the '
                    'prebuilt\'s own icon stays on the screen.'
                    % (mark, resource, config, target.name, wins))
                continue

        # Sorted, not in order: a string pool is not the document, and it keeps
        # its own order. What this compares is therefore the set of drawings in
        # the package against the set in the tree. The order they are drawn in
        # and the colour each one is filled with are not in a string pool at
        # all; those are held by make_icons.py --check, which compares the
        # resource files themselves byte for byte, and this check is the other
        # half: that what the build put in the package is those files.
        drawn = sorted(path_data(apk, member))
        want = sorted(authored(root, mark))
        if drawn != want:
            failures.append('%s: %s!%s draws\n    %s\nand the tree draws\n    %s'
                            % (mark, apk.name, member, '\n    '.join(drawn),
                               '\n    '.join(want)))
            continue
        checked += 1

    if failures:
        print('\n'.join(failures))
        return 1
    print('%d resources: each a compiled vector with the path data the tree holds; the '
          'browser overlay matches its target\'s own configuration' % checked)
    return 0


if __name__ == '__main__':
    sys.exit(main())
