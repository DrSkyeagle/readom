# 适配别的 Kindle 机型

这份文档回答一个问题：**把这套 Android 移到另一台 Kindle 上，要做哪些事、哪些已经是现成的。**
内容来自对本仓库的清点（补丁数 2026-11-04 按补丁目录重数）和对各机型原厂源码包的实测（2026-10-20 至 10-23），不是设想。**还没有在第二台机器上运行过任何一行**：这份文档是把工作量和边界算清楚，不是说已经支持。

## 一、前提：哪些机型可能行

| 条件 | 为什么 | PW3 的情况 |
|---|---|---|
| 能越狱、能在书库里运行 `.sh` | 整套安装、每次进入 Android 都靠它 | 5.16.2.1.1 可越狱 |
| 亚马逊公布了该固件的 GPL 源码 | 我们要在原厂内核源码上打补丁，编出能跑 Android 的内核 | `Kindle_src_5.16.2.1.1` |
| 原厂内核是 Linux 3.0.35 | 85 个内核补丁、交接模块、preinit 全是按它写的 | 3.0.35-lab126 |
| 显示控制器是 i.MX EPDC | 合成器、gralloc、波形接口都是按 `mxc_epdc` 写的 | i.MX6SL 的 EPDC |
| 内存 ≥ 512 MB、存储能腾出约 2.1 GB | 本机 512 MB 已经要靠 zram 才能同时开浏览器和读书 | 512 MB / 4 GB |

前两条**都不是瓶颈**：

- **越狱**：按 KindleModding 的机型表（2026-10-20 读取），从 Kindle 2 一直到 Colorsoft，每一代都有可用的越狱；只有最新的三款（Scribe 3、Scribe Colorsoft、2026 款 Kindle）写着 "None Available"。PW3 用的 WinterBreak/WinterBreak2 覆盖 5.16.4 之前的全部固件。
- **GPL 源码**：亚马逊为**每一代** Kindle 都公布了源码，一直到现在在售的机型（Source Code Notice 页，同日读取）。不存在"有越狱但没源码"的机型。

真正的瓶颈是第 3 条。下面这张表是从亚马逊自己的源码包里读出来的，不是推断的：

| 机型 | 代 | 查的固件 | 原厂内核 | 内存 | 屏幕 |
|---|---|---|---|---|---|
| Paperwhite 1 | 5 | 5.6.1.1（末版） | **2.6.31** | 256 MB | 758×1024 |
| Paperwhite 2 | 6 | 5.12.2.2（末版） | **3.0.35** | 256 MB | 758×1024 |
| Kindle 7（KT2） | 7 | 5.12.2.2（末版） | **3.0.35** | 256 MB | 600×800 |
| Voyage | 7 | 5.13.6（末版） | **3.0.35** | 512 MB | 1072×1448 |
| **Paperwhite 3** | 7 | 5.16.2.1.1（末版） | **3.0.35** | 512 MB | 1072×1448 |
| Oasis 1 | 8 | 5.16.2.1.1（末版） | **3.0.35** | 512 MB | 1072×1448 |
| Kindle 8（KT3） | 8 | 5.16.2.1.1（末版） | **3.10.53** | 512 MB | 600×800 |
| Oasis 2 | 9 | 5.16.2.1.1（末版） | **4.1.15** | 512 MB | 1264×1680 |
| Paperwhite 4 | 10 | 5.18.1.1.1 | **4.1.15** | 512 MB | 1072×1448 |
| Kindle 10（KT4） | 10 | 5.18.1.1.1 | **4.1.15** | 512 MB | 600×800 |
| Oasis 3 | 10 | 5.18.2.1.1 | **4.1.15** | 512 MB | 1264×1680 |
| Paperwhite 5 | 11 | 5.19.2 | **4.9** | 512 MB | 1236×1648 |

读法（任何人都能复核一遍，不用下载整包解压）：

```bash
curl -sL '<Source Code Notice 页上该机型的 Kindle_src_*.tar.gz>' \
  | tar -tzf - | grep -m1 '^gplrelease/linux-'
```

这张表说明两件事：

1. **同代、同固件不等于同内核。** Kindle 8（KT3）和 PW3、Oasis 1 都停在 5.16.2.1.1，SoC 也都是 i.MX6SoloLite，但它的内核是 3.10.53，另外两个是 3.0.35。所以"按 SoC 分组"是错的，要按内核版本分组。
2. **新机型不一定更难。** 内核补丁里有一批是把主线后来才有的东西回移（backport）到 3.0.35（例如 ambient capabilities 是主线 4.3 的、`process_vm_readv/writev` 是 3.2 的、binder/ashmem 在 3.0 时还在 staging）。内核越新，这类补丁就越不需要——4.1.15 的机型在这方面比 3.0.35 省事，而且硬件更好（i.MX7D 双核 Cortex-A7、8/32 GB 存储）。代价是整套补丁要在新内核上重新落位，一次。

按这张表分组，对本项目的意义：

