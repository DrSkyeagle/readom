#!/usr/bin/env python3
"""A PID 1 entry must be able to say why it stopped, and saying it must not hang.

A storage entry runs before anything else and before any mount. When it refuses
to continue it calls stop(), which writes a fixed reason to /dev/kmsg and to
/dev/console. Only the first reaches a person: the kernel log sits at the fixed
physical address on the command line, survives the restart, and the launcher
copies the `pw3-preinit` lines out of it onto the Kindle drive (boot/launcher/userlog.c, and test_klog_tags.py for the list).

Two things were wrong before V63, and this test guards both.

The reason could not get out. Both descriptors were acquired after the
pseudo-filesystem mounts, because /dev/kmsg was made from sysfs and sysfs is one
of the mounts; the ramdisk carried no character device nodes at all, so the one
earlier open() always failed with ENOENT. The two earliest failures -- a root
that is not RAM-backed, and a mount that does not take -- therefore reported to
nothing, and on the shipped entry that is a blank two-minute wait and a restart
with the reason nowhere. The nodes are in the ramdisk now (pack.LOG_NODES), and
report() opens them itself, so there is no ordering left to get wrong.

Saying it could hang. /dev/console is a serial port whose default termios has
IXON (tty_std_termios, drivers/tty/tty_io.c). An XOFF on the line, or noise on a
floating test pad that decodes as one, sets tty->stopped; uart_start() then never
calls start_tx(), the ring fills, and a *blocking* write sleeps in n_tty_write()
with no timeout (drivers/tty/n_tty.c). PID 1 would hang before writing the kernel
log or reaching its restart -- worse than the silence being fixed. So every
console descriptor keeps O_NONBLOCK for its writes and the kernel log is written
first. Note the open itself never blocks whatever is asked for: the kernel sets
O_NONBLOCK for /dev/console and restores the caller's flags (tty_io.c, "Don't let
/dev/console block"), which is also why O_NONBLOCK has to be asked for here to
survive into the writes.

This is a source check because the failure cannot be staged: it needs a mount to
fail, or a serial peer to send XOFF, on real hardware. What can be stated exactly
is that the code cannot be shaped the wrong way. Comments are stripped before
matching, so commenting a call out is a failure rather than a pass.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]

# Every file with a PID 1 entry, and the file defining its report().
ENTRIES = (
    ('device/amazon/pw3/preinit/TestFileInit.h', 'device/amazon/pw3/preinit/PreInit.c'),
)
failures = []


def check(ok, message):
    if not ok:
        failures.append(message)


def code(path):
    """The file with comments removed, so a commented-out call cannot pass."""
    text = (ROOT / path).read_text(encoding='utf-8')
    text = re.sub(r'/\*.*?\*/', ' ', text, flags=re.S)
    return re.sub(r'//[^\n]*', '', text)


def function(text, signature, path):
    """A function body, by brace depth."""
    at = text.find(signature)
    if at < 0:
        check(False, f'{path}: no {signature}')
        return None
    start = text.index('{', at)
    depth = 0
    for i in range(start, len(text)):
        if text[i] == '{':
            depth += 1
        elif text[i] == '}':
            depth -= 1
            if depth == 0:
                return text[start + 1:i]
    check(False, f'{path}: {signature} has no matching close brace')
    return None


for definer in dict.fromkeys(d for _, d in ENTRIES):
    text = code(definer)

    opener = function(text, 'static void open_log_channels(void)', definer)
    if opener is not None:
        # Opened once, and a failed open stays failed rather than retrying.
        guard = 'if (log_channels_tried) return;'
        check(guard in opener,
              f'{definer}: open_log_channels() must return early when it has already tried')
        head, _, body = opener.partition(guard)
        # The guard has to be the FIRST statement, and the only return: anything
        # that leaves earlier leaves without a channel open.
        check(not head.strip(),
              f'{definer}: open_log_channels() runs {head.strip()[:60]!r} before its guard')
        check('return' not in body,
              f'{definer}: open_log_channels() returns before it opens anything')
        # The descriptors have to be kept, not merely opened.
        check('kmsg_fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC)' in body,
              f'{definer}: open_log_channels() must open /dev/kmsg into kmsg_fd')
        check('console_fd = open("/dev/console", O_WRONLY | O_CLOEXEC | O_NONBLOCK)' in body,
              f'{definer}: open_log_channels() must open the console into console_fd, non-blocking')
        check('log_channels_tried = true;' in body,
              f'{definer}: open_log_channels() never records that it tried')

    reporter = function(text, 'static void report(const char *', definer)
    if reporter is not None:
        # It opens its own channels, so no caller has an order to get wrong.
        call = reporter.find('open_log_channels();')
        check(call >= 0, f'{definer}: report() must call open_log_channels() itself')
        first_write = reporter.find('dprintf(')
        check(call >= 0 and (first_write < 0 or call < first_write),
              f'{definer}: report() writes before opening its channels')
        # The kernel log is the channel that reaches a person, and the console
        # write can be refused or short, so the kernel log goes first.
        #
        # Checked on the writes, not on where the two names first appear. The
        # name test was satisfied by the `if (kmsg_fd >= 0)` guard alone, so
        # changing the write under it to dprintf(console_fd, ...) -- which stops
        # the reason reaching the kernel log at all -- passed.
        writes = re.findall(r'dprintf\(\s*(\w+)', reporter)
        check(writes[:2] == ['kmsg_fd', 'console_fd'],
              f'{definer}: report() must write the kernel log first and the console second; '
              f'it writes to {writes[:3]}')
        for name in ('kmsg_fd', 'console_fd'):
            check(re.search(r'if\s*\(\s*' + name + r'\s*>=\s*0\s*\)\s*dprintf\(\s*' + name,
                            reporter) is not None,
                  f'{definer}: the write guarded by {name} >= 0 must be the write to {name}')

# File-wide invariants: these hold at every open site, not just the first one.
for path in dict.fromkeys([p for p, _ in ENTRIES] + [d for _, d in ENTRIES]):
    text = code(path)
    for site in re.findall(r'open\("/dev/console"[^)]*\)', text):
        check('O_NONBLOCK' in site,
              f'{path}: {site} must keep O_NONBLOCK, or a stopped UART hangs PID 1')
    # Nothing may take the flag back off, which is what made the write block.
    check('F_SETFL' not in text and '~O_NONBLOCK' not in text,
          f'{path}: must not clear O_NONBLOCK on a log descriptor')
    # A second open must not discard a channel that already works.
    for line in text.splitlines():
        stripped = line.strip()
        if re.match(r'^(kmsg_fd|console_fd)\s*=\s*open\(', stripped):
            check(path in [d for _, d in ENTRIES] and 'log_channels_tried' in text
                  and stripped in function(text, 'static void open_log_channels(void)', path),
                  f'{path}: "{stripped}" assigns an open() straight to a live descriptor; '
                  'a failed re-open would throw away a working channel')

# The nodes themselves, in the one place that defines them, with the kernel's
# own device numbers. There is one packaging path now: build_entry.py. pack.py
# stopped having one of its own when the ram_boot=1 bundle assembler went, on
# 2026-10-29, so what is checked here is the definition and that build_entry
# uses it.
packer = (ROOT / 'tools/preinit-pack/pack.py').read_text(encoding='utf-8')
check("LOG_NODES = (('dev/kmsg', 1, 11), ('dev/console', 5, 1))" in packer,
      'tools/preinit-pack/pack.py: LOG_NODES must stay the kernel\'s own device numbers')
builder = (ROOT / 'boot/build_entry.py').read_text(encoding='utf-8')
check('entries += pack.log_nodes()' in builder,
      'boot/build_entry.py: must add the log nodes from pack.py, not a second copy')
check('pack.verify_log_nodes(pack.read_ramdisk_bytes(public))' in builder,
      'boot/build_entry.py: must verify the nodes in the archive that was written')

for message in failures:
    print('FAIL', message)
if failures:
    sys.exit(1)
print(f'{len(ENTRIES)} PID 1 entries report without an ordering to get wrong, '
      'and no console write can block')
