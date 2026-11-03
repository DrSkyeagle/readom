#!/usr/bin/env python3
"""Reproduce the GNU make hash-escaping cache bug with real compiler commands."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import subprocess
import tempfile
import time

MAKEFILE = '''include Kbuild.include
test_hash := \\#
PHONY := FORCE
targets := sample.o
.PHONY: FORCE
FORCE:
quiet_cmd_cc = CC sample.o
cmd_cc = gcc -D"STRINGIFY(s)=$(test_hash)s" -DVALUE={value} -c -o sample.o sample.c
sample.o: sample.c FORCE
\t$(call if_changed,cc)
-include .sample.o.cmd
'''


def run_case(include, directory, expect_rebuild):
    directory.mkdir()
    (directory/'Kbuild.include').write_bytes(include.read_bytes())
    (directory/'Makefile').write_text(MAKEFILE.format(value=1))
    source=directory/'sample.c'
    source.write_text('int sample(void) { return VALUE; }\n')
    # Backdated, because what this case is about is the command cache and not
    # the clock. if_changed rebuilds when any-prereq is non-empty, and $? holds
    # a prerequisite whose timestamp is not older than the target's -- so a
    # source and the object made from it landing in the same tick make the
    # second build recompile, which is the opposite of what the patched case
    # asserts. It failed exactly that way once (2026-10-25, during V83's suite;
    # the next run passed), and a gate that fails a release at random is worse
    # than no gate.
    old=time.time_ns()-2_000_000_000
    os.utime(source,ns=(old,old))
    def build():
        p=subprocess.run(['make','-s','sample.o'],cwd=directory,text=True,capture_output=True)
        if p.returncode: raise RuntimeError(p.stderr)
        obj=directory/'sample.o'
        return obj.stat().st_mtime_ns,hashlib.sha256(obj.read_bytes()).hexdigest()
    first=build(); second=build()
    assert (second[0] != first[0]) == expect_rebuild
    assert second[1] == first[1]
    (directory/'Makefile').write_text(MAKEFILE.format(value=2))
    changed_command=build()
    assert changed_command[0] != second[0] and changed_command[1] != second[1]
    source.write_text('int sample(void) { return VALUE + 1; }\n')
    # The VM may assign the same coarse write timestamp to consecutive writes.
    # Make's source-dependency check needs an observably newer source mtime.
    stamp=max(time.time_ns(),changed_command[0]+1)
    os.utime(source,ns=(stamp,stamp))
    changed_source=build()
    assert changed_source[0] != changed_command[0] and changed_source[1] != changed_command[1]
    return {'identical_second_build_recompiled':expect_rebuild,
            'command_change_recompiled':True,'source_change_recompiled':True,
            'initial_object_sha256':first[1]}


if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('before',type=Path)
    parser.add_argument('after',type=Path)
    parser.add_argument('receipt',type=Path)
    args=parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='pw3-kbuild-cache-') as tmp:
        root=Path(tmp)
        before=run_case(args.before,root/'before',True)
        after=run_case(args.after,root/'after',False)
    assert before['initial_object_sha256']==after['initial_object_sha256']
    result={'before':before,'after':after,'actual_compiler_object_unchanged':True,
            'make_version':subprocess.check_output(['make','--version'],text=True).splitlines()[0],
            'kernel_runtime_changed':False}
    args.receipt.write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result))
