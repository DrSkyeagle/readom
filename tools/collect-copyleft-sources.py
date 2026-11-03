#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Collect the source of every copyleft file in an exported PW3 system image, for a release.

A release that hands out the image has to hand out, beside it, the complete source of each
file under the GPL or LGPL (GPL-2.0 section 3, LGPL-2.1 section 6, GPL-3.0 section 6), and
of each MPL file. Which files those are is read from the image's own notice file
(/etc/NOTICE.html.gz), which module made each file from module-info.json, and which AOSP
project holds that module from the resolved manifest. A copyleft file whose source cannot be
named stops the script; nothing is written then.

Output, in a new directory:
  <name>-aosp-sources.tar.xz        the AOSP projects that made such files, at the manifest's
                                    revision, with this port's patches applied: what was built
  linux-3.0.35-amazon-5.16.2.1.1.tar.gz
                                    Amazon's kernel source, unmodified; the patches, the
                                    configuration and the build script are in this repository
  other archives                    copyleft sources of third-party apps
                                    (device/amazon/pw3/apps/sources.json)
  SOURCES.md, sources.json          every copyleft file of the image and where its source is;
                                    the written offer for sources too large to attach

Checks that the sources are those of the image:
  - every AOSP project is at the revision of the resolved manifest and has no change besides
    this port's patches, which are the ones the image was built with (export build-result.json);
  - the kernel build whose modules are in the image used the kernel archive named in
    source-lock.json and this repository's kernel patches.

Run as builder inside the project VM, after export-aosp-images.py and before another build
(module-info.json describes the build in the output directory).
Usage: collect-copyleft-sources.py --export DIR --entry DIR --name NAME --tag TAG
                                   --contact TEXT --output DIR
