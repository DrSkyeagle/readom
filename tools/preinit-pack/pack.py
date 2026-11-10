#!/usr/bin/env python3
"""Reads a built ramdisk and checks it against the contracts this port requires.

It never probes, mounts, repairs or writes a device. The bundle assembler that
used to live here packed the ram_boot=1 pre-init, and went with that mode on
2026-10-29; what is left is the library build_entry.py, export-aosp-images.py,
inspect-system-image.py and the smoke tools read it for.
"""
from __future__ import annotations
import gzip, hashlib, io, stat, struct
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

MAX_RAMDISK = 96 * 1024 * 1024
# Every entry contract of pw3-testfile-init that has been reviewed, oldest
# first, and whether it maps the image files by extent. Two checks read this and
# they used to read two copies of it: one asked "is exactly one of these in the
# binary", the other "does this one use extents", and a contract added to the
# second and not the first refused every export until someone noticed (V60).
# The contract is the string in TestFileInit.h, and the entry carrying it is
# what build_entry.py puts in the launcher. There is one, because there is one
# way this port mounts storage. V1 to V9 described entries that mounted /data
# through a loop device, or formatted it fresh every boot, or wanted five
# command-line keys; all of those went with the modes they belonged to on
# 2026-10-29, and a ramdisk built from one of them cannot boot against the
# command line this tree writes. Raising this to V12 added the read-only view
# of the stock store's root (the preinit binds it beside documents/ and covers
# its android/ again); V13 is the private/128gb-storage branch: the 3.5 GiB
# data image, the installer growing a kept image's file and the entry growing
# the filesystem inside it before Android mounts it, and that root view made
# read-write while android/ and documents/ keep their read-only mounts. It is
# one edit here and one in TestFileInit.h, and the packer refuses anything that
# is not it.
CURRENT_TESTFILE_CONTRACT = b'PW3_TESTFILE_INIT_V13_DM_CLUSTER_EXTENTS_FAT_READONLY_VERIFY_THEN_RW_ANDROID_TMPFS_COVERED_KINDLE_ROOT_VIEW_READ_WRITE_DATA_IMAGE_GROWN_IN_PLACE_DATA_SIZED_IMAGES_SYSTEM_STAMP_BACKGROUND_RECHECK_FAT_CROSSLINK_OPTIONAL_REPORT_AREA_IMAGE_OWNS_APP_CODE_THREE_KEY_CMDLINE_NO_PARTITIONING'
TESTFILE_CONTRACTS = (CURRENT_TESTFILE_CONTRACT,)
class Invalid(ValueError): pass

def require(ok, message):
    if not ok: raise Invalid(message)

def sha(path: Path) -> str:
    h = hashlib.sha256()
    with path.open('rb') as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b''): h.update(chunk)
    return h.hexdigest()

def bounded(path: Path, limit: int) -> bytes:
    require(path.is_file() and not path.is_symlink(), f'missing or non-regular input: {path.name}')
    with path.open('rb') as f: data = f.read(limit + 1)
    require(len(data) <= limit, f'input exceeds limit: {path.name}')
    return data

def ext4_info(path: Path) -> dict:
    size = path.stat().st_size
    require(1024*1024 <= size <= 2**32-1 and size % 512 == 0, 'raw system size cannot be stored as the expected FAT file')
    with path.open('rb') as f:
        first = f.read(4); f.seek(1024); s = f.read(1024)
    require(first != struct.pack('<I',0xed26ff3a), 'Android sparse image must be converted with simg2img')
    require(len(s)==1024 and struct.unpack_from('<H',s,0x38)[0]==0xef53, 'raw ext4 superblock magic missing')
    state=struct.unpack_from('<H',s,0x3a)[0]; log=struct.unpack_from('<I',s,0x18)[0]
    incompat=struct.unpack_from('<I',s,0x60)[0]; ro=struct.unpack_from('<I',s,0x64)[0]
    orphan=struct.unpack_from('<I',s,0xe8)[0]
    require(state==1 and not (incompat & 4) and orphan==0, 'ext4 is not clean; repairs are prohibited')
    require(log<=2 and not incompat & ~0x2d2 and not ro & ~0x7f, 'ext4 features exceed the reviewed PW3 3.0.35 subset')
    block_size=1024 << log; count=struct.unpack_from('<I',s,4)[0]
    if incompat & 0x80: count |= struct.unpack_from('<I',s,0x150)[0] << 32
    require(count>0 and count*block_size==size, 'ext4 block count does not equal fixed raw file size')
    return {'bytes':size,'block_size':block_size,'blocks':count,'state':state,
            'incompat_features':incompat,'ro_compat_features':ro,'last_orphan':orphan,'sha256':sha(path)}

