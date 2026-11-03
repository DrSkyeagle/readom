#!/usr/bin/env python3
import argparse,hashlib,json,os,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
ap=argparse.ArgumentParser();ap.add_argument('--build-dir',required=True,type=Path);ap.add_argument('--kernel-source',required=True,type=Path);args=ap.parse_args()
os.environ['PW3_KERNEL_BUILD_DIR']=str(args.build_dir.resolve())
os.environ['PW3_KERNEL_SOURCE_DIR']=str(args.kernel_source.resolve())
def run(command,**kw):subprocess.run(command,check=True,**kw)
sdk=subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip()
flags=['-std=c++11','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined','-isystem',sdk+'/usr/include/c++/v1']
with tempfile.TemporaryDirectory(prefix='pw3-usb-tests-') as d:
    binary=Path(d)/'usb-gate-test'
    run(['clang++',*flags,'-I'+str(ROOT/'native'),str(ROOT/'native/UsbGate.cpp'),str(ROOT/'native/PosixChecks.cpp'),str(ROOT/'tests/gate_test.cpp'),'-o',str(binary)])
    run([str(binary)])
    source=Path(d)/'core';source.mkdir()
    manifest=json.loads((ROOT/'userspace-patch/patch-manifest.json').read_text())
    for rel,meta in manifest['files'].items():
        p=source/rel;p.parent.mkdir(parents=True,exist_ok=True)
        p.write_bytes((ROOT/'adb-contract/sources'/rel.replace('/','__')).read_bytes())
        assert hashlib.sha256(p.read_bytes()).hexdigest()==meta['before']
    patches=[ROOT/'userspace-patch'/item['patch'] for item in manifest['patches']]
    for patch,item in zip(patches,manifest['patches']):assert hashlib.sha256(patch.read_bytes()).hexdigest()==item['sha256']
    for patch in patches:run(['patch','-p1','--batch','-i',str(patch)],cwd=source)
    for rel,meta in manifest['files'].items():assert hashlib.sha256((source/rel).read_bytes()).hexdigest()==meta['after']
    for patch in reversed(patches):run(['patch','-R','-p1','--batch','-i',str(patch)],cwd=source)
    for rel,meta in manifest['files'].items():assert hashlib.sha256((source/rel).read_bytes()).hexdigest()==meta['before']
run(['python3',str(ROOT/'tests/test_adbd.py')]);run(['python3',str(ROOT/'tests/test_ffs_lifecycle.py')]);run(['python3',str(ROOT/'tests/test_exporter.py')])
result={'passed':True,'groups':['native gating and POSIX checks with ASan/UBSan','exact AOSP adbd functions board flag on/off with ASan/UBSan','full original source preserved with board flag off','patch apply/reverse SHA256','exact old FunctionFS empty probe lifecycle with ASan/UBSan','actual selected-build nine modules ELF symbol dependency and relocation audit','artifact corruption and wrong composition refusal','new-output-only component export and every-file hash'], 'device_accessed':False,'android_cross_compile':False,'usb_runtime_verified':False}
(ROOT/'tests/test-results.json').write_text(json.dumps(result,indent=2)+'\n')
(ROOT/'tests/usb-gate-test').unlink(missing_ok=True)
print(json.dumps(result))
