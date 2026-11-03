#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""The home screen's layout and the code that answers it have to agree.

`res/xml/default_workspace_pw3.xml` is read once, when the workspace database is
created, and a row it cannot build is **dropped without a word** on the screen or
in any log a reader will see. Two of its rows cannot be checked any other way
before the device is in front of you:

  * the entry for all applications (0014) is a `<shortcut>` whose intent names no
    activity. It works only because `Launcher.startAppShortcutOrInfoActivity`
    recognises the action and opens the list itself, so the string in the layout
    and `Utilities.ACTION_ALL_APPS_ENTRY` are one fact written twice. Change
    either and the tap starts nothing at all: the intent resolves to no activity,
    and the reader gets a toast about an application that is not there;
  * the first cell is the downloader's own entry, and the application it installs
    takes that cell over by looking the entry up by component
    (WeReadInstaller/HomeScreenCell.java). Move the activity and the cell is left
    showing a downloader for something that is installed.

The rest is what this file has already been got wrong by, each line below written
after it happened:

  * an XML comment may not contain `--`. The layout went into a build with one
    and `aapt2` refused it (2026-10-22);
  * a `<shortcut>` needs both `launcher:title` and `launcher:icon` to resolve to
    resources, or `ShortcutParser` returns -1 and the row is dropped
    (AutoInstallsLayout);
  * an edit to the layout reaches a device that already has a home screen only
    if `pw3_workspace_layout_version` moves with it (0015). Nothing in a build
    notices a version that stayed behind, so the version is held here against a
    hash of the layout: change the file and this test says which number to move.
    0011 moved the number and not the layout, 0012 moved it again to undo that;
  * what a layout change can carry is a favorite or a shortcut in a cell, and
    nothing else (0015). A folder, a widget, a hotseat cell or a `<resolve>`
    cannot be told apart from what is on the screen already, so it is passed over
    there -- silently, as far as the reader is concerned.