def static_arm_elf(data: bytes, name: str):
    require(len(data)>=52 and data[:7]==b'\x7fELF\x01\x01\x01', f'{name} must be ELF32 little-endian')
    typ,machine=struct.unpack_from('<HH',data,16)
    require(machine==40 and typ in (2,3), f'{name} is not an ARM executable')
    phoff=struct.unpack_from('<I',data,28)[0]; entsz,count=struct.unpack_from('<HH',data,42)
    require(entsz==32 and 0<count<128 and phoff+entsz*count<=len(data), f'{name} has invalid program headers')
    require(struct.unpack_from('<I',data,36)[0] & 0xff000000 == 0x05000000, f'{name} is not ARM EABI5')
    entry=struct.unpack_from('<I',data,24)[0]; executable_entry=False
    for i in range(count):
        ptype,offset,vaddr,paddr,filesz,memsz,flags,align=struct.unpack_from('<8I',data,phoff+i*entsz)
        require(ptype!=3, f'{name} has a dynamic interpreter; a static executable is required')
        if ptype==1:
            require(filesz<=memsz and offset+filesz<=len(data),f'{name} has an invalid load segment')
            if flags & 1 and vaddr <= (entry & ~1) < vaddr+memsz: executable_entry=True
    require(executable_entry,f'{name} entry is not inside an executable load segment')

@dataclass
class Entry:
    name: str
    mode: int
    uid: int = 0
    gid: int = 0
    data: bytes = b''
    rdevmajor: int = 0
    rdevminor: int = 0

# The log channels a PID 1 entry opens before it can mount anything. They are
# here rather than in one packer because every path that ships an entry needs
# them: without the nodes the entry's first stop() reports to nowhere, since
# /dev/kmsg was only ever made from sysfs and sysfs is one of the mounts that
# can fail -- and the kernel log is the only channel that reaches a person (the
# fixed physical address in log_pos_goal, read back by the stock side after the
# next boot). A node in the ramdisk needs no mount at all. It also lets the
# kernel's own initial open of /dev/console succeed, so PID 1 starts with 0/1/2
# bound to the console rather than free for the next open() to claim.
#
# The numbers are the kernel's own, read out of the tree this project compiles:
# MEM_MAJOR 1 with minor 11 for kmsg (include/linux/major.h, drivers/char/mem.c),
# TTYAUX_MAJOR 5 with minor 1 for console (include/linux/major.h, tty_io.c).
# The tmpfs mounted over /dev later hides them; open descriptors stay valid.
LOG_NODES = (('dev/kmsg', 1, 11), ('dev/console', 5, 1))

def log_nodes() -> list[Entry]:
    return [Entry(name, stat.S_IFCHR | 0o600, 0, 0, b'', major, minor)
            for name, major, minor in LOG_NODES]

def verify_log_nodes(entries: list[Entry]) -> None:
    """Exact metadata for both nodes, on the archive that was actually written."""
    files={e.name:e for e in entries}
    for name,major,minor in LOG_NODES:
        node=files.get(name)
        require(node is not None,f'ramdisk lacks {name}, so a storage entry cannot report an early failure')
        require(stat.S_ISCHR(node.mode) and node.mode & 0o7777 == 0o600 and not node.uid and not node.gid
                and not node.data and (node.rdevmajor,node.rdevminor)==(major,minor),
                f'{name} must be a root-owned character device {major}:{minor} with mode 0600 and no data')
    parent=files.get('dev')
    require(parent is not None and stat.S_ISDIR(parent.mode),'ramdisk lacks the /dev directory holding the log nodes')