"""
import argparse
import gzip
import hashlib
import html
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import tempfile
import urllib.request
import xml.etree.ElementTree as ET

PORT = Path(__file__).resolve().parents[1]
REPOSITORY_PATH = 'device/amazon/pw3'
KERNEL_COMPONENT = 'vendor/amazon/pw3-hardware'
KERNEL_ARCHIVE_NAME = 'linux-3.0.35-amazon-5.16.2.1.1.tar.gz'
# Licences whose terms require the source to go with the binary.
COPYLEFT = re.compile(r'General Public Licen[cs]e|Mozilla Public Licen[cs]e|Eclipse Public Licen[cs]e'
                      r'|Common Development and Distribution Licen[cs]e|European Union Public Licen[cs]e',
                      re.IGNORECASE)
NOTICE_BLOCK = re.compile(r'<div class="file-list">(.*?)</div><!-- file-list -->\s*'
                          r'<pre class="license-text">(.*?)</pre><!-- license-text -->', re.S)


class Untraceable(Exception):
    pass


def sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def copyleft_blocks(notice):
    """[(installed files, first line of the licence text)] of the copyleft blocks."""
    if notice[:2] == b'\x1f\x8b':
        notice = gzip.decompress(notice)
    text = notice.decode('utf-8', 'replace')
    blocks = NOTICE_BLOCK.findall(text)
    if not blocks:
        raise RuntimeError('No licence blocks in the notice file')
    found = []
    for files, licence in blocks:
        licence = html.unescape(licence)
        if COPYLEFT.search(licence):
            names = [html.unescape(f).strip() for f in files.split('<br/>') if f.strip()]
            first = next((line.strip() for line in licence.splitlines() if line.strip()), '')
            found.append((names, first))
    return found


def project_paths(manifest):
    root = ET.parse(manifest).getroot()
    return {p.get('path') or p.get('name'): p.get('revision') for p in root.iter('project')}


def owner(directory, projects):
    best = None
    for path in projects:
        if (directory == path or directory.startswith(path + '/')) and (best is None or len(path) > len(best)):
            best = path
    return best


def trace(blocks, modules, projects, upstreams):
    """Map each copyleft file to its source: ('aosp', project), ('kernel',) or ('repository', module)."""
    installed = {}
    for name, info in modules.items():
        for path in info.get('installed', []):
            installed.setdefault(path, []).append(name)
    sources, problems = {}, []
    for files, licence in blocks:
        for name in files:
            if name == '/kernel':
                sources[name] = {'licence': licence, 'sources': [('kernel',)]}
                continue
            found = installed.get('out/target/product/pw3' + name, [])
            if not found and name.endswith('.a'):  # a static library linked into some file
                stem = name.rsplit('/', 1)[-1][:-2]
                found = [stem] if stem in modules else []
            if not found:
                problems.append(name + ': no module installs it')
                continue
            traced = []
            for module in found:
                for directory in modules[module].get('path', []):
                    if directory == KERNEL_COMPONENT:
                        traced.append(('kernel',))
                    elif directory == REPOSITORY_PATH or directory.startswith(REPOSITORY_PATH + '/'):
                        if module not in upstreams:
                            problems.append('%s: module %s of this repository has no entry in apps/sources.json'
                                            % (name, module))
                        traced.append(('repository', module))
                    elif directory.startswith('prebuilts/'):
                        problems.append('%s: prebuilt %s (%s); its source is not in the AOSP tree'
                                        % (name, module, directory))
                    else:
                        project = owner(directory, projects)
                        if project is None:
                            problems.append('%s: %s is in no project of the manifest' % (name, directory))
                        else:
                            traced.append(('aosp', project))
            if traced:
                sources[name] = {'licence': licence, 'sources': sorted(set(traced))}
    if problems:
        raise Untraceable('\n'.join(problems))
    return sources


def load_patch_tool():
    spec = importlib.util.spec_from_file_location('pw3_patch_series', PORT / 'tools/apply-patch-series.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def patch_series():
    lines = (PORT / 'patches/projects.txt').read_text().splitlines()
    return dict(line.split() for line in lines if line.strip() and not line.startswith('#'))


def stage_project(aosp, project, revision, patches, built, stage):
    """The project at revision plus this port's patches, checked against the tree that was built.

    built: the patch ledgers of the image's projects (export build-result.json, aosp_patches)."""
    tree = aosp / project
    head = subprocess.run(['git', '-C', str(tree), 'rev-parse', 'HEAD'], check=True,
                          capture_output=True, text=True).stdout.strip()
    if head != revision:
        raise RuntimeError('%s is at %s, the manifest says %s' % (project, head, revision))
    target = stage / project
    target.mkdir(parents=True)
    archive = subprocess.run(['git', '-C', str(tree), 'archive', '--format=tar', revision],
                             check=True, capture_output=True).stdout
    with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
        tar.extractall(target)
    ledger = '.pw3-applied-patches.json'
    if patches:
        expected = [{'name': p.name, 'sha256': sha256(p)} for p in patches]
        if built.get(project) != expected:
            raise RuntimeError(project + ': the image was built with other patches than this repository has')
        load_patch_tool().apply_series(target, patches)
        (target / ledger).unlink()
    # Nothing in the built tree may differ from the staged one: no stray edit, no extra file.
    # The same guarantee as verify-tree-matches-series.py, which every export makes from the
    # files the series touches; this one has the whole project staged already, so it compares
    # all of it. Both are kept: one is what a release can afford, the other what a build can.
    status = subprocess.run(['git', '-C', str(tree), 'status', '--porcelain=v1', '-z', '--untracked-files=all'],
                            check=True, capture_output=True).stdout.decode()
    fields = status.split('\0')
    entries, index = [], 0
    while index < len(fields) and fields[index]:
        entries.append(fields[index])
        index += 2 if fields[index][0] in 'RC' else 1  # a rename or copy is followed by its old name
    for entry in entries:
        code, path = entry[:2], entry[3:]
        if code[0] in 'RC':
            raise RuntimeError('%s/%s: renamed in the built tree' % (project, path))
        if path == ledger:
            continue
        built_file, staged_file = tree / path, target / path
        if code.strip() == 'D':
            if os.path.lexists(staged_file):
                raise RuntimeError('%s/%s: deleted in the built tree only' % (project, path))
        elif not os.path.lexists(staged_file) or not same(built_file, staged_file):
            raise RuntimeError('%s/%s: the built tree differs from the revision and the patches' % (project, path))
    commit_time = int(subprocess.run(['git', '-C', str(tree), 'log', '-1', '--format=%ct', revision],
                                     check=True, capture_output=True, text=True).stdout)
    return commit_time, len([e for e in entries if e[3:] != ledger])


