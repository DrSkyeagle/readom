# AOSP 8.1 ART 主机工具与 QEMU linux-user

构建环境在 ARM64 Linux 中通过 QEMU 6.2 执行 AOSP 的 x86 主机工具。目标 Android ARM 运行库仍保持原来的分支；下述两份补丁只改变非 Android 的 Linux x86 主机 ART 内存处理。

实际完整构建先后暴露两个问题：

| 实际失败 | 最小复现 | 修正 |
| --- | --- | --- |
| `dex2oatd` 创建 ARM boot image 时，低 4 GiB 指针检查失败 | `map32-probe.c`：`MAP_32BIT` 返回高地址，而普通低地址 hint 可用 | `0001-host-x86-low4g-allocation.patch` 选择 ART 已有的、有边界检查的低地址分配器 |
| 修正分配后，垃圾回收出现 `Bad Baker pointer` | `madvise-probe.c`：模拟的 x86 进程收到成功返回，但 65536 字节仍全部非零；原生 ARM64 进程正确清零 | `0002-host-x86-explicit-madvise-zeroing.patch` 选择 ART 已有的显式清零路径 |

两者都保留 debug 断言、Baker/read barrier、垃圾回收和默认构建并行度。第二个问题也在单线程 dex2oat 重放中出现，因此没有以降低并行度作为修复。

上游 QEMU 6.2 的 [`TARGET_NR_madvise` 实现](https://github.com/qemu/qemu/blob/v6.2.0/linux-user/syscall.c#L11141-L11147)直接返回成功，没有执行 `MADV_DONTNEED` 的匿名页丢弃语义。这与本构建环境的实际复现一致。不能把这项发现泛化为所有 QEMU 版本或系统模拟模式的问题。

## 重现与回归

这两个小程序仅分配、检查和释放自己的匿名内存，不读设备、不改系统设置。`map32-probe.c` 是 Linux x86 的探针，必须用 x86 编译器构建；`madvise-probe.c` 可分别构建原生和模拟架构的版本。返回 0 表示所检查的主机契约成立，返回 1 表示已复现差异，其他非零返回表示探针本身未能完成。

实际产品回归使用 `tools/build-aosp-host-art.sh`：它取得与完整构建相同的互斥锁，应用固定顺序补丁，重新构建 `dex2oatd` 和真正的 ARM `boot.art` 目标。探针通过或主机工具编译通过，均不能代替此目标成功；部分生成的 `.art`/`.oat` 文件不能用作有效构建产物。

主机原始失败、单线程复现、探针和本次回归日志位于私有工作目录 `work/android-port/build-share/dex2oat-host-debug/` 与 `logs/`。当前结果以当次构建退出状态和产物散列为准。这个回归只证明主机构建路径，不代表 Android 或 PW3 已经启动。

## 主机 Java 打包过程

恢复完整构建后，原 x86 JDK 的 `jar` 和 JarJar 在 QEMU 中持续占用 CPU 数分钟。相同真实输入交给同一个原 JDK、仅增加解释执行选项后，分别在约 3.1 秒和 21.8 秒完成。另用原生 Java 8 对照，1752 个 JAR 条目及 1087 个 JarJar 条目的类与资源内容一致；普通 JAR 只有 JDK 版本产生的清单元数据不同。

构建路径中的 `java`/`jar` 包装器仍执行 AOSP 原 JDK，分别增加 `-Xint`/`-J-Xint`，保留原始 JAR 实现、清单元数据和 x86 JNI 库。Jack 服务和 javac 使用已接入的原生 Java 8。解释执行选项只作用于主机 JVM，不关闭 Android 的 ART 编译、JIT、GC 或安全检查。

`javadoc` 已单独切换为原生 Java 8 执行 AOSP 原始 `tools.jar` 和 Doclava，明确固定 `LC_ALL=C` 并保留所有 `-J` JVM 参数。此前相同输入的原生与完成的 x86 执行已有 2,990 份输出逐字节相同的记录；本次当前输入又核对了两个 API 清单和全部 2,988 份 stub，原始工具类与原生 JDK 工具类的结果一致。生产构建生成的 stub 也与独立运行一致。只改变主机执行方式，不替换 Android 的运行库或编译选项；不将 stub 对照结果当作实机验证。
