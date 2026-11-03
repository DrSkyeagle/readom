#!/usr/bin/env python3
"""Write the patches that give Android 8.1 the time zone rules of a current tzdb release.

Android 8.1 (r81) carries tzdata2017b. Since then IANA changed the rules of many
zones (Brazil without daylight saving time from 2019, Mexico from 2022,
Kazakhstan on one offset from 2024, Greenland, Egypt, Jordan, Syria, Paraguay,
...). Two copies of the rules are read at run time and both are replaced:

  system/timezone/output_data/iana/tzdata   bionic and libcore (java.util.TimeZone)
  external/icu/icu4c/source/stubdata/icudt58l.dat   ICU4C and android.icu

This follows system/timezone/update-tzdata.py of 8.1, with the inputs current
tooling needs:
  * bionic: zic and the "rearguard" data form are built from the same tzdb
    release (the 2017 zic and tools do not know the newer syntax; rearguard
    keeps daylight saving time non-negative, as 8.1's code expects). zic runs
    with "-b fat": libcore in 8.1 reads only the 32-bit section of each zone.
    All of backzone is included, as 8.1 did. 8.1's ZoneCompactor packs the
    result; every ID of 8.1 stays, but for one alias IANA has removed since
    (REMOVED_UPSTREAM).
  * ICU: the time zone resources ICU publishes for that release in the format
    of ICU 4.4 and later (unicode-org/icu-data, tzdata/icunew/<release>/44/le)
    replace those in icudt58l.dat, with icupkg built from 8.1's ICU 58 source
    (the procedure of the ICU user guide, "Updating the Time Zone Data").
Not regenerated: tzlookup.xml (8.1 keeps it by hand; it only lists the zones of
each country for telephony) and the ICU text sources under data/misc (the build
uses the packed icudt58l.dat).

Run inside the build chroot:
  update-timezone-data.py INPUT_DIR AOSP_DIR PATCH_ROOT
INPUT_DIR holds tzdata<release>.tar.gz and tzcode<release>.tar.gz (signed by
IANA; their SHA-256 is pinned below) and the four .res files (git blob SHA-1
pinned below). AOSP_DIR is the unpatched 8.1 tree. The patches go to
PATCH_ROOT/aosp-timezone and PATCH_ROOT/aosp-icu.
"""
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

RELEASE = '2026e'
TZDATA_SHA256 = 'b26882805f26aac59d5b222978e6580484b834ccdc98be89df2f05a6dc53a652'
TZCODE_SHA256 = 'cc3d27ca2a0d8399504551b920970d80af83bfb9c216e8082a15491921935d54'
ICU_DATA_COMMIT = 'bae5ca647fa41703393dbf00240ec1b807a9c353'   # "ICU-23249 TZDB 2026e"
ICU_RES = {   # git blob SHA-1 of tzdata/icunew/2026e/44/le/<name>
    'zoneinfo64.res': '5608fdda237b438641858cc6525b7b7dd0b1a58b',
    'metaZones.res': '79b4c6bca4e8b0cac6cfc190f06cb71ae66a9c06',
    'timezoneTypes.res': '8cc23114c5d29b03b01bf2359940e94d00e14d5c',
    'windowsZones.res': '5d8a978952cb0bc075ce07a09b94fb402c28464b',
}
# IDs of 8.1's data that IANA has removed since, as has every later Android: the
# deprecated alias Canada/East-Saskatchewan (2017c; America/Regina remains). The
# time zone list of 8.1's Settings offers canonical IDs only.
REMOVED_UPSTREAM = {'Canada/East-Saskatchewan'}
JDK = 'prebuilts/jdk/jdk8/linux-x86/bin'   # JAVA_BIN_DIR overrides (the build VM's native tools)
TZDATA_PATH = 'output_data/iana/tzdata'               # in system/timezone
ICU_DAT_PATH = 'icu4c/source/stubdata/icudt58l.dat'    # in external/icu


def run(*cmd, **kw):
    print('+', ' '.join(str(c) for c in cmd), flush=True)
    return subprocess.run([str(c) for c in cmd], check=True, **kw)


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def git_blob(path):
    data = Path(path).read_bytes()
    return hashlib.sha1(b'blob %d\0' % len(data) + data).hexdigest()