| 组 | 机型 | 对本项目意味着什么 |
|---|---|---|
| **A：同内核（3.0.35）** | Voyage、Oasis 1 | 第六节的源码级实测：Oasis 1 的内核源码与 PW3 **逐字节相同**；Voyage 的源码不同，但当时的 79 个 Android 补丁全部能打上，而且亚马逊写明 Voyage 与 PW3 用**同一个 defconfig**。要做的是面板参数、PMIC/前光/触摸的设备差异、交接配置、重新测量。**第二个机型最适合选 Voyage**：同配置、同分辨率（1072×1448）、同内存（512 MB）。Oasis 1 用另一个 defconfig（duet，2G/2G 划分），交接的私有页表要改，Wi‑Fi 换成了 Broadcom 芯片、驱动要另找（第六、七节） |
| **A−：同内核但内存不够** | Paperwhite 2、Kindle 7 | 内核一样，但 256 MB 装不下这套 Android（本机 512 MB 已靠 zram 才够）。可以编出来，体验不是本项目的目标 |
| **B：3.10.53** | Kindle 8 | 一次内核重新落位，外加 600×800 的界面重做（密度、字号、边距都要重算） |
| **C：4.1.15** | Oasis 2、Paperwhite 4、Kindle 10、Oasis 3 | 一次内核重新落位换四个机型，而且硬件更好、还在被亚马逊更新（越狱要盯固件）。**长期收益最大的一步** |
| **D：MediaTek + 4.9 及以后** | Paperwhite 5、Kindle 11、Scribe、Paperwhite 6、Colorsoft | 不是 i.MX EPDC，显示这一层（gralloc、合成器、波形接口）要对着联发科的 EPD 驱动重写，不是移植而是重做 |

PW1 的 2.6.31 不在考虑范围：那一代的内核太老，Android 8.1 要的东西要补的量和重写相当。

## 二、已经是"换个机型就能用"的部分

内核补丁共 85 个：`patches/android-core/` 下 84 个，加上 `patches/` 根目录 1 个让 3.0.35 的构建脚本能在新版 Perl 下运行的修正（`patches/test-kernel/` 下另有 2 个只用于 QEMU 测试内核，不计入）。其中 **56 个不碰硬件代码**（binder、ashmem、SELinux、seccomp、qtaguid、路由、ASLR、capabilities、TCP、计时等），**29 个碰硬件代码**，约三分之二与硬件无关。

判定看每个补丁改到的文件：只要有一个文件属于某颗具体芯片的驱动，或属于 SoC／板级代码（`arch/arm/mach-mx6/`），就算碰硬件。芯片驱动指 EPDC 与 PXP（`drivers/video/mxc`、`drivers/dma/pxp`）、USB 控制器（`drivers/usb/gadget/arcotg_udc.c`）、max77696 电源管理芯片的各个子驱动（`drivers/power`、`drivers/mfd`、`drivers/hwmon`、背光、电源键）、Cypress 触摸、霍尔传感器（`drivers/char/wario_hall.c`）和 ath6kl（`compat-wireless/drivers/net/wireless`）。不算的是 `drivers/` 下的通用框架（`drivers/staging/android` 里的 binder 与 ashmem、`drivers/base/power` 的唤醒源、`evdev`、USB gadget 的功能层 `android.c`／`f_mtp.c`、`uid_cputime`）、ARM 架构公共代码（`arch/arm/kernel`、`arch/arm/mm`）和 `compat-wireless` 里的 cfg80211。按这个标准，碰硬件的 29 个补丁改的全是硬件相关文件，没有一边改驱动一边改通用代码的混合补丁（`1000` 把控制 boardid 的开关声明在 `drivers/staging/android/Kconfig` 里，按它控制的代码算）。

AOSP 一侧的补丁共 156 个，分在 25 个项目里（`frameworks/base` 65 个、`packages/apps/Launcher3` 21 个、`system/core` 11 个等）。它们都在用户空间、不改驱动，换机型基本原样沿用。少数是按本机的 CPU 或内核版本写的兼容处理：`bionic` 和 `swiftshader` 各有一个针对 Cortex‑A9 的修正；`wificond` 的 `0004`、`0005` 让它读得懂 3.0 内核的 cfg80211 缺少或用旧格式给出的属性，换到新内核上就成了走不到的后备路径。另有两个写进了本机的设备事实，换机型要复核，见第三节第 8 条。

这些也与机型无关：
- CPU 合成器（`composer/`）：按像素格式和矩形工作，源码里一个分辨率常量都没有（查过 `composer.cpp`、`LumaCompose.*`、`BufferFormat.h`）；已支持翻转与 90°/180°/270° 旋转。
- gralloc、电源 HAL、lights HAL 的结构（只有节点路径是设备相关的）。
- 启动交接的整体做法（`boot/`）：在原厂内核里装一个模块，由它跳进我们的内核；**每个机型+固件一份 JSON 配置**（`boot/profiles/*.json`），不是改代码。
- 输入法：尺寸全写在 `dimens.xml` 里、单位是 dp，随密度缩放；源码里唯一的 1072 是一句解释布局取舍的注释。
- 安装器在设备上跑的那段 C 代码：屏幕几何来自生成的 `screens.h`（`PW3_SCREEN_REGION_W` 等），代码里没有写死的分辨率。
- MTP/adb 的 USB 组合。

生成位图的两个程序不在此列：分辨率写死在程序的模块常量里，见下一节第 5 条。

## 三、每个新机型必须做的事

按工作量从大到小：

1. **原厂内核源码 + 补丁适配**（最大的一块）
   - 29 个补丁碰到硬件代码：EPDC（含 PXP）9 个（波形文件、温度、DMA、请求队列快照、开机第一帧、挂起等）、USB 控制器 8 个、max77696 电源管理芯片 5 个（电池与充电、电量计、前光、I2C、关机）、霍尔传感器与电源键 2 个、Wi‑Fi 2 个、触摸 1 个、板级的 IDME 与总线调频 2 个。
   - 新机型如果仍是 i.MX6SL + max77696 + ath6kl 这一套，多数能直接套用；Voyage 与 PW3 用同一个 defconfig，最可能属于这种情况，但板上实际是哪些芯片仍要上机核对。换了 SoC（i.MX6SLL/6ULL）、PMIC 或 Wi‑Fi 芯片，就要按同样的意图重写；Oasis 1 的 Wi‑Fi 已知是 Broadcom（第六节）。
