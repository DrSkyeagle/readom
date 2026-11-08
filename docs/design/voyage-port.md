# 移植到 Kindle Voyage

Voyage（第 7 代，2015 年款）是这套系统的第二个机型。设备目录、启动配置和构建接线都已经在仓库里，
**但没有任何一行在 Voyage 上跑过**：启动配置里还留着 `TODO(voyage-emulator)`，等模拟器把原厂
5.13.6 的固件事实读出来。这份文档写清楚两件事——它和 PW3 到底差在哪，以及还差什么。

## 一、和 PW3 差在哪

| 项 | PW3 | Voyage | 依据 |
|---|---|---|---|
| SoC | i.MX6SL | 同一颗 | 亚马逊公开的 GPL 源码里两块板共用板级文件 |
| 内核配置 | `imx60_wario_defconfig` | 同一份 | 源码包 `BUILD_HOW-TO.txt` 自己写明 |
| 屏幕 | 1072×1448，300 ppi | 同尺寸同密度 | 两台机器的面板规格一致，所以旋转、密度、字号、边距都不用改 |
| 内存与划分 | 512 MiB，3G/1G | 相同 | `imx60_wario_defconfig` 同一份，`identity_limit` 因此沿用 |
| 电源管理 | max77696 | 同一颗 | 板级与 defconfig 相同 |
| 触摸 | Cypress cyttsp4 | 同一颗 | 同上 |
| Wi-Fi | AR6003（ath6kl） | 同一颗 | 同上；校准与固件的**路径**仍要读，见第四节 |
| 霍尔传感器 | 有 | 有 | 同上，磁吸保护套的休眠走同一条 |
| 机器号 / 板 ID | 4091（Wario） | 4091（Wario） | 板 ID 来自 IDME，运行时读 |

**结论：Voyage 与 PW3 的差异不在硬件，在产品身份和固件版本。** 所以这一侧要改的只有四处：产品名、
板名、启动配置、以及 helper 里“这台机器是哪个固件”的那一项判断。

## 二、改了哪些文件

| 文件 | 改了什么 |
|---|---|
| `device/amazon/voyage/voyage.mk` | 继承 `device/amazon/pw3/pw3.mk`，只覆盖 `PRODUCT_NAME`、`PRODUCT_DEVICE`、`PRODUCT_BRAND`、`PRODUCT_MODEL` |
| `device/amazon/voyage/BoardConfig.mk` | `include` pw3 的那一份，只覆盖 `TARGET_BOOTLOADER_BOARD_NAME` |
| `device/amazon/voyage/AndroidProducts.mk`、`vendorsetup.sh` | 指向 `voyage.mk`，加 `voyage-user` / `voyage-userdebug` 两个 lunch 组合 |
| `boot/profiles/voyage-5.13.6.json` | 新的启动配置：机器号、RAM 布局、ATAG 种类、E101 文案，以及待填的固件事实 |
| `boot/devicedata/devicedata.c`、`devicedata.h` | 新增 `dd_voyage_5_13_6` 与 `DD_PROFILE` 宏，helper 由宏选结构体 |
| `boot/launcher/helper.c` | 四处 `&dd_pw3_5_16_2_1_1` 改成 `&DD_PROFILE`；`stock_profile` 检查改读结构体里的 release/version |
| `boot/build_launcher.py` | 新增 `--profile`，把 `device_data_profile` 传成 `-DDD_PROFILE`，E101 文案按该 profile 核对 |
| `boot/build_entry.py` | 新增 `--profile`，向下传给 `build_launcher.py`；带 `TODO(` 的 profile 直接拒绝构建 |
| `installer/build_payload.py` | 新增 `--model`，数据镜像大小与 AOSP 产品目录按机型取 |
| `tools/build-aosp-system.sh`、`run-aosp-build-job.sh` | 新增 `PW3_AOSP_PRODUCT`，lunch、输出目录、makefile 指纹按它走 |
| `tools/prepare-aosp-device.sh` | 除 pw3 外再同步机型的设备目录（继承要两份都在 AOSP 树里） |
| `tools/tests/test_profile_matches_devicedata.py`、`tools/fill-voyage-profile.py` | 守住 profile 与 C 结构体两份事实一致；从设备事实填 profile 并同步改写结构体 |

安装器的 C 代码一行没改：E101 画面上写的机型来自 profile 的 `supported_zh` / `supported_en`，
`render_screens.py` 早就按 `--profile` 取这两个值，Voyage 只要把 profile 填对。

## 三、为什么继承而不是复制

