# Android 下的 USB 文件传输（MTP）：调研与方案

Android 侧的 USB 文件传输（MTP）和 adb 是怎么接到 PW3 的 Linux 内核 3.0 上的：需求、调研结论、内核与用户空间的方案、上机要验的项目，以及开发中遇到的三个问题。2026-10-15 调研，2026-10-16 实现。

## 要什么

- 普通使用者把 Kindle 接到电脑上就能拷文件：Windows、macOS（Android File Transfer 或 OpenMTP）、Linux 都认 MTP，不用装驱动。
- 发行版（`user` 构建）默认关闭 adb（见 `release-build.md`），开发者选项里可以打开，与 MTP 同时用；电脑第一次连接时 Kindle 上弹出授权对话框。
- USB 在 Android 下必须始终可用：原厂系统之外，它是电脑与 Readom 之间唯一的通道——Android 跑起来之后，原厂的 MTP 就不在了。

## 起点

做这套设计之前，Android 侧的 USB 只为开发用的 adb 准备：

- 内核：Linux 内核 3.0.35 的 `g_ffs`（`CONFIG_USB_FUNCTIONFS=m`），只有一个 FunctionFS 实例，给 adb 用。UDC 是 `arcotg`，以模块加载。
- **USB 模块只在开发用的测试入口加载**（`ro.boot.pw3.adb=1`）。正常启动时 Android 下没有任何 USB 数据功能。
- `g_ffs` 没有 `/sys/class/android_usb`，所以：
  - AOSP 8.1 的 UsbService 里不会创建 UsbDeviceManager，也就没有 USB 状态、MTP、"USB 用途"选择；
  - 没有 UsbDebuggingManager，电脑第一次连 adb 时的授权对话框接不上，只能预先放入主机公钥。
- 而且 8.1 的 SystemServer 只在设备声明了 USB 主机或配件功能时才启动 UsbService，PW3 两者都没有。

## 调研结论

| 问题 | 结论 | 依据 |
|---|---|---|
| 8.1 的 MTP 用哪种内核接口 | `MtpServer` 按 `/dev/usb-ffs/mtp/ep0` 是否存在选 FunctionFS（`MtpFfsHandle`），否则用内核 `f_mtp` 的 `/dev/mtp_usb`（`MtpDevHandle`） | `frameworks/av/media/mtp/MtpServer.cpp` |
| 走 FunctionFS 做 MTP 行不行 | 不行：要 `FUNCTIONFS_ENDPOINT_ALLOC`、`FUNCTIONFS_ENDPOINT_DESC`、多实例 `g_ffs`、v2 描述符，3.0 的 `f_fs.c` 都没有 | 3.0 `f_fs.c`、`g_ffs.c` 只有一个静态实例 |
| **8.1 的 adbd 还认 android-3.0 的 `f_adb`（`/dev/android_adb`）吗** | **不认**：`usb_init()` 直接调 `usb_ffs_init()`，旧接口已删除 | `system/core/adb/daemon/usb.cpp` |
| 那 adb 怎么和 MTP 同时用 | 组合 gadget 里放一个 FunctionFS 功能：android-3.4 内核的 `android.c` 就有（"ffs" 功能，"adb" 是它的别名），8.x 时代跑在 3.4 内核上的设备都这样用 | AOSP `kernel/common` deprecated/android-3.4（40bb591c）`android.c` |
| 3.0 的 `f_fs.c` 能不能接进去 | 能：回调接口（`functionfs_ready_callback`、`closed`、`check_dev`）和 3.4 版相同 | 本机内核 `f_fs.c`、`g_ffs.c` |
| 插线后使用者要做什么 | 8.1 默认数据"锁定"：Kindle 上只显示"正在通过 USB 充电"，要下拉通知点"传输文件"，电脑才看得到文件；每次重插都要再点 | `UsbDeviceManager.java`：`mUsbDataUnlocked` 断开即复位 |

## 方案

### 内核