2. **启动交接的配置**（`boot/profiles/<model>-<firmware>.json`）
   - 机器号、RAM 布局、原厂命令行、ATAG 种类、原厂函数的字节与哈希（交接模块要在原厂内核里找到它们）。
   - 这些都要在目标机器上读出来，不能照抄。
3. **显示契约**（`device/amazon/<model>/epdc/EpdcContract.h`）
   - 分辨率、旋转方向、每像素位数；本机写死 1072×1448 + `FB_ROTATE_CCW`，改成该机型的值。
   - 波形文件来自设备自己的分区（不打包）；灰阶曲线要按该面板重新量一次（本机的做法见 `eink-ui.md`）。
4. **AOSP 设备目录**（`device/amazon/<model>/`）
   - 密度（本机 `ro.sf.lcd_density=300`、`xhdpi`）、字号与边距的覆盖层、导航栏尺寸：按分辨率和物理尺寸重算。
   - 分区与数据镜像大小（本机 system 512 MiB、data 1.75 GiB，合计 2304 MiB；只读那个取装得下的最小的 2 的幂，数据那个按实测需要留余量）。
5. **设备上要显示的两张位图**（`installer/screens/render_screens.py`、`device/amazon/pw3/sleep/make-sleep-picture.py`）
   - 分辨率和每一个像素尺寸都是程序里的模块常量，**不是命令行参数**：`SCREEN_W/SCREEN_H`、`WIDTH/HEIGHT`，以及由密度 1.875 换算出来的字号（标题 28 sp、正文 20/17 sp）、行距、边距、进度条高度。换机型是按新密度把这一套重算一遍，不是改两个数；600×800 的机型（KT3、KT4）尤其要重排，1072 px 宽的版面放不进 800 px。
   - 休眠画面还要重新出图：`make-sleep-picture.py` 要求原图高度正好等于屏幕高度、宽度不小于屏幕宽度，否则直接退出。
   - C 侧不用动：几何从生成的 `screens.h` 来。
6. **安装器的机型检查**：型号或固件不符时显示 E101 并停止，电量不足显示 E102，空间不够显示 E401。核对的依据（原厂内核版本串等）和 E101 画面上写的机型名都来自第 2 条那份启动配置，新机型要一并写上。
7. **重新测量**：冷启动、翻页延迟、功耗、灰阶。本机的数值不能照搬。功耗用 `tools/power-log.c` 记录，它读写的是 PW3 的 max77696 电量计和前光，换 PMIC 要改；本项目还没有按实测写 `power_profile.xml`。
8. **按机型复核两个框架补丁**：它们写进去的是本机的事实，不是 AOSP 的缺陷。
   - `0043`（电池统计只问会回答的无线电）：本机 ath6kl 不记录链路层统计，也没有调制解调器，所以这两项一次都不问。换成会报告链路层统计的 Wi‑Fi 芯片时要复核——补丁把 `getSupportedFeatures()` 的结果缓存起来只问一次，若 Wi‑Fi 服务还没就绪时返回 0，会被当成"不支持"记住。
   - `0044`（没有日历提供者时直接回答"没有事件"）：本机精简掉了日历应用。若某个机型的镜像装回日历，这条就是多余的。

## 四、建议的仓库结构（等真有第二个机型时再动）

现在所有设备相关的东西都在 `device/amazon/pw3/` 一个目录里。等第二个机型出现，再把以下内容提到 `device/amazon/common/`：
合成器、gralloc、电源/lights HAL 的通用部分、输入法、安装器、休眠画面、`sepolicy` 的通用规则。

**现在不提前重构**：没有第二台机器可验证，拆分只会在没有收益的情况下增加破坏现有系统的风险（本仓库的一贯做法是先量后改）。这份文档把边界写清楚，就是为了那一天来的时候知道怎么拆。

## 五、结论

- 这套系统**结构上支持多机型**：配置与硬件代码分得开，85 个内核补丁里 56 个与硬件无关，启动交接已经是按机型配置的。
- 但**每个新机型仍然是一次真实的移植工作**，不是改个参数：要有该机型的越狱、源码、一台机器，和重新测量。越狱和源码对几乎所有机型都已经具备（见第一节），所以真正的门槛是**原厂内核版本**和一台能上手的机器。
- v0.1.0 只在 Kindle Paperwhite 3（第 7 代）+ 固件 5.16.2.1.1 上验证过。文档和安装器都按这个事实写，不声称支持别的机型。
- **源码层面实测到哪一步，见第六、七节**：Oasis 1 与 PW3 拿到的是同一份内核源码，Voyage 的源码不同但当时的 79 个补丁全部可用，
  而且亚马逊自己写明 Voyage 与 PW3 用同一个 defconfig。两个机型的内核都能编出来，各自的 147 项内核配置请求全部生效；
  Oasis 1 还差 Wi‑Fi 驱动和交接私有页表的一处改动。
  这些都还没有在任何第二台机器上运行过。

## 六、实测：把这套补丁打到另外两个机型的原厂内核上（2026-10-21 至 10-23）

第二、三节对内核补丁的判断来自清点；这一节把补丁实际打到另外两棵原厂内核树上，再往下编译。

做法：补丁是有序累积的（后面的改动依赖前面的），所以每个先干跑、通过才真打，失败的跳过而
后面的继续——这样失败是「这一个打不上」而不是「从这里开始全崩」。**对照组是 PW3 自己的干净树**：
必须全部应用，否则方法本身有问题。对照组通过。实测时 `patches/android-core/` 共 79 个补丁；
此后加入的 4 个（`0403`、`1055`、`1056`、`1057`）还没有在另外两棵树上重打过。

