#!/usr/bin/env python3
"""Bound the pinned ARM gzip decompressor's RAM footprint from real build files.
No hardware input or access; not a DMA, cache, loader or recovery validation.
"""
import argparse,hashlib,json,os,platform,struct,subprocess,zlib
from pathlib import Path

def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def require(ok,msg):
    if not ok:raise RuntimeError(msg)
def elf_sections(p):
    d=p.read_bytes();require(d[:6]==b'\x7fELF\x01\x01' and struct.unpack_from('<H',d,18)[0]==40,'Not ARM32 LE ELF')
    offset=struct.unpack_from('<I',d,32)[0]
    size,count,names=struct.unpack_from('<HHH',d,46)
    require(size==40 and 0<names<count<4096 and offset+count*size<=len(d),'Invalid ELF sections')
    table=[struct.unpack_from('<10I',d,offset+i*size) for i in range(count)]
    st=table[names];strings=d[st[4]:st[4]+st[5]];out={}
    for h in table:
        require(h[0]<len(strings),'Invalid ELF section name')
        end=strings.find(b'\0',h[0]);require(end>=h[0],'Unterminated ELF section name')
        name=strings[h[0]:end].decode('ascii')
        if name:out[name]={'type':h[1],'flags':h[2],'address':h[3],'offset':h[4],'bytes':h[5]}
    return out