| 补丁 | 内容 |
|---|---|
| `android-core/1040-android-gadget-mtp-ptp-functionfs.patch` | android-3.0（563897de）的组合 gadget，只留 MTP、PTP；adb 是 android-3.4 的 "ffs" 功能（FunctionFS），"adb" 为其别名；`composite.c` 加 `usb_remove_config()`；编成模块 `g_android.ko`。它自己注册 functionfs 文件系统，所以不再编 `g_ffs` |
| `android-core/1041-wakeup-source-stats-follow-android.patch` | 与 MTP 无关，只是同一批构建：唤醒源统计改成 Linux 内核 3.4、也就是 Android 读取的格式（`hit_count` 换成 `wakeup_count`，新增 `expire_count`） |
| `android-core/1042-usb-no-keep-awake-debug.patch` | 去掉调试开关 `pw3_keep_awake`（让 USB PHY 常醒、保留一份时钟引用）：1039 让插线能唤醒控制器之后，插线检测已经正常，不再需要它 |

- 编译检查（2026-10-16）：三份补丁一起，`g_android.ko` 79 KB，vermagic 与现内核一致，所有模块的重定位类型都受支持，无未解析符号。
- 必须带 `KCFLAGS='-fno-pic -fno-pie'`（这个 GCC 默认生成位置无关代码，3.0 的模块加载器处理不了）。

### 用户空间

| 位置 | 改动 |
|---|---|
| `tools/kernel-hardware/init.pw3.usb.modules.rc` | 每次开机都加载 `fsl_otg_arc → arcotg_udc → g_android`，每步之后由 `pw3-usb-check` 核对绑定；在 `post-fs` 同步完成，保证 system_server 启动 UsbService 时 `/sys/class/android_usb` 已在 |
| `pw3-usb-check`（`UsbGate.cpp`） | 只在 adb 必须认证的构建上放行（`ro.secure=1`、`ro.adb.secure=1`）；按 `prepare → otg → udc → gadget` 逐级核对，最后一级核对 `g_android`、`android_usb` 和 FunctionFS；`g_ffs` 等其他 gadget 模块一律视为冲突；`serial` 一级：USB 序列号每台机器随机生成一次，存在 `persist.pw3.usb.serial`，不读出厂序列号 |
| `device/.../init.pw3.usb.rc` | gadget 核对通过后，开机时写入厂商、产品名、序列号和 ffs 别名，挂载 adb 的 FunctionFS；`pw3-ffs-check` 确认挂载无误、adb 认证已开，才置上 `sys.pw3.ffs_ready=1`，没有它 adbd 不启动。`none`、`adb`、`mtp`、`mtp,adb`、`ptp`、`ptp,adb` 的切换都写在这个文件里（不导入 AOSP 的 `init.usb.rc`），是 AOSP 非 configfs 设备 `init.<hw>.usb.rc` 的标准写法：VID 18d1，PID 依次为 `adb` 4EE7、`mtp` 4EE1、`mtp,adb` 4EE2、`ptp` 4EE5、`ptp,adb` 4EE6。**带 adb 的配置分两步**：先 `start adbd`，等它写完 FunctionFS 描述符、置上 `sys.usb.ffs.ready=1` 再 `enable 1`；adbd 停止或重启时把这个属性清零（原因见下文「插着线，总线上什么都没有」） |
| 产品属性 | `persist.sys.usb.config=mtp`（`initialization.mk`）。`userdebug` 构建里 `post_process_props.py` 的 `mangle_default_prop` 会给它接上 `,adb`（条件是 `ro.debuggable=1`），`user` 构建不接。开机后 `UsbDeviceManager` 又把 `mtp`、`ptp` 从这个属性里剥掉（AOSP 原注释：OTA 不会重置它），`getDefaultFunctions()` 在打开了 adb 时只返回 `adb`，没打开时返回 `mtp`。所以过过一次框架之后，打开了 adb 的设备开机先是 adb gadget，MTP 由框架起来以后的 UsbDeviceManager 切上去（见下面 frameworks/base 一行）；产品默认值 `mtp` 只管第一次开机。**这一步在 2026-11-04 之前每次都失败**：`waitForState` 只等 1 s，而 init 那一刻正串行跑 `bootstat` 的四个 exec（1.2–2.1 s），切换排在后面，超时后退回 `adb`——开了 USB 调试的设备开机后没有 MTP（g、h、i 三版的开机日志）。frameworks/base 补丁 0075 让这个等待乘 `ro.hw_timeout_multiplier`（4 s），k 版开机实测切换成功 |
| adbd 补丁（`system/core`） | `0002-pw3-adbd-usb-only`：只走 USB、不开 TCP、必须认证，并在 adbd 最终的用户和 SELinux 上下文下确认 FunctionFS 的 ep0 能打开。`0005-pw3-adbd-without-boot-opt-in`：去掉 0002 原有的"开机参数显式开启"（`ro.boot.pw3.adb=1`）要求，adb 开不开交给 Android 自己的开关（开发者选项、`sys.usb.config`） |
| frameworks/base | `0032-pw3-usb-service-for-usb-device`：内核提供 `/sys/class/android_usb` 时也启动 UsbService。`0033-pw3-usb-files-without-choice-when-unlocked`：没有安全锁屏（PIN、图案、密码）、也没有禁止 USB 文件传输的限制时，开机完成后和每次拔线后的默认状态就是 MTP、数据已解锁（等同使用者点了"传输文件"；打开了 adb 时是 `mtp,adb`）；设了锁屏密码保持 AOSP 的做法；拔线期间新设了密码，下次插线重新锁定 |
| SELinux | MTP 沿用 AOSP 的策略（`/dev/mtp_usb` 已标为 `mtp_device`）。本项目只为自己的检查程序加规则：`pw3_init_check` 可以查看 adb 的 FunctionFS（`functionfs` 的 search、getattr），`persist.pw3.usb.` 属性归 `pw3_boot_prop`（`device/amazon/pw3/sepolicy`） |

