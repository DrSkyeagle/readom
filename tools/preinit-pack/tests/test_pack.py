#!/usr/bin/env python3
"""Format/negative tests use clearly synthetic fixtures; no boot package is produced."""
import base64, gzip, hashlib, importlib.util, json, stat, struct, sys, tempfile, unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('pw3_pack',ROOT/'pack.py');m=importlib.util.module_from_spec(spec);sys.modules[spec.name]=m;spec.loader.exec_module(m)
class PackerTests(unittest.TestCase):
    def setUp(self):self.tmp=tempfile.TemporaryDirectory(prefix='pw3-pack-format-test-');self.root=Path(self.tmp.name)
    def tearDown(self):self.tmp.cleanup()
    def write(self,path,data):
        p=self.root/path;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(data if isinstance(data,bytes) else data.encode());return p
    def ext4(self,**changes):
        p=self.write('not-a-filesystem-superblock-fixture.raw',b'\0'*(1024*1024));s=bytearray(1024)
        fields={4:('<I',256),0x18:('<I',2),0x38:('<H',0xef53),0x3a:('<H',1),0x60:('<I',0x42),0x64:('<I',3)}
        for off,(fmt,value) in fields.items():struct.pack_into(fmt,s,off,value)
        for off,(fmt,value) in changes.items():struct.pack_into(fmt,s,int(off),value)
        with p.open('r+b') as f:f.seek(1024);f.write(s)
        return p
    def test_ext4_header_only_metadata_not_fsck(self):self.assertEqual(m.ext4_info(self.ext4())['bytes'],1024*1024)
    def test_dirty_ext4_refuses(self):
        p=self.ext4();data=bytearray(p.read_bytes());struct.pack_into('<H',data,1024+0x3a,0);p.write_bytes(data)
        with self.assertRaises(m.Invalid):m.ext4_info(p)
    def test_recovery_feature_refuses(self):
        p=self.ext4();data=bytearray(p.read_bytes());struct.pack_into('<I',data,1024+0x60,0x46);p.write_bytes(data)
        with self.assertRaises(m.Invalid):m.ext4_info(p)
    def test_unknown_feature_refuses(self):
        p=self.ext4();data=bytearray(p.read_bytes());struct.pack_into('<I',data,1024+0x64,0x400);p.write_bytes(data)
        with self.assertRaises(m.Invalid):m.ext4_info(p)
    def test_sparse_never_accepted_as_raw(self):
        p=self.ext4()
        with p.open('r+b') as f:f.write(struct.pack('<I',0xed26ff3a))
        with self.assertRaises(m.Invalid):m.ext4_info(p)
    def test_size_mismatch_refuses(self):
        p=self.ext4()
        with p.open('ab') as f:f.write(b'\0'*4096)
        with self.assertRaises(m.Invalid):m.ext4_info(p)
    def test_cpio_roundtrip(self):
        es=[m.Entry('dev',stat.S_IFDIR|0o755),m.Entry('a',stat.S_IFREG|0o750,0,2000,b'fixture'),m.Entry('link',stat.S_IFLNK|0o777,data=b'a')]
        p=self.write('fixture.cpio',m.cpio_bytes(es));self.assertEqual({e.name:e for e in m.read_ramdisk(p)},{e.name:e for e in es})
    def test_cpio_traversal_refuses(self):
        p=self.write('bad.cpio',m.cpio_bytes([m.Entry('../escape',stat.S_IFREG|0o600,data=b'fixture')]))
        with self.assertRaises(m.Invalid):m.read_ramdisk(p)
    def test_cpio_symlink_ancestor_refuses(self):
        p=self.write('bad.cpio',m.cpio_bytes([m.Entry('x',stat.S_IFLNK|0o777,data=b'/tmp'),m.Entry('x/file',stat.S_IFREG|0o600,data=b'fixture')]))
        with self.assertRaises(m.Invalid):m.read_ramdisk(p)
