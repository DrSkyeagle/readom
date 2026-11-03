# ramdisk 校验与文件映射的回归检查

在仓库根目录执行 ramdisk 读取与校验库（`pack.py`）的测试：

```sh
python3 -m unittest discover -s tools/preinit-pack/tests -v
```

多段映射回归调用生产 `TestFileMapper.h`，只在系统调用边界提供文件元数据、FIBMAP 和 DM 回复。映射按 FAT 簇查询（每簇首尾两个扇区，簇内必须连续）。检查单段、1024 段接受与 1025 段拒绝，覆盖、越界、内部和跨文件重叠，簇大小非法、簇内不连续、文件末簇不满，512 B 与 64 KiB 簇，三类文件（系统镜像、数据镜像、报告区）各自的名字、uuid、次设备号与写权限，报告区缺失可接受但出现即按固定 64 KiB 核验，损坏回复长度和偏移等 160 项条件。

它已登记在 `tools/run-tests.py`（`file-mapper`，只在 Linux 上运行），所以在构建虚拟机里跑整套即可：

```sh
bash tools/vm.sh shell bash -lc 'cd /mnt/port && python3 tools/run-tests.py'
```

预期输出 `MULTIEXTENT_GUARD_TEST_PASS checks=160` 且退出码为 0。该测试不替代真实旧内核的 DM 行为、ARM 全流程和整盘写入核验，那些要在设备上验。