**插线自动解锁的理由**：原厂 Kindle 插线即出现 U 盘；只会插线的使用者不会去 Kindle 上点通知。没有锁屏密码时，拿到设备的人本来就能在界面上看到全部内容，自动解锁不增加暴露面；设了密码，就要先解锁再选"传输文件"，与 AOSP 相同。解锁状态在拔线时就预先设好，而不是插线那一刻再切：切换功能要重启 gadget，带 adb 时 adbd 也跟着重启，可能超过 UsbDeviceManager 1 秒的断开去抖，去抖一到就恢复默认状态、重新锁上。

## 上机验证

0. **开机时没插线、之后再插**：电脑要认出设备（下文「插着线，总线上什么都没有」就是这一条不过）。开机就插着线也要认。
1. Windows、macOS（OpenMTP 或 Android File Transfer）、Linux（gvfs）都能看到存储并读写大文件。
2. 调试版：adb 与 MTP 同时可用；电脑第一次连 adb 时 Kindle 上弹出授权对话框。
3. 发行版设置：开发者选项里打开、关闭 adb。
4. 拔插 50 次，醒着、睡着各半；休眠中插线能唤醒（内核 1036、1039）。
5. 接充电头时能休眠，接电脑时不休眠。
6. 大文件传输速度。

已有的实测（2026-10-23，公开发行前的开发构建；macOS 上用 libmtp 跑 `tools/test-mtp.sh`）：设备在 MTP 下被认出；32 MB 随机数据上传 5.1 s（6.3 MB/s），取回 1.6 s（20.0 MB/s）；同时经 adb 在设备上找到这个文件，两侧 SHA-256 一致。这覆盖了第 1 项的 macOS 部分、第 2 项的"adb 与 MTP 同时可用"和第 6 项。拔插 50 次、Windows 和 Linux 上的读写尚未在真机上验证；其余各项没有单独的实测记录。

## 踩过的坑

开发期间在设备上遇到的三个问题，每个按现象、原因、现在的做法写。

