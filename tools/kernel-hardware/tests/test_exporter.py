from pathlib import Path
import argparse,io,json,os,sys,tarfile,tempfile
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT));import export_component as e
# The built kernel to export from, by argument or by the variable
# kernel-hardware/tests/run_tests.py sets, so tools/run-tests.py can run this
# wherever a build left one behind.
ap=argparse.ArgumentParser();ap.add_argument('--build-dir',type=Path,default=os.environ.get('PW3_KERNEL_BUILD_DIR'));args=ap.parse_args()
if args.build_dir is None:raise SystemExit('Needs --build-dir or PW3_KERNEL_BUILD_DIR')
BUILD=Path(args.build_dir)
RECEIPT=e.sha((BUILD/'build-result.json').read_bytes())
FILES=('build-result.json','kernel.config','kernel.release','modules.tar.gz','Module.symvers','module-relocations.json','zImage')
def refused(path,label):
    try:e.checked_build(path,expected_receipt_sha256=e.sha((path/'build-result.json').read_bytes()))
    except (ValueError,KeyError,IndexError,UnicodeError):return
    raise AssertionError('Accepted '+label)
def update(root,name,data):
    (root/name).write_bytes(data);r=json.loads((root/'build-result.json').read_text())
    r['artifacts'][name]={'bytes':len(data),'sha256':e.sha(data)}
    (root/'build-result.json').write_text(json.dumps(r))
def change_tar(data, transform):
    out=io.BytesIO()
    with tarfile.open(fileobj=io.BytesIO(data),mode='r:gz') as src,tarfile.open(fileobj=out,mode='w:gz') as dest:
        for m in src.getmembers():
            body=src.extractfile(m).read() if m.isfile() else None
            if body is not None:body=transform(m.name,body);m.size=len(body)
            dest.addfile(m,io.BytesIO(body) if body is not None else None)
    return out.getvalue()
modules,proof=e.checked_build(BUILD,RECEIPT)
assert proof['receipt_sha256']==RECEIPT
assert proof['patch_count']>0 and set(modules)==set(e.MODULES)
with tempfile.TemporaryDirectory(prefix='pw3-usb-export-') as directory:
    root=Path(directory)/'build';root.mkdir();saved={n:(BUILD/n).read_bytes() for n in FILES}
    def reset():
        for n,d in saved.items():(root/n).write_bytes(d)
    reset();(root/'kernel.config').write_bytes(b'corrupt');refused(root,'hash mismatch')
    reset();update(root,'kernel.config',saved['kernel.config'].replace(b'CONFIG_USB_G_ANDROID=m',b'# CONFIG_USB_G_ANDROID is not set'));refused(root,'wrong gadget composition')
    reset();update(root,'kernel.config',saved['kernel.config']+b'CONFIG_USB_FUNCTIONFS=m\n');refused(root,'g_ffs beside the Android gadget')
    reset();update(root,'Module.symvers',b'');refused(root,'missing symbols')
    reset();r=json.loads(saved['module-relocations.json'])
    for m in r['modules']:
        if m['module'].endswith('/g_android.ko'):m['unsupported']=['R_ARM_TEST']
    update(root,'module-relocations.json',json.dumps(r).encode());refused(root,'failed relocation receipt')
    reset();update(root,'kernel.release',b'other-kernel\n');refused(root,'mismatched release')
    reset()
    def bad_dep(name,data):
        if name.endswith('/modules.dep'):
            data=data.replace(b'kernel/drivers/usb/gadget/g_android.ko: kernel/drivers/usb/gadget/arcotg_udc.ko',b'kernel/drivers/usb/gadget/g_android.ko: kernel/drivers/usb/gadget/g_ether.ko')
        return data
    update(root,'modules.tar.gz',change_tar(saved['modules.tar.gz'],bad_dep));refused(root,'unreviewed gadget dependency')
    reset()
    def bad_elf(name,data):
        if name.endswith('/g_android.ko'):return b'BAD!'+data[4:]
        return data
    update(root,'modules.tar.gz',change_tar(saved['modules.tar.gz'],bad_elf));refused(root,'non-ELF selected module')
    reset();out=Path(directory)/'out';manifest=e.export(root,out,RECEIPT)
    for f in manifest['files']:
        d=(out/f['path']).read_bytes();assert len(d)==f['bytes'] and e.sha(d)==f['sha256']
    assert len(list((out/'vendor/lib/modules').glob('*.ko')))==9
    assert not (out/'vendor/firmware').exists()
    assert not (out/'userspace-patch/patched-source').exists()
    mk=(out/'Android.mk').read_text()
    assert mk.startswith('LOCAL_PATH := $(call my-dir)\n') and mk.endswith('\ninclude $(LOCAL_PATH)/native/Android.mk\n')
    assert mk.count('include $(LOCAL_PATH)/native/Android.mk')==1
    product=(out/'pw3-usb-component.mk').read_text()
    for name in e.MODULES:
        # Every module is a prebuilt with the kernel's licence, installed by name, not copied.
        assert mk.count('LOCAL_MODULE := '+e.module_package(name)+'\n')==1 and mk.count('LOCAL_SRC_FILES := vendor/lib/modules/'+name+'.ko\n')==1
        assert e.module_package(name) in product.split('\n')[3].split() and name+'.ko' not in product
    assert mk.count('LOCAL_NOTICE_FILE := $(LOCAL_PATH)/COPYING.kernel\n')==len(e.MODULES)
    assert product.count('vendor/etc/init/')==3 and 'ueventd.pw3.fragment.rc' in product
    try:e.export(root,out,RECEIPT)
    except ValueError:pass
    else:raise AssertionError('Overwrote existing output')
print('Actual selected-build nine-module closure and 9 refusal cases passed')
