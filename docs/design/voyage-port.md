# 移植到 Kindle Voyage

Voyage（第 7 代，2014 年款）是这套系统的第二个机型。设备目录、启动配置、身份校验和内核补丁都在仓库里，
而且**已经在模拟器（QEMU 里跑原厂 5.13.6 固件）上跑通了完整启动链**：交接进 Voyage 内核 → handoff-init →
preinit 挂载镜像 → Android `boot_completed=1`，并用模拟器专用的显示选源选项把 Android 桌面读出来。
**真机上一行都还没跑过**，所有真机结论都还是空的。这份文档写清楚：和 PW3 差在哪、仓库里已经做了什么、
模拟器验到了哪里、哪些开关只属于模拟器、真机上还必须测什么。

## 一、和 PW3 差在哪

| 项 | PW3 | Voyage | 依据 |
|---|---|---|---|
| SoC | i.MX6SL | 同一颗 | 亚马逊公开的 GPL 源码里两块板共用板级文件 |
| 内核配置 | `imx60_wario_defconfig` | 同一份 | 源码包 `BUILD_HOW-TO.txt` 自己写明 |
| 屏幕 | 1072×1448，300 ppi | 同尺寸同密度 | 两台机器的面板规格一致，所以旋转、密度、字号、边距都不用改 |
| 内存与划分 | 512 MiB，3G/1G | 相同 | `imx60_wario_defconfig` 同一份，`identity_limit` 因此沿用 |
| 电源管理 | max77696 | 同一颗 | 板级与 defconfig 相同 |
| 触摸 | Cypress cyttsp4 | 同一颗 | 同上 |
| Wi-Fi | AR6003（ath6kl） | 同一颗 | 同上；校准与固件的**路径**仍要读，见第二节 |
| 霍尔传感器 | 有 | 有 | 同上，磁吸保护套的休眠走同一条 |
| 机器号 / 板 ID | 4091（Wario） | 4091（Wario） | 板 ID 来自 IDME，运行时读 |
| 原厂固件 | 5.16.2.1.1 | 5.13.6 | 两台机器的原厂系统不同，`/sbin/init`、`/etc/upstart/init.sh`、`/usr/sbin/eips` 的字节和哈希都不同 |
| 原厂内核 build 串 | `#8 PREEMPT Tue Aug 1 12:49:59 UTC 2023` | `#2 PREEMPT Thu Apr 8 09:39:57 UTC 2021` | 交接要跑原厂内核自己的函数体，release 相同、build 串不同 |
| CPU 身份（MIDR） | `0x412fc09a` | 同一颗 Wario 硅片 | 真机同一值；模拟器的 Cortex-A9 报 `0x410fc090`，只在模拟器构建里合成 |

**结论：Voyage 与 PW3 的差异不在硬件，在产品身份和固件版本。** 这一侧要改的是产品名、板名、启动配置、
身份校验表，以及把“这台机器是哪个固件”的事实从写死的 PW3 换成由 profile 提供。

## 二、仓库里已经做了什么