def tzdata_ids(data):
    """The zone IDs in the index of a ZoneCompactor file."""
    index, data_offset = struct.unpack('>ii', data[12:20])
    return {data[o:o + 40].rstrip(b'\0').decode() for o in range(index, data_offset, 52)}


def binary_patch(project_path, old_bytes, new_bytes, header, output):
    """A git binary patch replacing one file of a project, with full preimage hash."""
    with tempfile.TemporaryDirectory() as tmp:
        git = ['git', '-C', tmp, '-c', 'user.name=pw3', '-c', 'user.email=pw3@localhost']
        run(*git, 'init', '-q')
        target = Path(tmp) / project_path
        target.parent.mkdir(parents=True)
        target.write_bytes(old_bytes)
        run(*git, 'add', '-A')
        run(*git, 'commit', '-q', '-m', 'base')
        target.write_bytes(new_bytes)
        run(*git, 'add', '-A')
        diff = run(*git, 'diff', '--cached', '--binary', '--full-index', '--no-color',
                   capture_output=True).stdout.decode()
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(header + '---\n' + diff)
    print('wrote', output, len(diff), 'bytes')


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    inputs, aosp, patch_root = (Path(a).resolve() for a in sys.argv[1:])
    tzdata_tar = inputs / ('tzdata%s.tar.gz' % RELEASE)
    tzcode_tar = inputs / ('tzcode%s.tar.gz' % RELEASE)
    if sha256(tzdata_tar) != TZDATA_SHA256 or sha256(tzcode_tar) != TZCODE_SHA256:
        sys.exit('tzdb archives do not match the pinned SHA-256')
    for name, blob in ICU_RES.items():
        if git_blob(inputs / name) != blob:
            sys.exit('%s does not match the pinned blob' % name)

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        tzdb = tmp / 'tzdb'
        tzdb.mkdir()
        for tar in (tzcode_tar, tzdata_tar):
            with tarfile.open(tar) as t:
                t.extractall(tzdb)
        if (tzdb / 'version').read_text().strip() != RELEASE:
            sys.exit('unexpected tzdb version')
        run('make', '-C', tzdb, 'CC=gcc', 'zic')
        run('make', '-C', tzdb, 'PACKRATDATA=backzone', 'rearguard.zi')   # all of backzone, as 8.1
        zi = tzdb / 'rearguard.zi'
        zoneinfo = tmp / 'zoneinfo'
        run(tzdb / 'zic', '-b', 'fat', '-d', zoneinfo, zi)

        # ZoneCompactor's setup file, as update-tzdata.py writes it: the links,
        # then every name (zones and links) once, sorted.
        links, names = set(), set()
        for line in zi.read_text().splitlines():   # ziguard.awk keeps the full keywords
            fields = line.split()
            if fields[:1] == ['Link']:
                links.add('Link %s %s' % (fields[1], fields[2]))
                names.add(fields[2])
            elif fields[:1] == ['Zone'] and fields[1] != 'Factory':   # not in 8.1's region list
                names.add(fields[1])
        setup = tmp / 'setup'
        setup.write_text(''.join('%s\n' % l for l in sorted(links)) + ''.join('%s\n' % n for n in sorted(names)))
        classes = tmp / 'classes'
        classes.mkdir()
        jdk = Path(os.environ.get('JAVA_BIN_DIR', aosp / JDK))
        run(jdk / 'javac', '-d', classes, aosp / 'system/timezone/zone_compactor/main/java/ZoneCompactor.java')
        out = tmp / 'out'
        out.mkdir()
        run(jdk / 'java', '-cp', classes, 'ZoneCompactor', setup, zoneinfo, tzdb / 'zone.tab', out,
            'tzdata' + RELEASE)
        new_tzdata = (out / 'tzdata').read_bytes()
        old_ids = tzdata_ids((aosp / 'system/timezone' / TZDATA_PATH).read_bytes())
        new_ids = tzdata_ids(new_tzdata)
        if new_tzdata[:12].rstrip(b'\0') != ('tzdata' + RELEASE).encode():
            sys.exit('tzdata header is not tzdata' + RELEASE)
        if old_ids - new_ids - REMOVED_UPSTREAM:   # a saved zone ID (persist.sys.timezone) stays valid
            sys.exit('zone IDs of 8.1 missing: %s' % sorted(old_ids - new_ids - REMOVED_UPSTREAM))
        print('tzdata: %d IDs (8.1: %d), new: %s' % (len(new_ids), len(old_ids), sorted(new_ids - old_ids)))

        # icupkg of 8.1's ICU 58, built out of tree.
        icu_src = aosp / 'external/icu/icu4c/source'
        build = tmp / 'icu'
        build.mkdir()
        # ICU 58 includes xlocale.h for strtod_l, which glibc 2.26 removed (fixed in
        # ICU 60). icupkg parses no numbers; the host tool is built without it.
        run(icu_src / 'runConfigureICU', 'Linux', '--disable-renaming', cwd=build,
            env=dict(os.environ, CPPFLAGS='-DU_USE_STRTOD_L=0'))
        for d in ('lib', 'bin'):   # made by the top-level Makefile, which builds far more
            (build / d).mkdir()
        for part in ('stubdata', 'common', 'i18n', 'tools/toolutil', 'tools/icupkg'):
            run('make', '-j6', '-C', build / part)
        icupkg = build / 'bin/icupkg'
        env = dict(os.environ, LD_LIBRARY_PATH='%s:%s' % (build / 'lib', build / 'stubdata'))   # stub libicudata
        dat = tmp / 'icudt58l.dat'
        shutil.copyfile(aosp / 'external/icu' / ICU_DAT_PATH, dat)
        before = run(icupkg, '-l', dat, env=env, capture_output=True).stdout.decode().split()
        for name in ICU_RES:
            if 'icudt58l/' + name not in before and name not in before:
                sys.exit('%s is not in icudt58l.dat' % name)
            run(icupkg, '-a', name, '-s', inputs, dat, env=env)
        after = run(icupkg, '-l', dat, env=env, capture_output=True).stdout.decode().split()
        if sorted(after) != sorted(before):
            sys.exit('the item list of icudt58l.dat changed')
        check = tmp / 'check'
        check.mkdir()
        for name in ICU_RES:
            run(icupkg, '-x', name, '-d', check, dat, env=env)
            if (check / name).read_bytes() != (inputs / name).read_bytes():
                sys.exit('%s was not replaced' % name)
        new_dat = dat.read_bytes()

    old_tzdata = (aosp / 'system/timezone' / TZDATA_PATH).read_bytes()
    binary_patch(TZDATA_PATH, old_tzdata, new_tzdata, HEADER_TZ.format(release=RELEASE),
                 patch_root / 'aosp-timezone' / ('0001-pw3-tzdata%s.patch' % RELEASE))
    old_dat = (aosp / 'external/icu' / ICU_DAT_PATH).read_bytes()
    binary_patch(ICU_DAT_PATH, old_dat, new_dat, HEADER_ICU.format(release=RELEASE, commit=ICU_DATA_COMMIT[:12]),
                 patch_root / 'aosp-icu' / ('0001-pw3-time-zones-of-tzdata%s.patch' % RELEASE))


HEADER_TZ = """From: PW3 Android port
Subject: [PW3] timezone: tzdata{release}

The rules of bionic and libcore move from tzdata2017b to tzdata{release}
(IANA, signed by Paul Eggert). Generated by tools/update-timezone-data.py:
zic of the same release, rearguard form with backzone, "zic -b fat" (libcore
of 8.1 reads the 32-bit section), packed by 8.1's ZoneCompactor. The same
release goes into ICU's data (external/icu).
"""

HEADER_ICU = """From: PW3 Android port
Subject: [PW3] icu: the time zones of tzdata{release}

zoneinfo64, metaZones, timezoneTypes and windowsZones in icudt58l.dat are
the resources ICU publishes for tzdata{release} (unicode-org/icu-data
{commit}, tzdata/icunew/{release}/44/le; format 44 is read by ICU 4.4 and
later), put in with icupkg of this ICU 58. Generated by
tools/update-timezone-data.py, together with bionic's tzdata.
"""

if __name__ == '__main__':
    main()
