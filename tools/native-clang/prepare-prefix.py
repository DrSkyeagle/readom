#!/usr/bin/env python3
"""Freeze a host-native compiler prefix. This does not activate it in AOSP."""
from pathlib import Path
import argparse,hashlib,json,os,shutil,stat,struct,tempfile

def sha(data):return hashlib.sha256(data).hexdigest()
def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--binary',type=Path,required=True);p.add_argument('--original-prefix',type=Path,required=True)
 p.add_argument('--store',type=Path,required=True);p.add_argument('--receipt',type=Path,required=True)
 a=p.parse_args();binary=a.binary.read_bytes()
 if binary[:6]!=b'\x7fELF\x02\x01' or struct.unpack_from('<H',binary,18)[0]!=183:raise ValueError('Expected native AArch64 compiler')
 original=a.original_prefix
 wrappers={name:(original/'bin'/name).read_bytes() for name in ('clang','clang++')}
 resource='/src/prebuilts/clang/host/linux-x86/clang-4053586/lib64/clang/5.0.300080'
 adapter='''#!/bin/sh
set -eu
case "${0##*/}" in clang++.real) mode=g++ ;; clang.real) mode=gcc ;; *) exit 77 ;; esac
exec "$(dirname "$0")/clang.native" "--driver-mode=$mode" -resource-dir "'''+resource+'''" "$@"
'''
 identity={'compiler_sha256':sha(binary),'wrapper_sha256':{n:sha(d) for n,d in wrappers.items()},
           'adapter_sha256':sha(adapter.encode()),'resource_directory':resource,
           'llvm_revision':'40629e6d4dbbaa22cf42174790b45a8075ed3971',
           'clang_revision':'d3aef9bb1abef05fe425b86e8023214d39fe6acd'}
 key=sha(json.dumps(identity,sort_keys=True).encode())[:20];dest=a.store/key
 if dest.exists():raise ValueError('Frozen prefix already exists; verify/reuse its receipt instead of replacing it')
 a.store.mkdir(parents=True,exist_ok=True)
 with tempfile.TemporaryDirectory(prefix='.prefix-',dir=a.store) as td:
  stage=Path(td)/'frozen';prefix=stage/'host/linux-x86/clang-4053586';(prefix/'bin').mkdir(parents=True)
  original_chroot='/src/prebuilts/clang/host/linux-x86/clang-4053586'
  for source in original.iterdir():
   if source.name!='bin':(prefix/source.name).symlink_to(original_chroot+'/'+source.name,target_is_directory=source.is_dir())
  for source in (original/'bin').iterdir():
   if source.name not in ('clang','clang++','clang.real','clang++.real'):
    (prefix/'bin'/source.name).symlink_to(original_chroot+'/bin/'+source.name)
  (prefix/'bin/clang.native').write_bytes(binary)
  for name,data in wrappers.items():
   (prefix/'bin'/name).write_bytes(data)
   (prefix/'bin'/(name+'.real')).write_text(adapter)
  for f in (prefix/'bin').iterdir():
   if not f.is_symlink():f.chmod(0o555)
  record={**identity,'schema':1,'key':key,'prefix_root':str(dest/'host'),
          'chroot_base':'/opt/pw3-native-clang-prebuilts','activated':False}
  (stage/'manifest.json').write_text(json.dumps(record,indent=2)+'\n');(stage/'manifest.json').chmod(0o444)
  for d in sorted([v for v in stage.rglob('*') if v.is_dir() and not v.is_symlink()],key=lambda x:len(x.parts),reverse=True):d.chmod(0o555)
  stage.rename(dest);dest.chmod(0o555)
 a.receipt.parent.mkdir(parents=True,exist_ok=True)
 a.receipt.write_text(json.dumps(record,indent=2)+'\n')
 print(json.dumps({'prepared':True,'key':key,'compiler_sha256':identity['compiler_sha256'],'aosp_activated':False}))
if __name__=='__main__':main()