| 文件 / 对象 | 做了什么 |
|---|---|
| `device/amazon/voyage/voyage.mk`、`BoardConfig.mk`、`AndroidProducts.mk`、`vendorsetup.sh` | 继承 pw3 的产品/板配置，只覆盖 `PRODUCT_NAME`、`PRODUCT_DEVICE`、`PRODUCT_BRAND`、`PRODUCT_MODEL` 与 `TARGET_BOOTLOADER_BOARD_NAME` |
| `boot/profiles/voyage-5.13.6.json` | 固件事实全部填好：stock cmdline、内核 release/version、四个原厂函数体的大小/哈希、四个 stock 文件（init、init.sh、init.exe、eips）的 path/bytes/sha256、`stock_cpuid`、Wi-Fi 校准/固件路径 |
| `boot/devicedata/devicedata.c/.h` | 新增 `dd_voyage_5_13_6` 与 `dd_identity_voyage_5_13_6[]`，`dd_profile` 引用身份表；`DD_PROFILE` 由构建选结构体 |
| `boot/launcher/helper.c` | `original_boot()` 与 eips 检查改读 `DD_PROFILE` 的身份表；archive 上限改读 profile 的 `device_archive_max` |
| `boot/launcher/launcher-template.sh`、`boot/build_launcher.py` | 回退屏的 eips 哈希改成 `@EIPS_HASH@`，由构建从 profile 替换；`--profile` 选机型 |
| `boot/build_entry.py` | `--profile` / `--kernel-source`；给 handoff-init 传 `-DKERNEL_RELEASE=<kernel.release>` |
| `boot/build_module.py`、`boot/handoff/` | target-lock 的 `TARGET_STOCK_RELEASE` / `TARGET_STOCK_VERSION` / `TARGET_CPUID` 从 profile 读；`PWS_EMULATOR` 与 `direct_commit` 仅模拟器构建 |
| `patches/android-core/1060-…patch` | 发行补丁序列第 86 个：`readom.emulator=1` 门控的 Cortex-A9 TLB lockdown 探测旁路，无 token 时 no-op |
| `tools/build-preinit.py`、`tools/preinit-sha256.c` | 脱离 AOSP 用 musl + 自有 SHA-256 构建 Voyage 的 `pw3-testfile-init` |
| `tools/build-voyage-release.py` | 从官方 v0.1.0 发布物 bootstrap Voyage system/data 镜像与 manifest 草稿（验证用，见第六节） |
| `tools/emulator/` | 合成 Voyage 面板/波形生成器、QEMU 启动脚本、guest 准备/暂停准备脚本、回读校验工具 |
| `installer/build_payload.py`、`tools/prepare-aosp-device.sh`、`build-aosp-system.sh`、`run-aosp-build-job.sh` | 安装器 C 代码未改；载荷尺寸、设备目录同步和 AOSP product 都按机型取 |
| `tools/tests/` | `test_profile_matches_devicedata.py` 守住 profile 与 C 结构体；新工具各有宿主测试；`patch-series` 覆盖 1060 |

安装器的 C 代码一行没改：E101 画面上写的机型来自 profile 的 `supported_zh` / `supported_en`，
`render_screens.py` 早就按 `--profile` 取这两个值。设备目录只有四个文件、其余继承 pw3，这是有意的：
两块板在这套系统用到的每一处硬件上都相同，复制一份就是几百个文件要成对修改。三样东西**故意不覆盖**，
因为它们命名的是硬件实现而不是产品：`ro.hardware.gralloc/hwcomposer/lights/power = pw3`、模块名
`gralloc.pw3` 等，以及 `init.pw3*.rc`、`fstab.pw3*`、`pw3-preinit.conf` 和入口命令行里的
`androidboot.hardware=pw3`——init 按 `ro.hardware` 找 `/init.<名字>.rc`，改掉只会得到找不到 rc 的系统。

## 三、模拟器上验到了哪里

| 关卡 | 结果 | 证据（`readom-port/` 内） |
|---|---|---|
| 合成面板/波形被原厂 5.13.6 驱动接受（`source=stored`） | ✅ | `emu-voyage/` 环境报告与 `emo/` 面板读回 |
| 身份校验 + `dd_archive`（含 panel/WRF/设备数据） | ✅ | `voyage-e2e-report` §4 |
| probe 走**真实** `pm_suspend`（`pm_test=core`，卸载 fsr_keypad） | ✅ | `voyage-e2e-report` §6B.1（`elapsed_ms=908`，`suspend debug: Waiting for 5 seconds`） |
| commit 走真实 suspend → transition 到 Voyage 内核 | ✅ | `voyage-e2e-report` §6B.2 |
| patch 1060 在 test cmdline 下生效（`TLB lockdown probe skipped`） | ✅ | `voyage-e2e-report` §6B.2、`kernel-fix-report` §5.2 |
| handoff-init（BEGIN/DRIVER/SCREEN/END，PMIC keeper 恢复） | ✅ | `voyage-e2e-report` §6B.3 |
| preinit（V10 contract、dm-linear 挂载、system SHA256） | ✅ | `voyage-e2e-report` §6B.4 |
| Android init 到 `sys.boot_completed=1` | ✅ | `voyage-e2e-report` §6B.5 |
| Android 合成结果在面板上可读 | ✅ | `qemu-epdc-report` §6.3（面板与参考图 823,296/823,296 逐像素相同；75 s 后只有时钟变化） |
| installer 发行版端到端（安装 → 安装产物启动） | ⏳ 进行中 | `installer-e2e-report` |
| 触摸 / 重启回原厂 | ⏳ 未验证 | 尚未执行（见第五节） |