`device/amazon/voyage/` 里只有四个文件，其余全部来自 `device/amazon/pw3/`。这是有意的：两块板在
这套系统用到的每一处硬件上都相同，复制一份就是三百个文件要成对修改，而第一次改漏的时候，两台机器
就悄悄跑上了不同的系统。

三样东西**故意不覆盖**，因为它们命名的是硬件实现而不是产品：`ro.hardware.gralloc/hwcomposer/
lights/power = pw3`、模块名 `gralloc.pw3` / `hwcomposer.pw3` / `lights.pw3` / `power.pw3` /
`libhealthd.pw3`，以及 `init.pw3*.rc`、`fstab.pw3*`、`pw3-preinit.conf` 和入口命令行里的
`androidboot.hardware=pw3`。最后这一项尤其容易改错：init 是按 `ro.hardware` 去找 `/init.<名字>.rc`
的，而这个产品装的 rc 文件就是 pw3 那一套，改掉名字只会得到一个找不到 rc 的系统。

## 四、启动配置里还缺什么

profile 里六个字段是空的或写着 `TODO(voyage-emulator)`，它们都是**这台机器上读出来的事实**，不能照
PW3 抄：抄错一个 token，每一台 Voyage 都会在下载任何东西之前停在 E101。

| 字段 | 从哪来 |
|---|---|
| `cmdline_base` | 运行中系统的 `/proc/cmdline`，去掉 `root=` 与 `rootwait` |
| `stock_kernel_version` | `uname -v`（release 已知是 `3.0.35-lab126`，build 串不是） |
| `stock_functions` | 原厂内核里 `pm_test_show`、`l2x0_flush_all`、`l2x0_disable`、`l2x0_inv_all` 四个函数体的大小与 SHA-256，加内核镜像本身的 SHA-256 |
| `wifi_board`、`wifi_firmware` | 该固件 `/etc/upstart/wifim` 选中的 AR6003 校准与固件路径；模拟器日志里出现过 WAN 校准文件，而 PW3 用的是 WFO，这一项可能是真差异 |
| `module_kernel_source`、`module_kernel_output` | Voyage 内核树与它的构建目录，等内核那一侧编出一次才有 |
| `footprint_profile` | 先按 `pw3`：同一个 defconfig、同样是 3G/1G，解压器的物理包络由内核自己的构建文件推出来，不是抄数字 |

`tools/fill-voyage-profile.py` 是填这些字段的那一步：吃一份从设备读出来的事实 JSON，校验（命令行必须
单行且不含 `root=`、哈希必须是 64 位十六进制、函数体不能是 0 字节、路径必须绝对），写回 profile，
**并把 `devicedata.c` 里那个结构体一起改写**，两份事实因此不会各改各的。`--check` 只报告还有没有
`TODO`，不改任何东西。

## 五、怎么构建

构建机上的顺序和发行清单一样，只是多给一个 `--model` / `--profile`：

```sh
PW3_AOSP_PRODUCT=voyage PW3_BUILD_VARIANT=userdebug bash tools/build-aosp-system.sh
python3 installer/build_payload.py --model voyage --data-format 1 --version vX --output <载荷目录>
python3 installer/screens/render_screens.py --profile boot/profiles/voyage-5.13.6.json --output <画面目录> ...
python3 boot/build_entry.py --profile boot/profiles/voyage-5.13.6.json --name Readom.sh ... --output <入口目录>
```

不给 `--model` / `--profile` 时全部退回 pw3，所以 PW3 那一条构建命令一个字都不用改。

## 六、还没验证的

- **没有在 Voyage 上跑过任何一行。** 设备目录和构建接线是照源码和文档写的，不是照实测写的。
- 启动配置里六个字段还没填，`build_entry.py` 会因此拒绝构建 Voyage 的入口（这是有意的）。
- 板级初始化是否真的走同一支分支没有核对过：原厂内核按运行时板 ID 分流，Voyage 的板 ID 走进哪些分支要在机器上查。
- 显示、触摸、前光、电量、休眠都没有在 Voyage 的面板上量过；PW3 的灰阶曲线和刷新策略是那块屏的实测值。
- 安装器仍然只认 PW3：在 Voyage 上它会在第一步报 E101 并停住，不改任何文件。

## 七、模拟器上怎么验

第一步不是整机，是**身份**：在 QEMU 里跑原厂 5.13.6，把 `/proc/cmdline`、`/proc/version`、板 ID 读出来，
填进 profile 和结构体，然后只验 `dd_device_matches` 这一件事——它能过，才说明这台“机器”被认成了
Voyage 而不是 PW3。第二步才是交接：用 `boot/build_launcher.py` 造一个只启动、不安装的入口，看内核
被装进内存、跳过去、Android 的 init 起来。真机上的显示与功耗仍然要另外量。
