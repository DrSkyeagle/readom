#!/usr/bin/env python3
"""Reject module relocations unsupported by the pinned ARM 3.0 loader."""
import json
import re
import subprocess
import sys
from pathlib import Path

# arch/arm/kernel/module.c:apply_relocate; no Thumb2 in the Wario config.
supported = {'R_ARM_NONE', 'R_ARM_ABS32', 'R_ARM_PC24', 'R_ARM_CALL', 'R_ARM_JUMP24',
             'R_ARM_V4BX', 'R_ARM_PREL31', 'R_ARM_MOVW_ABS_NC', 'R_ARM_MOVT_ABS'}
root = Path(sys.argv[1])
report = []
for path in sorted(root.rglob('*.ko')):
    output = subprocess.check_output([sys.argv[2], '-rW', str(path)], text=True)
    # Debug sections are never relocated by the module loader. GCC emits
    # supported relocations here too, so checking all sections is stricter.
    kinds = sorted(set(re.findall(r'\bR_ARM_[A-Z0-9_]+\b', output)))
    report.append({'module': str(path.relative_to(root)), 'relocations': kinds,
                   'unsupported': sorted(set(kinds) - supported)})
if not report:
    raise SystemExit('No modules found; relocation audit was not performed.')
print(json.dumps({'modules': report, 'scope': 'ELF relocation compatibility only'}, indent=2))
if any(item['unsupported'] for item in report):
    raise SystemExit('Unsupported ARM module relocations; do not load these artifacts.')
