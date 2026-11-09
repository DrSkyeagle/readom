#!/bin/bash
set -euo pipefail
[[ "$(uname -s)" == Linux && "$(hostname)" == ${READOM_BUILD_HOST:-lima-pw3} ]] || exit 1
ART=/mnt/build-share/kernel-smoke
# Do not execute a mixture left by an interrupted kernel/userspace rebuild.
python3 - "$ART" <<'VERIFY'
import hashlib,json,sys
from pathlib import Path
root=Path(sys.argv[1]); record=json.loads((root/'test-build.json').read_text())
for name,key in [('zImage','zImage_sha256'),('initramfs.cpio.gz','initramfs_sha256'),
                 ('kernel.config','kernel_config_sha256'),('patches.json','kernel_patches_sha256')]:
    if hashlib.sha256((root/name).read_bytes()).hexdigest()!=record[key]:
        raise SystemExit('Refusing mixed smoke artifacts: '+name)
VERIFY
set +e
QEMU_AUDIO_DRV=none timeout 45s qemu-system-arm \
  -machine vexpress-a9 -cpu cortex-a9 -m 256 -smp 1 \
  -display none -monitor none -serial stdio -nic none -no-reboot \
  -kernel "$ART/zImage" -initrd "$ART/initramfs.cpio.gz" \
  -append 'console=ttyAMA0 rdinit=/init panic=1 loglevel=6' \
  > "$ART/console.log" 2>&1
status=$?
set -e
cat "$ART/console.log"
python3 - "$ART" "$status" <<'PY'
import hashlib,json,re,sys
from pathlib import Path
root=Path(sys.argv[1]); text=(root/'console.log').read_text(errors='replace')
panic='Kernel panic' in text or 'Oops:' in text or 'BUG:' in text
passed=int(sys.argv[2]) == 0 and 'PW3_KERNEL_SMOKE_PASS' in text and 'PW3_KERNEL_SMOKE_FAIL' not in text and not panic
network=re.search(r'network-smoke SUMMARY: checks=(\d+) failed=(\d+)', text)
process_vm=re.search(r'process-vm-smoke SUMMARY: checks=(\d+) failed=(\d+)', text)
passed=passed and process_vm is not None and int(process_vm.group(2))==0 and 'PW3_PROCESS_VM_PASS' in text
result={'platform':'QEMU vexpress-a9','qemu_exit_status':int(sys.argv[2]),
    'kernel_interface_suite_passed':passed,'pw3_hardware_tested':False,
    'kernel_panic_or_oops_detected':panic,
    'rtc_backed_alarm_clock_available':'RTC_BACKED_ALARM_CLOCK_AVAILABLE=1' in text,
    'alarm_scope':'timerfd expiry while awake; no RTC wake or suspend test',
    'network_contract_checks':int(network.group(1)) if network else None,
    'network_contract_failures':int(network.group(2)) if network else None,
    'network_scope':'Isolated dummy interfaces and kernel syscall contracts; no received RA, Wi-Fi or Android netd test',
    'process_vm_checks':int(process_vm.group(1)) if process_vm else None,
    'process_vm_failures':int(process_vm.group(2)) if process_vm else None,
    'process_vm_scope':'Native ARM32 ABI, copy faults, DAC, COW, mmap races and bounded pin release; no SELinux policy, durable storage writeback or physical cache test',
    'android_runtime_tested':False,
    'sha256':{name:hashlib.sha256((root/name).read_bytes()).hexdigest()
              for name in ['zImage','kernel.config','patches.json','initramfs.cpio.gz','test-build.json','console.log']}}
(root/'result.json').write_text(json.dumps(result,indent=2)+'\n')
if not passed: raise SystemExit('Kernel interface suite did not pass; inspect console.log.')
PY