| 机型 | 固件 | 原厂内核树 | 本项目 79 个补丁 |
|---|---|---|---|
| PW3（对照） | 5.16.2.1.1 | 42,058 个文件，总哈希 `5d6b2cd6…` | **79/79** |
| Oasis 1（第 8 代） | 5.16.2.1.1 | **与 PW3 逐字节相同**：42,058 个文件，总哈希一样 | **79/79**（同一棵树） |
| Voyage（第 7 代） | 5.13.6（它自己的末版） | 42,058 个文件，总哈希 `a609502a…`——**与 PW3 不同** | **79/79** |

两个结论：

**一、Oasis 1 和 PW3 拿到的是同一份内核源码。** 压缩包哈希不同（差 3,827 字节），但解开之后
42,058 个文件的内容总哈希完全一致，差别只在打包元数据。所以 Oasis 1 在「补丁落位」这一步的工作量
**是零**——补丁打的就是它固件编译时用的那棵树。

**二、Voyage 的树与 PW3 不同，但 79 个补丁仍然全部干净应用。** 不同固件、不同源码，补丁照样落位，
这一条比 Oasis 1 那条更有信息量。

### 机型差别在 defconfig 里，而亚马逊自己写明了哪台用哪个

源码包里的 `BUILD_HOW-TO.txt` 直接给了对应关系：

```
Kindle Oasis:                                                          imx60_duet_defconfig
Kindle Paperwhite (6th & 7th generation), Kindle Voyage (7th generation),
Kindle (7th generation):                                               imx60_wario_defconfig
```

所以 **Voyage 和 PW3 用同一个 defconfig**，Oasis 1 用 duet。这让 Voyage 成为代价最小的第二个
机型：同一个配置、补丁全部可用、分辨率同为 1072×1448、内存同为 512 MB。

`imx60_duet_defconfig` 与 `imx60_wario_defconfig` 差 **103 行**，其中与本项目相关的：

| 差异 | 对本项目意味着什么 |
|---|---|
| `VMSPLIT_2G` / `PAGE_OFFSET=0x80000000`（PW3 是 3G / `0xC0000000`） | 内核虚拟布局不同。**启动交接依赖内存布局**，影响见本节"剩下的两件事"里的交接一段 |
| `CONFIG_FALCON=y`，BIOS 固定在 `0x9F5E0000`/`0x9F5F0000` | PW3 是 `FALCON=n`。这片物理内存被占着 |
| `FB_MXC_EINK_WORK_BUFFER_ADDR=0x9F000000`，保留 4 MB | 交接的目的地要避开这片保留区；本机"目的地留在低 128 MiB"的规则已经把它排除在外 |
| Wi‑Fi 栈不同：PW3 是 ath6kl / AR6003（模块），duet 是内建 cfg80211 + LIB80211 + HOSTAP（板级里是 brcm） | 碰 Wi‑Fi 的两个补丁（`0310`、`1045`）要按新芯片重写；ath6kl 固件策略那一项（`CONFIG_LAB126_ANDROID_ATH6KL_FIRMWARE`）对它无意义，在 duet 的机型片段里关掉 |
| 接近传感器 `PIC12LF1822` → `SX9500` | |
| 命令行多 `no_console_suspend` | |

### 原厂内核按运行时板 ID 分机型，不是按编译期

这棵树里编译期只有两个变体（`MX6SL_WARIO_BASE`、`MX6SL_WARIO_WOODY`），而板级代码用
`lab126_board_is(BOARD_ID_WHISKY_WFO)` 这样的**运行时**判断区分更多板子（板 ID 来自 IDME）。
WOODY 守着的是霍尔传感器的「一次性」控制和电池驱动分支——磁吸外壳加外壳电池那一类硬件。

意思是：原厂一个内核二进制服务多块板子，机型差异有相当一部分不在源码分支里，而在运行时读到的
板 ID 上。移植时要查的是那台机器的板 ID 走进哪些分支，不是去找「这个机型的代码在哪」。

### 再往下一级：Voyage 的内核编出来了

补丁能打上不等于能编，所以按本项目自己的流程编了一次（同一个工具链 `aosp-arm-gcc49`、同样的
`KCFLAGS=-fno-pic -fno-pie`、同样的固定时间戳）：

| 步骤 | Voyage（5.13.6 的树） |
|---|---|
| `patches/*.patch` + `patches/android-core/*.patch` | **80 个全部应用**（当时 79 + 1） |
| `imx60_wario_defconfig` + 本项目的配置片段 | **本项目要求的 147 个 Kconfig 选项逐项核对通过** |
| `make uImage` | **退出码 0，零个 error**，`uImage` 3,075,740 字节 |

第二条比第三条更值得注意：`merge-kconfig.py --check` 是逐项核对「请求的每个选项在最终 `.config`
里真的是请求的那个值」，147 项全过说明**这套内核配置在 Voyage 的树上完全可满足**，没有哪一项
因为缺少依赖或被别的选项否决而落空。

### Oasis 1 也编出来了，但首次构建有 8 项配置静默落空

同样的流程，换 `imx60_duet_defconfig`。内核**编译通过**（`uImage` 3,245,276 字节、零 error），
但 `merge-kconfig.py --check` 报出 **8 个请求的选项没有生效**。这正是这道检查存在的理由：没有它，
编出来的是一个静默少了这些功能的内核。