### 没插线时 adbd 一就绪，系统就卡死

**现象**：2026-10-16，这套 gadget 第一次在设备上跑：不插线点入口启动 Android，约 40 秒后系统卡住，看门狗复位回原厂系统。

**证据**（原厂内核保存下来的上一次启动的内核日志）：
- 前 40 秒正常：存储检查、各服务、USB 接口检查（`pw3-ffs-check`）都通过。
- 40.6 s 启动 adbd，40.87 s 两条 FunctionFS `read descriptors`（新格式被 3.0 拒绝，再写旧格式），之后没有任何记录，也没有看门狗预超时中断的输出（`imx2_wdt.c` 的预超时中断会打印寄存器）。
- 说明 CPU 在关中断的状态下停住：访问时钟已关的外设，总线不返回，60 秒后看门狗复位。

**原因**：
- adbd 写完字符串后，`functionfs_ready_callback` → `android_enable` → `usb_gadget_connect` → `fsl_pullup`，它不管控制器状态，直接读写 USBCMD。
- 没插线时控制器处于停止状态（`udc->stopped`，开机绑定后还挂起等待 OTG 检测），PHY 低功耗、时钟关闭。驱动自己的 `nuke()` 就注明"停止时时钟已关，不能清 FIFO"。
- Lab126 原来的 gadget 从不调用 connect/disconnect，所以原厂没碰到；此前 Android 侧用的 `g_ffs` 也不调用。Android 的组合 gadget 按通用约定调用，第一次走到这条路就卡死。
- 同样会在没插线时碰寄存器的还有：`android_disable` 里的 `usb_ep_dequeue(ep0)`（UsbDeviceManager 切换功能、adbd 重启），f_mtp、f_fs 读写被中断时的端点出队（拔线后 MTP 服务醒来），以及端点停用。

**现在的做法**（内核 1044，`arcotg_udc.c`）：
- 驱动已有的规则"控制器停止时不碰寄存器"贯彻到 gadget 能调用的所有操作：上拉、端点出队、端点停用、清 FIFO、读帧号、远程唤醒。停止时只改软件状态。
- 上拉拆成两个条件：gadget 是否要连接（`softconnect`，只由 connect/disconnect 改），本次插线会话是否允许上拉（`dp_allowed`，插线时由 `dp_pullup()` 置位，拔线时清除）。运行位 = 有驱动 && 要连接 && 有 VBUS && 会话允许，在控制器运行时写入。
  - 原厂驱动插线时会把 `softconnect` 强行置 1，gadget 还没准备好（如开机时 adbd 未就绪、切换功能的瞬间）时电脑也会枚举到一个没有配置的设备，可能要重插才认。
  - 现在等 gadget 准备好才上拉。
- 依据：Lab126 后续内核在同一控制器上的做法，3.10（KT3）的 `ci_udc_pullup` 没有 VBUS 时不碰硬件，4.1（PW4）的则先给控制器上电再写。

**顺带补上的观测手段**（内核 1043，`printk.c`）：
- 内核日志写在原厂会保存的那块固定内存里，但还留在 CPU 缓存里的部分，看门狗复位时会丢掉。这次日志的最后一段因此残缺。
- 现在每条日志写入后立即从 L1、L2 写回内存，开机清空旧日志后也写回一次。之后任何卡死，下一次在原厂系统里点入口时存下的 `previous-kernel.txt` 都有直到卡死前的日志（按入口日志的允许清单筛过，见「插着线，总线上什么都没有」）。

### 拔线再插回，电脑看不到设备（未能复现）

