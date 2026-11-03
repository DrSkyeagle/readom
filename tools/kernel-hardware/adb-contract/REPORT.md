# PW3 g_ffs 与 AOSP 8.1 adbd 接口契约

**本版旧 FunctionFS 与 AOSP 8.1 的 V1 描述符回退在源码上相容；仅导出模块和挂载 ep0，仍不足以证明“可认证且绝不 TCP”的 USB ADB 已可用。** 已确认的接合点是 ARC 平台驱动必须先成功 probe、旧 composite 的失败重试限制、未知主机首次授权 UI 缺少框架接线，以及 libusb 对非空 serial 的要求。父任务另行实现 USB-only adbd 编译开关；本报告不修改该实现。

范围：只读 Amazon 3.0.35、当前 owned 配置/补丁和固定 AOSP `android-8.1.0_r81`。没有加载模块、访问 Kindle、读取密钥/私有资产或查询原机 ID。公开来源缓存在 `sources/`；`sourcehashes.json`、`aosp-sourcehashes.json` 保存路径、URL、SHA。四个既有本地 init/ueventd 文件也已与官方 tag 字节核对相同。现有 owned android-core 补丁截至 0800 未改 `drivers/usb/` 或 `include/linux/usb/`。

## 1. g_ffs 的真实参数和默认字符串

本版 `drivers/usb/gadget/g_ffs.c` 直接 include `composite.c` 与 `f_fs.c`，因此参数不是只看 g_ffs 本文件中的三条 class 参数。

| 参数 | 本版支持 | 语义 |
|---|---|---|
| `idVendor` / `idProduct` | 是，ushort | `composite.c:50–56,1146–1149`；非零覆盖设备描述符。缺省为 g_ffs 的 `0x1d6b:0x0105` |
| `bcdDevice` | 是，ushort | 非零覆盖设备版本 |
| `iManufacturer` / `iProduct` | 是，charp | UTF-8 字符串覆盖；不必传入设备身份 |
| `iSerialNumber` | 是，charp | 只有传入非 NULL 字符串才分配 serial string ID；不传时 g_ffs 默认 `iSerialNumber=0` |
| `bDeviceClass` / `bDeviceSubClass` / `bDeviceProtocol` | 是，byte | g_ffs:90–95；应保留 device class 的 per-interface 默认值0，ADB 的 0xff/0x42/0x01 是 interface class/subclass/protocol |
| `functions=adb` | **否** | 本版没有现代 g_ffs 的 functions 参数，也没有通过它选择 mount name 的逻辑 |

这些 composite override 参数的 sysfs permission 为0，属于加载时参数；不能假定以后能在 `/sys/module/g_ffs/parameters/` 修改。