It reads the applied tree rather than the patches: a later patch may change any
of this, and the question is what the tree says after the whole series.
"""
import argparse
import hashlib
import json
import re
import sys
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

PORT = Path(__file__).resolve().parents[2]
INSTALLER_JAVA = PORT / 'device/amazon/pw3/apps/WeReadInstaller/src/org/readom/wereadinstaller/Installer.java'
INSTALLER_MANIFEST = PORT / 'device/amazon/pw3/apps/WeReadInstaller/AndroidManifest.xml'
HANDOVER = (PORT / 'device/amazon/pw3/apps/WeReadInstaller/src/org/readom'
            '/wereadinstaller/HomeScreenCell.java')
LAUNCHER_NS = '{http://schemas.android.com/apk/res-auto/com.android.launcher3}'
# The home screen layout and the version of it in res/values/config.xml, which
# have to move together (0015). Whoever edits the layout records the new pair.
LAYOUT_PIN = (2, 'c7d6b08da4b41d4c7448d9b315fd791821199aca2869832ae351e35660c70343')
# What WorkspaceLayoutDifference.keyOf() makes of each entry here: a component
# for a <favorite>, the intent's action for a <shortcut>, which names no
# activity. Every device that has ever been seeded keeps a row carrying its
# key, so these strings are not names the layout is free to choose -- they are
# stored on the reader's device. Pinned separately from the digest above
# because the two failures are not the same: an entry added or moved reaches a
# first open and nobody else, while a key that *changes* reaches everybody, as
# a second cell beside the one they already have.
LAYOUT_KEYS = (
    'com.android.documentsui/.LauncherActivity',
    'com.android.settings/.Settings',
    'info.plateaukao.einkbro/.activity.BrowserActivity',
    'org.readom.action.ALL_APPS',
    'org.readom.wereadinstaller/.StatusActivity',
)

TREE = None


def attribute(element, name):
    return element.get(LAUNCHER_NS + name, element.get(name))


class HomeScreenEntries(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.layout_path = TREE / 'res/xml/default_workspace_pw3.xml'
        cls.layout_text = cls.layout_path.read_text()
        cls.layout = ET.fromstring(cls.layout_text)       # also the "--" check
        cls.utilities = (TREE / 'src/com/android/launcher3/Utilities.java').read_text()
        cls.provider = (TREE / 'src/com/android/launcher3/LauncherProvider.java').read_text()

    def shortcuts(self):
        return self.layout.findall('shortcut')

    def test_all_apps_entry_action_is_the_one_the_launcher_answers(self):
        """The action in the layout and the constant in Utilities are one string.

        Changing the action is worse than it looks, because the action is also
        what tells this entry apart from the rows already on a home screen
        (WorkspaceLayoutDifference.keyOf). A device that has the old row would
        keep it -- nothing is ever removed from a workspace that exists -- and
        the launcher would no longer answer it, leaving a cell that starts
        nothing beside the new one. So: don't.
        """
        constant = re.search(r'ACTION_ALL_APPS_ENTRY\s*=\s*"([^"]+)"', self.utilities)
        self.assertIsNotNone(constant, 'Utilities.ACTION_ALL_APPS_ENTRY is gone')
        entries = [s for s in self.shortcuts()
                   if constant.group(1) in (attribute(s, 'uri') or '')]
        self.assertEqual(len(entries), 1,
                         'the layout has no <shortcut> carrying %s' % constant.group(1))
        uri = attribute(entries[0], 'uri')
        self.assertEqual(uri, '#Intent;action=%s;end' % constant.group(1),
                         'the entry has to name that action and nothing else: ' + uri)

    def test_all_apps_entry_has_a_title_and_an_icon_that_exist(self):
        """Without both, ShortcutParser returns -1 and the row is dropped."""
        entry = self.shortcuts()[0]
        title = attribute(entry, 'title')
        icon = attribute(entry, 'icon')
        self.assertTrue(title and title.startswith('@string/'), 'no title: %r' % title)
        self.assertTrue(icon and icon.startswith('@drawable/'), 'no icon: %r' % icon)
        name = title.split('/', 1)[1]
        for values in ['res/values/strings.xml', 'res/values-zh-rCN/strings.xml']:
            text = (TREE / values).read_text()
            self.assertIn('name="%s"' % name, text, '%s is not in %s' % (name, values))
        self.assertTrue((TREE / ('res/drawable/%s.xml' % icon.split('/', 1)[1])).is_file(),
                        '%s is not in res/drawable' % icon)

    def test_an_entry_keeps_the_identity_devices_already_hold(self):
        """The keys the difference mechanism compares, which devices store.

        V84 put a second 全部应用 on the one device that had a home screen. The
        rename to org.readom moved the all-applications action from
        org.kdandroid.action.ALL_APPS, that cell carries no component, so
        keyOf() falls back to the action -- and the row already on the screen
        answered to the old one. Nothing was on the screen under the new key,
        so the layout placed it, which is exactly what the mechanism is for.

        A key here is therefore not the layout's to rename. Adding an entry is
        safe and so is moving one; changing or dropping a key is a removal, and
        the difference mechanism has no way to express a removal.
        """
        keys = []
        for entry in self.layout:
            if entry.tag == 'favorite':
                package = attribute(entry, 'packageName')
                activity = attribute(entry, 'className')
                keys.append('%s/%s' % (package, activity[len(package):]
                                       if activity.startswith(package + '.') else activity))
            elif entry.tag == 'shortcut':
                uri = attribute(entry, 'uri') or ''
                action = re.search(r'action=([^;]+)', uri)
                self.assertIsNotNone(action, 'a shortcut with no action: ' + uri)
                keys.append(action.group(1))
        self.assertEqual(tuple(sorted(keys)), LAYOUT_KEYS,
                         'the keys this layout is stored under changed.\n'
                         'Gone from the pin: %s\nNew in the layout: %s\n'
                         'A key that is gone is a cell every existing home screen keeps, '
                         'and the entry that replaced it is placed beside it. Keep the key, '
                         'or accept the duplicate and say so here.'
                         % (sorted(set(LAYOUT_KEYS) - set(keys)),
                            sorted(set(keys) - set(LAYOUT_KEYS))))

    def test_the_layout_names_no_application_the_image_does_not_ship(self):
        """Which applications a reader installs is not the image's business.

        The first answer to "WeRead is not on the home screen after an upgrade"
        named WeRead in this layout. It worked, and it was wrong at the root: once
        one package is in the layout, the next one is an argument rather than a
        principle. The downloader hands its own cell over (HomeScreenCell) and an
        install finds a cell on the screen in front of the reader (0016); neither
        of those needs a name here.
        """
        package = re.search(r'PACKAGE\s*=\s*"([^"]+)"', INSTALLER_JAVA.read_text()).group(1)
        self.assertNotIn(package, self.layout_text,
                         'the default layout names ' + package)

    def test_the_first_cell_is_the_entry_the_handover_hands_over(self):
        """HomeScreenCell updates the row of this exact activity, or nothing happens."""
        entries = [f for f in self.layout.findall('favorite')
                   if attribute(f, 'packageName') == 'org.readom.wereadinstaller']
        self.assertEqual(len(entries), 1, 'expected one entry for the downloader')
        self.assertEqual((int(attribute(entries[0], 'x')), int(attribute(entries[0], 'y'))), (0, 1),
                         'the downloader is the first application cell')
        activity = attribute(entries[0], 'className')
        self.assertIn('android:name=".%s"' % activity.rsplit('.', 1)[1],
                      INSTALLER_MANIFEST.read_text(),
                      'the installer does not declare ' + activity)
        self.assertIn('new ComponentName(context, %s.class)' % activity.rsplit('.', 1)[1],
                      HANDOVER.read_text(),
                      'the handover looks for a different activity than the layout places')

    def test_the_handover_names_the_table_the_launcher_exports(self):
        """The authority, the two permissions, the columns and the item type."""
        handover = HANDOVER.read_text()
        manifest = (TREE / 'AndroidManifest.xml').read_text()
        installer_manifest = INSTALLER_MANIFEST.read_text()
        authority = re.search(r'content://([^/"]+)/favorites', handover).group(1)
        self.assertIn('android:authorities="%s"' % authority, manifest,
                      'the launcher does not export ' + authority)
        for name in ['com.android.launcher3.permission.READ_SETTINGS',
                     'com.android.launcher3.permission.WRITE_SETTINGS']:
            self.assertIn('android:name="%s"' % name, manifest,
                          'the launcher no longer declares ' + name)
            self.assertIn(name, installer_manifest, 'the installer does not ask for ' + name)
        settings = (TREE / 'src/com/android/launcher3/LauncherSettings.java').read_text()
        theirs = re.search(r'ITEM_TYPE_APPLICATION\s*=\s*(\d+)', settings).group(1)
        ours = re.search(r'ITEM_TYPE_APPLICATION\s*=\s*(\d+)', handover).group(1)
        self.assertEqual(ours, theirs, 'ITEM_TYPE_APPLICATION is %s, not %s' % (theirs, ours))
        # Against the CREATE TABLE itself rather than the constants: `_id` is
        # BaseColumns', not the launcher's, and the schema is what a write hits.
        schema = settings.split('addTableToDb')[1]
        for column in re.findall(r'private static final String [A-Z_]+ = "([a-zA-Z_]+)";', handover):
            self.assertIn('"%s ' % column, schema,
                          'the favorites table has no column called ' + column)

    def test_a_layout_change_reaches_a_home_screen_that_already_exists(self):
        """The layout's version moves with the layout, and the launcher reads it.

        Both halves are pinned here because nothing in a build can notice the
        half that is missing: an edit without the version reaches nobody who has
        the system, and the version without the mechanism below is a number
        nobody reads.
        """
        config = (TREE / 'res/values/config.xml').read_text()
        version = re.search(r'<integer name="pw3_workspace_layout_version">(\d+)</integer>',
                            config)
        self.assertIsNotNone(version, 'res/values/config.xml has no layout version (0015)')
        digest = hashlib.sha256(self.layout_path.read_bytes()).hexdigest()
        self.assertEqual((int(version.group(1)), digest), LAYOUT_PIN,
                         'the layout and its version have to move together. This layout is\n'
                         '  %s\n'
                         'at version %s; the pair recorded in this file is version %d and\n'
                         '  %s\n'
                         'So either the version in res/values/config.xml has not moved with the\n'
                         'edit to res/xml/default_workspace_pw3.xml -- and the edit reaches\n'
                         'nobody who already has the system -- or it has, and LAYOUT_PIN here\n'
                         'has to say so.' % (digest, version.group(1), LAYOUT_PIN[0], LAYOUT_PIN[1]))
        self.assertIn('R.integer.pw3_workspace_layout_version', self.provider,
                      'LauncherProvider does not read the layout version')
        self.assertIn('WorkspaceLayoutDifference.read(', self.provider,
                      'nothing works out what a newer layout added (0015)')
        self.assertIn('mDifference.wants(values)', self.provider,
                      'the difference is not consulted where the parser inserts a row')
        self.assertTrue((TREE / 'src/com/android/launcher3/WorkspaceLayoutDifference.java')
                        .is_file(), 'WorkspaceLayoutDifference.java is gone')

    def test_the_layout_carries_only_what_a_layout_change_can_deliver(self):
        """A folder, a widget, a hotseat cell or a <resolve> is passed over (0015).

        Each of them would reach a first open and nothing else, without a word
        anywhere the reader can see, so the layout does not have one. A <resolve>
        is the sharp case: the difference answers "this one is already on the
        screen" with the same -1 that means "this one could not be added", and
        ResolveParser reads that as "try the next alternative" -- which would put
        the alternative on the screen beside the entry that is already there.
        """
        for element in list(self.layout):
            self.assertIn(element.tag, ('favorite', 'shortcut'),
                          '<%s> is not carried by a layout change (0015)' % element.tag)
            self.assertEqual(list(element), [],
                             '<%s> has children, so it is a container' % element.tag)
            self.assertIsNone(attribute(element, 'container'),
                              'a cell outside the screens is not carried by a layout change')

    def test_an_installed_application_lands_on_the_screen_in_front_of_you(self):
        """AOSP searches from screen 1 up, which on this device is a new page (0016)."""
        task = (TREE / 'src/com/android/launcher3/model/AddWorkspaceItemsTask.java').read_text()
        self.assertIn('int preferredScreenIndex = 0;', task,
                      'an install would make a second page instead of using screen 0 (0016)')
        self.assertIn('if (screenId == Workspace.FIRST_SCREEN_ID) {', task,
                      'the search does not know which screen it is looking at')
        self.assertIn('occupied.markCells(0, 0, profile.numColumns, 1, '
                      'FeatureFlags.QSB_ON_FIRST_SCREEN);', task,
                      'row 0 of the first screen is the search box; an icon written there '
                      'is dropped by the next load (LoaderCursor)')

    def test_every_cell_of_the_layout_is_in_the_grid_and_alone_in_it(self):
        """A cell outside the grid, or two rows in one cell, is a row dropped."""
        profile = (TREE / 'go/res/xml/device_profiles.xml').read_text()
        columns = int(re.search(r'numColumns="(\d+)"', profile).group(1))
        rows = int(re.search(r'numRows="(\d+)"', profile).group(1))
        taken = {}
        for element in list(self.layout):
            if element.tag not in ('favorite', 'shortcut', 'resolve'):
                continue
            cell = (int(attribute(element, 'x')), int(attribute(element, 'y')))
            self.assertTrue(0 <= cell[0] < columns and 0 <= cell[1] < rows,
                            'cell %s is outside the %dx%d grid' % (cell, columns, rows))
            self.assertNotIn(cell, taken, 'two entries in cell %s' % (cell,))
            taken[cell] = element.tag
        self.assertNotIn((0, 0), taken, 'row 0 of screen 0 is the search box (0004)')

    def test_the_schema_version_and_the_way_back_from_it_agree(self):
        """SCHEMA_VERSION, the switch below it and downgrade_schema.json agree.

        This is about the tables and not about the layout any more (0015). It
        stays because the file that says how to go back a version is written by
        hand: a release that adds a column and forgets it leaves a device that
        downgrades with a database the older release cannot read.
        """
        version = int(re.search(r'SCHEMA_VERSION\s*=\s*(\d+)', self.provider).group(1))
        cases = [int(n) for n in re.findall(r'^\s*case (\d+):', self.provider, re.M)]
        self.assertIn(version, cases,
                      'onUpgrade has no case %d, so a database at %d is destroyed and '
                      'remade instead of carried forward' % (version, version))
        self.assertEqual(max(cases), version, 'onUpgrade knows a version past SCHEMA_VERSION')
        schema = json.loads(re.sub(r'//.*', '', (TREE / 'res/raw/downgrade_schema.json').read_text()))
        self.assertEqual(schema['version'], version,
                         'downgrade_schema.json is at %s, SCHEMA_VERSION at %d'
                         % (schema['version'], version))
        self.assertIn('downgrade_to_%d' % (version - 1), schema,
                      'nothing says how to go back one version from %d' % version)

    def test_no_comment_carries_a_double_hyphen(self):
        """ET would have raised in setUpClass; this says which line, for whoever broke it."""
        for number, line in enumerate(self.layout_text.splitlines(), 1):
            body = line.replace('<!--', '').replace('-->', '')
            self.assertNotIn('--', body, 'line %d: an XML comment may not contain "--"' % number)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--launcher-tree', type=Path, required=True,
                        help='packages/apps/Launcher3 with the patch series applied')
    arguments, rest = parser.parse_known_args()
    global TREE
    TREE = arguments.launcher_tree
    unittest.main(argv=[sys.argv[0]] + rest, verbosity=2)


if __name__ == '__main__':
    main()
