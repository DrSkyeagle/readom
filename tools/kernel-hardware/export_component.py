#!/usr/bin/env python3
"""Export rebuilt public hardware modules and reviewed init / USB startup code.

Reads a completed local build; never accesses a device, key or private firmware.
All artifact hashes, module symbols, dependencies and ARM relocations are checked
before output creation. Refuses replacement of an existing output directory.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import struct
import tarfile
import tempfile

HERE=Path(__file__).resolve().parent
EXPECTED_RECEIPT='3bac300307b47c301b62cc1b83259965f5a66467ce49290ee9e307be004f7fce'
SOURCE_SHA='fa89453deb9947700cbebde2e840230e69969549be2116a0a72331ac5d2fe6c9'
MODULES={
 'cyttsp4_core':'kernel/drivers/input/touchscreen/cyttsp4_core.ko',
 'cyttsp4_i2c':'kernel/drivers/input/touchscreen/cyttsp4_i2c.ko',
 'cyttsp4_mt_b':'kernel/drivers/input/touchscreen/cyttsp4_mt_b.ko',
 'cfg80211':'kernel/compat-wireless/net/wireless/cfg80211.ko',
 'ath6kl_sdio':'kernel/compat-wireless/drivers/net/wireless/ath/ath6kl/ath6kl_sdio.ko',
 'mxc_epdc_fb':'kernel/drivers/video/mxc/mxc_epdc_fb.ko',
 'fsl_otg_arc':'kernel/drivers/usb/otg/fsl_otg_arc.ko',
 'arcotg_udc':'kernel/drivers/usb/gadget/arcotg_udc.ko',
 'g_android':'kernel/drivers/usb/gadget/g_android.ko',
}
CONFIG={'CONFIG_MODULES':'y','CONFIG_USB_GADGET':'m','CONFIG_USB_ARC':'m',
 'CONFIG_USB_G_ANDROID':'m','CONFIG_USB_FUNCTIONFS':'n',
 'CONFIG_USB_OTG':'y','CONFIG_MXC_OTG':'m','CONFIG_USB_OTG_UTILS':'y',
 'CONFIG_USB_FSL_ARC_OTG':'y',
 'CONFIG_LAB126_ANDROID_ATH6KL_FIRMWARE':'y','CONFIG_LAB126_ANDROID_DISABLE_TOUCH_FW_UPDATE':'y',
 'CONFIG_CYPRESS_CYTTSP4_BUS':'y','CONFIG_ATH6KL_SDIO':'m','CONFIG_CFG80211':'m',
 'CONFIG_TOUCHSCREEN_CYPRESS_CYTTSP4':'m','CONFIG_TOUCHSCREEN_CYPRESS_CYTTSP4_I2C':'m',
 'CONFIG_TOUCHSCREEN_CYPRESS_CYTTSP4_MT_B':'m','CONFIG_TOUCHSCREEN_CYPRESS_CYTTSP4_LOADER':'n',
 'CONFIG_TOUCHSCREEN_CYPRESS_CYTTSP4_DEVICE_ACCESS':'n','CONFIG_TOUCHSCREEN_CYPRESS_CYTTSP4_DEBUG_MODULE':'n',
 'CONFIG_TOUCHSCREEN_CYPRESS_CYTTSP4_INCLUDE_FW':'n','CONFIG_TOUCHSCREEN_CYPRESS_CYTTSP4_AUTO_LOAD_TOUCH_PARAMS':'n',
 'CONFIG_LAB126_ANDROID_EPDC':'y','CONFIG_LAB126':'y','CONFIG_MACH_MX6SL_WARIO':'y',
 'CONFIG_FB_MXC_EINK_PANEL':'m','CONFIG_FB_MXC_EINK_REAGL':'n','CONFIG_MXC_PXP_V2':'y',
 'CONFIG_DMA_ENGINE':'y','CONFIG_SPI':'y','CONFIG_SPI_IMX':'y','CONFIG_SPI_IMX_VER_2_3':'y',
 'CONFIG_FW_LOADER':'y','CONFIG_ZLIB_INFLATE':'y','CONFIG_MFD_MAX77696':'y','CONFIG_SENSORS_MAX77696':'y'}
RELOCS={2:'R_ARM_ABS32',28:'R_ARM_CALL',43:'R_ARM_MOVW_ABS_NC',44:'R_ARM_MOVT_ABS'}
def sha(data): return hashlib.sha256(data).hexdigest()
def fail(message): raise ValueError(message)

def elf(data):
    if len(data)<52 or data[:6]!=b'\x7fELF\x01\x01': fail('Expected ELF32 little-endian module')
    h=struct.unpack_from('<16sHHIIIIIHHHHHH',data)
    if h[1:3]!=(1,40) or h[11]!=40 or h[6]+40*h[12]>len(data): fail('Invalid ARM module header')
    sections=[struct.unpack_from('<IIIIIIIIII',data,h[6]+40*i) for i in range(h[12])]
    def area(s):
        if s[4]+s[5]>len(data): fail('ELF section exceeds module bytes')
        return data[s[4]:s[4]+s[5]]
    def string(table, offset):
        end=table.find(b'\0',offset)
        if offset>=len(table) or end<0: fail('Invalid ELF string')
        return table[offset:end].decode('ascii')
    names=area(sections[h[13]])
    modinfo=None; required=[]; actual=set()
    for s in sections:
        if string(names,s[0])=='.modinfo': modinfo=area(s)
        if s[1]==2:
            if s[9]!=16 or s[5]%16 or s[6]>=len(sections): fail('Invalid ELF symbol table')
            tab=area(sections[s[6]]); body=area(s)
            for pos in range(0,len(body),16):
                name,_,_,info,_,index=struct.unpack_from('<IIIBBH',body,pos)
                if name and index==0 and info>>4!=2: required.append(string(tab,name))
        if s[1]==9:
            if s[9]!=8 or s[5]%8: fail('Invalid ARM relocation table')
            for pos in range(0,s[5],8):
                _,info=struct.unpack_from('<II',area(s),pos); kind=info&255
                if kind not in RELOCS: fail('Unreviewed ARM relocation in selected USB module')
                actual.add(RELOCS[kind])
    if not required or modinfo is None or not actual: fail('Missing required module metadata')
    attrs={}
    for item in modinfo.split(b'\0'):
        if b'=' in item:
            key,value=item.split(b'=',1);attrs.setdefault(key.decode(),[]).append(value.decode())
    return required,actual,attrs

def checked_build(root,expected_receipt_sha256=EXPECTED_RECEIPT):
    root=Path(root); receipt=(root/'build-result.json').read_bytes(); record=json.loads(receipt)
    if sha(receipt)!=expected_receipt_sha256: fail('Build receipt differs from the explicitly selected kernel build')
    if record.get('profile')!='android-core' or record.get('kcflags')!='-fno-pic -fno-pie' or \
       record.get('source_lock',{}).get('kernel',{}).get('member_sha256')!=SOURCE_SHA:
        fail('Build is not the pinned Amazon Android kernel')
    artifacts={}
    for name in ('kernel.config','kernel.release','modules.tar.gz','Module.symvers','module-relocations.json','zImage'):
        data=(root/name).read_bytes();entry=record.get('artifacts',{}).get(name,{})
        if len(data)!=entry.get('bytes') or sha(data)!=entry.get('sha256'): fail('Artifact hash mismatch: '+name)
        artifacts[name]=data
    config={}
    for line in artifacts['kernel.config'].decode().splitlines():
        m=re.fullmatch(r'(CONFIG_\w+)=(.*)',line); n=re.fullmatch(r'# (CONFIG_\w+) is not set',line)
        if m: config[m[1]]=m[2]
        if n: config[n[1]]='n'
    for name,value in CONFIG.items():
        if config.get(name,'n')!=value: fail('USB configuration mismatch: '+name)
    release=artifacts['kernel.release'].decode().strip()
    if not re.fullmatch(r'[A-Za-z0-9_.+\-]+',release): fail('Invalid kernel release')
    prefix='lib/modules/'+release+'/'
    with tarfile.open(fileobj=io.BytesIO(artifacts['modules.tar.gz']),mode='r:gz') as archive:
        members=archive.getmembers(); by_name={m.name:m for m in members}
        if len(by_name)!=len(members): fail('Duplicate archive paths')
        def member(name):
            m=by_name.get(prefix+name)
            if not m or not m.isfile() or m.size>16*1024*1024: fail('Missing/unsafe module archive input')
            return archive.extractfile(m).read()
        deps={}
        for line in member('modules.dep').decode().splitlines():
            if not line.strip(): continue
            if ':' not in line: fail('Malformed modules.dep')
            name,items=line.split(':',1)
            if name in deps: fail('Duplicate dependency entry')
            deps[name]=items.split()
        expected={path:[] for path in MODULES.values()}
        expected[MODULES['g_android']]=[MODULES['arcotg_udc']]
        expected[MODULES['ath6kl_sdio']]=[MODULES['cfg80211']]
        if any(deps.get(k)!=v for k,v in expected.items()): fail('Unreviewed USB ELF dependency graph')
        modules={name:member(path) for name,path in MODULES.items()}
    exports={}
    for line in artifacts['Module.symvers'].decode().splitlines():
        fields=line.split()
        if len(fields)>=3:
            if fields[1] in exports and exports[fields[1]]!=fields[2]: fail('Ambiguous exported symbol provider')
            exports[fields[1]]=fields[2]
    audit=json.loads(artifacts['module-relocations.json']); metadata={}
    for name,data in modules.items():
        required,relocs,attrs=elf(data)
        if len(attrs.get('vermagic',[]))!=1 or attrs['vermagic'][0].split()[0]!=release: fail('USB module vermagic mismatch')
        if attrs.get('license')!=(['Dual BSD/GPL'] if name=='ath6kl_sdio' else ['GPL']): fail('Unexpected module license declaration')
        providers={'vmlinux'}|{p.removeprefix('kernel/').removesuffix('.ko') for p in expected[MODULES[name]]}
        if any(exports.get(symbol) not in providers for symbol in required): fail('USB symbol missing or supplied outside module closure')
        selected=[x for x in audit['modules'] if x['module']==MODULES[name].removeprefix('kernel/')]
        if len(selected)!=1 or selected[0].get('unsupported')!=[] or set(selected[0].get('relocations',[]))!=relocs:
            fail('ARM relocation receipt does not match actual module')
        parameters={x.split(':',1)[0] for x in attrs.get('parm',[])}
        # The Android gadget takes its strings, IDs and functions from android_usb in sysfs;
        # its only parameters are composite.c's generic ones, which init never sets.
        if name=='g_android' and parameters!={'idVendor','idProduct','bcdDevice','iManufacturer','iProduct','iSerialNumber'}:
            fail('Compiled Android gadget parameter contract differs')
        metadata[name]={'path':MODULES[name],'bytes':len(data),'sha256':sha(data),
            'license':attrs['license'][0],'vermagic':attrs['vermagic'][0],
            'undefined_symbols_checked':len(required),'relocations':sorted(relocs),'elf_dependencies':expected[MODULES[name]]}
    # Refuse a build changed in place during this read.
    if (root/'build-result.json').read_bytes()!=receipt: fail('Build receipt changed during verification')
    proof={'receipt_sha256':sha(receipt),'patch_count':len(record.get('patches',{})),
        'patches':record.get('patches',{}),'kernel_release':release,
        'artifacts':{n:{'bytes':len(d),'sha256':sha(d)} for n,d in artifacts.items()},
        'source':record['source_lock']['kernel'],'cross_compiler':record['source_lock']['cross_compiler'],
        'modules':metadata}
    return modules,proof

def module_package(name):
    """The build module that installs /vendor/lib/modules/NAME.ko."""
    return 'pw3-kmod-'+name.replace('_','-')

def export(build_dir,output,expected_receipt_sha256=EXPECTED_RECEIPT):
    output=Path(output).resolve()
    if output.exists(): fail('Output already exists')
    modules,proof=checked_build(build_dir,expected_receipt_sha256)
    output.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.usb-stage-',dir=output.parent) as temp:
        stage=Path(temp)/'component';stage.mkdir()
        for name,data in modules.items():
            target=stage/'vendor/lib/modules'/(name+'.ko');target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(data)
        for name in ('init.pw3.usb.modules.rc','init.pw3.hardware.rc','init.pw3.epdc.rc'):
            target=stage/'vendor/etc/init'/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(HERE/name,target)
        shutil.copyfile(HERE/'ueventd.pw3.fragment.rc',stage/'vendor/etc/ueventd.pw3.fragment.rc')
        for directory in ('native','policy','userspace-patch'):
            for source in sorted((HERE/directory).rglob('*')):
                if not source.is_file() or '__pycache__' in source.parts or 'patched-source' in source.parts: continue
                dest=stage/source.relative_to(HERE);dest.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(source,dest)
        shutil.copyfile(HERE/'COPYING.kernel',stage/'COPYING.kernel')
        shutil.copyfile(HERE/'README.md',stage/'README.md')
        # The modules are prebuilt modules rather than copied files, so that each
        # carries the kernel's licence into the image's legal notices
        # (/system/etc/NOTICE.html.gz, Settings > About > Legal information).
        # Before native/Android.mk, which sets LOCAL_PATH to its own directory.
        (stage/'Android.mk').write_text('LOCAL_PATH := $(call my-dir)\n'+''.join(
            '\ninclude $(CLEAR_VARS)\nLOCAL_MODULE := '+module_package(name)+'\nLOCAL_MODULE_CLASS := ETC\n'
            'LOCAL_MODULE_STEM := '+name+'.ko\nLOCAL_SRC_FILES := vendor/lib/modules/'+name+'.ko\n'
            'LOCAL_MODULE_PATH := $(TARGET_OUT_VENDOR)/lib/modules\nLOCAL_NOTICE_FILE := $(LOCAL_PATH)/COPYING.kernel\n'
            'include $(BUILD_PREBUILT)\n' for name in sorted(modules))+'\ninclude $(LOCAL_PATH)/native/Android.mk\n')
        copies=[p for p in sorted((stage/'vendor').rglob('*')) if p.is_file() and p.suffix!='.ko']
        (stage/'pw3-usb-component.mk').write_text('''ifndef PW3_USB_COMPONENT_ROOT
$(error Set PW3_USB_COMPONENT_ROOT to the verified public USB component)
endif
PRODUCT_PACKAGES += pw3-usb-check '''+' '.join(module_package(n) for n in sorted(modules))+'''
PRODUCT_COPY_FILES += \\
'''+ ' \\\n'.join('    $(PW3_USB_COMPONENT_ROOT)/'+str(p.relative_to(stage))+':$(TARGET_COPY_OUT_VENDOR)/'+str(p.relative_to(stage/'vendor')) for p in copies)+'\n')
        (stage/'build-verification.json').write_text(json.dumps(proof,indent=2)+'\n')
        manifest={'schema':1,'private':False,'contains_private_firmware':False,'contains_keys':False,
            'bootable_bundle':False,'device_runtime_verified':False,'build_receipt_sha256':proof['receipt_sha256'],
            'usb_load_order':['fsl_otg_arc','arcotg_udc','g_android'],
            'boot_order_groups':[['cyttsp4_core','cyttsp4_i2c','cyttsp4_mt_b','cfg80211'],['mxc_epdc_fb']],
            'wifi_load':'ath6kl_sdio only when explicitly requested by wificond',
            'private_firmware_installation_required':['ath6k/AR6003/hw2.1.1/board.bin','ath6k/AR6003/hw2.1.1/fw-4.bin','epdc/pw3/waveform.pw3.gz'],
            'runtime_dependencies':[{'consumer':'arcotg_udc','provider':'fsl_otg_arc','reason':'otg_get_transceiver requires registered OTG platform driver'}],
            'usb_default':'every boot: MTP; adb only when Android turns it on (developer options), with authentication',
            'serial':'16 bytes from urandom once per device, hex encoded, kept in persist.pw3.usb.serial; no factory ID, not an authentication secret',
            'not_proven':['MTP and adb through the Android gadget on the device','RSA pairing UI','USB suspend behavior'],
            'files':[{'path':str(p.relative_to(stage)),'bytes':p.stat().st_size,'sha256':sha(p.read_bytes())} for p in sorted(stage.rglob('*')) if p.is_file()]}
        (stage/'component-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
        stage.rename(output)
    return manifest

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir',required=True);parser.add_argument('--output',required=True)
    parser.add_argument('--expected-receipt-sha256',default=EXPECTED_RECEIPT)
    args=parser.parse_args()
    try:
        result=export(args.build_dir,args.output,args.expected_receipt_sha256)
        print(json.dumps({'staged':True,'build_receipt_sha256':result['build_receipt_sha256'],'device_runtime_verified':False}))
    except (ValueError,OSError,KeyError,IndexError,UnicodeError,struct.error,tarfile.TarError) as error:
        print(json.dumps({'staged':False,'error':str(error) if isinstance(error,ValueError) else 'Unreadable or invalid build input'}))
        raise SystemExit(1)