- **现象**：2026-10-16 晚出现过两次：已连接的 USB 拔线后再插回，电脑上看不到设备（PMIC 看到了线，正在充电）。
- **复现**：之后的开发构建上按 5 种条件复现，都正常：开机后首插；开机 3 分钟内拔插（`pw3_diag` 采样：拔线后控制器停止、低功耗，gadget 仍要求连接；插回 0.4 s 高速配置）；插着线熄屏亮屏后拔插；软件模拟拔插（只走控制器一侧）；拔线后系统真正挂起、唤醒后再插线（UIC 走了出错那次同样的 `notify_otg_out` 分支）。
- **原因**：尚未查明。可疑的是这条路径本身：此前的 `g_ffs` 下，adbd 每次重连都把整个 gadget 驱动注销、重新注册，控制器状态从头初始化；组合 gadget 一直挂着，只调连接、断开，原厂驱动在这条路径上没有经过验证。
- **现在的做法**：内核 1046 在插线、拔线、VBUS 变化、每次设定上拉时各记一行（`pw3-usb:`），再出现时一份 `previous-kernel.txt` 就能看出断在哪一步。上机验证第 4 项"拔插 50 次"覆盖醒着、睡着两种情况。

### 插着线，总线上什么都没有

- **现象**：2026-10-20，在一个公开发行前的构建上，Android 下插线，电脑这边 `ioreg` 里没有任何新设备（不是认不出 MTP，是根本没有枚举）。同一根线、同一个 hub，回到原厂系统立刻出现"Amazon Kindle"。离线读出设备上的 `data.img`：`persist.sys.usb.config` 是 `adb`，`/data/misc/adb/adb_keys` 的时间戳停在首次安装那次启动——**adb 只在首次安装那次能用，之后再没用过**。
- **原因**是顺序：adb 走 FunctionFS，描述符是 adbd 写的，gadget 要绑定这个功能必须先有描述符。AOSP 里凡是把 adb 走 FunctionFS 的设备都等 `sys.usb.ffs.ready=1` 再 `enable 1`（`init.usb.configfs.rc`、`device/linaro/hikey`）；本项目当时抄的是 AOSP 通用 `init.usb.rc` 里那套一步写完的处理——那是给内核自带 adb 功能用的，它不需要用户空间给任何东西。内核那边，1044 之后 `fsl_apply_pullup()` 只在控制器没停时才碰寄存器，描述符没就绪时 gadget 就一直不连接，表现正是"总线上什么都没有"。
- **为什么上一节的 5 种拔插条件没抓到**：那几次用的都是开发期间的测试入口（v0.1.0 之前已删除），它会把框架扣住等人放行，**UsbDeviceManager 从来没跑过**。它一跑就会重写 `sys.usb.config`（AOSP 注释 b/23631400：无条件重写），之后还要按插线和数据解锁的状态切换功能，于是 `enable 0` / `enable 1` 发生在 adbd 之后——这条路在设备上一次都没走过。
- **现在的做法**：带 adb 的配置分两步（`init.pw3.usb.rc`，见上文「用户空间」）；上机验证加了第 0 项，专门验开机后再插线。
- **日志也缺了一环**：1046 打的 `pw3-usb:` 行当时不在入口日志的允许清单里，唯一能直接回答"断在哪一步"的证据被筛掉了（那次的 `previous-kernel.txt` 只留了 3 行，全是 PMIC 的）。现在允许清单把本项目自己打的 `pw3-` 开头的行整体放进去（`boot/launcher/userlog.c`；只有转发 logcat 正文的 `pw3-boot-report` 排除在外）。

## 风险

- 组合 gadget 的连接与断开（`usb_gadget_connect/disconnect`）和 1036、1039 的插线处理都会碰 UDC。没插线时碰寄存器会卡死，1044 已经把 gadget 能调到的操作都挡在控制器停止状态之外；拔线再插回不枚举出现过两次、没能复现，原因未明，见「踩过的坑」。
- 3.0 的 `f_mtp` 传 4 GB 以上的文件没有实测。PW3 上碰不到：MTP 提供的是 `/data` 里的存储，`/data` 只有 1.75 GiB（`BoardConfig.mk`）；移植到存储更大的机型时要实测。
- ffs 功能在 adbd 打开端点前让整个 gadget 保持断开；`mtp,adb` 下 MTP 也要等 adbd 就绪，属正常行为。