| 没生效的 | 为什么 | 怎么处理 |
|---|---|---|
| `LAB126_ANDROID_POWER_SUPPLY_ABI`、`LAB126_ANDROID_BACKLIGHT_BLANKING`、`LAB126_ANDROID_PMIC_I2C_STRICT`、`WARIO_HALL_INPUT`、`LAB126_ANDROID_DISABLE_TOUCH_FW_UPDATE`、`LAB126_ANDROID_PRIVATE_IDENTIFIERS`（6 项） | **本项目自己的补丁在 Kconfig 里写了 `depends on MX6SL_WARIO_BASE`**，而 Oasis 1 是 `WOODY`。它们另外依赖的芯片（MAX77696 的 battery/charger/backlight/MFD、`WARIO_HALL`、Cypress 触摸）在 duet 配置里**都是开着的** | 依赖改成只写它改的那个驱动，见下一小节 |
| `LAB126_ANDROID_ATH6KL_FIRMWARE` | **真的是别的 Wi‑Fi 芯片**：duet 配置里没有 `ATH6KL_SDIO` | 关着是对的，由 duet 的机型片段写明关闭；Wi‑Fi 那两个补丁要按新芯片重写 |
| `CONFIG_ARCH_MMAP_RND_BITS`：要 16 得 8 | 它用 2G/2G 划分，用户地址空间只有一半，mmap 随机化的位数上限因此更低 | 这是那台机器内存布局的硬约束，ASLR 熵只能到 8，机型片段照实写 8 而不是硬塞 |

8 项里 6 项是本项目自己的写法问题，2 项是硬件事实，分别在下面两小节处理。

### Kconfig 依赖写硬件，不写板子

`MX6SL_WARIO_BASE`（"Classic Wario"）和 `MX6SL_WARIO_WOODY`（"Woody based Wario"）是同一个
`choice` 的两支，PW3 是前者，Oasis 1 是后者。而这 7 个选项（上表的 6 项加 ath6kl 固件那一项）各自改的是**一个驱动**，
依赖应当写这个驱动：

| 选项 | 它改的文件 | 改前的依赖 | 现在的依赖 |
|---|---|---|---|
| `LAB126_ANDROID_POWER_SUPPLY_ABI` | `drivers/power/max77696-{battery,charger}.c` | `MX6SL_WARIO_BASE && BATTERY_MAX77696 && CHARGER_MAX77696` | `BATTERY_MAX77696 && CHARGER_MAX77696` |
| `WARIO_HALL_INPUT` | `drivers/char/wario_hall.c` | `WARIO_HALL && INPUT && MX6SL_WARIO_BASE` | `WARIO_HALL && INPUT` |
| `LAB126_ANDROID_BACKLIGHT_BLANKING` | `drivers/video/backlight/max77696-bl.c` | `BACKLIGHT_MAX77696 && MX6SL_WARIO_BASE` | `BACKLIGHT_MAX77696` |
| `LAB126_ANDROID_DISABLE_TOUCH_FW_UPDATE` | `drivers/input/touchscreen/cyttsp4_loader.c` | `MX6SL_WARIO_BASE && TOUCHSCREEN_CYPRESS_CYTTSP4` | `TOUCHSCREEN_CYPRESS_CYTTSP4` |
| `LAB126_ANDROID_PMIC_I2C_STRICT` | `drivers/mfd/max77696-i2c.c` | `MFD_MAX77696 && MX6SL_WARIO_BASE` | `MFD_MAX77696` |
| `LAB126_ANDROID_PRIVATE_IDENTIFIERS` | `arch/arm/mach-mx6/boardid.c` | `MX6SL_WARIO_BASE` | `LAB126`——`boardid.o` 本来就是 `obj-$(CONFIG_LAB126)` 编出来的 |
| `LAB126_ANDROID_ATH6KL_FIRMWARE` | `compat-wireless/…/ath6kl/` | `MX6SL_WARIO_BASE && LAB126 && ATH6KL_SDIO` | `LAB126 && ATH6KL_SDIO` |

依据不是「看起来应该行」：改之前，这 7 个补丁里 `WARIO_BASE` 这个字串**各只出现一次**，就在那条
`depends on` 上，代码体里一处都没有。所以板级符号只是作者当时手上那块板，不是条件。

**结果**（同一流程，`imx60_duet_defconfig` + 本项目的配置片段）：静默落空从 **8 项降到 2 项**，
剩下两项都是硬件事实而不是本项目自己挡的——`ARCH_MMAP_RND_BITS` 要 16 得 8（那台机器 2G/2G
划分的硬约束），`LAB126_ANDROID_ATH6KL_FIRMWARE` 得 n（duet 板上没有 ath6kl，关着是对的；问题在
配置片段无条件要它，见下一小节）。

Oasis 1 的内核**编译通过、0 error**，uImage **3,245,980 字节**——比改依赖之前的 3,245,276 多
**704 字节**，正是这 6 段代码第一次被编进去；28 个模块。

**对照组，PW3 必须一字不变**：同一棵改过的树，换 `imx60_wario_defconfig` 加同一个片段，147 项
请求全部生效，生成的 `.config` 与发行内核那一份**逐字节相同**。Kconfig 文本不进二进制（内核是
"源码 + .config"的函数），所以改依赖对 PW3 内核零影响。为了不只停在推理上，又从新解开的源码树把
发行内核整棵重建了一次：`zImage`（发行物用的就是它）、`System.map`、`Module.symvers`、内核配置和
28 个模块全部逐字节相同；`uImage` 只有开头 64 字节的 U-Boot 头不同，那是 `mkimage` 写进去的创建时间
和随之变化的头校验和，数据校验和两份一样。

### 配置片段分两半：一半是这套 Android 要的，一半是这块板子有的

依赖改对之后，duet 上剩下的 2 项落空出在**配置片段**：片段无条件要
`LAB126_ANDROID_ATH6KL_FIRMWARE=y`（那块板没这颗芯片）和 `ARCH_MMAP_RND_BITS=16`（那台机器的上限是 8）。
还有一条更隐蔽——`CONFIG_LOCALVERSION="-lab126-pw3-android81"` **在任何板上都会被接受**，所以为 Oasis 1
编出来的内核会自称 pw3，而 `/system/lib/modules` 就是按这个字串命名的。Kconfig 不会拒绝它，所以任何
检查都抓不到。

