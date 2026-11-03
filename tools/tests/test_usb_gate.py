#!/usr/bin/env python3
"""The USB gate's invariants, read out of init.pw3.usb.rc.

USB is the only channel this device has to a computer, and the file that decides
whether it works has now cost two incidents:

  V59 (2026-10-20) enabled the gadget before adbd had written FunctionFS's
  descriptors, so a boot that asked for adb left the cable dead -- "插线了啊",
  with nothing on the bus. The fix was to enable in two steps.

  V66 (2026-10-21) showed the two steps do not recover: `adb root` makes adbd
  exit so init can start it again as root, and a service that exits without
  having been disabled or reset never reaches "stopped" -- init publishes
  "restarting" instead (system/core/init/service.cpp, Service::Reap). So the
  `init.svc.adbd=stopped` handler never fired, sys.usb.ffs.ready stayed 1, the
  new adbd setting it to 1 was no change, and the gadget was never taken through
  enable 0 / enable 1 to re-bind the FunctionFS instance the kernel had reset
  when the old adbd closed ep0. `adb devices` was empty for 80 seconds and a
  replug did not help, because replugging changes no property either.

Both are invariants of this one file, and both are checkable without a device.
What is not checkable here is whether the device then comes up -- that needs the
cable, and this says nothing about it.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
RC = ROOT / 'device/amazon/pw3/rootdir/init.pw3.usb.rc'
failures = []


def check(ok, message):
    if not ok:
        failures.append(message)


text = RC.read_text(encoding='utf-8')
# Comments carry the reasoning and also the words being looked for, so they go.
body = '\n'.join(line for line in text.splitlines() if not line.lstrip().startswith('#'))

# Every `on` block and the commands under it, which is how init reads the file:
# a command belongs to the most recent section header.
sections, current = {}, None
for line in body.splitlines():
    if not line.strip():
        continue
    if not line[0].isspace():
        current = line.strip()
        sections.setdefault(current, [])
    elif current is not None:
        sections[current].append(line.strip())

configs = ('adb', 'mtp,adb', 'ptp,adb')
checked = 0

for config in configs:
    # Step one: adbd starts, gated on this port's own FunctionFS check.
    starts = [header for header, commands in sections.items()
              if header.startswith('on ') and f'sys.usb.config={config} ' in header + ' '
              and any(command == 'start adbd' for command in commands)]
    check(len(starts) == 1,
          f'exactly one section should start adbd for sys.usb.config={config}; found {starts}')
    for header in starts:
        checked += 1
        check('sys.pw3.ffs_ready=1' in header,
              f'{header}: adbd must not start unless pw3-ffs-check has passed '
              f'(sys.pw3.ffs_ready=1); without that gate adbd runs against whatever '
              f'/dev/usb-ffs/adb happens to be')

    # Step two: the gadget is enabled only once adbd has written the descriptors.
    enables = [header for header, commands in sections.items()
               if header.startswith('on ') and f'sys.usb.config={config} ' in header + ' '
               and any(command.endswith('/enable 1') for command in commands)]
    check(len(enables) == 1,
          f'exactly one section should enable the gadget for sys.usb.config={config}; '
          f'found {enables}')
    for header in enables:
        checked += 1
        check('sys.usb.ffs.ready=1' in header,
              f'{header}: this is the V59 failure -- enabling the gadget for a configuration '
              f'carrying adb before adbd has written FunctionFS\'s descriptors leaves the '
              f'cable dead, with MTP too, there being one gadget')
        commands = sections[header]
        # The same gadget, written in full. Matching only the `/enable 0` suffix
        # let the disable name a different device -- `android9/enable 0` before
        # `android0/enable 1` -- and this still reported every gate passing.
        offs = [c for c in commands if re.fullmatch(r'write\s+(\S+)/enable\s+0', c)]
        ons = [c for c in commands if re.fullmatch(r'write\s+(\S+)/enable\s+1', c)]
        check(len(offs) == 1 and len(ons) == 1,
              f'{header}: expected one `write .../enable 0` and one `write .../enable 1`, '
              f'found {offs} and {ons}')
        if len(offs) == 1 and len(ons) == 1:
            off_path = re.fullmatch(r'write\s+(\S+)/enable\s+0', offs[0]).group(1)
            on_path = re.fullmatch(r'write\s+(\S+)/enable\s+1', ons[0]).group(1)
            check(off_path == on_path,
                  f'{header}: disables {off_path} and enables {on_path}; the gadget that is '
                  f'taken down has to be the one that is brought back up, or the composite is '
                  f'never re-bound to the FunctionFS instance')
            check(commands.index(offs[0]) < commands.index(ons[0]),
                  f'{header}: the disable has to come first')

# The property must not outlive the descriptors it speaks for. A service that
# exits without having been disabled never reaches "stopped" -- init takes the
# restart branch (Service::Reap) -- so the handler on init.svc.adbd=stopped does
# not fire, and sys.usb.ffs.ready would sit at 1 with no adbd and no descriptors.
# onrestart runs inside Reap() before that state change, which is the one hook
# that sees a restart that is not a stop.
#
# Not, as this said until 2026-10-21, because the new adbd's own 1 would be no
# change for init to trigger on: init queues a property change after every
# successful set and matches triggers on the new value alone
# (property_service.cpp, init.cpp, action.cpp). What that mechanism was invoked
# to explain -- `adb root` taking the connection down -- is unexplained again,
# and docs/known-issues.md says so.
service = None
for header, commands in sections.items():
    if header.startswith('service adbd '):
        service = (header, commands)
check(service is not None, 'no adbd service in the file')
if service:
    checked += 1
    header, commands = service
    check(any(re.fullmatch(r'onrestart\s+setprop\s+sys\.usb\.ffs\.ready\s+0', command)
              for command in commands),
          'the adbd service needs `onrestart setprop sys.usb.ffs.ready 0`. Without it a '
          'restart that is not a stop leaves the property claiming FunctionFS descriptors '
          'that went when adbd did, and the only hook that sees such a restart is this one '
          '(Service::Reap runs onrestart before publishing "restarting").')
    check(any(command == 'disabled' for command in commands),
          'the adbd service must stay `disabled` so that only the gated sections start it')

# And nothing may react to the property going to 0: the gadget must not be
# touched at a moment when there may be no cable, which is the hazard that
# locked V44 up (touching the USB controller's registers while it is stopped).
for header, commands in sections.items():
    if 'sys.usb.ffs.ready=0' in header:
        check(not any('/enable' in command for command in commands),
              f'{header}: writing the gadget\'s enable when the descriptors have just gone '
              f'can touch the controller with no cable attached; nothing should trigger on '
              f'the 0')

# The anchors: if the file stops being shaped like this, these questions are no
# longer the right ones to ask and that must fail rather than pass quietly.
check('sys.usb.configfs 0' in body or 'sys.usb.configfs=0' in body,
      'the file no longer says this is the non-configfs gadget; re-aim this test')
check(checked >= 7, f'only {checked} gates were found; the extraction has stopped working')

for message in failures:
    print('FAIL', message)
if failures:
    sys.exit(1)
print(f'PW3_USB_GATE_PASS  {checked} gates: adb starts behind this port\'s FunctionFS check, '
      f'the gadget is enabled only after the descriptors are written, a restart puts the '
      f'property back rather than leaving it claiming descriptors that have gone, and '
      f'nothing touches the gadget on the 0')