缺省 product 是 `USB Function Filesystem`，configuration 是 `FunctionFS`。缺省 manufacturer 由 kernel sysname、kernel release 和 gadget name 组成（`composite.c:1154–1162`），**不读取原机 serial、MAC、board ID 或校准文件**。本版 g_ffs 没有 `needs_serial` 要求。AOSP adbd 的 FunctionFS 字符串表只有 `ADB Interface`；其连接 banner 使用 product name/model/device 和 feature 信息，所核对路径不主动填入设备 serial。[g_ffs 对应本地源码；AOSP USB 字符串](https://android.googlesource.com/platform/system/core/+/android-8.1.0_r81/adb/daemon/usb.cpp)、[连接 banner](https://android.googlesource.com/platform/system/core/+/android-8.1.0_r81/adb/adb.cpp)

## 2. 模块、挂载与描述符的必要顺序

g_ffs module init **只注册 FunctionFS 文件系统**（g_ffs:172–177），尚未向 UDC 注册 gadget。`functionfs_check_dev_callback()` 对 mount name 返回0，因此 `mount functionfs adb /dev/usb-ffs/adb ...` 的 `adb` 名称可用；这里的 name 不是额外 module 参数。`mount_single()` 表示该旧实现使用单个实例，不是现代多函数 FunctionFS 管理模型。

推荐的依赖顺序是：

1. OTG/transceiver runtime 对象准备好，再让 ARC UDC 平台驱动成功 probe；按父任务核验后的 modules.dep/运行依赖顺序加载。
2. 加载 `g_ffs.ko`，使 FunctionFS 文件系统可挂载。**module 存在或 insmod 返回，均不能替代 ARC 平台绑定成功检查。**
3. 挂载到 AOSP 固定路径 `/dev/usb-ffs/adb`，UID/GID 2000。旧 f_fs 默认根0500、endpoint文件0600；设置 `uid=2000,gid=2000` 后，默认降权为 shell 的 adbd 具备 DAC 访问条件。SELinux 仍须独立成立。
4. native preflight 确认真实 FunctionFS mount、ep0 和 ARC runtime 就绪，再启动 adbd。
5. adbd 打开 ep0，写 descriptors，再写 strings。内核 strings handler 先创建 ep1/ep2，再触发 **`functionfs_ready_callback → usb_composite_probe → usb_gadget_probe_driver`**；写 strings 返回成功后，adbd 打开 bulk endpoints 并建立 USB transport。

重要失败边界：`composite.c:1284–1299` 先赋全局 `composite=driver`，再调用 UDC probe。ARC 的 `arcotg_udc.c:2437–2447` 在 controller 尚不存在或已有 gadget 时直接返回 ENODEV/EBUSY，未进入 composite bind/unbind；g_ffs ready 失败只清 `gfs_registered`。后续 adbd 每秒重试不能可靠修复这个早期失败，因为下一次 composite probe 会发现全局 composite 非空而返回 EINVAL。须在首次 strings 写入前满足真实 UDC 条件，不把无限重试当成依赖排序。

现 owned `init.pw3.usb.rc` 已有 `pw3-mount-functionfs`、mount/check、`sys.pw3.ffs_ready` 和 boot opt-in 门禁，比“只启动 adbd”的旧描述完整；当前待接合的是 generated USB module rc 的加载/运行状态检查和事件触发。`CheckUsb()` 目前证明 mount 类型及 ep0 存在，**不证明 ARC 已成功绑定，也不证明 adbd 降权/SELinux 域能打开 ep0**。

配置至少明确：`USB_GADGET=m`、`USB_GADGET_ARC=y`、`USB_ARC=m`、`USB_FUNCTIONFS=m`、`USB_FUNCTIONFS_GENERIC=y`、`USB_FUNCTIONFS_ETH=n`、`USB_FUNCTIONFS_RNDIS=n`；ARC 选择的 dual-speed/OTG utilities 以及父任务确认的 OTG transceiver 运行依赖须保留。包内只提供所需 UDC/FunctionFS 闭包，不能混入可抢占 UDC 的 mass-storage、RNDIS 或其它 gadget。旧版产物名是 `g_ffs.ko`，不能套用现代 `libcomposite.ko/usb_f_fs.ko/configfs/functions=adb` 配方。

## 3. V2→V1 回退是实际代码路径

AOSP `adb/daemon/usb.cpp:255–310` 首次打开 ep0 后先写 V2；`adb_write` 返回负值时，构造 V1 的 16字节 header（magic1、完整长度、fs_count=3、hs_count=3），附 full/high-speed 的 ADB interface 和两条 bulk endpoint，再写同一个 ep0。[官方实现](https://android.googlesource.com/platform/system/core/+/android-8.1.0_r81/adb/daemon/usb.cpp)

旧 `f_fs.c:1788–1790` 只接受 `FUNCTIONFS_DESCRIPTORS_MAGIC=1`，故 V2 被 EINVAL 拒绝；`ffs_ep0_write:444–450` 在失败时不推进 READ_DESCRIPTORS 状态，V1 可以继续写。V1 后写 strings；旧内核支持对应 strings magic2。ADB endpoints 固定为 ep1 OUT、ep2 IN，协议 interface 为0xff/0x42/0x01，和 V1 描述符相符。[AOSP 路径/协议常量](https://android.googlesource.com/platform/system/core/+/android-8.1.0_r81/adb/adb.h)

旧内核没有 `FUNCTIONFS_ENDPOINT_ALLOC` 扩展，并不因此必然阻断此 tag：AOSP 在 ioctl 失败后缩小预分配请求，最终仍以16 KiB同步 read/write 运行（usb.cpp:326–351）。这里没有要求 FunctionFS AIO 支持。

`sys.usb.ffs.ready=1` 在 descriptors/strings 成功后设置，**早于 bulk endpoints 全部打开和 transport 注册**。它是 descriptor-ready 信号，不是 USB host 枚举、bulk可用、RSA认证或 shell已可用的验收结果。

## 4. 认证必须保持；首次授权仍有真实阻断

`adbd_auth.cpp` 将 `auth_required` 初始化为 true；userdebug/eng 编译允许 `ALLOW_ADBD_NO_AUTH`，所以运行时 **必须保留 `ro.adb.secure=1`**。`ro.secure=1` 使默认 adbd 降权为 shell，但 userdebug 的认证后 `adb root` 仍是单独允许路径；不能把默认降权描述为永久禁止 root。现 owned `initialization.mk` 已明确上述两个 ro 属性，USB service 保留 auth control socket。[auth](https://android.googlesource.com/platform/system/core/+/android-8.1.0_r81/adb/adbd_auth.cpp)、[daemon main](https://android.googlesource.com/platform/system/core/+/android-8.1.0_r81/adb/daemon/main.cpp)、[编译条件](https://android.googlesource.com/platform/system/core/+/android-8.1.0_r81/adb/Android.mk)

真实授权链：

- adbd 通过 `/adb_keys` 或 `/data/misc/adb/adb_keys` 验证已授权 host 的 RSA 签名；本任务未读取这些路径。
- 未知 host 的公钥需发往 init 创建的 `adbd` local socket，由 framework 确认；framework 没连接时只设置 retry，不自动授权。
- AOSP8.1 `UsbService.java:128–129` **只有 `/sys/class/android_usb` 存在时才创建 UsbDeviceManager**。
- `UsbDeviceManager.java:239–243` 创建 UsbDebuggingManager；启用 ADB 后，该管理器才启动线程连接 `adbd` reserved socket、显示确认 UI，并在用户允许后返回 OK。
- stock g_ffs 不创建 android_usb class。按本任务的纯 g_ffs 配置，不能假定现有 Android 首次配对 UI 会工作。[UsbService](https://android.googlesource.com/platform/frameworks/base/+/android-8.1.0_r81/services/usb/java/com/android/server/usb/UsbService.java)、[UsbDeviceManager](https://android.googlesource.com/platform/frameworks/base/+/android-8.1.0_r81/services/usb/java/com/android/server/usb/UsbDeviceManager.java)、[UsbDebuggingManager](https://android.googlesource.com/platform/frameworks/base/+/android-8.1.0_r81/services/usb/java/com/android/server/usb/UsbDebuggingManager.java)

因此：公共模块包无授权 key，且本任务未添加 framework 授权接线时，未知 host 仍会停在 unauthorized。可以保留已被用户授权的私有公钥路径或另做保留确认机制的接线；**不能关闭认证、嵌入公共通用 key，或用虚假 android_usb 目录冒充旧 USB 功能接口**。这些是后续明确工作，尚未在本报告完成。

stock adbd 还有三个 TCP 分支，父任务 USB-only 编译 guard 必须全部覆盖：

1. `daemon/main.cpp:189–198` 的 service/persist adb tcp port 属性。
2. `main.cpp:199–201` 缺少或不可访问 ep0 时的默认5555回退；`service.adb.tcp.port=-1` **不会**禁止它。
3. `drop_privileges():131–141` 在不降权分支建立 `tcp:<server_port>` smart-socket listener；这不是上述端口属性分支。

本报告只指出 source contract；独立 `0002-pw3-adbd-usb-only.patch` 与 BoardConfig 开关由父任务实现。默认 root preflight 成功不能替代这些失败路径的明确禁止。

## 5. Host VID 与 serial

AOSP8.1 的 `is_adb_interface()` 只匹配 interface class/subclass/protocol；已核对 Linux、libusb、macOS 后端，没有对 `0x1d6b` 施加 ADB VID 白名单拒绝。因此 **不需要仅为了绕一个不存在的该层白名单而冒用另一厂商 VID**。主机 OS 的 device-node 权限、驱动匹配和 Windows 驱动安装不在这项结论中。[匹配函数](https://android.googlesource.com/platform/system/core/+/android-8.1.0_r81/adb/transport_usb.cpp)、[Linux 后端](https://android.googlesource.com/platform/system/core/+/android-8.1.0_r81/adb/client/usb_linux.cpp)、[libusb 后端](https://android.googlesource.com/platform/system/core/+/android-8.1.0_r81/adb/client/usb_libusb.cpp)

这个 tag 的 legacy Linux 后端允许取不到 serial 时注册空字符串；但 libusb 后端 `usb_libusb.cpp:314–325` 在 serial 为空或读取失败时直接 return，不注册 transport。该 tag 在 Linux/macOS 通过 `ADB_LIBUSB=1` 选择 libusb。故“不填 serial”保证没有默认设备身份读取，却不能承诺所有 host 后端都可用。

若父任务决定提供兼容接口，建议明确为 **加载前的本地运行时随机 serial**：例如受信任 RAM launcher/native helper 生成16随机字节，编码为32个十六进制字符，仅作为 `iSerialNumber` 的加载参数。要求非空、严格字符/长度校验，不从原厂 serial、MAC、board ID、hostname 派生，不写入公开源码/公共归档；不自动复用跨设备固定串。此值不是认证密钥，且本报告没有生成任何实例。每次启动改变 serial 会改变 host 的 `adb -s` 标识；这是需说明的操作代价。

## 6. AOSP8.1 firmware 搜索与早期 restorecon

本地 `hardware-research/aosp-8.1/firmware_handler.cpp` 已与官方 tag 字节匹配。**固件搜索顺序确实是**：

1. `/etc/firmware/`
2. `/vendor/firmware/`
3. `/firmware/image/`

`ProcessFirmwareEvent():81–90` 直接把上述目录字符串与 `uevent.firmware` 拼接，选第一个可 open 且 fstat 成功的文件；不是找到同名但错误内容后自动尝试下一个。因此包内应使用已审阅的相对 firmware name，并避免更前位置的冲突。`.booting` 存在时每100ms按同一顺序重试；结束启动仍找不到时向 loading 写 -1。[官方 firmware handler](https://android.googlesource.com/platform/system/core/+/android-8.1.0_r81/init/firmware_handler.cpp)

restorecon 的精确边界：

- init 第二阶段 `selinux_restore_context()` 在 ueventd 填充 `/dev` 前修复列出的 `/dev`、socket/random/properties、policy/context 文件和 `/dev/block` 等；**该函数不递归 restorecon `/etc/firmware`、`/vendor/firmware` 或 `/firmware/image`**。[init.cpp:915–941](https://android.googlesource.com/platform/system/core/+/android-8.1.0_r81/init/init.cpp)
- ueventd coldboot `RegenerateUevents()` 即时调用 `HandleFirmwareEvent()`，其内部 fork 子进程；随后才 `ForkSubProcesses → DoRestoreCon(/sys递归) → WaitForSubProcesses`。不能把 coldboot 的全局 restorecon 当作每次 firmware open 之前的同步保证。
- hot-event 回调也先 `HandleFirmwareEvent()` fork，再 `DeviceHandler::HandleDeviceEvent()`；后者在 add/change/online 时修复 sysfs 权限并按状态 restorecon。因此 firmware 子进程与该 restorecon 可以并行。[ueventd.cpp:137–165,201–213,280–281](https://android.googlesource.com/platform/system/core/+/android-8.1.0_r81/init/ueventd.cpp)、[devices.cpp:191–207,375–379](https://android.googlesource.com/platform/system/core/+/android-8.1.0_r81/init/devices.cpp)
- firmware handler 自身没有 restorecon；它先打开 `/sys<DEVPATH>/loading` 和 `data`，再寻找固件。应单独验证 sysfs 标签、firmware 文件标签、DAC 和真实加载结果；目录存在或 export成功不能代替该验证。

## 验证边界

本轮完成了源码路径追踪、官方固定 tag 内容核对和哈希记录。没有执行 target module probe、FunctionFS mount、描述符写入、host枚举、RSA认证、初次配对 UI、TCP监听检查或 firmware加载。模块 ELF/exports/relocations 闭包与运行时 OTG platform binding 由父任务另行验证；上述层次不能互相替代。