def same(a, b):
    if os.path.islink(a) or os.path.islink(b):
        return os.path.islink(a) and os.path.islink(b) and os.readlink(a) == os.readlink(b)
    return a.is_file() and b.is_file() and sha256(a) == sha256(b)


def write_tar(path, stage, roots):
    """A reproducible tar.xz: sorted names, owner 0, mode 644 or 755, each project's commit time."""
    with tarfile.open(path, 'w:xz', format=tarfile.PAX_FORMAT) as tar:
        for root, mtime in sorted(roots.items()):
            base = stage / root
            names = [base] + sorted(base.rglob('*'), key=lambda p: p.relative_to(base).as_posix())
            for item in names:
                info = tar.gettarinfo(str(item), arcname=item.relative_to(stage).as_posix())
                info.uid = info.gid = 0
                info.uname = info.gname = ''
                info.mtime = mtime
                if info.isdir():
                    info.mode = 0o755
                elif info.isfile():
                    info.mode = 0o755 if item.stat().st_mode & 0o111 else 0o644
                if info.isfile():
                    with item.open('rb') as stream:
                        tar.addfile(info, stream)
                else:
                    tar.addfile(info)


def fetch(item, cache):
    """An upstream archive from the cache, else downloaded; size and SHA-256 as recorded."""
    path = cache / item['file']
    if not (path.is_file() and path.stat().st_size == item['bytes'] and sha256(path) == item['sha256']):
        cache.mkdir(parents=True, exist_ok=True)
        request = urllib.request.Request(item['url'], headers={'User-Agent': 'pw3-collect-sources'})
        fd, name = tempfile.mkstemp(prefix='.fetch-', dir=cache)
        try:
            with urllib.request.urlopen(request, timeout=300) as response, os.fdopen(fd, 'wb') as out:
                shutil.copyfileobj(response, out, 1 << 20)
            if Path(name).stat().st_size != item['bytes'] or sha256(name) != item['sha256']:
                raise RuntimeError(item['url'] + ': not the recorded archive (size or SHA-256)')
            os.replace(name, path)
        finally:
            if os.path.exists(name):
                os.unlink(name)
    return path


def check_kernel(export_dir, entry_dir, kernel_build, archive):
    """That the sources about to be published are the ones every shipped kernel binary came from.

    The binding is the binaries themselves. It used to be the hash of the
    kernel build's receipt against the one the export recorded, which is a hash
    of a container and not of anything that runs: build-result.json carries
    compiled_at_utc, so a kernel rebuilt from the same sources, producing a
    byte-identical zImage and byte-identical modules, has a different receipt
    and was refused. (That is how this step came to be refused on V74 -- the
    kernel had been rebuilt, nothing in it had changed, and the tool said the
    image was built with another kernel.)

    What ships is three things, and each is tied to this build: the zImage and
    the display module, which the entry's own receipt names, and the modules in
    the system image. Tying only the image's modules would leave the kernel
    itself unbound -- a change inside the kernel, with those modules unchanged,
    would pass. The build's own artifacts are checked against its receipt
    first, so that the files being compared are the ones the receipt describes.
    """
    lock = json.loads((PORT / 'source-lock.json').read_text())
    if sha256(archive) != lock['kernel']['member_sha256']:
        raise RuntimeError('The kernel archive is not the one in source-lock.json')
    receipt = kernel_build / 'build-result.json'
    built = json.loads(receipt.read_text())
    for name in ('zImage', 'modules.tar.gz'):
        recorded, file = built['artifacts'][name], kernel_build / name
        if file.stat().st_size != recorded['bytes'] or sha256(file) != recorded['sha256']:
            raise RuntimeError('%s is not the one %s describes' % (file, receipt))
    from_build = {}
    with tarfile.open(kernel_build / 'modules.tar.gz') as modules:
        for member in modules.getmembers():
            if not (member.isfile() and member.name.endswith('.ko')):
                continue
            # Matched by name below, so two members of the same name would let
            # one of them answer for the other.
            name = Path(member.name).name
            if name in from_build:
                raise RuntimeError('two modules named %s in %s' % (name, kernel_build / 'modules.tar.gz'))
            from_build[name] = hashlib.sha256(modules.extractfile(member).read()).hexdigest()
    entry = json.loads((entry_dir / 'build-result.json').read_text())
    if entry['kernel_sha256'] != built['artifacts']['zImage']['sha256']:
        raise RuntimeError('The entry in %s carries another kernel than this build made' % entry_dir)
    if entry['epdc_module_sha256'] not in set(from_build.values()):
        raise RuntimeError('The display module in %s is not one this kernel build made' % entry_dir)
    inspection = json.loads((export_dir / 'filesystem-inspection.json').read_text())
    in_image = {entry['path']: entry['sha256'] for entry in inspection['entries']
                if entry['path'].endswith('.ko')}
    if not in_image:
        raise RuntimeError('The image has no kernel modules; ' +
                           str(export_dir / 'filesystem-inspection.json') + ' lists none')
    for path, digest in sorted(in_image.items()):
        name = Path(path).name
        if name not in from_build:
            raise RuntimeError('%s is in the image and not in %s' % (path, kernel_build / 'modules.tar.gz'))
        if from_build[name] != digest:
            raise RuntimeError('%s in the image is not the one this kernel build made' % path)
    if built['source_lock']['kernel']['member_sha256'] != lock['kernel']['member_sha256']:
        raise RuntimeError('The kernel build used another kernel archive')
    patches = {str(p.relative_to(PORT)): sha256(p)
               for directory in (PORT / 'patches', PORT / 'patches/android-core')
               for p in sorted(directory.glob('*.patch'))}
    if built['patches'] != patches:
        raise RuntimeError('The kernel was built with other patches than this repository has')
    return len(patches)