def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--output',type=Path,required=True)
    ap.add_argument('--profile',choices=['pw3','vexpress'],default='pw3');a=ap.parse_args()
    require(platform.node()=='lima-pw3' and os.geteuid()!=0,'Use project VM builder')
    virtual=a.profile=='vexpress';flavor='loader-stage' if virtual else 'android-core'
    out=Path('/home/builder/pw3-build/out-'+flavor);source=Path('/home/builder/pw3-build/linux-'+flavor)
    export=Path('/mnt/build-share/'+flavor+'-kernel');prefix='/home/builder/toolchains/aosp-arm-gcc49/bin/arm-linux-androideabi-'
    base=0x60000000 if virtual else 0x80000000
    locked={'arch/arm/boot/compressed/head.S':'7cba08a3c830058b537183f9c7dd1def7e6a6c7939e96bb5bff32091188e0a52',
            'arch/arm/boot/compressed/vmlinux.lds.in':'ec0fec295f09aa3c12ab52d793344f275469b3d8a725423bf00233a881d82221'}
    require(all(sha(source/k)==v for k,v in locked.items()),'Decompressor source changed; formula review required')
    files={n:out/n for n in ['.config','vmlinux','arch/arm/boot/compressed/vmlinux','arch/arm/boot/zImage','arch/arm/boot/Image']}
    hashes={n:sha(p) for n,p in files.items()}
    require(hashes['arch/arm/boot/zImage']==sha(export/'zImage') and hashes['.config']==sha(export/'kernel.config'),'Export/build mismatch')
    config=files['.config'].read_text().splitlines()
    require('CONFIG_KERNEL_GZIP=y' in config and '# CONFIG_AUTO_ZRELADDR is not set' in config and
            '# CONFIG_THUMB2_KERNEL is not set' in config and 'CONFIG_ZBOOT_ROM_TEXT=0x0' in config and
            'CONFIG_ZBOOT_ROM_BSS=0x0' in config and 'CONFIG_CPU_BIG_ENDIAN=y' not in config,'Unsupported decompressor configuration')
    def symbols(p):
        text=subprocess.check_output([prefix+'nm','-an',str(p)],text=True);r={}
        for line in text.splitlines():
            parts=line.split()
            if len(parts)==3:
                try:r[parts[2]]=int(parts[0],16)
                except ValueError:pass
        return r
    compressed=files['arch/arm/boot/compressed/vmlinux'];sym=symbols(compressed);kernel=symbols(files['vmlinux'])
    sections=elf_sections(compressed);stack=sections['.stack'];stack_end=stack['address']+stack['bytes']
    require(stack['type']==8 and stack['bytes']==4096 and stack['address']>=sym['_end'],'Unexpected stack layout')
    require(all(v['address']+v['bytes']<=stack_end for v in sections.values() if v['flags']&2),'Unexpected allocated section beyond stack')
    zimage=files['arch/arm/boot/zImage'].read_bytes();raw=files['arch/arm/boot/Image'].read_bytes()
    require(sym['start']==0 and sym['_text']==0 and sym['_edata']==len(zimage),'Unsupported zImage link layout')
    require(struct.unpack_from('<III',zimage,0x24)==(0x016f2818,0,len(zimage)),'zImage header mismatch')
    require(sym['zreladdr']==base+0x8000 and kernel['_stext']==0xc0008000,'Unexpected output base')
    compressed_bytes=zimage[sym['input_data']:sym['input_data_end']]
    dec=zlib.decompressobj(31);got=dec.decompress(compressed_bytes)+dec.flush()
    require(dec.eof and not dec.unused_data and got==raw,'Actual gzip content does not match built Image')
    require(struct.unpack_from('<I',compressed_bytes,len(compressed_bytes)-4)[0]==len(raw),'Decompressor size trailer mismatch')
    # All expressions reproduce the locked ARM instructions, including rounding.
    entry=base+0x8000;image_end=entry+len(raw);copy_offset=sym['restart']&~31
    reloc_padding=(sym['reloc_code_end']-sym['restart']+256)&~255
    relocated_start=(image_end+reloc_padding)&~255
    copied_bytes=(sym['_edata']-copy_offset+31)&~31
    delta=relocated_start-(entry+copy_offset)
    kernel_end=kernel['_end']-0xc0000000+base
    pgdir=(entry-16384)&~16383
    ranges=[{'name':'kernel output and BSS','start':entry,'end':kernel_end},
            {'name':'decompressor page directory','start':pgdir,'end':pgdir+16384},
            {'name':'initial compressed image, BSS, stack and malloc','start':entry,'end':entry+stack_end+65536},
            {'name':'relocated compressed image','start':relocated_start,'end':relocated_start+copied_bytes},
            {'name':'relocated BSS, stack and malloc','start':entry+sym['__bss_start']+delta,'end':entry+stack_end+delta+65536}]
    start=min(r['start'] for r in ranges)&~4095;end=(max(r['end'] for r in ranges)+4095)&~4095
    require(all(base+0x4000<=r['start']<r['end']<base+0x4000000 for r in ranges),'Footprint collides with tags or proposed initramfs')
    require(all(sha(p)==hashes[n] for n,p in files.items()),'Build inputs changed during extraction')
    result={'schema':'pw3-kernel-footprint-v1','profile':a.profile,'input_sha256':hashes,'locked_source_sha256':locked,
            'entry':entry,'machine_id':2272 if virtual else 4091,'atag_destination':base+0x1000,'initramfs_destination_candidate':base+0x4000000,
            'staging_lower_bound':base+0x8000000,'zimage_bytes':len(zimage),'inflated_image_bytes':len(raw),
            'gzip_matches_real_image':True,'ranges':ranges,'page_rounded_envelope':{'start':start,'bytes':end-start,'end':end},
            'decompressor_copy_delta':delta,'decompressor_copy_bytes':copied_bytes,
            'no_overlap_with_atag_page_or_initramfs_candidate':True,'derived_from_actual_elf_and_pinned_source':True,
            'physical_memory_map_bound':False,'staged_object_bound':False,'dma_cache_or_recovery_verified':False,'execute_allowed':False,
            'scope':'Static byte/range contract only; no physical boot or executed ARM decompressor test',
            'script_sha256':sha(Path(__file__))}
    a.output.mkdir(exist_ok=False);(a.output/'footprint.json').write_text(json.dumps(result,indent=2)+'\n')
    dis=subprocess.check_output([prefix+'objdump','-d','--start-address=0','--stop-address='+str(sym['reloc_code_end']),str(compressed)],text=True)
    (a.output/'decompressor-disassembly.txt').write_text(dis)
    print(json.dumps({k:result[k] for k in ['schema','zimage_bytes','inflated_image_bytes','page_rounded_envelope','no_overlap_with_atag_page_or_initramfs_candidate','execute_allowed']},indent=2))
if __name__=='__main__':main()