所以片段按"回答哪个问题"拆开：

| 文件 | 回答什么 | 项数 |
|---|---|---|
| `config/android-core.config` | 这套 Android 对**任何**内核的要求（Binder、ashmem、cgroup、SELinux、文件系统…） | 144 |
| `config/models/pw3.config` | 这台 PW3 的事实：3G/1G 划分所以 16 位随机化、AR6003 所以要 ath6kl 固件策略、内核自称 `-lab126-pw3-android81` | 3 |
| `config/models/duet.config` | 这台 Oasis 1 的事实：2G/2G 划分所以 8 位、没有 ath6kl 所以关掉、自称 `-lab126-duet-android81` | 3 |

这几个片段都**不碰** VMSPLIT / PAGE_OFFSET，这两项跟随 defconfig——移植时少一处要改的。

`merge-kconfig.py` 收多个片段，并且**拒绝**同一个选项被两处请求，也拒绝一个片段里重复写同一项——不是按顺序覆盖。按顺序
覆盖的话，值取决于参数顺序，理由写在两个地方而没有任何东西说哪个生效；改基础片段的人看不到某个
机型早已悄悄覆盖过它。机型要不同的值，前提是基础片段不要有这个值。

**结果**：两个机型都是 **147 项请求、0 项落空**，而 PW3 的 `.config` 仍与发行内核**逐字节相同**。
`tools/tests/test_kconfig_fragments.py` 里有 6 项守着这条拆分：两半不许有交集、一个片段内不许重复、
`build-kernel.sh` 认识的机型必须各有片段、**每个**调用方都必须把两半都传进去，以及 `merge-kconfig.py`
本身的拒绝与合并。调用方那一条防的是只传基础片段：那样核对照样通过，机型那 3 项却没人问——
`prepare-kernel-hardware.sh` 在每次 AOSP 构建时都跑，最初就只传了基础片段。

### 剩下的两件事：Wi‑Fi 与交接

**一、Wi‑Fi：芯片是 Broadcom BCM4343W，驱动还没有能用的。** `imx60_duet_defconfig` 里 `CONFIG_WLAN=y`，而 **ath6kl 三项全是 `is not set`，也没有任何 Broadcom／Marvell 驱动被打开**；整份 defconfig 里编成模块（`=m`）的 31 项中一个 Wi‑Fi 驱动都没有。芯片型号写在板级文件里。

对照两份 defconfig：`imx60_duet_defconfig` 有 `CONFIG_MX6SL_WARIO_WOODY=y`、
`CONFIG_BLUETOOTH_PWRCTRL=y`、`CONFIG_WARIO_WOODY_WAN=y`，而 `imx60_wario_defconfig`（PW3）
三个都没有。`CONFIG_MX6SL_WARIO_WOODY` 守着的那段代码在 `arch/arm/mach-mx6/board-mx6sl_wario.c`
里（duet 没有自己的板级文件，和 wario 共用一个、靠 `lab126_board_is()` 在运行时分流），
注释是 `/* Broadcom 4343W Bluetooth structures */`，配套 `brcm_gpio_wifi_bt_init()`、
`brcm_gpio_wifi_power_enable()`、`brcm_gpio_wifi_set_normal_mode()`，蓝牙经 UART3
（`imx6sl_add_imx_uart(2, &mx6sl_wario_uart3_data)`，只对 `BOARD_ID_WHISKY_WFO`、
`BOARD_ID_WHISKY_WAN`、`BOARD_ID_WOODY` 启用）。

**BCM4343W 是 Wi‑Fi 加蓝牙的二合一芯片**，Wi‑Fi 走 SDIO。它的 FullMAC 驱动是 `brcmfmac`，
这份归档的 `compat-wireless/drivers/net/wireless/brcm80211/brcmfmac/` 里有 18 个文件
（同目录下还有 `brcmsmac` 51 个、`ath/ath6kl` 41 个、`mwifiex` 36 个）。PW3 是用内核配置
把 compat-wireless 这棵树接进来的（`CONFIG_COMPAT_WIRELESS=y`、`CONFIG_ATH6KL=m`、
`CONFIG_ATH6KL_SDIO=m`），duet 没有接，所以它的无线模块是在内核配置之外另行构建的。
第七节实际构建过：这份归档里的 brcmfmac 编不过它自己的内核，原厂 Oasis 1 上跑的那份驱动不在 GPL 包里。

所以这一项剩下的顺序是：

1. 上机确认固件与 NVRAM 校准文件在原厂分区里的路径和名字。BCM4343W 和 ath6kl 一样需要它们；
   它们是每台设备自己的，按本项目对设备专属数据的做法，和屏幕波形一样从这台机器的原厂系统现场读取，不打包；
2. 拿到一个能加载的驱动，三条路见第七节末尾的表；其中回移主线 `brcmfmac` 对 BCM4343W 的支持是 B 那条路；
3. 本项目那两个补丁（`0310` 固件与板级数据策略、`1045` 等待网卡被识别）写的是 ath6kl 的事实，
   要按新驱动重写——这是最后一步。

**另外要注意**：这份 `imx60_duet_defconfig` 来自 **PW3 固件 5.16.2.1.1 的源码归档**。亚马逊一棵树里放着许多机型的 defconfig，只有本次发布对应的那个必然是最新的。所以"duet 的 defconfig 里没有 Wi‑Fi 驱动"证明的是**这份归档里的那个 defconfig**如此，不等于 Oasis 1 出厂固件就是这么编的。本节全部实测都建立在这份归档上，这一点对每条结论都同样成立。

