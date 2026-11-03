# ARM Android 运行时检查

这个测试在无网络的 VExpress QEMU 中运行实际构建的 AOSP ARM `dalvikvm`、ART 和 Java 核心库。system 镜像以只读 SD 卡文件接入，`/data` 是有大小上限的 RAM 文件系统，测试以 shell 用户、SELinux enforcing 运行。它不启动 Android 框架、桌面或微信读书，不接触 PW3 硬件。

`ArtSmoke.java` 检查全部预编译 boot image 已实际映射、UTF-8、SHA-256 已知答案、大整数、原生压缩库、共享文件映射与刷写，以及四线程分配和垃圾回收。它只在 RAM 中的独立目录创建一个文件并删除。最终成功需要 Java 明确报告 12 项检查通过、进程退出 0、SELinux enforcing 为 1、QEMU 正常关机，以及 system 镜像散列未改变。必须实际捕获 Android 日志，且不能出现子进程崩溃或退回无 boot image 运行。

运行前需要完整 AOSP 构建、转换完成且已检查的 raw system 镜像，以及 `tools/build-aosp-runtime-test-kernel.sh` 的真实构建结果。运行器使用实际 ramdisk/init/策略；只添加合成测试 rc、测试 DEX、运行脚本和 VExpress 虚拟块设备的专用标签。所有输入散列、虚拟机参数、原始串口日志和结果都写入指定的新目录，不覆盖旧结果。

测试内核从已通过的公共接口测试配置派生，增加虚拟 SD 读卡器。Amazon 内核 MMC 通用源码有两处未保护的 Lab126 专用引用，因此仅在独立测试源码树中应用 `patches/test-kernel/` 的两份条件编译修正。PW3 实机内核和原来的无磁盘接口测试内核保持各自构建记录；不能将虚拟读卡器的结果作为真实 eMMC 或 DMA 的证据。

```sh
python3 /mnt/port/tools/aosp-runtime-smoke/run.py \
  --system-image /mnt/build-share/aosp-images/system.raw.img \
  --output /mnt/build-share/aosp-runtime-smoke-v1
```

任何缺失输入、编译错误、超时、Java 失败、非正常关机或镜像变化都不能记为通过。成功也只覆盖上述运行时检查，不代表完整 Android 系统或固件已经可用。

加上 `--check-strings` 可同时运行 Cortex-A9 Bionic 字符串复制回归：`strcpy`、`stpcpy`、`strcat` 共29,376个用例，覆盖长度、地址对齐、返回指针、边界外字节保持和保护页。测试程序通过真实动态 libc 函数调用，关闭编译器内建替换；所用编译命令、源文件和二进制散列随运行记录保存。该检查曾在原始镜像的 `strcpy` 中复现非法指令；两处汇编存储修正后通过。

## 只能在 userdebug 镜像上跑（2026-10-22）

这个测试的做法是：init 把测试载荷拷进 `/data`，再以 shell 域起一个服务去跑它。**发行（`user`）镜像不接受这件事**，而且不是"换个目录"能解决的——AOSP 自己的三条 `neverallow` 把每条路都封了：

- `/data/local/tmp`（`shell_data_file`）：shell 读得、也执行得，但 `init.te` 的 `neverallow init shell_data_file:dir { write add_name remove_name }` 不许 init 往里写。开发期间实测到的就是 `init: Unable to open '/data/local/tmp/art-smoke.jar': Permission denied`。
- `/data/nativetest`（`nativetest_data_file`）：init 写得，但 `shell.te` 只在 `userdebug_or_eng` 里给 shell 读执行权，而 `domain.te` 的 `neverallow { domain userdebug_or_eng(-shell) } nativetest_data_file:file no_x_file_perms` 在 `user` 上对**所有域**禁了执行。开发期间实测：`avc: denied { search } ... nativetest_data_file ... permissive=0`。
- 让服务留在 init 域（这样就能读 `/data/nativetest`）：`init.te` 的 `neverallow init { file_type fs_type }:file execute_no_trans` 不许 init 不换域地 exec。
- `androidboot.selinux=permissive` 在 `user` 构建里被 init 忽略（`ALLOW_PERMISSIVE_SELINUX` 只在 userdebug/eng 打开）。

要让它在发行镜像上跑，只能往镜像里加策略规则——那是用系统的代价换一个测试，不做。所以 `tools/run-tests.py` 在导出是 `user` 时把这三项列为"不在这里跑"，并说明原因；发行时另外用同一棵源码树构一次 userdebug、导出、再跑这三项。这不是对发行镜像的直接证据：两种变体的差别是调试工具、`ro.debuggable` 和 `userdebug_or_eng` 那部分策略，而 45 个 ART 启动文件两者逐字节相同（`docs/design/release-build.md`），所以"这棵树的运行时与服务起得来"这个结论是可以搬过去的，"发行镜像的策略放不放行"则由上机验证。
