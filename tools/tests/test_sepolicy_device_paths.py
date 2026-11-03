#!/usr/bin/env python3
"""Every /dev path a PW3 program names must be one its domain is allowed to see.

On 2026-10-21 V65 installed and then refused to start. The reason, in the kernel
log:

    pw3-init-check path="/dev/block/dm-3" dev=tmpfs ino=973
      scontext=u:r:pw3_init_check:s0
      tcontext=u:object_r:pw3_report_block_device:s0
      tclass=blk_file permissive=0

The report area's device is labelled apart from the two image devices, so that
only `pw3_boot_report` may write it (`file_contexts`). `pw3_init_check` holds
`allow ... dm_device:blk_file getattr`, which covers dm-0 and dm-1 -- dm-2 carries
`userdata_block_device` in the same file -- but not that separate label -- and InitCheck.cpp stat()s dm-3. In enforcing mode the
stat failed, a long `&&` chain went false, and the start was refused with a
message about extents that had nothing to do with the real cause.

Nothing caught it before the device did, and that is the part worth fixing: a
missing rule on a path only reached once the Android side is up is invisible to
every test here, because no test boots that far. This one does not boot
anything. It reads the paths out of the programs, resolves each through
file_contexts the way the kernel would, and asks whether the domain has a rule
for the label that comes back. That is a static question with a static answer.

The first version of this checked the named path and nothing else, and the
device found what it had missed within minutes of V66 starting:

    pw3-boot-report name="block" scontext=u:r:pw3_boot_report:s0
      tcontext=u:object_r:block_device:s0 tclass=dir permissive=0

Opening /dev/block/dm-3 walks /dev/block first, and that domain had a rule for
the node and none for the directory. The report area had therefore never been
written on any start, and the service still exited 0, because a denied stat and
an installation with no area read the same way.

So every parent directory is now checked too, for `search` and by name. That one
permission is the one worth checking automatically: every named path needs it
regardless of what the program then does with the path, so it needs no guess
about the call. What the program needs on the object itself depends on the call,
and that is derived from the call too -- `access()` by its mode (F_OK asks for
nothing on the final inode, R_OK for read), `stat` for getattr, `open` for open
plus read or write by its flags, an ifstream for open+read.

An adversarial review on 2026-10-21 found three mistakes in the first version of
all this, each of which made it pass on a real regression, and they are the
reason the file reads as it does now:

  * `r_dir_file(d, t)` was expanded to `blk_file` and `chr_file` as well as the
    classes it actually grants. Permissions a macro does not grant are not
    granted, and inventing them meant that replacing the V65 rule with that
    macro reproduced the V65 denial and passed.
  * every `access()` was treated as `F_OK`, so changing one to `R_OK` added a
    requirement nothing noticed.
  * only the call's first line was read, so `open("/dev/x",` with its flags on
    the next line looked like a read-only open.

What it still does not check: `/sys`. That cannot be done this way -- genfscon
uses the real device paths inside sysfs (/devices/platform/imx-i2c.0/...) while
these programs read /sys/class/... symlinks, and that correspondence is a fact
only the device has. Guessing it would make a test that passes for the wrong
reason. Paths built at run time cannot be read either, so the accesses they lead
to are written down in REVIEWED instead, with where each was read from, and a
program that builds a path without such an entry fails here.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
POLICY = ROOT / 'device/amazon/pw3/sepolicy'

# The programs that run in a domain of their own, and where their source is.
# A domain with no entry here is not checked, so adding a program means adding
# a line -- which is the point: the list is what somebody has looked at.
DOMAINS = {
    'pw3_init_check': ['device/amazon/pw3/initcheck/InitCheck.cpp'],
    'pw3_boot_report': ['device/amazon/pw3/initcheck/BootReport.cpp'],
    'pw3_system_verify': ['device/amazon/pw3/verify/SystemVerify.cpp'],
    'pw3_fg_state': ['device/amazon/pw3/fuelgauge/FgState.cpp',
                     'device/amazon/pw3/fuelgauge/main.cpp'],
}
# Paths every domain reaches through libc or the runtime rather than by name,
# and which the platform policy already covers for coredomain.
IGNORED = {'/dev/null', '/dev/zero', '/dev/urandom', '/dev/random', '/dev/ptmx'}

# The platform's own labelling, for the paths these programs name that this
# device's file_contexts does not label. Quoted from the AOSP tree this project
# builds (system/sepolicy/private/file_contexts, line numbers of 8.1.0_r81) so
# that the label is read off the policy that ships and not off memory. A /dev
# path that neither table labels is a failure, not a path to skip: before
# 2026-10-21 they were skipped, and that silently excused every rule this
# project holds for a platform-labelled node -- deleting
# `allow pw3_init_check dm_device:blk_file getattr` broke nothing here.
PLATFORM_CONTEXTS = (
    ('/dev/block/dm-[0-9]+', 'dm_device', 'private/file_contexts:72'),
    ('/dev/device-mapper', 'dm_device', 'private/file_contexts:82'),
    ('/dev/graphics(/.*)?', 'graphics_device', 'private/file_contexts:88'),
    ('/dev/kmsg', 'kmsg_device', 'private/file_contexts:108'),
    ('/dev/block(/.*)?', 'block_device', 'private/file_contexts:71'),
    ('/dev/ashmem', 'ashmem_device', 'private/file_contexts:68'),
    ('/dev/binder', 'binder_device', 'private/file_contexts:70'),
    ('/dev/hwbinder', 'hwbinder_device', 'private/file_contexts:90'),
    ('/dev/vndbinder', 'vndbinder_device', 'private/file_contexts:169'),
    # Not file_contexts: this is a mounted filesystem, labelled whole by
    # `genfscon functionfs / u:object_r:functionfs:s0`
    # (private/genfs_contexts:120). Its mount point and everything under it
    # therefore carry that one type once init.pw3.usb.rc has mounted it.
    ('/dev/usb-ffs/adb(/.*)?', 'functionfs', 'private/genfs_contexts:120'),
    # /dev and /dev/usb-ffs are not in file_contexts: init mounts /dev as tmpfs
    # with this context and mkdir inherits it, which is also what the V66 denial
    # reported for the parent it was refused (dev=tmpfs).
    ('/dev/usb-ffs', 'device', 'init.pw3.usb.rc mkdir, on the /dev tmpfs'),
    ('/dev', 'device', 'system/core/rootdir/init.rc, mount tmpfs /dev'),
)

# Labels the platform already grants every domain `dir search` on, so that a
# rule here would be redundant rather than missing.
BASE_DIR_SEARCH = {
    'device': 'public/domain.te:61  allow domain device:dir search;',
}

# Paths under /dev that are not device nodes. The blk_file/chr_file guess below
# is right for the nodes init creates and wrong for a mounted filesystem's own
# root and the files in it, so those are named here instead of guessed. This
# table says only what the object *is*. Whether a rule is needed for it is
# worked out from the calls that reach it, which is the part that must not be
# annotated: an annotation reading "this one needs nothing" goes on being true
# after somebody changes the call.
PATH_CLASSES = (
    ('/dev/usb-ffs/adb', 'dir', 'the FunctionFS mount point'),
    ('/dev/usb-ffs/adb/ep0', 'file', 'the control endpoint, a file in that filesystem'),
)

# Permission sets, quoted from system/sepolicy/public/global_macros of the tree
# this project builds. Only the ones these .te files use; an unknown macro in a
# rule is a failure below rather than an empty set, because an empty set would
# make every permission check pass.
MACRO_PERMS = {
    'r_file_perms': {'getattr', 'open', 'read', 'ioctl', 'lock', 'map'},
    'w_file_perms': {'open', 'append', 'write', 'lock', 'map'},
    'r_dir_perms': {'open', 'getattr', 'read', 'search', 'ioctl', 'lock'},
    'w_dir_perms': {'open', 'search', 'write', 'add_name', 'remove_name', 'lock'},
}
MACRO_PERMS['rw_file_perms'] = MACRO_PERMS['r_file_perms'] | MACRO_PERMS['w_file_perms']
MACRO_PERMS['ra_file_perms'] = MACRO_PERMS['r_file_perms'] | {'append'}
MACRO_PERMS['rw_dir_perms'] = MACRO_PERMS['r_dir_perms'] | MACRO_PERMS['w_dir_perms']
# Every domain this port declares for itself. The other .te files here augment
# domains AOSP already defines (hal_light, surfaceflinger, init, healthd, ...),
# whose base rules live in the AOSP tree and not here; this test deliberately
# makes no claim about those, because "no rule in this directory" is not
# "denied on the device" for them.
OWN_DOMAINS = sorted(
    name for source in POLICY.glob('*.te')
    for name in re.findall(r'(?m)^type\s+([\w]+)\s*,\s*domain\s*;',
                           source.read_text(encoding='utf-8')))
failures = []


def check(ok, message):
    if not ok:
        failures.append(message)


def contexts():
    """file_contexts as (pattern, label), longest literal prefix first.

    The kernel picks the most specific match; for the exact paths these programs
    name, matching the literal entries first and the prefixed ones after is the
    same answer and needs no regex engine for the common case.
    """
    out = []
    for line in (POLICY / 'file_contexts').read_text(encoding='utf-8').splitlines():
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) < 2:
            continue
        pattern, label = parts[0], parts[-1]
        out.append((pattern, label.split(':')[2] if label.count(':') >= 2 else label))
    # More specific (longer) patterns win, as in the real matcher.
    return sorted(out, key=lambda item: -len(item[0]))


def label_of(path, table):
    """The label the kernel would give this path, device table before platform."""
    for pattern, label in list(table) + [(p, l) for p, l, _ in PLATFORM_CONTEXTS]:
        try:
            if re.fullmatch(pattern, path):
                return label
        except re.error:
            if pattern == path:
                return label
    return None


def rules_for(domain):
    """(type, class) -> the permissions this policy's allow rules give the domain.

    Set braces are expanded in all three positions, and a permission that names
    one of the macros above is expanded to its set. An unrecognised macro adds
    the literal token, so a check for `search` against it fails rather than
    passing on an empty set.
    """
    granted = {}
    names = lambda part: re.findall(r'[\w$-]+', part.replace('{', ' ').replace('}', ' '))

    def give(target, klass, perms):
        granted.setdefault((target, klass), set()).update(perms)

    for source in POLICY.glob('*.te'):
        text = re.sub(r'#[^\n]*', '', source.read_text(encoding='utf-8'))
        # (?<![\w-]) because `allow` is a keyword and not a substring: without
        # it this matched inside `neverallow`, `dontaudit ... allow` and
        # `allowxperm`, so a rule turned into a *prohibition* was still counted as
        # granting the permission. Confirmed on 2026-10-21 by changing one of this
        # port's allow rules to `neverallow` and watching the check pass.
        for match in re.finditer(r'(?<![\w-])allow\s+([^:;]+?)\s+([^:;]+?):\s*([\w{}\s]+?)\s+([^;]*);', text):
            sources, targets, classes, perms = (part.strip() for part in match.groups())
            if domain not in names(sources):
                continue
            expanded = set()
            for perm in names(perms):
                expanded |= MACRO_PERMS.get(perm, {perm})
            for target in names(targets):
                for klass in names(classes):
                    give(target, klass, expanded)
        # r_dir_file(d, t) is exactly `allow d t:dir r_dir_perms; allow d
        # t:{file lnk_file} r_file_perms;` (public/te_macros:68, read in the
        # tree this project builds). The first version of this also handed the
        # macro `blk_file` and `chr_file`, on the reasoning that a device node
        # of that type is reached the same way. That reasoning was wrong and the
        # consequence was a test that passed on a real regression: replacing
        # `allow pw3_init_check pw3_report_block_device:blk_file getattr` with
        # `r_dir_file(pw3_init_check, pw3_report_block_device)` reproduced the
        # V65 denial and this file said nothing. Found in an adversarial review
        # on 2026-10-21. Permissions a macro does not grant are not granted.
        for match in re.finditer(r'\b(r_dir_file|rw_dir_file)\s*\(\s*([\w$]+)\s*,\s*([\w$]+)\s*\)', text):
            macro, who, what = match.groups()
            if who != domain:
                continue
            give(what, 'dir', MACRO_PERMS['r_dir_perms' if macro == 'r_dir_file' else 'rw_dir_perms'])
            file_perms = MACRO_PERMS['r_file_perms' if macro == 'r_dir_file' else 'rw_file_perms']
            for klass in ('file', 'lnk_file'):
                give(what, klass, file_perms)
    return granted


def xperms_for(domain):
    """(type, class) -> the ioctl commands allowxperm gives this domain."""
    granted = {}
    for source in POLICY.glob('*.te'):
        text = re.sub(r'#[^\n]*', '', source.read_text(encoding='utf-8'))
        for match in re.finditer(
                r'allowxperm\s+([\w{}\s]+?)\s+([\w{}\s]+?):\s*([\w{}\s]+?)\s+ioctl\s*\{([^}]*)\}', text):
            sources, targets, classes, commands = match.groups()
            names = lambda part: re.findall(r'[\w$-]+', part.replace('{', ' ').replace('}', ' '))
            if domain not in names(sources):
                continue
            numbers = {int(value, 16) for value in re.findall(r'0x[0-9a-fA-F]+', commands)}
            for target in names(targets):
                for klass in names(classes):
                    granted.setdefault((target, klass), set()).update(numbers)
    return granted


def parents(path):
    """Every directory the kernel walks to reach this path, outermost first."""
    parts = path.strip('/').split('/')
    return ['/' + '/'.join(parts[:i]) for i in range(1, len(parts))]


# What the kernel checks on the object itself, by the call that reaches it. Each
# one was read in the kernel this project compiles, not recalled:
#
#   access(p, F_OK)  nothing. selinux_inode_permission masks the request down to
#                    MAY_READ|WRITE|EXEC|APPEND and returns 0 when nothing is
#                    left -- "No permission to check.  Existence test."
#                    (security/selinux/hooks.c). Only `search` on the way in,
#                    which the parent walk above covers.
#   statfs(p)        nothing on the object: superblock_has_perm with
#                    FILESYSTEM__GETATTR (selinux_sb_statfs), and
#                    `allow domain fs_type:filesystem getattr`
#                    (public/domain.te:195) gives every domain that already.
#   stat/lstat(p)    getattr.
#   open(p, flags)   open, plus read or write as the flags on that line say.
#   opendir(p)       the dir class: open, read, search.
#   Read(p)          this project's ifstream helper: the file class, open+read.
#
# A call this cannot name is a failure, not a pass: the whole point is that
# nobody has to remember which permission a line needs.
CALL_PERMS = {
    # access() is the mode's business, not the call's: F_OK needs nothing, and
    # R_OK/W_OK/X_OK do ask for read/write/execute. needed() reads the mode off
    # the line. Treating every access() as F_OK was a hole found in review on
    # 2026-10-21: changing F_OK to R_OK on the FunctionFS endpoint adds a real
    # requirement for `functionfs:file read`, which this policy does not grant.
    'access': (None, None),
    'statfs': (None, frozenset()),
    'stat': (None, frozenset({'getattr'})),
    'lstat': (None, frozenset({'getattr'})),
    'opendir': ('dir', frozenset({'open', 'read', 'search'})),
    'Read': ('file', frozenset({'open', 'read'})),
}
ACCESS_MODES = {'F_OK': frozenset(), 'R_OK': frozenset({'read'}),
                'W_OK': frozenset({'write'}), 'X_OK': frozenset({'execute'})}

# InitCheck.cpp calls the system calls through wrappers that record the first
# failure (the chains there fold a failed call and a value that does not match
# into the same false, and the message describes the value). A wrapper behaves
# exactly as the call it wraps -- same arguments, same return, same errno -- so
# it asks for exactly the same permissions. Spelling them out here rather than
# matching any capitalised name keeps this a list somebody looked at.
WRAPPERS = {'Stat': 'stat', 'LStat': 'lstat', 'Access': 'access',
            'StatFs': 'statfs', 'Open': 'open'}
for _wrapper, _real in WRAPPERS.items():
    if _real in CALL_PERMS:
        CALL_PERMS[_wrapper] = CALL_PERMS[_real]

# What reading the sources cannot see, read by hand instead, with where it was
# read from. Two kinds of thing end up here and nothing else should:
#
#   paths   built at run time, so no literal names them -- SystemVerify.cpp:100
#           opens a path that came out of the record the start wrote.
#   ioctls  commands, which are names in the source and numbers in the policy.
#
# These are declarations, not exemptions: each one is checked against the policy
# exactly like an extracted access, so a rule that goes missing fails here. And
# every domain in DOMAINS must appear, even with nothing in it, so that adding a
# domain makes somebody look at both lists.
REVIEWED = {
    'pw3_init_check': {
        'paths': [],
        'ioctls': [
            ('dm_device', 'chr_file', [0xfd07, 0xfd0c],
             'DM_DEV_STATUS and DM_TABLE_STATUS, asked through /dev/device-mapper '
             '(preinit/TestFatMapper.h, preinit/TestFileMapper.h)'),
            ('graphics_device', 'chr_file', [0x4600, 0x4680],
             'FBIOGET_VSCREENINFO and the EPDC query on fb0 (epdc/EpdcContract.h)'),
        ],
    },
    'pw3_boot_report': {
        'paths': [],
        'ioctls': [
            ('graphics_device', 'chr_file', [0x4600, 0x4602, 0x4680],
             'the fb0 queries BootReport.cpp makes for the panel summary'),
        ],
    },
    'pw3_system_verify': {
        'paths': [
            ('dm_device', 'blk_file', {'open', 'read', 'getattr', 'ioctl'},
             'SystemVerify.cpp:100 opens record.device, which the start wrote as '
             '"/dev/block/dm-1" (preinit/TestFileInit.h, block_node("dm-1", ...))'),
            ('block_device', 'dir', {'search'},
             'the directory that same open walks -- missing until 2026-10-21, which '
             'is why the background SHA256 had never run'),
            ('pw3_system_stamp_file', 'dir', {'open', 'read'},
             'SystemVerify.cpp:190 opens /data/pw3 to fsync it after unlinking the '
             'record, so that the deletion survives a power loss'),
        ],
        # BLKGETSIZE64 and BLKROGET go to a blk_file, and this policy has no
        # extended permission rule for that class; with no xperm rule the
        # decision is the ordinary one (patches/android-core/0002 xperms
        # backport), so `ioctl` in the allow rule is the whole requirement.
        'ioctls': [],
    },
    'pw3_fg_state': {
        'paths': [
            ('sysfs_pw3_fg_snapshot', 'file', {'open', 'read'},
             'FgState.cpp reads the gauge snapshot, whose sysfs path genfs_contexts '
             'labels at /devices/system/wario_battery/wario_battery0/fg_android_snapshot'),
        ],
        'ioctls': [],
    },
}


NAMES = '|'.join(sorted(set(list(CALL_PERMS) + ['open'] + list(WRAPPERS)), key=len, reverse=True))


def calls_for(path, lines):
    """Every call that reaches `path`, and every mention it cannot account for.

    Returns ([(name, call text)], [unexplained lines], [lines that only hold a
    prefix something is concatenated onto]). Five shapes
    account for the mentions in these programs, and a fourth mention is reported
    rather than assumed harmless:

      f("/dev/x", ...)            the call names the path.
      NAME = "/dev/x"; f(NAME)    a named constant, used elsewhere.
      m.source != "/dev/x"        compared, not opened: the path came out of
                                  /proc/self/mountinfo and nothing touches it.
      "/dev/x/" + name            a prefix: the path is built at run time, so
                                  the access it leads to has to be in REVIEWED.
      for (const char* p : {"/dev/x", ...}) { f(p, ...) }
    """
    found, unexplained, aliases, built = [], [], set(), []
    quoted = re.escape(f'"{path}"')
    for index, line in enumerate(lines):
        if f'"{path}"' not in line:
            continue
        direct = re.search(r'\b(%s)\s*\(\s*%s' % (NAMES, quoted), line)
        if direct:
            found.append((direct.group(1), call_text(lines, index, direct.start())))
            continue
        # Compared against a value, so no access to this path happens here.
        if re.search(r'(?:==|!=)\s*%s' % quoted, line) or re.search(r'%s\s*(?:==|!=)' % quoted, line):
            continue
        alias = re.search(r'\b(\w+)\s*=\s*%s\s*;' % quoted, line)
        if alias:
            aliases.add(alias.group(1))
            continue
        # A prefix the program concatenates something onto: the path does not
        # exist in the source, so nothing here can say what it is. The access it
        # leads to belongs in REVIEWED, which the caller insists on.
        if re.search(r'%s\s*\+' % quoted, line) or re.search(r'\+\s*%s' % quoted, line):
            built.append(index + 1)
            continue
        # for (const char* p : {...}) with the call in the body.
        loop = None
        for back in range(index, max(index - 8, -1), -1):
            loop = re.search(r'for\s*\(\s*const\s+char\s*\*\s*(\w+)\s*:\s*\{', lines[back])
            if loop:
                break
        if loop:
            variable = loop.group(1)
            for ahead in range(back, min(back + 10, len(lines))):
                call = re.search(r'\b(%s)\s*\(\s*%s\s*[,)]' % (NAMES, re.escape(variable)), lines[ahead])
                if call:
                    found.append((call.group(1), call_text(lines, ahead, call.start())))
                    break
            else:
                unexplained.append(index + 1)
            continue
        unexplained.append(index + 1)
    for name in aliases:
        uses = [(match.group(1), call_text(lines, i, match.start()))
                for i, line in enumerate(lines)
                for match in [re.search(r'\b(%s)\s*\(\s*%s\s*[,)]' % (NAMES, re.escape(name)), line)]
                if match]
        check(uses, f'{path} is bound to {name} and nothing calls on {name}; '
                    f'if the constant is dead, delete it, and if it is reached some other way, '
                    f'teach calls_for() that shape')
        found += uses
    return found, unexplained, built


def needed(name, text):
    """(class override or None, permissions) for a call, from its whole text.

    `text` is the call from its name to its closing parenthesis, continuation
    lines included: `open("/dev/x",` on one line and `O_RDWR | O_CLOEXEC)` on
    the next is one call with one set of flags, and reading only the first line
    of it saw no flags and assumed read-only. Found in review on 2026-10-21.
    """
    name = WRAPPERS.get(name, name)
    if name == 'open':
        perms = {'open'}
        if 'O_WRONLY' in text:
            perms.add('write')
        elif 'O_RDWR' in text:
            perms |= {'read', 'write'}
        else:                                # O_RDONLY, explicit or by omission
            perms.add('read')
        return None, frozenset(perms)
    if name == 'access':
        modes = [mode for mode in ACCESS_MODES if re.search(r'\b%s\b' % mode, text)]
        if not modes:
            return None, None                # unreadable mode: a failure above
        perms = frozenset().union(*(ACCESS_MODES[mode] for mode in modes))
        return None, perms
    return CALL_PERMS[name]


def call_text(lines, index, after):
    """A call starting at lines[index][after:], up to its closing parenthesis."""
    collected, depth, started = [], 0, False
    for line in lines[index:index + 6]:
        piece = line[after:] if line is lines[index] else line
        after = 0
        for character in piece:
            collected.append(character)
            if character == '(':
                depth += 1
                started = True
            elif character == ')':
                depth -= 1
                if started and depth <= 0:
                    return ''.join(collected)
        collected.append(' ')
    return ''.join(collected)


table = contexts()
checked = 0
reached = 0
reviewed = 0
for domain, sources in DOMAINS.items():
    policy_file = POLICY / f'{domain}.te'
    check(policy_file.is_file(), f'{domain}: no {policy_file.name}')
    if not policy_file.is_file():
        continue
    granted = rules_for(domain)
    # The hand-read list, checked like anything else.
    check(domain in REVIEWED,
          f'{domain} is in DOMAINS and not in REVIEWED. Reading the source cannot see a path '
          f'built at run time or an ioctl command, so somebody has to look; put the entry in '
          f'(an empty one is an answer, as long as it was looked at).')
    entry = REVIEWED.get(domain, {'paths': [], 'ioctls': []})
    for target, klass, perms, why in entry['paths']:
        missing = perms - granted.get((target, klass), set())
        reviewed += 1
        check(not missing,
              f'{domain} needs {sorted(perms)} on {target}:{klass} and the policy is missing '
              f'{sorted(missing)}. This is not read out of the source -- {why}.')
    for target, klass, commands, why in entry['ioctls']:
        allowed = xperms_for(domain).get((target, klass), set())
        absent = [hex(command) for command in commands if command not in allowed]
        reviewed += 1
        check(not absent,
              f'{domain} makes ioctls {absent} on {target}:{klass} and no allowxperm lists '
              f'them. With an extended permission rule present for that class, a command not '
              f'in it is denied. {why}.')
        # An extended permission rule decides *which* commands; the ordinary
        # rule decides whether ioctl is allowed at all, and an xperm without it
        # is dead. Checking only the xperm list missed exactly that: removing
        # `ioctl` from `allow pw3_init_check dm_device:chr_file { open read
        # ioctl }` left this file silent (review, 2026-10-21).
        reviewed += 1
        check('ioctl' in granted.get((target, klass), set()),
              f'{domain} makes ioctls on {target}:{klass} and the allow rule for it does not '
              f'grant `ioctl`, so every command is denied however the allowxperm reads. {why}.')
    for relative in sources:
        source = ROOT / relative
        check(source.is_file(), f'{relative} does not exist; the DOMAINS list is out of date')
        if not source.is_file():
            continue
        # A program that names no device is fine -- SystemVerify.cpp works on
        # /system and /proc/mounts. The anchor below is what guards against the
        # extraction silently finding nothing at all.
        text = source.read_text(encoding='utf-8')
        lines = text.splitlines()
        paths = sorted(set(re.findall(r'"(/dev/[^"\s]*)"', text)))
        for path in paths:
            if path in IGNORED:
                continue
            checked += 1
            label = label_of(path, table)
            check(label is not None,
                  f'{domain} names {path} and neither this device\'s file_contexts nor the '
                  f'PLATFORM_CONTEXTS table in this test says what label it carries. Add it '
                  f'with the line of the AOSP file_contexts it is quoted from; skipping it '
                  f'would excuse whatever rule this project holds for it.')
            if label is None:
                continue
            klass = 'blk_file' if path.startswith('/dev/block/') else 'chr_file'
            named = [entry for entry in PATH_CLASSES if entry[0] == path]
            if named:
                klass = named[0][1]
            # And the permissions each call at this path actually needs. This is
            # what a hand audit of the three domains did once, on 2026-10-21;
            # here it is every run, so that changing a call without changing the
            # rule fails before a device sees it.
            reaching, unexplained, built = calls_for(path, lines)
            for line_number in built:
                check(entry['paths'],
                      f'{domain}: {relative}:{line_number} builds a path from {path} at run '
                      f'time, and {domain} has no hand-read access in REVIEWED. Nothing here '
                      f'can see what that path becomes, so the access it needs has to be '
                      f'written down with where it was read from.')
            reached += len(reaching)
            for line_number in unexplained:
                check(False,
                      f'{domain}: {relative}:{line_number} names {path} and this test cannot tell '
                      f'what reaches it, so it cannot say which permission that needs. Either '
                      f'write the call on that line, or add its shape to calls_for().')
            for name, call_line in reaching:
                override, perms = needed(name, call_line)
                call_klass = override or klass
                check(perms is not None,
                      f'{domain} reaches {path} with {name}() and this test cannot read what '
                      f'that asks for from `{call_line.strip()[:70]}`. An access() needs its '
                      f'mode named (F_OK/R_OK/W_OK/X_OK); add the shape to needed().')
                if perms is None:
                    continue
                if not perms:
                    # access(F_OK) and statfs() need nothing on the object, so a
                    # path reached only that way needs no rule for it at all and
                    # this says nothing about one. The parent walk below is what
                    # covers those calls.
                    continue
                check((label, call_klass) in granted,
                      f'{domain} reaches {path} with {name}() and no allow rule in '
                      f'{POLICY.name}/ grants it {label}:{call_klass} at all. On the device '
                      f'this is a denial, and the program reports whatever its surrounding '
                      f'condition says instead.')
                missing = perms - granted.get((label, call_klass), set())
                check(not missing,
                      f'{domain} reaches {path} with {name}(), which needs {sorted(perms)} on '
                      f'{label}:{call_klass}, and the policy is missing {sorted(missing)}.')
            # And every directory on the way in, for `search`. This is the V66
            # failure: a rule for the node, none for /dev/block, and a report
            # area that was never written while the service exited 0.
            for directory in parents(path):
                parent_label = label_of(directory, table)
                check(parent_label is not None,
                      f'{domain} names {path}, whose parent {directory} has no label in '
                      f'either table; add it to PLATFORM_CONTEXTS with its source line')
                if parent_label is None or parent_label in BASE_DIR_SEARCH:
                    continue
                check('search' in granted.get((parent_label, 'dir'), set()),
                      f'{domain} names {path} but has no `{parent_label}:dir search` for its '
                      f'parent {directory}. The open walks the path first, so on the device '
                      f'this denies before the node is ever reached.')

# The path that cost a device start, pinned: if the extraction or the labelling
# ever stops reaching it, this test has stopped testing the thing it was written
# for, and that must fail rather than pass quietly.
anchor = label_of('/dev/block/dm-3', table)
check(anchor == 'pw3_report_block_device',
      f'/dev/block/dm-3 resolves to {anchor!r}; file_contexts no longer labels the report device')
check(('pw3_report_block_device', 'blk_file') in rules_for('pw3_init_check'),
      'pw3_init_check has no rule for pw3_report_block_device:blk_file -- this is the V65 failure')
check('/dev/block/dm-3' in (ROOT / 'device/amazon/pw3/initcheck/InitCheck.cpp').read_text(encoding='utf-8'),
      'InitCheck.cpp no longer names /dev/block/dm-3; re-aim this test')
# The types this policy declares for itself. AOSP does not know them, so every
# rule for one of them is here by necessity -- which makes two things checkable
# without reading any source: a type nothing is allowed on is a rule that can
# never fire, and a type nothing labels is a type nothing can ever carry. Both
# are how a rule quietly stops doing anything, and the nine .te files that
# augment AOSP's own domains (hal_light, hal_power, surfaceflinger, init, ...)
# are reachable this way even though their base rules are not here to read.
declared, allowed_types, labelled, labelled_by_transition = set(), set(), set(), set()
for source in POLICY.glob('*.te'):
    body = re.sub(r'#[^\n]*', '', source.read_text(encoding='utf-8'))
    declared |= {name for name, _ in re.findall(
        r'(?m)^type\s+([\w]+)\s*,\s*(fs_type|dev_type|sysfs_type|file_type|data_file_type)', body)}
    allowed_types |= set(re.findall(r'(?<![\w-])allow\s+[\w{}\s]+?\s+([\w]+):', body))
    allowed_types |= set(re.findall(r'\b(?:r_dir_file|rw_dir_file)\s*\(\s*[\w$]+\s*,\s*([\w$]+)\s*\)', body))
    # A type_transition both uses a type and puts it on something: it is how an
    # inode created in a domain gets a type when no file_contexts entry can
    # reach it. pw3_storage.te labels pre-init's loop backing nodes that way,
    # because AOSP init mounts a new /dev over the inodes they were made on and
    # restorecon never sees them.
    transitions = set(re.findall(r'type_transition\s+[\w]+\s+[\w]+:[\w]+\s+([\w]+)', body))
    allowed_types |= transitions
    labelled_by_transition |= transitions
for name in ('file_contexts', 'genfs_contexts', 'property_contexts'):
    path = POLICY / name
    if path.is_file():
        labelled |= set(re.findall(r'u:object_r:([\w]+):s0', path.read_text(encoding='utf-8')))
# An _exec type is labelled by file_contexts and used by init_daemon_domain(),
# which this parser does not expand; they are covered by the domain checks.
for name in sorted(declared):
    if name.endswith('_exec'):
        continue
    check(name in allowed_types,
          f'{name} is declared in {POLICY.name}/ and no allow rule anywhere in it grants '
          f'anything on it. Either something lost its rule or the type is dead.')
    check(name in labelled or name in labelled_by_transition,
          f'{name} is declared in {POLICY.name}/ and nothing puts it on anything -- no '
          f'file_contexts or genfs_contexts entry and no type_transition -- so nothing can '
          f'ever carry it and its rules can never fire.')

# Who must be allowed what on the types this policy declares, read off the
# programs that touch them rather than off the policy -- copying the policy here
# would make a check that always passes. This is what the "nothing is allowed on
# it" check above cannot see: a type with two users keeps its rule when one of
# them loses it. These domains are AOSP's own (hal_light, hal_power,
# system_server, ueventd), so their base rules are not in this directory; but
# AOSP has never heard of these types, so every rule for one of them is, which
# is what makes the pairs checkable at all.
TYPE_USERS = [
    ('hal_light', 'sysfs_pw3_fl_lot', 'file', {'open', 'read'},
     'lights.c select_steps() opens PW3_FL_LOT_INPUT and reads it to pick the light table'),
    ('hal_power', 'sysfs_pw3_bl_power', 'file', {'open', 'write'},
     'power.c opens /sys/class/backlight/max77696-bl/bl_power O_WRONLY to blank the frontlight'),
    ('hal_power', 'sysfs_pw3_cpu_min_freq', 'file', {'open', 'write'},
     'power.c raises the lowest CPU frequency for PowerHint.LAUNCH'),
    ('system_server', 'sysfs_pw3_power', 'file', {'open', 'read', 'write'},
     'libsuspend writes /sys/power/state and reads /sys/power/wakeup_count'),
    ('ueventd', 'pw3_private_firmware_file', 'file', {'open', 'read'},
     'ueventd is the only reader of the per-device calibration and waveform assets'),
    ('ueventd', 'pw3_private_firmware_file', 'dir', {'search', 'read'},
     'and it has to walk and list that tree to find them'),
    ('pw3_init_check', 'pw3_data_config_file', 'file', {'open', 'read'},
     'InitCheck.cpp reads the pre-init configuration left in the RAM root'),
]
for who, what, klass, perms, why in TYPE_USERS:
    missing = perms - rules_for(who).get((what, klass), set())
    check(not missing,
          f'{who} needs {sorted(perms)} on {what}:{klass} and {POLICY.name}/ is missing '
          f'{sorted(missing)}. {why}.')

# Every domain this port declares must be covered. Three failures in two days
# were all a domain's own missing rule, and before 2026-10-21 this file looked
# at three of the five.
check(sorted(DOMAINS) == OWN_DOMAINS,
      f'this port declares the domains {OWN_DOMAINS} and this test covers {sorted(DOMAINS)}; '
      f'a domain nobody checks is where the next one of these goes')
# The V66 failure, pinned the same way: the directory, not the node.
check('search' in rules_for('pw3_boot_report').get(('block_device', 'dir'), set()),
      'pw3_boot_report has no `block_device:dir search` -- this is the V66 failure: it '
      'could not walk /dev/block, so the report area was never written on any start')
check(parents('/dev/block/dm-3') == ['/dev', '/dev/block'],
      f'parents() no longer walks the path: {parents("/dev/block/dm-3")}')
check(label_of('/dev/block', table) == 'block_device' and label_of('/dev', table) == 'device',
      'the parent directories of the report node no longer resolve; PLATFORM_CONTEXTS is stale')
# And that the call extraction still reaches the calls it was written for. An
# extraction that quietly finds nothing checks nothing while still passing,
# which is the shape of every defect this file is about.
report_calls = calls_for('/dev/block/dm-3',
                         (ROOT / 'device/amazon/pw3/initcheck/BootReport.cpp')
                         .read_text(encoding='utf-8').splitlines())[0]
report_names = sorted({name for name, _ in report_calls})
check(report_names == ['open', 'stat'],
      f'calls_for() finds {report_names} at the report area in BootReport.cpp, not the '
      f'open() and stat() that are there; the extraction has stopped working')
check(any('O_WRONLY' in line for name, line in report_calls if name == 'open'),
      'calls_for() no longer sees that the report area is opened for writing, so the '
      'policy could lose `write` on it without this test saying anything')

for message in failures:
    print('FAIL', message)
if failures:
    sys.exit(1)
print(f'PW3_SEPOLICY_DEVICE_PATHS_PASS  {len(DOMAINS)} of this port\'s {len(OWN_DOMAINS)} own '
      f'domains, {checked} /dev paths all resolved to a label, {reached} calls checked against '
      f'the permissions they need, every parent directory granted search, {reviewed} '
      f'hand-read accesses checked too')