def read_ramdisk(path: Path) -> list[Entry]:
    return read_ramdisk_bytes(bounded(path,MAX_RAMDISK))

def read_ramdisk_bytes(packed: bytes) -> list[Entry]:
    if packed[:2]==b'\x1f\x8b':
        with gzip.GzipFile(fileobj=io.BytesIO(packed)) as f: data=f.read(MAX_RAMDISK+1)
    else: data=packed
    require(len(data)<=MAX_RAMDISK,'uncompressed ramdisk exceeds review limit')
    at=0; entries=[]; seen=set(); trailer=False
    while at+110<=len(data):
        require(data[at:at+6]==b'070701','ramdisk must use newc cpio, not CRC/legacy cpio')
        try: vals=[int(data[at+6+i*8:at+14+i*8],16) for i in range(13)]
        except ValueError as e: raise Invalid('invalid cpio header') from e
        ino,mode,uid,gid,nlink,mtime,size,devmaj,devmin,rmaj,rmin,namesz,check=vals
        require(0<namesz<=4096 and at+110+namesz<=len(data),'invalid cpio name length')
        namebytes=data[at+110:at+110+namesz]
        require(namebytes[-1:]==b'\0' and b'\0' not in namebytes[:-1],'invalid cpio filename')
        try: name=namebytes[:-1].decode('utf-8')
        except UnicodeDecodeError as e: raise Invalid('non-UTF8 cpio name') from e
        at=(at+110+namesz+3)&~3
        require(at+size<=len(data),'truncated cpio member')
        body=data[at:at+size];at=(at+size+3)&~3
        if name=='TRAILER!!!': require(size==0,'invalid cpio trailer');trailer=True;break
        if name in ('.','./'): require(stat.S_ISDIR(mode),'invalid cpio root entry');continue
        if name.startswith('./'):name=name[2:]
        require(not name.startswith('/') and all(p not in ('','.', '..') for p in name.split('/')),'unsafe cpio member path')
        require(name not in seen and len(entries)<20000,'duplicate/too many cpio entries');seen.add(name)
        require(stat.S_IFMT(mode) in (stat.S_IFREG,stat.S_IFDIR,stat.S_IFLNK,stat.S_IFCHR,stat.S_IFBLK,stat.S_IFIFO),'unsupported cpio member type')
        require(not stat.S_ISREG(mode) or nlink==1,'hardlinked regular cpio members require explicit support')
        require(stat.S_ISREG(mode) or stat.S_ISLNK(mode) or size==0,'unexpected data in cpio special member')
        entries.append(Entry(name,mode,uid,gid,body,rmaj,rmin))
    require(trailer and not data[at:].strip(b'\0'),'missing trailer or appended cpio archive')
    # A later member must never traverse a symlink from an earlier member.
    mapping={e.name:e for e in entries}
    for e in entries:
        parent=PurePosixPath(e.name).parent
        while str(parent)!='.':
            require(str(parent) not in mapping or stat.S_ISDIR(mapping[str(parent)].mode),'cpio member has a non-directory ancestor')
            parent=parent.parent
    return entries

def cpio_bytes(entries: list[Entry]) -> bytes:
    output=bytearray()
    for ino,e in enumerate([*sorted(entries,key=lambda x:(x.name.count('/'),x.name)),Entry('TRAILER!!!',0)],1):
        name=e.name.encode()+b'\0';vals=[ino,e.mode,e.uid,e.gid,1,0,len(e.data),0,0,e.rdevmajor,e.rdevminor,len(name),0]
        output+=b'070701'+''.join(f'{v:08x}' for v in vals).encode()+name
        output+=b'\0'*((-len(output))%4);output+=e.data;output+=b'\0'*((-len(output))%4)
    output+=b'\0'*((-len(output))%512)
    return bytes(output)