模拟器“能证明”的是：交接链、内核启动、handoff-init、preinit、Android 启动到 `boot_completed`、
以及合成器送出的内容确实到了 EPDC 面板。模拟器“不能证明”的是真实 EPDC 波形指令/灰阶/鬼影/刷新时序
（QEMU 只建模扫描输出）、真实 suspend 的 SoC 低功耗与唤醒源、前光/触摸校准/Wi-Fi 校准/电池/充电等
外设行为，以及 eMMC 性能——这些都要真机。详细清单见 `voyage-e2e-report` §6A.7。

## 四、模拟器专用开关清单

| 开关 | 位置与默认 | 作用 |
|---|---|---|
| `readom.emulator=1`（内核 cmdline） | 仓库：patch 1060；**只出现在测试入口**的 cmdline，发行 cmdline 永不含 | 跳过 `bus_freq` 的 Cortex-A9 TLB lockdown 探测，改填 i.MX6SL 静态映射的 VA/PA/attr；无 token 时 no-op，真机行为不变 |
| `PWS_EMULATOR`（模块编译宏） | 仓库：`boot/build_module.py --emulator`；strict/发行模块不定义 | 只合成硬件 attestation（MIDR/DCP/USB/engine fields），probe 与默认 commit 仍是真实 `pm_suspend`；模块多带 `direct_commit` 参数 |
| `direct_commit`（模块参数） | 仓库：模拟器模块内，0644，默认 0 | 0=真实 suspend（生产路径）；1=commit 直接用跳转绕过 `pm_suspend`，只作为 QEMU 上某个驱动卡住 suspend 时的运行期兜底 |
| `-global driver=imx.epdc,property=update-source,value=on` | QEMU fork（不在本仓库）；默认 off | 面板改由 EPDC update buffer 扫描输出，Android 合成结果才可读；不传则与改动前逐像素一致，只用于模拟器验证。简写 `imx.epdc.update-source=on` 无效（类型名带点），要用长格式 |
| 停用 `deviced`（`/etc/upstart/deviced.conf.off`） | guest 运行时（模拟器专用） | 模拟器没有实现 fsr_keypad 的 I2C，deviced 会卡在 D 状态，破坏 freeze |
| 卸载 `fsr_keypad`（+ `drv26xx_haptics`），先停 `lab126_gui`/`x`/`lab126` | guest 运行时：`tools/emulator/prepare-voyage-suspend.sh` | 原厂 suspend 在模拟器上会因 fsr_keypad 的 suspend 回调等 mutex 而挂死；卸载后真实 suspend 才能完成。真机/发行路径不依赖它 |
| 串口 console（`ttymxc0`）+ 字符限速工具 | 模拟器运行方式 | 入口必须从串口 console 启动；SSH shell 会让 pm freeze 失败。TCG 下 UART 会丢字符，长命令要限速 |

## 五、真机必测项

- **真实面板波形/灰阶/鬼影/刷新时序**：QEMU 不执行波形指令，合成面板只证明数据通路。
- **前光与 VCOM**：前光亮度曲线、VCOM 校准值、温度补偿都要在这块屏上量。
- **触摸与按键**：cyttsp4 触摸校准、物理键盘/电源键、霍尔盖的休眠与唤醒。
- **Wi-Fi 校准**：profile 现在按运行中的 5.13.6 读的是 WAN 校准文件；Wi-Fi-only 机型可能写 WFO，装真机前要确认。
- **休眠功耗与 PMIC keeper**：真实 suspend 的电流、唤醒源、LDO7/BUCK1 恢复后的电压。
- **真机安装**：installer 端到端（下载/校验/写入/安装后启动）目前只在模拟器上推进，真机还没走。
- **重启回原厂**：从 Android 长按电源键回 Kindle 系统、以及启动失败时的自动回退。

## 六、bootstrap system 镜像与 AOSP 全量构建

| 路线 | 是什么 | 用途与边界 |
|---|---|---|
| bootstrap 镜像（`tools/build-voyage-release.py`） | 官方 v0.1.0 的 PW3 `system.img` 换入 Voyage 内核的 32 个模块、改写 `build.prop` 的 7 类身份属性；data 镜像由发布模板解压 | 只在模拟器上验证交接/内核/preinit/Android 启动；**不是 AOSP 构建产物**，用户空间仍是 PW3 那一份，不能直接作为发行镜像 |
| AOSP 全量构建（`PW3_AOSP_PRODUCT=voyage`） | 用本仓库的 Voyage product（继承 pw3）走完整 AOSP 构建，生成 Voyage 身份的 system 镜像 | 正式发行必须走这条；需要 AOSP 树和构建 VM，本机没有 AOSP，未做 |
| 测试入口（`build_launcher.py --emulator`） | 无 installer 的只启动入口，内嵌 Voyage 内核、模拟器模块、公开 initramfs 与设备数据 | 只用于模拟器 e2e；不能装到真机（helper 与模块都是模拟器专用构件） |