**二、交接：2G/2G 上 lowmem 本身就是恒等映射，要改的只有私有页表的一处。**

容易得出的错误结论是"这套机制在 2G/2G 上不成立"：如果把 `identity_limit` 当成 `TASK_SIZE`，2G/2G 下它是 `0x7F000000`，低于物理内存起点 `0x80000000`，看起来一个合法目的地都没有。漏掉的一步是：那台机器上 `PAGE_OFFSET` 和 `PHYS_OFFSET` 相等，lowmem 本身就是恒等映射。推导如下。

先说清本机为什么要自己建一个窗口。交接的物理计划有一条契约：所有目的地必须在 `identity_limit` 以下。本机 3G/1G 时 `PAGE_OFFSET=0xC0000000`，而 i.MX6SL 的 `PHYS_OFFSET=0x80000000`，两者**不等**——`__phys_to_virt(x) = x − PHYS_OFFSET + PAGE_OFFSET`，所以 lowmem 的虚拟地址比物理地址高 `0x40000000`，内核自己的映射**不是**恒等的。于是 `stage.c` 的 `prepare()` 把私有页表的 `[0, 0xBF0)` 这 3056 个 1 MiB 段清零、自己写进要用的恒等段，`[0xBF0, 4096)` 照抄活页表（模块自己的代码和内核都在那上面，`pw3_enter_private_mapping` 写完 TTBR0 还要再执行一条 `bx` 才跳进控制页，所以它必须留着）。`identity_limit` 因此等于 `IDENTITY_SECTIONS<<20`，而那个数又正好是本机的 `TASK_SIZE`——这是巧合，不是因果。

duet 是 `CONFIG_VMSPLIT_2G`（机型片段里 `ARCH_MMAP_RND_BITS=8` 的由来）：`PAGE_OFFSET=0x80000000`，而 MX6SL 的 `PHYS_OFFSET` 也是 `0x80000000`（`plat-mxc/include/mach/memory.h`；这块树按 `CONFIG_RUNTIME_PHYS_OFFSET` 在运行时取值，`stage.c` 读的就是运行时的 `PHYS_OFFSET`）。两者相等代入上面那条式子：**`virt == phys`，内核的 lowmem 映射就是一张现成的恒等表**。

所以那台机器上根本不需要"在用户那一半建窗口"，交接要的 1:1 映射内核已经给了。真正要改的只有一处，而且是局部的：`prepare()` 现在无条件把 `[0, identity_limit)` 清零，这个布局假设窗口在用户半边；duet 上目的地在内核半边且已经 1:1，而模块自己的代码在 `0x7F000000`–`0x80000000`（`MODULES_VADDR = PAGE_OFFSET − 16 MiB`）——**正好在要清零的范围里**，清了它，`pw3_enter_private_mapping` 写完 TTBR0 就会取不到下一条指令。按这台机器的事实，私有表应当原样照抄活页表、什么都不清。

所以契约应当这样表述：`identity_limit` 是「我们将要运行的那张表里，1:1 区域的上界」——本机 `0xBF000000`（窗口是模块自己建的），duet 是 lowmem 的终点（512 MiB 即 `0xA0000000`，由内核提供）。`pw3_plan.c` 那条 `identity_limit > boot_ram_base` 的检查在 duet 上**通过**（`0xA0000000 > 0x80000000`）；它拒绝的是"上界低于内存起点"这种**写坏了的计划**，不是在拒绝某类机器。

**旁证**：原厂内核自己的 `setup_mm_for_reboot()` 就是 `identity_mapping_add(current->active_mm->pgd, 0, TASK_SIZE)`（`arch/arm/mm/idmap.c`），kexec 随后 `cpu_reset(reboot_code_buffer_phys)`。也就是说本项目这条 `identity_limit` 契约和内核自己 soft-boot 的做法是同一套，并非本项目独创；而在 `PAGE_OFFSET == PHYS_OFFSET` 的机器上，内核那句 `identity_mapping_add(…, 0, TASK_SIZE)` 同样覆盖不到 RAM——它不需要覆盖，因为 lowmem 已经是恒等的。

**这一条由构建检查，不靠文档。** `stage.c` 的 `IDENTITY_SECTIONS` 从配置文件的 `identity_limit`
推出来，不另写一个常数；旁边一条断言用**内核自己的** `MODULES_VADDR` 核对模块区在界限之上：

```c
BUILD_BUG_ON(TARGET_IDENTITY_LIMIT>(unsigned int)MODULES_VADDR);
```

本机两者相等（都是 `0xbf000000`）所以通过；按 duet 填一个 `identity_limit`（要盖过内存顶端，
`0xA0000000`）就会高过它的模块区 `0x7f000000`，**模块编译不过**，错误直接落在这一行。反向验证过：把
PW3 配置里的 `identity_limit` 改成 `0xc0000000` 重编，构建停在这一行，报 `size of unnamed array is negative`。
所以做 duet 的人会在构建时被拦住，并被注释指到这一节，而不是在一台停在 TTBR0 之后、连日志都写不出来的
设备上查。那次移植要做的是让私有页表多保留一段从活页表照抄的区域（模块区），而不是把配置里的界限
写大；本项目不预先写一条没有机器能跑的路径。

另外两点：