def verify_ramdisk_fstab(entries: list[Entry]) -> None:
    """Accept only exact reviewed metadata, without autoformat or repair flags.

    There is one data arrangement: an extent of the data image on the FAT user
    store, mapped as dm-2. The dedicated-partition table went with the
    storage=ext4 mode on 2026-10-29.
    """
    files = {e.name: e for e in entries}
    test_entry = files.get('pw3-testfile-init')
    if 'fstab.pw3-testfile' in files:
        require(test_entry is not None and CURRENT_TESTFILE_CONTRACT in test_entry.data,
                'testfile metadata requires its matching native entry contract')
    expected = {
        'fstab.pw3': [
            ['tmpfs', '/data', 'tmpfs', 'nosuid,nodev,size=128m', 'notrim'],
            ['tmpfs', '/cache', 'tmpfs', 'nosuid,nodev,size=16m', 'notrim'],
        ],
        'fstab.pw3-testfile': [
            ['/dev/block/dm-2', '/data', 'ext4',
             'noatime,nosuid,nodev,errors=remount-ro,data=ordered,barrier=1', 'notrim'],
            ['tmpfs', '/cache', 'tmpfs', 'nosuid,nodev,size=16m', 'notrim'],
        ],
        # Swap in RAM only (zram): no storage device may be named here.
        'fstab.pw3-swap': [
            ['/dev/block/zram0', 'none', 'swap', 'defaults', 'zramsize=268435456,max_comp_streams=-1'],
        ],
    }
    for entry in entries:
        if not PurePosixPath(entry.name).name.startswith('fstab'):
            continue
        require(stat.S_ISREG(entry.mode) and entry.uid == 0 and entry.gid == 0 and
                not (entry.mode & 0o022), 'ramdisk fstab must be a root-owned non-writable regular file')
        if not entry.data.strip():
            continue
        require(entry.name in expected, 'unreviewed ramdisk fstab path')
        try:
            lines = entry.data.decode('ascii').splitlines()
        except UnicodeDecodeError as error:
            raise Invalid('ramdisk fstab must be ASCII') from error
        records = [line.split() for line in lines if line.strip() and not line.lstrip().startswith('#')]
        require(records == expected[entry.name], 'ramdisk fstab differs from reviewed profile metadata')

def verify_ramdisk(entries: list[Entry]) -> dict:
    files={e.name:e for e in entries}
    required=['init','default.prop','init.pw3-main.rc','init.pw3.rc','init.pw3.usb.rc','ueventd.pw3.rc','sepolicy']
    for name in required:require(name in files and stat.S_ISREG(files[name].mode) and files[name].data, f'actual built ramdisk lacks {name}')
    static_arm_elf(files['init'].data,'AOSP init')
    require(files['init'].mode & 0o111,'AOSP init is not executable')
    if 'pw3-testfile-init' in files:
        entry = files['pw3-testfile-init']
        require(stat.S_ISREG(entry.mode) and entry.uid == 0 and entry.gid == 0 and
                entry.mode & 0o100 and not entry.mode & 0o022,
                'testfile pre-init must be a root-owned executable, not a data file')
        static_arm_elf(entry.data, 'testfile pre-init')
        require(sum(marker in entry.data for marker in TESTFILE_CONTRACTS) == 1,
                'testfile pre-init lacks one exact reviewed entry contract')
    for folder in ['dev','proc','sys','system']:
        require(folder in files and stat.S_ISDIR(files[folder].mode),f'actual ramdisk lacks directory {folder}')
    props={}
    for line in files['default.prop'].data.decode().splitlines():
        if '=' in line and not line.lstrip().startswith('#'):
            key,value=line.split('=',1);props.setdefault(key.strip(),[]).append(value.strip())
    for key in ['ro.secure','ro.adb.secure']:require(props.get(key)==['1'],f'ramdisk must contain exactly one {key}=1')
    require(b'on property:sys.pw3.storage_ready=1' in files['init.pw3-main.rc'].data and
            b'pw3-verify-storage' in files['init.pw3-main.rc'].data,'PW3 storage gate missing from built main rc')
    require(struct.unpack_from('<I',files['sepolicy'].data)[0]==0xf97cff8c,'built SELinux policy magic missing')
    verify_ramdisk_fstab(entries)
    require('pw3-preinit.conf' not in files,'ramdisk already contains bundle-specific pre-init configuration; refusing ambiguous replacement')
    return files