def offer_text(name, contact, offers):
    lines = []
    for item in offers:
        lines += [
            '### %s' % item['what'], '',
            'For at least three years from the date of this release, and for as long as this release is offered,',
            'anyone can ask for a complete machine-readable copy of the corresponding source code of %s,'
            % item['what'],
            'which is part of %s (%s). It will be provided for no more than the cost of making the copy,'
            % (name, item['licence']),
            'as a download or, on request, on a physical medium. Ask through: %s.' % contact, '',
            'The same source is published by its authors: %s (%s bytes, MD5 %s).'
            % (item['url'], format(item['bytes'], ','), item['md5']),
            'It is not attached to the release: %s' % item['reason'], '',
        ]
    return lines


def write_report(output, args, sources, projects, revisions, upstream_files, offers, kernel_patches):
    files = {name: {'licence': v['licence'], 'sources': [list(s) for s in v['sources']]}
             for name, v in sorted(sources.items())}
    report = {
        'schema': 1, 'release': args.name, 'repository_tag': args.tag,
        'aosp_projects': {p: revisions[p] for p in sorted(projects)},
        'kernel': {'archive': KERNEL_ARCHIVE_NAME, 'patches_in_repository': kernel_patches},
        'upstream_archives': upstream_files, 'offers': offers, 'files': files,
        'assets': {p.name: {'bytes': p.stat().st_size, 'sha256': sha256(p)}
                   for p in sorted(output.iterdir()) if p.suffix in ('.gz', '.xz')},
    }
    (output / 'sources.json').write_text(json.dumps(report, indent=2, ensure_ascii=False) + '\n')
    by_source = {}
    for name, v in sorted(sources.items()):
        for s in v['sources']:
            by_source.setdefault(s, []).append(name)
    aosp = '%s-aosp-sources.tar.xz' % args.name
    lines = [
        '# Sources of %s' % args.name, '',
        '%s 系统镜像里按 GPL、LGPL 等许可发布的每个文件，以及它的源码在哪里。中文说明见文末。' % args.name, '',
        'Every file of the %s system image under a licence that requires its source (GPL, LGPL, MPL; '
        'read from the image\'s /etc/NOTICE.html.gz), and where that source is.' % args.name, '',
        '| Source | Files in the image | Where |', '|---|---|---|',
    ]
    for source, names in sorted(by_source.items()):
        shown = ', '.join('`%s`' % n for n in names[:6]) + (', ... (%d files)' % len(names) if len(names) > 6 else '')
        if source[0] == 'aosp':
            where = '`%s` (revision `%s`)' % (aosp, revisions[source[1]][:12])
            label = 'AOSP `%s`' % source[1]
        elif source[0] == 'kernel':
            where = '`%s` + `patches/`, `config/android-core.config`, `tools/build-kernel.sh` at tag `%s`' % (
                KERNEL_ARCHIVE_NAME, args.tag)
            label = 'Linux 3.0.35 (Lab126)'
        else:
            module = source[1]
            entry = args.upstreams[module]
            parts = ['this repository at tag `%s`' % args.tag]
            parts += ['`%s`' % a['file'] for a in entry.get('attach', [])]
            parts += ['written offer below (%s)' % o['what'] for o in entry.get('offer', [])]
            where = '; '.join(parts)
            label = 'module `%s`' % module
        lines.append('| %s | %s | %s |' % (label, shown, where))
    lines += ['', 'The AOSP projects are at the revisions of `config/aosp-8.1.0_r81-resolved.xml`, '
              'with the patches of `patches/` applied as listed in `patches/projects.txt`. '
              'The whole AOSP tree of the build is available from https://android.googlesource.com '
              'at those revisions; the build instructions are in this repository\'s README.', '',
              'The installer of the release carries, besides this image, the same kernel and the handoff module '
              '`pw3_first_kernel.ko` (`boot/handoff/`, GPL-2.0-only, built by `boot/build_module.py` against '
              'the same kernel source); its other programs are this repository\'s, linked with musl and zstd '
              '(MIT, BSD-3-Clause; versions in `source-lock.json`).', '']
    if offers:
        lines += ['## Written offer', ''] + offer_text(args.name, args.contact, offers)
    lines += ['## 中文说明', '',
              '- `%s`：上表中 AOSP 项目的源码，按 manifest 的版本，打上本项目补丁，就是构建镜像时用的源码。' % aosp,
              '- `%s`：亚马逊发布的内核源码原样；本项目的内核补丁、配置和构建脚本在本仓库标签 `%s` 下。' % (
                  KERNEL_ARCHIVE_NAME, args.tag),
              '- 其余压缩包：镜像里第三方应用的上游源码（`device/amazon/pw3/apps/sources.json`）。',
              '- 安装程序里另有同一个内核和交接模块 `pw3_first_kernel.ko`（`boot/handoff/`，GPL-2.0-only），'
              '源码同上；其余程序都是本仓库的代码，链接了 musl 和 zstd。']
    if offers:
        lines.append('- 太大、无法附在发行页上的源码（%s）：自本版本发布起至少三年内，可通过 %s 索取，'
                     '只收复制成本；它的作者也在上面的地址公开发布同一份源码。'
                     % ('、'.join(o['what'] for o in offers), args.contact))
    (output / 'SOURCES.md').write_text('\n'.join(lines) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--export', required=True, type=Path, help='directory written by export-aosp-images.py')
    parser.add_argument('--entry', required=True, type=Path,
                        help='directory written by boot/build_entry.py, for the kernel that ships in it')
    parser.add_argument('--name', required=True, help='release name, the prefix of the archive names')
    parser.add_argument('--tag', required=True, help='tag of this repository the release is built from')
    parser.add_argument('--contact', required=True, help='where to ask for sources (written offer)')
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--aosp', type=Path, default=Path('/home/builder/android-src'))
    parser.add_argument('--manifest', type=Path, default=PORT / 'config/aosp-8.1.0_r81-resolved.xml')
    parser.add_argument('--kernel-build', type=Path, default=Path('/mnt/build-share/android-core-kernel'))
    parser.add_argument('--kernel-archive', type=Path,
                        default=Path('/mnt/kernel-inputs/amazon-5.16.2.1.1/linux-3.0.35.tar.gz'))
    parser.add_argument('--cache', type=Path, default=Path('/mnt/build-share/upstream-sources'))
    parser.add_argument('--notice', type=Path, help='notice file to use instead of the image\'s (tests)')
    args = parser.parse_args()
    output = args.output.resolve()
    if output.exists():
        raise SystemExit('Refusing to overwrite ' + str(output))
    export = json.loads((args.export / 'build-result.json').read_text())
    if args.notice:
        notice = args.notice.read_bytes()
    else:
        notice = subprocess.run(['debugfs', '-R', 'cat /etc/NOTICE.html.gz', str(args.export / 'system.raw.img')],
                                check=True, capture_output=True).stdout
    blocks = copyleft_blocks(notice)
    # module-info.json is the only thing that says which module made each file,
    # and the build writes it only when it is asked to. One written before the
    # image is about a different build: on 2026-10-19 it still named the USB
    # gadget module g_ffs.ko, which had been renamed g_android.ko two days
    # earlier, so the file in the image belonged to no module and a rehearsal
    # stopped on it. Being told to run it first was not enough; it is checked.
    module_info = args.aosp / 'out/target/product/pw3/module-info.json'
    if not module_info.exists():
        raise SystemExit('No %s: run "make module-info.json" in the build.' % module_info)
    # Against the image in the output tree, and by content, not against the
    # export's copy of it: a copy's mtime is when it was copied, which is always
    # after the build wrote module-info.json, so comparing with the copy refused
    # every build (and it refused this one, after build-aosp-system.sh started
    # making the file itself). What has to be true is that the output tree still
    # holds the image that was exported, and that module-info.json is not older
    # than it.
    produced = args.aosp / 'out/target/product/pw3/system.img'
    exported = json.loads((args.export / 'build-result.json').read_text())
    if not produced.exists() or sha256(produced) != exported['files']['system.img']['sha256']:
        raise SystemExit(
            'the output tree no longer holds the image this export was made from, so\n'
            'module-info.json there is about a different build. Nothing written.')
    if module_info.stat().st_mtime < produced.stat().st_mtime:
        raise SystemExit(
            'module-info.json is older than the image it would describe, so it is about an\n'
            'earlier build. Build again (build-aosp-system.sh makes it after the images),\n'
            'then run this again. Nothing written.')
    modules = json.loads(module_info.read_text())
    revisions = project_paths(args.manifest)
    args.upstreams = json.loads((PORT / REPOSITORY_PATH / 'apps/sources.json').read_text())['modules']
    try:
        sources = trace(blocks, modules, revisions, args.upstreams)
    except Untraceable as problem:
        raise SystemExit('Copyleft files without a known source; nothing written:\n' + str(problem))
    kernel_patches = check_kernel(args.export, args.entry, args.kernel_build, args.kernel_archive)
    projects = sorted({s[1] for v in sources.values() for s in v['sources'] if s[0] == 'aosp'})
    modules_used = sorted({s[1] for v in sources.values() for s in v['sources'] if s[0] == 'repository'})
    series = patch_series()
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.pw3-sources-', dir=output.parent) as temp:
        stage, result = Path(temp) / 'stage', Path(temp) / 'result'
        stage.mkdir()
        result.mkdir()
        roots = {}
        for project in projects:
            patches = sorted((PORT / 'patches' / series[project]).glob('*.patch')) if project in series else []
            roots[project], changed = stage_project(args.aosp, project, revisions[project], patches,
                                                    export['aosp_patches'], stage)
            print('%-24s %s  %d changed by patches' % (project, revisions[project][:12], changed), flush=True)
        write_tar(result / ('%s-aosp-sources.tar.xz' % args.name), stage, roots)
        shutil.copyfile(args.kernel_archive, result / KERNEL_ARCHIVE_NAME)
        upstream_files, offers = [], []
        for module in modules_used:
            entry = args.upstreams[module]
            for item in entry.get('attach', []):
                if item['file'] not in [u['file'] for u in upstream_files]:
                    shutil.copyfile(fetch(item, args.cache), result / item['file'])
                    upstream_files.append(item)
            for item in entry.get('offer', []):
                if item not in offers:
                    offers.append(item)
        write_report(result, args, sources, projects, revisions, upstream_files, offers, kernel_patches)
        result.rename(output)
    print('Written: ' + str(output))


if __name__ == '__main__':
    main()
