# 启动链

从「在 Kindle 书库里点一下」到「Android 的 init 跑起来」之间的全部代码。
这一整块是 **GPL-2.0-only**（见 [THIRD_PARTY.md](../THIRD_PARTY.md)）：交接模块是内核模块，
其余程序和它一起分发。

```
handoff/          交接模块 pw3_first_kernel.ko：在原厂内核里把 Readom 的内核和启动参数
                  装进内存，再跳过去。不写分区、不改引导。tests/ 下是 QEMU 夹具
handoff-init/     交接之后那个 PID 1：把原厂挂起路径留在挂起电平上的 PMIC 寄存器
                  （LDO7「keeper」1.8 V→3.2 V、BUCK1 的电源时序）放回去，加载 EPDC
                  驱动并把「正在启动 Android」重画上屏，再原样 exec 存储入口
launcher/         书库里那一下点击实际执行的程序（取锁、轮转日志、写报告区、调安装器）
devicedata/       每次启动现场从原厂系统读设备专属数据（波形、Wi-Fi 校准、存储布局）
profiles/         机型描述，一个 JSON 一款机器
build_module.py   编交接模块
build_launcher.py 编 launcher 和 helper
build_entry.py    把上面这些 + 内核 + 安装器 + 画面压成一个 Readom.sh
```

## 两件决定了这套设计的事

**入口里不含任何设备数据。** 波形、Wi-Fi 校准、MAC 这些每台机器都不一样的东西，一律每次启动
现场读（`devicedata/`），不打进发行件。这是它能开源的前提——否则发出去的就是某一台 Kindle 的
身份。入口的构建回执里有 `contains_device_data: false`，发行清单第 10 步会核它。

**启动镜像要保持不变。** `boot-*.art`/`.oat`/`.vdex` 一变，使用者 `/data` 里所有应用的机器码
就作废，升级后第一次开机要重编译（一个大应用 1–3 分钟，装得多就更久）。所以改 framework 资源
只改值、不加名；每次构建后逐字节比对这几个文件。理由和做法在
[docs/design/release-build.md](../docs/design/release-build.md)。

## 这里的代码怎么测

交接这件事没法在宿主机上验，所以有一套 QEMU 夹具：一个第一级 Linux 加载模块、被拒绝它该拒绝的
东西、再跳到第二级。套件里叫 `handoff-in-qemu` 和 `handoff-client`，要构建机里的 loader 内核。
`launcher/` 和 `devicedata/` 的逻辑有宿主机测试（`user-log`、`device-data`、`launcher-name`、
`launcher-call-sites`、`handoff-start-picture` 等），`tools/run-tests.py --list` 列得全。
