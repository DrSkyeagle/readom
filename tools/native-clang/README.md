# 可选 ARM64 主机 Clang

本项目的 Linux 构建机是 ARM64，AOSP 8.1 随附的主机工具是 x86。这个可选加速方案从原工具链 `repo.prop` 所指向的 LLVM/Clang 提交编译 ARM64 主机程序，减少逐条编译命令的 x86 模拟开销。Android 仍按原参数生成 ARM32 代码；用于构建其他工具的 i386/x86_64 目标也保留。

## 版本与替换范围

- LLVM：`40629e6d4dbbaa22cf42174790b45a8075ed3971`。
- Clang：`d3aef9bb1abef05fe425b86e8023214d39fe6acd`。
- 对应原始目录：`prebuilts/clang/host/linux-x86/clang-4053586`，版本 `5.0.300080`。
- 只切换主机 `clang`/`clang++` 的实际可执行文件；保留原 Python 包装器、原 resource directory、目标运行库、sanitizer、libFuzzer 和其他工具。没有删除或禁用 AOSP 测试。
- Soong 和 Make 使用本项目的 `PW3_NATIVE_CLANG_BIN` 补丁；不设置会改变目标运行库选择的 `LLVM_PREBUILTS_BASE`。

`../build-native-clang.sh` 在隔离 VM 中检查源码提交及干净工作树，构建候选编译器。其 LLVM 自身的测试/示例目标未编译，这一点不等同于编译器已通过完整上游测试套件。

## 生成与启用步骤

下面的路径是当前项目 VM 的布局。调用这些步骤前须先按项目脚本准备 AOSP 构建环境和原始工具链，不得改动正在进行的 AOSP 构建。

1. 运行 `../build-native-clang.sh`，得到 `/home/builder/pw3-native-clang/build/bin/clang-5.0` 和 `/mnt/build-share/native-clang/native-build-result.json`。
2. 用 `prepare-prefix.py --binary <候选ELF> --original-prefix <VM中的原Clang目录> --store /home/builder/toolchains/pw3-native-clang --receipt <记录文件>` 冻结单独副本。目录键由编译器、包装器和适配脚本散列生成；已有副本拒绝覆盖。
3. 将该副本的 `host/` 只读 bind 到 chroot 内 `/opt/pw3-native-clang-prebuilts`。保留原工具链。此时还不创建启用标记。
4. 用实际构建图的 `ninja -t commands <目标>` 收集四条真实命令，写成 JSON `{目标对象路径: 命令字符串}`。当前样本为 PW3 `PreInit.c`、gralloc `mapper.cpp`、ART `mem_map.cc` 的 i386 和 x86_64 版本。仅接受固定工具链的单条编译命令。
5. 在 chroot 内运行 `compare.py --commands <命令JSON> --output <不存在的临时目录> --native-prefix /opt/pw3-native-clang-prebuilts/linux-x86/clang-4053586`。它重新执行原编译器和候选编译器，将输出放在独立目录，保留 driver trace、命令参数、依赖文件内容散列及对象散列。只去掉调试节和 `.comment` 后，比较整个 ELF 对象，包括机器码、数据及重定位。
6. 四个对象全部一致、依赖文件内容一致、编译器在测试期间未变化后，把 `result.json` 保存为 `/mnt/build-share/native-clang/verified-comparison.json`。用 `record-activation.py` 生成 `/mnt/build-share/native-clang/activation.json`，该步骤再次检查结果和实际冻结文件。
7. 常规构建脚本会在 AOSP 独占锁内执行 `../provision-native-clang.sh`，核验编译器、副本、对照脚本和结果散列，再确认 bind 源及只读属性，最后写入 chroot 的启用标记。

删除 `activation.json` 后，下次构建会清除旧启用标记并使用原始工具链。存在但无效的配置会阻止构建。构建期间不要删除配置或切换挂载；只读编译器副本保留到使用它的构建结束。

## 当前证据的范围

四组实际命令已经通过对象和依赖内容对照，并已用于完整 AOSP 构建。单个样本编译时间下降仅证明这四个样本的主机耗时；不能据此承诺整个系统的完成时间，也不能证明所有可能的编译输入行为完全等价。完整镜像以及实机启动仍须分别验证。