bootstrap 路线的价值是让内核、交接、preinit、Android 启动在真机固件上被验证，而不必先有 AOSP 构建；
它的风险是“系统身份”只改了 `build.prop` 与模块，其余仍是 PW3 的字节，所以发行前必须由 AOSP 全量构建
替换，并在真机上重新验收。两条路用同一套 profile 与身份校验。

## 七、还没验证的与风险

- **没有任何一行在真机上跑过**：第五节的所有项都还是空的。
- **installer 端到端仍在进行**：release-v2 入口与 payload 已构建（哈希见第九节），安装后的启动与严格模块的负向硬件门验证还要等结果。
- **触摸/重启回原厂未验证**：模拟器是 headless 启动，触摸注入需要带显示 console 的启动方式。
- **显示质量未验证**：模拟器只证明合成结果到了面板，灰阶/刷新策略是 PW3 面板的实测值。
- **bootstrap 镜像不是 AOSP 构建产物**：发行前必须替换，且发行镜像要重新在真机验收。
- **内核/模块的字节可复现性依赖构建树路径**：3.0.35 把源码绝对路径写进镜像，只有在 `/root/readom-kernel` 规范路径下构建才逐字节可复现，见 `kernel-fix-report` §4.3 与 `release-build`。
- **profile 的 Wi-Fi 路径按 WAN 文件记录**：Wi-Fi-only 机型可能不同，见第五节。

## 八、怎么构建（两条路）与复现约束

```sh
# AOSP 路线（正式发行，本机无 AOSP 树，未做）
PW3_AOSP_PRODUCT=voyage PW3_BUILD_VARIANT=user bash tools/build-aosp-system.sh
python3 installer/build_payload.py --model voyage --data-format 1 --version vX --output <payload>
python3 installer/screens/render_screens.py --manifest <payload>/manifest.json \
  --profile boot/profiles/voyage-5.13.6.json --output <screens> ...
python3 boot/build_entry.py --profile boot/profiles/voyage-5.13.6.json --name Readom.sh ... --output <entry>

# bootstrap 路线（模拟器验证用）
python3 tools/build-preinit.py --output <preinit> --board voyage
python3 boot/build_module.py boot/profiles/voyage-5.13.6.json --kernel <zImage> \
  --public-initramfs <initramfs> --cmdline <cmdline> --output <module> --emulator
python3 boot/build_launcher.py ... --profile boot/profiles/voyage-5.13.6.json --emulator --output <entry>
```

发行物的逐字节复现要求构建树在 `/root/readom-kernel`（bind mount 到 `/home/builder/readom-kernel`
也可以），否则内核/模块里的源码路径串会变、哈希随之改变；这一点对任何嵌入内核或模块的产物都成立。
模拟器测试入口的 cmdline 末尾加 `readom.emulator=1`，发行入口不加；`direct_commit=1` 只在需要绕过
QEMU suspend 时手动设置，默认 0 走与真机一致的真实 suspend。

## 九、证据索引

- `readom-port/voyage-e2e-report`：模拟器端到端（§6A.7 能证明/不能证明清单、§6B v3 全链路）。
- `readom-port/qemu-epdc-report`：`update-source` 选项、Android 面板可读的定量证据。
- `readom-port/display-analysis`：面板噪声的根因分析（固定扫描地址 vs update buffer）。
- `readom-port/kernel-fix-report`：patch 1060 根因、发行内核重建、路径依赖（§4.3）。
- `readom-port/qemu-suspend-research`：真实 suspend 与生产路径一致性的调研。
- `readom-port/bootstrap-build-report`：system/data/payload/screens 的构建与哈希。
- `readom-port/preinit-build-report`：独立构建的 Voyage `pw3-testfile-init`。
- `readom-port/test-entry-report`、`release-entry-report`：test-entry v3 与 release-v2 的产物与校验。
- `readom-port/installer-e2e-report`：installer 模拟器端到端进度（进行中）。
- `readom-port/emu-voyage/`：runs、shots、面板与波形证据、启动与准备脚本。