- Falcon 与 e-ink 工作缓冲占的 `0x9F000000`–`0x9F610000` **不需要单独写进 `forbidden`**：本机的规矩是"目的地留在低 128 MiB"（`0x80000000`–`0x88000000`），那条规矩已经把它排除在外；
- 上真机之前，先在 QEMU 的 vexpress 上拿一棵 `VMSPLIT_2G` 的测试内核跑通（测试内核由 `tools/build-loader-stage-kernel.sh` 构建，地址空间划分写在 `config/test-kernel/vexpress.config` 里）。注意 vexpress 的 RAM 起点是 `0x60000000`，和 `PAGE_OFFSET=0x80000000` 不相等，所以它复现不了"lowmem 恒等"这一条——要复现得把测试内核的 `PHYS_OFFSET` 也对齐，或者接受它只验证"照抄活页表"这条路径本身。

### 这一节没有证明的事

补丁能打上**不等于能编**，能编**不等于能跑**。上面全部是源码层面的结论，没有任何一行在第二台
机器上执行过。Oasis 1 内核侧还剩两件：**Wi‑Fi 驱动**（芯片已知是 BCM4343W，驱动从哪来见第七节），
以及**让私有页表保留模块区、不清掉已经是恒等的 lowmem**（那台机器 `PAGE_OFFSET == PHYS_OFFSET`，
交接要的 1:1 映射内核已经给了，所以这一项比本机简单）。配置本身已经齐了（147 项请求 0 项落空），
安全参数那一项按硬件事实写进了机型片段；Voyage 虽然和 PW3 同一个 defconfig、而且同为
3G/1G 所以交接机制对它成立，仍然需要那台机器、它的板 ID、交接配置和重新测量。

## 七、实测：Oasis 1 的 Wi‑Fi 不是配置问题，是驱动问题（2026-10-23）

第六节查明芯片是 BCM4343W、归档里带着 brcmfmac 的源码。这一节按 duet 的 defconfig 实际构建了
四次，每次失败回答一个问题。

**1. `CONFIG_BRCMFMAC=m` 配出来是 `n`。** 归档里有 `compat-wireless/drivers/net/wireless/brcm80211/`
（18 个 brcmfmac 文件），但 `compat-wireless/Kconfig` 只 `source` 了 ath，
`compat-wireless/drivers/net/wireless/Makefile` 也只有 `obj-$(CONFIG_ATH_COMMON) += ath/`。
**源码在，接线不在。**

对照 PW3 这一侧：`imx60_wario_defconfig` 开着 `CONFIG_COMPAT_WIRELESS=y`、
`COMPAT_WIRELESS_MODULES=m`，而内核顶层 Makefile 有 `drivers-y += compat-wireless/`——
所以本项目一直在用的 ath6kl 就是从 compat-wireless 编出来的，这条路本身是通的。

**2. 接上 Kconfig 和 Makefile 之后，编译报 `KERNEL_VERSION` 未定义、`netdev_hw_addr` 没有
`dmi_addr`。** 同一个根因：ath 的 Makefile 第一行是
`NOSTDINC_FLAGS += -I$(srctree)/compat-wireless/include`，brcm80211 的 Makefile 没有这一行，
所以 compat 的垫片头文件根本不在包含路径上。

**3. 补上之后换成链接冲突**：`multiple definition of wext_handle_ioctl`、`lib80211_get_crypto_ops`……
duet 的 defconfig 把内核自带的无线栈（`LIB80211` 及三个软件密码、`HOSTAP`、`WEXT_SPY`、
`CFG80211=y`）编进内核，而 compat 又提供同名符号；wario 的 defconfig 这几项全是关的。
把它们按 wario 的样子关掉、`CFG80211` 改成 `m`、补上 `NL80211_TESTMODE=y`（compat 的
`nl80211.c` 要它才编得过）之后，这一层过了。

**4. 然后报错落到 `drivers/staging/brcm80211/`**，而不是 compat 那一份：这棵树里 brcm80211
**有两份**，staging 里那份是更旧的快照，而且两份声明的是**同一批符号**（`BRCMFMAC`、
`BRCMUTIL`），所以选不开；`CONFIG_STAGING=y` 又是本项目必须开的（binder、ashmem 在那里）。
staging 那份编不过这棵内核：`wl_iw.c` 用了 `dhd_wait_pend8021x`，以及 `iw_handler_def` 里
早就没有的 `num_private`／`private_args` 字段。

**结论**：这份归档里**没有一份能对着它自己的内核编出来的 Broadcom 驱动**。原厂 Oasis 1 上跑的
那个驱动，源码不在这份 GPL 包里。所以 Oasis 1 的 Wi‑Fi 驱动有三条路。推荐 A：它不用新写驱动代码，
也符合本项目对私有二进制的处理原则（不分发，只在使用者自己的设备上读取）；它是否成立取决于原厂模块
能否加载进本项目的内核，这要在真机上核对。

| 路 | 做什么 | 代价 |
|---|---|---|
| **A（推荐）** | 安装时从**使用者自己的设备**上取出原厂的 `.ko` 并加载 | 要求本项目的内核能加载原厂编出来的模块（vermagic 与符号对得上）。目前没有现成的路径：启动程序里原有一段加载原厂 REAGL 模块的分支，因为三个前提都不具备（内核按不带 REAGL 构建、没有步骤提供模块、没有任何地方打开它），在 v0.1.0 之前删掉了；安装器也还没有从设备上提取模块的步骤。做这个方案时要连同这两部分一起补上 |
| B | 把较新的 brcmfmac 回移到 3.0.35 | 真正的移植工作量，而且固件和 NVRAM 仍然要从设备上取 |
| C | 修 staging 那份的 wext 用法 | 它是 3.0 时代的快照，对 BCM4343W 这颗 FullMAC 的支持本身存疑 |

机型片段 `config/models/duet.config` 里写的是这个结论，而不是一串「打开 brcmfmac」的配置——
配置能写出来，但它会打开一个编不出来的驱动，那是把问题藏进构建里。