class RamFstabTests(unittest.TestCase):
    def entry(self, data=None, **changes):
        values = dict(name='fstab.pw3', mode=stat.S_IFREG|0o644,
                      data=data if data is not None else b'tmpfs /data tmpfs nosuid,nodev,size=128m notrim\ntmpfs /cache tmpfs nosuid,nodev,size=16m notrim\n')
        values.update(changes)
        return m.Entry(**values)
    def test_reviewed_source_and_legacy_empty_accept(self):
        source=ROOT.parents[1]/'device/amazon/pw3/rootdir/fstab.pw3'
        m.verify_ramdisk_fstab([self.entry(source.read_bytes())])
        m.verify_ramdisk_fstab([self.entry(b'')])
        m.verify_ramdisk_fstab([])
    def test_persistent_devices_and_maintenance_flags_refuse(self):
        baseline=self.entry().data
        for old,new in [(b'tmpfs /data',b'/dev/mmcblk0p4 /data'),
                        (b'notrim',b'notrim,formattable'),(b'notrim',b'encryptable=footer'),
                        (b'notrim',b'voldmanaged=userdata:auto'),(b'notrim',b'wait'),
                        (b'/cache',b'/system'),(b'nosuid,nodev',b'defaults')]:
            with self.subTest(new=new), self.assertRaises(m.Invalid):
                m.verify_ramdisk_fstab([self.entry(baseline.replace(old,new))])
    def test_swap_metadata_names_only_the_ram_device(self):
        source=ROOT.parents[1]/'device/amazon/pw3/rootdir/fstab.pw3-swap'
        data=source.read_bytes()
        m.verify_ramdisk_fstab([self.entry(), self.entry(data,name='fstab.pw3-swap')])
        for old,new in [(b'/dev/block/zram0',b'/dev/block/mmcblk0p4'),(b'/dev/block/zram0',b'/dev/block/dm-2'),
                        (b'zramsize=268435456,',b'zramsize=268435456,formattable,'),(b',max_comp_streams=-1',b''),(b'swap',b'ext4')]:
            with self.subTest(new=new),self.assertRaises(m.Invalid):
                m.verify_ramdisk_fstab([self.entry(data.replace(old,new),name='fstab.pw3-swap')])
    def test_testfile_metadata_matches_native_entry_contract(self):
        source=ROOT.parents[1]/'device/amazon/pw3/rootdir/fstab.pw3-testfile'
        data=source.read_bytes()
        native=self.entry(m.CURRENT_TESTFILE_CONTRACT,name='pw3-testfile-init')
        metadata=self.entry(data,name='fstab.pw3-testfile')
        m.verify_ramdisk_fstab([native,metadata])
        for entries in [[metadata],[native,self.entry(data.replace(b'/dev/block/dm-2',b'/dev/block/loop2'),name='fstab.pw3-testfile')],
                        [native,self.entry(data.replace(b'notrim',b'notrim,formattable'),name='fstab.pw3-testfile')]]:
            with self.subTest(entries=entries),self.assertRaises(m.Invalid):m.verify_ramdisk_fstab(entries)
    def test_extra_missing_and_duplicate_records_refuse(self):
        baseline=self.entry().data
        for data in [baseline.splitlines(keepends=True)[0],baseline+baseline,
                     baseline+b'/dev/mmcblk0p1 /system ext4 ro wait\n']:
            with self.subTest(data=data), self.assertRaises(m.Invalid):
                m.verify_ramdisk_fstab([self.entry(data)])
    def test_alias_symlink_and_untrusted_owner_refuse(self):
        for change in [dict(name='etc/fstab.pw3'),dict(name='fstab.other'),
                       dict(mode=stat.S_IFLNK|0o777),dict(mode=stat.S_IFREG|0o666),
                       dict(uid=1000),dict(gid=1000)]:
            with self.subTest(change=change),self.assertRaises(m.Invalid):
                m.verify_ramdisk_fstab([self.entry(**change)])

class LogNodeTests(unittest.TestCase):
    """The char nodes a storage entry opens before it can mount anything.

    build_entry.py adds /dev/kmsg and /dev/console to the public initramfs and
    then reads the archive back to check them, so the device numbers have to
    survive cpio_bytes() and come out of read_ramdisk_bytes() unchanged.
    """
    def archive(self,*entries):
        return m.read_ramdisk_bytes(m.cpio_bytes([m.Entry('dev',stat.S_IFDIR|0o755),*entries]))
    def test_log_nodes_are_the_two_the_kernel_uses(self):
        """The generator's own output, against the numbers written out here.

        Checked against literals and not against LOG_NODES: building the pair by
        hand in this test and comparing it with itself is what let `major, minor`
        be swapped inside log_nodes() with every check still passing.
        """
        self.assertEqual([(e.name,e.mode,e.uid,e.gid,e.data,e.rdevmajor,e.rdevminor)
                          for e in m.log_nodes()],
                         [('dev/kmsg',stat.S_IFCHR|0o600,0,0,b'',1,11),
                          ('dev/console',stat.S_IFCHR|0o600,0,0,b'',5,1)])
    def test_verify_log_nodes_refuses_what_is_wrong_with_them(self):
        good=[m.Entry('dev',stat.S_IFDIR|0o755),*m.log_nodes()]
        m.verify_log_nodes(good)                      # the real pair passes
        def swapped(e):
            return m.Entry(e.name,e.mode,e.uid,e.gid,e.data,e.rdevminor,e.rdevmajor)
        for broken in ([e for e in good if e.name!='dev/console'],          # one missing
                       [e for e in good if e.name!='dev'],                  # no /dev
                       [swapped(e) if e.name=='dev/kmsg' else e for e in good],
                       [m.Entry(e.name,stat.S_IFREG|0o600,e.uid,e.gid,e.data,0,0)
                        if e.name=='dev/kmsg' else e for e in good],        # a plain file
                       [m.Entry(e.name,e.mode,0,0,b'',e.rdevmajor,e.rdevminor)
                        if e.name!='dev/kmsg' else
                        m.Entry(e.name,stat.S_IFCHR|0o666,e.uid,e.gid,e.data,e.rdevmajor,e.rdevminor)
                        for e in good]):               # mode 0666
            with self.assertRaises(m.Invalid):
                m.verify_log_nodes(broken)
    def test_char_node_device_numbers_survive_the_round_trip(self):
        nodes=m.log_nodes()
        back={e.name:e for e in self.archive(*nodes)}
        for want in nodes:
            got=back[want.name]
            self.assertEqual((got.mode,got.uid,got.gid,got.data,got.rdevmajor,got.rdevminor),
                             (want.mode,want.uid,want.gid,b'',want.rdevmajor,want.rdevminor))
        # And the numbers that come back are the kernel's, written out here: a
        # round trip through one implementation can be wrong at both ends.
        self.assertEqual([(e.name,back[e.name].rdevmajor,back[e.name].rdevminor) for e in nodes],
                         [('dev/kmsg',1,11),('dev/console',5,1)])
    def test_gzip_and_raw_read_the_same(self):
        raw=m.cpio_bytes([m.Entry('dev',stat.S_IFDIR|0o755),
                          m.Entry('dev/kmsg',stat.S_IFCHR|0o600,0,0,b'',1,11)])
        self.assertEqual([(e.name,e.mode,e.rdevmajor,e.rdevminor) for e in m.read_ramdisk_bytes(raw)],
                         [(e.name,e.mode,e.rdevmajor,e.rdevminor)
                          for e in m.read_ramdisk_bytes(gzip.compress(raw,mtime=0))])
    def test_data_in_a_char_node_refuses(self):
        with self.assertRaises(m.Invalid):
            self.archive(m.Entry('dev/kmsg',stat.S_IFCHR|0o600,0,0,b'x',1,11))
    def test_a_node_under_a_non_directory_refuses(self):
        with self.assertRaises(m.Invalid):
            m.read_ramdisk_bytes(m.cpio_bytes([m.Entry('dev',stat.S_IFREG|0o644,0,0,b'x'),
                                               m.Entry('dev/kmsg',stat.S_IFCHR|0o600,0,0,b'',1,11)]))
    def test_an_unsupported_member_type_refuses(self):
        with self.assertRaises(m.Invalid):
            self.archive(m.Entry('dev/kmsg',stat.S_IFSOCK|0o600,0,0,b'',1,11))

if __name__=='__main__':unittest.main(verbosity=2)
