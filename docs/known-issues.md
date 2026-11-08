# 已知问题

这里记的是**已经知道、查过、决定不改**的问题，以及查过、确认无害的日志噪声，免得同一条被重新查一遍。第六到第九节是开发期间出现、已经修掉的缺陷；修法本身带着取舍和限制，所以也记在这里。英文版：`known-issues.en.md`。

每条给出：现象、查到的原因、为什么不改、什么情况下要改。

## 一、浏览器偶尔启动即崩

**现象**：点开 EinkBro，约 4.5 秒后应用消失，系统弹"EinkBro 已停止"。再点一次正常。

**原因**：EinkBro 在协程里构造 `TextToSpeech`，传进去的 `Context` 是一个底层尚未附加的 `ContextWrapper`，于是 `TtsEngines.getDefaultEngine()` 里 `mContext.getContentResolver()` 抛 NPE。这是应用自己的竞态，与本项目的框架无关——框架这条路径没有改动。

**什么时候会碰上**：只在机器很忙的时候。开发期间在一次首次开机时出现过一次，当时 dexopt 刚编译完微信读书、验证脚本正在压机器；同一构建随后连开三次都正常（冷启动 4.8 s / 4.5 s）。

**2026-11-02 专门去复现，复现不出来。** 在设备上起两个内建忙循环（`sh -c 'while :; do :; done'`，不周期起进程）把单核压住，一分钟负载从 1.68 推到 **9.19**（比第十四节那次让系统界面 ANR 的 9.11 还高），在这个负载下连续冷启动浏览器 **8 次，8 次全部正常**，`logcat -b crash` 全程 0 行。所以这一条的频率是「开发期一次、事后 8 次压力复现为 0」，不是「机器忙就会崩」。

**如果哪天它变成可复现，改法在我们这一侧，但不是只接住崩的那一行**：崩的那一行是 AOSP 自己的 `frameworks/base` 里 `TtsEngines.getDefaultEngine()` 的 `mContext.getContentResolver()`（`core/java/android/speech/tts/TtsEngines.java`）。只在那里接住不够：它接下来走 `getHighestRankedEngineName()` → `getEngines()`，同一个没附加的 `Context` 在 `mContext.getPackageManager()` 上照样抛；而且 `TtsEngines` 自己碰不到回调。要接就在 `TextToSpeech.initTts()` 里把查引擎那一整段接住，再 `dispatchOnInit(ERROR)`——`TextToSpeech` 本来就有这条错误通道（`OnInitListener.onInit(ERROR)`），"系统里没有引擎"走的就是它。把构造期的 NPE 变成 `ERROR` 上报，而不是让进程死掉，是这条通道本来的用途。**现在不做**：复现不出来就验不了，一个验不了的 try/catch 和没有一样（见 `checks-can-pass-while-switched-off` 那类教训），而且这台机器没有扬声器、TTS 本来无用，崩溃又只见过一次。

**为什么不改**：APK 是上游签名的发行件，不改它的代码（`device/amazon/pw3/apps/EinkBro/PW3-SOURCE.md`）。系统里带的是上游 16.7.1（2026-10-11 发布），它的发行说明里没有这条的修复。设备没有扬声器，TTS 在这台机器上本来就无用，但崩溃发生在查引擎之前，关掉任何设置都绕不过。

**什么情况下要改**：上游修了就跟版本；或者它从"偶发"变成"常发"（正常负载下也崩），那就要重新挑浏览器。

## 二、日志里的 avc denied

SELinux 是 enforcing。一次正常开机加六分钟压测会留下十来条 `avc: denied`，全部查过：

| 条目 | 是什么 | 有没有影响 |
|---|---|---|
| `untrusted_app` → `graphics_device:dir search`（约 6 条，`comm="Chrome_InProcGp"`） | WebView 的 GPU 进程探 `/dev/graphics` | 没有。AOSP 本来就不让普通应用看这个目录，WebView 探不到就走软件路径，而本机本来就是软件渲染 |
| `netd`/`wificond` → `module_request kmod="netdev-wlan0"` | 内核想按别名自动加载网卡模块 | 没有。模块由 init 显式加载（system/core 补丁 0003/0004），`wlan0` 已经在，Wi-Fi 正常 |
| `netd` → `module_request kmod="netdev-dummy0"` | netd 想建 dummy 接口 | 没有。本机不需要 dummy0，接口表只有 `lo`、`sit0`、`wlan0` |
| `netd` → `module_request kmod="ipt_TCPMSS"` | netd 的规则集里有一条 MSS 钳制规则 | 没有。那是给网络共享/VPN 用的，本机两者都没有；实测 72 条 iptables 规则里一条 TCPMSS 都没有 |
| `netd` → `SELinux: unrecognized netlink message type=20 for sclass=32` | netd 想按 UID 枚举或销毁套接字。20 是 `SOCK_DIAG_BY_FAMILY`，而本机内核是 3.0.35，`sock_diag` 这套东西是 Linux 3.3 才有的 | **有，但很小**。SELinux 的消息表不认得它，所以请求以 EINVAL 回来（日志里紧跟着就是 `Failed to dump IPv4 sockets for UID: Invalid argument` 和 `Error destroying sockets`）；就算放行，这个内核也没有实现它。后果是 **Wi‑Fi 断开时，应用那些已经建立的 TCP 连接不会被强制关掉**，要等它们自己超时——表现为断网后应用多"卡"一会儿才报错，而不是立刻。每次网络状态变化出现一次，不成循环。**为什么不修**：补的不是 SELinux 那一行表项，而是整套 `sock_diag`（`net/core/sock_diag.c`、`inet_diag` 重做、`udp_diag`，跨 net/core 与 net/ipv4、net/ipv6 几百行），为一个没人量过的收益去动网络栈，代价和风险都不成比例。**这个内核其实有一条做同样事的路**：Lab126 在 `net/ipv4/tcp_ipv4.c` 里加了 `/proc/tcpreset`（`proc_tcpreset_write`，`CONFIG_LAB126` 下）：写 `42` 或 `43` 加一串要跳过的端口号，就按模式重置这台机器的 IPv4 TCP 连接——正是 netd 想通过 `SOCK_DESTROY` 做的事。所以修这一项**不等于**回移整套 `sock_diag`；但也不是改一行：netd 的 `SockDiag::destroySockets` 先用 `SOCK_DIAG_BY_FAMILY` **枚举**再销毁，而 `/proc/tcpreset` 只有「全部重置、按端口跳过」这一种粒度，按 UID 销毁它做不到，而且要在 netd 这个核心守护进程里塞一条只有这台机器有的路，那是垫片。**要不要动，先量**：断网之后应用到底多久才报错。量出来是几秒就不值得动，是几十秒再谈。 |
| 开机第一行：`proc_dir_entry '/proc/tcpreset' already registered`，带一页寄存器和调用栈 | **原厂内核自己的毛病，和本项目无关。**Lab126 把 `/proc/tcpreset` 建在 `tcp_proc_register()` 里，而那个函数每个地址族调一次——`tcp4_proc_init()` 和 `tcp6_proc_init()`——第二次就撞上已经存在的名字，`proc_register()` 回它一个 WARN 加完整调用栈。栈里逐帧写得很清楚：`tcp6_proc_init` → `tcp_proc_register` → `create_proc_entry` → `proc_register` → `warn_slowpath_fmt` | **没有**：第二次注册失败，`/proc/tcpreset` 仍然只有一个、仍然能用（它是 IPv4 的功能，IPv6 本来也用不上）。但它值得修：内核环只有 32 KB，这一页在第 0.47 秒就吃掉约 1.5 KB，而那正是 panic 现场最需要的一段；更要紧的是入口的日志过滤器**故意**保留 `WARNING`、`Backtrace:` 和寄存器（`boot/launcher/userlog.c`），所以这二十多行会进到每一份交给使用者的 `android/logs/<时间>/` 里、而且在最前面，读起来像是本项目自己的调用栈。本项目的内核修了它（`patches/android-core/1056-tcpreset-registered-once.patch`，一个 `if`），发行版内核不再打印这一段；只有打这个补丁之前的内核日志里还会看到 |
| `init` → `module_request kmod="configfs"` | 看上去像 AOSP 通用 init.rc 在挂 configfs，其实是**本项目自己的 `init.pw3-main.rc` 曾经在挂**：那一行以及它下面两条 `/config/sdcardfs`，加上另一处 `mount pstore` 和它下面六条。内核里这两个文件系统都没有，所以每次开机白失败十条 | 没有。本机 USB 走传统 gadget 路径，configfs 没挂载，adb 和 MTP 都正常。**那十行已经删掉**（`init.pw3-main.rc` 里留着说明），发行版的日志里不再有这条拒绝 |

表里的拒绝，除 `sock_diag` 那条外，都是"内核先问一声、被拒、各方继续"的形态，不是功能失败；**要改的唯一理由**是哪天真的需要其中某个功能（比如网络共享），那时该补的是内核配置，不是 SELinux 策略。`sock_diag` 那条不一样：它确实让一件事做不成，而该补的也不是策略，是内核里本来就没有的那套接口。

**但不要因此把所有 `avc: denied` 都当成无害。** 上表里 `scontext` 全是 AOSP 自带的域。开发期间（2026-10-21）有一版装上之后起不来，就是因为一条 `scontext=u:r:pw3_init_check:s0` 的拒绝——本项目自己的域缺了一条规则，启动检查于是报了一条和真因毫无关系的话。判断方法很直接：

- `scontext` 是 `u:r:pw3_*` → **不正常**，本项目的程序被自己的策略挡住了，照着 `tcontext` 和 `tclass` 补规则（`tools/run-tests.py` 的 `sepolicy-device-paths` 会静态查 `/dev` 路径这一类）。
- `scontext` 是别的域且在上表里 → 已查过，继续。
- `scontext` 是别的域但不在上表里 → 还没查过，查了再说。

交到 Kindle 盘上的那份日志会保留 `pw3_*` 域的拒绝行（`boot/launcher/userlog.c`），正是为了这种时候有东西可看。

## 三、`adb root` 之后 `adb devices` 会变空：设备跳到了另一个 adb 服务

这一节只和开发用的 `userdebug` 构建有关：发行版是 `user` 构建（`ro.debuggable=0`），没有 `su`。

**现象**（2026-10-21 实测）。执行 `adb root` 之后：

- 设备**仍然在 USB 上**（`ioreg` 里还是 "Kindle Paperwhite 3 development port"，序列号对得上）；
- Android **仍然在跑**（Kindle 盘没有挂出来，说明没有回原厂）；
- 但 `adb devices` 从此为空，等了 80 秒没有恢复。当时的记录说拔插数据线之后又出现了，但那次没有核对它是被哪个 adb 服务接走的，所以这一点不能当结论。

**原因**：开发用的电脑上有**两个 adb 服务**，设备在它们之间换了一个。`adb root` 让 adbd 退出重启，原来持有设备的那个服务丢掉它，另一个服务把它接走。

**量到的**（2026-10-29，`userdebug` 构建）：做一次框架重启（`su 0 stop`，三秒后 `su 0 start`），每 12 秒同时看四样东西，看了 100 秒——

- `ioreg` 里那条 "Kindle Paperwhite 3 development port" **全程都在，匹配数一直是 3，一次都没断**；
- 设备**从 5097 这个 adb 服务跳到了 5038**：触发前 `5097=1`、`5038=0`，十二秒后变成 `5038=1`、`5097=0`，此后不变。

所以**框架重启根本不碰 USB**，`adb devices` 为空只是被问的那个服务不再持有设备。`adb root` 那次没有当场查别的端口，但现象一致，按奥卡姆剃刀是同一件事。

**排除过的两种解释。** 一种是"带 adb 的配置分两步使能，`adb root` 之后新的 adbd 再把 `sys.usb.ffs.ready` 置成 1，值没变所以 init 不触发，网关再也不走 `enable 0`／`enable 1`"。源码里核过，这是错的：`PropertySetImpl()` 在每次成功更新之后无条件调用 `property_changed()`，不和旧值比较；`property_changed()` 直接入队；`Action::CheckPropertyTriggers()` 拿触发器的值和**新值**比，从不看旧值（`system/core/init/property_service.cpp`、`init.cpp`、`action.cpp`）。把 1 写到 1 上照样触发。另一种是"网关真的下去了"，依据是 `system_profiler SPUSBDataType | grep -c` 匹配 0 次。这个依据是坏的：那条命令在当时的受限 shell 里**一行都不输出**（被沙箱挡住），「匹配 0 次」并不说明设备不在总线上。查 USB 总线要用 `ioreg -p IOUSB -l -w 0`。

**怎么办**：`adb devices` 空了，**先查别的端口**（5038、5097、5037），不要先怀疑硬件；要确认设备在不在总线上，用 `ioreg -p IOUSB -l -w 0 | grep -i "Kindle Paperwhite 3"`，**不要用 `system_profiler`**。需要配对实测这类要重启框架的测量时，每一轮之后重新解析端口就行，不需要拔插数据线。

**也不是"睡着了"**（2026-10-29 在开发构建上实测）。"设备休眠所以从总线上消失"是最顺手的猜测，而它是错的：线插在电脑上，熄屏 100 秒设备全程在总线上（每 5 秒查一次，`ioreg` 稳定 3 条匹配、`adb` 稳定 1 台），内核日志在那个时间窗里**一条 `PM: suspend` 都没有**——插着电脑时这台机器熄屏不挂起。所以总线上什么都没有的时候，该看的是数据线，不是屏幕。

**要 root 权限时**：不要用 `adb root`。`userdebug` 构建里有 `su`，用 `su 0 <命令>` 提权就行，它不重启 adbd。`tools/measure-apk-install.sh` 和 `tools/verify-device.sh` 都是这样做的，后者在默认端口上找不到设备时会自己去查 5097 和 5038，并把该用的命令打出来。

`init.pw3.usb.rc` 里那行 `onrestart setprop sys.usb.ffs.ready 0` 留着，但**它不是这一条的修复**。留它的理由是：adbd 没了，它写的那些描述符也就没了，属性不该继续声称它们在。0 这一跳没有任何触发器，所以不会在可能没插线时去碰 USB 控制器的寄存器——没插线时碰这些寄存器会让设备卡死。

## 四、每点一次 Android，Kindle 盘上多一个文件夹（只有不带安装器的入口才这样）

**现象**：用电脑打开 Kindle 盘，根目录下有一串 `pw3-first-kernel-<时间戳>` 文件夹，每点一次就多一个。

**谁会碰到**：**2026-10-29 之后没有人**——`build_entry.py` 的 `--installer` 那五个参数已经改成必填，造不出这种入口了。留下这一条是因为盘上可能还有以前留下的文件夹。当时会碰到的是自己用 `boot/build_entry.py` 构建入口、而且没带 `--installer` 的人；现在仍能构建出这种启动器的只有 `build_launcher.py`，而只有 `boot/handoff/tests/run_client_test.py` 那一个测试这么用，它在 QEMU 里跑、碰不到任何真盘。**发行版的 `Readom.sh` 不这样**：它写进 `android/logs/<时间>/`，和安装器同一个文件夹，自动只留最近 5 次。

**为什么不改**：自动清理只在安装器自己的文件夹里做，而且只删入口自己写的那种形状——名字是入口的时间戳格式、里面只有普通文件——不按前缀匹配，也不递归（`boot/launcher/userlog.c`）。不带安装器的入口写在盘的根目录，在使用者的盘根目录按前缀删东西不是本项目做的事（`boot/launcher/helper.c` 里 `PW3_OUTPUT_PREFIX` 上方的说明）。

## 五、后台复核之前那一两分钟

**现象**：跳过完整校验的那次开机，系统镜像要等到开机后约两分钟才被重新读一遍。

**原因与取舍**：见 `design/release-build.md`。开机时查的是 FAT 表和目录树（约 85 ms），能发现"别的文件占着镜像的簇"；内容本身的完整校验放到开机之后的后台做，为的是省掉每次开机那约 19 秒（576 MiB 时实测 20.9 秒，镜像改成 512 MiB 后按同一速率外推，未重测）。

**为什么不改**：另一条路是每次开机都付那约 19 秒，或者移植 dm-verity（3.0.35 要回移驱动，而且本机可用的 511 MB 内存下每次读都要重算，摊下来更贵）。**对损坏来说，两分钟后必然发现**；对"有人在 Kindle 侧执行代码"来说，他同样能换掉启动器本身，这一两分钟不改变攻击面。

## 六、改了默认桌面布局的升级，只补上缺的条目

**开发期间的缺陷**：升级到改了默认桌面的版本，桌面回到出厂布局——使用者挪过的图标、加过的页面、自己装的应用的图标都没了。应用和数据不受影响，清掉的只有"哪个图标在哪一格"。

**原因**：默认布局只在数据库第一次建立时套用一次（`loadDefaultFavoritesIfNecessary`），所以改了布局的版本对已经装过的机器根本不生效。当时用的办法是把 `SCHEMA_VERSION` 往前推一格——AOSP 判断"这个数据库还能不能往下用"靠它，走不通就 `createEmptyDB()` 清表重套。于是每改一次布局就把整张桌面表删一次，而那张表是"使用者排过什么"的唯一记录。后来又为这个代价补了三处（布局里按名字写微信读书、清表之后把非自带应用排队放回、`downgrade_schema.json` 跟着推），一个机制要三处找补，就是机制本身错了：表结构和桌面内容是两件事，被当成了一个数字。

**现在的做法**（`aosp-launcher3/0015`）：拆成两个数字。`SCHEMA_VERSION` 只管表结构（29 不动）；布局自带版本号 `pw3_workspace_layout_version`（`res/values/config.xml`），改布局就推它。装过的机器升级时只投递**差集**：

- 已经在桌面上的条目跳过——不管使用者把它挪到了哪一格，还是放进了文件夹。认同一个条目看它打开哪个 activity，没有 activity 的（全部应用那一格）看 action，不看存下来的 intent：布局解析器和安装各写一套 flag，同一个应用两种写法；
- 不在桌面上的放进去：布局指定的格子空着就用它，被占了就用同屏第一个空格子，整屏满了就不放（日志里说一句）。使用者排好的东西不会为镜像的条目让位；
- 不删不挪任何已有的行。表里的行归使用者，布局是"桌面至少有什么"的下限，不是一面镜子。第一屏第 0 行是搜索框，它不是这张表里的行，所以差集和 `LoaderCursor` 一样把那一行标成占用，否则放到框后面的条目会在下次加载时被丢掉而不是显示出来。

**还剩的代价**：把一个条目从一格挪到另一格、或者从布局里去掉，只对第一次开机的机器生效；反过来，使用者自己删掉的那四个自带图标（全部应用那一格删不掉），会在改布局的那一版被补回来一次。要连这个也避掉，就得再记一份"使用者删过什么"的名单——为这点收益多一套状态，不值得。**不由差集投递的**还有文件夹、小部件、hotseat 格子和 `<resolve>`：这四种没法和桌面上已有的东西比对，重复放一次比不放更糟，所以跳过并记日志。本机布局里没有这四种，`tools/tests/test_home_screen_entries.py` 守着这一点，也把布局版本号和布局文件的哈希钉在一起——改了布局忘了推版本号，它会把该推的数字说出来。

**这条路在设备上走过一次**（V84，2026-10-25）：布局版本 1→2 的那次升级，在当时唯一一台已有桌面的设备上，差集放进了**第二个**「全部应用」。原因是改名把这一格的 action 从 `org.kdandroid.action.ALL_APPS` 换成了 `org.readom.action.ALL_APPS`，而这一格没有 activity、按 action 认，桌面上那一行认不出新键。机制按设计工作了，错在键改了名：存在使用者设备上的键不能改，改键就等于删掉一个条目，而差集表达不了删除。之后 `tools/tests/test_home_screen_entries.py` 加了 `LAYOUT_KEYS`，把布局里的五个键单独钉住，改了键会被点名（提交 `2c22d1a`）。

自己装的应用图标落在哪一格是另一件事，见 `design/app-install.md`。

## 七、系统镜像坏了而大小没变：再点一次会自己查出来并重装

**开发期间的缺陷**：点 Android 之后出现启动画面，等一两分钟又回到原厂系统，没有任何错误码；再点一次一样。

**原因**：安装器按状态文件里的哈希判断镜像是不是现成的，不会每次重读 512 MiB（那是每次点击多付约 19 秒，按 576 MiB 的 20.9 秒实测外推）。镜像内容被改坏而大小不变时，状态文件仍然说"装好了"，于是直接交接；Android 侧的 preinit 读完整镜像、哈希不符、拒绝启动并重启。两边各自都对，合起来就是一个没有出路的循环——唯一的办法是接电脑把 `android/system.img` 删掉再点一次，而"只会插数据线的人"做不到。

**现在的做法**：按 `design/diagnostics.md` 定下的那条——让原厂侧**读到**"Android 拒绝了这个镜像"，而不是去猜。

- preinit 的每一次 `stop()` 都把原因写进报告区（`PreInit.c` 的 `report_stop`）。报告区的块设备在镜像校验**之前**就映射好了，所以这条路走得通；映射之前的那些 `stop()` 没地方写，就不写。
- 下一次点击，安装器读报告区（`install.c` 的 `read_report`）。原因是"系统镜像哈希不符"时，它把状态文件里的 `system=ready` 抹掉、重装镜像，并先显示一屏说明（`PW3_SCREEN_REPAIR`）。数据镜像不动。
- 已经处理过的报告不会再处理一次：状态文件记 `android=<启动序号>`，序号**不一样**的报告才是新的。不是"更大"——报告区读不出来的时候（写到一半断电，校验和就不符），下一次写入从 1 重新开始，按"更大"判断的话这台机器就再也不会被听见，死循环原样回来。
- 反过来，第八节那条"编译做完了"判的恰恰是"更大"。两种错代价不对等：拒绝少判一次是永久死路，完成早清一次是让人在 dexopt 中途按电源键。所以两个方向分开问。
- 报告区自己的首行坏掉也不会让通道死掉：把魔数补回去之后首行仍然解析得出来、正文校验和仍然对得上，就是本项目写的那份坏掉了，清零重来。没有这个判断，一个字节的翻转就能让安装器（因为大小对）和 preinit（因为"不是我们的"）各自放手，报告再也写不出来。
- 别的拒绝原因（FAT 交叉、挂载失败等）重装解决不了，安装器只把原因记进日志——但使用者至少有了一句话可看、可转交，而不是一个没有出路的循环。
- **"读不出来"和"摘要不对"是两件事，只有后者重装。** 瞬时读错误会让这次开机退回原厂，而镜像其实是好的；把它当成损坏，就会去要 196 MB（而本地载荷装完就删了，离线就是 E202），把一个下次点击重读一遍就好的情形变成死路。所以 preinit 分开报：读完了还不符是 `SHA256 rejected`，读不过去是 `could not be read through`，安装器只认前者。

启动画面上的说明因此不再叫人连电脑删 `system.img`，而是写"回到了书库就再点一次，会自己查出来并重装，不需要电脑"。

## 八、升级后的"还在编译"提示：编译做完才消失，不按点击次数

**开发期间的缺陷**：换了启动镜像的那种升级，装完要重新编译 `/data` 里的应用。提示这件事的画面曾经只在装完后的前三次点击显示；如果这三次都在编译完成前被打断，第四次起显示的是普通的"约三分钟，十分钟没出来就按住电源键"——而设备其实还在编译，这句话反而是在诱导使用者再打断它一次。

**现在的做法**：和第七节读同一份报告。编译发生在 `SystemServer.startOtherServices()` 里的 `updatePackagesIfNeeded()`，而 `ActivityManagerService.systemReady()` 在同一个函数里晚 700 多行，所以 `sys.boot_completed=1` 必然晚于编译做完。安装器读的就是这一行。

**"有报告"本身不算证据。** 采样程序**故意**不等 `boot_completed` 才起——它在存储就绪时就起，因为"起不来的那次开机才是最该有报告的"（`pw3-boot-report.rc`），采样两分钟就写。所以编译超过两分钟的那次升级，报告是在编译当中写出来的；把"有一份新报告"当成"编译做完了"，就会在编译中途把提示清掉，正是旧行为错的那个方向。安装器因此只认报告里最后一次采样到的 `sys.boot_completed=1`。

代价是提示会多出现一次：那次升级开机的报告说没完成，所以它之后的那一次点击仍然提示，而那时编译其实已经做完了。多一次是看一眼就过去，少一次是让人在 dexopt 中途按住电源键——方向是对的。

状态文件里记的是 `compiling=1`，以及置位那一刻报告区里的启动计数 `compiling_after=`。装上换了启动镜像的系统时置位；读到一份同时满足三条的报告时清掉：走到了 `boot_completed`、由这次装上的系统写出（报告里记着构建号）、计数大于 `compiling_after`。**不按点击次数递减，也没有上限。** 后两条防的是旧报告冒充：同一个系统可能在这次安装之前就走完过一次启动，而报告的副本可以从电脑上放回去（`installer/install/install.c` 里 `compiling_after` 的说明）。

**编译本身要多久**：换了启动镜像之后，`/data` 里的应用按 `pm.dexopt.ab-ota=speed-profile` 重编（frameworks/base 补丁 0047）。有**参考画像**的应用按画像重编，微信读书实测 **159 秒**；没有的只把 dex 快速化（quicken），**60–93 秒**（frameworks/base 补丁 0051、frameworks/native 补丁 0008）。没有后一条退路时，没有画像就退回全量编译，要 **1214 秒**。

**2026-11-04 又量到两种**：启动镜像不变、只换了构建指纹的升级（f→g→h），开机对微信读书跑 `dexoptNeeded=-3`（原 oat 可用、只因过滤器重编），**约 70 秒**，两次都是；手动 `cmd package compile -m speed -f` 全量编译微信读书 **21 分 41 秒**。所以文档里的几组数各有条件：按画像 159 s、无画像快速化 60–93 s、同镜像换指纹 70 s、全量 speed 1214–1410 s（`design/app-install.md`）或 1301 s（11-04）；安装画面上写的"每个应用大约三到五分钟"是给有画像那种情况留的上限，不是实测均值。

**而且这笔不是只付一次**：后台编译任务把一个应用编到 `speed` 之前，每一次换版本的开机都会把它再按 `quicken` 编一遍，编出来还是 `quicken`——升级开机拿 `speed-profile` 判"要不要编"，没有参考画像又退回 `quicken`，判的那一级和落下的那一级对不上。2026-11-04 在 k、l 两版的开机上，微信读书各被这样编了一次，**70 秒和 74.6 秒**。改法记在 `open-items.md`。

注意"用过"不等于"有参考画像"：应用运行时写的是**当前**画像，参考画像只有按画像编的那几种级别才会写出（`PackageDexOptimizer.isProfileUpdated()`）。本机的后台编译任务编的是 `speed`（`pw3.mk` 的 `pm.dexopt.bg-dexopt=speed`，见第十二节），不写参考画像，所以使用者自己装的应用在这种升级之后一般走 quicken 那条，之后设备空闲时再由后台任务编回 `speed`。

## 九、UsageStatsService 曾经自激成满核空转：判定器把调度间隙当成了改时

**现象**：2026-10-20 的实验里反复清应用数据、强制停止、启动超时之后，`system_server` 连续三十多分钟占满单核，把正在跑的 dex2oat 挤到 0.36%，连 `pm clear` 都排不进 binder 队列。后来自己平息。

**线索**：SIGQUIT 的线程栈（`/data/anr/trace_NN`）指向 `UsageStatsService.onStatsReloaded → postOneTimeCheckIdleStates`。日志里 `Time changed in UsageStats by +2/+3/+4/-3 seconds` 和 `Flushing usage stats to disk` 一直在刷。

**已经排除**：不是时钟走速——屏幕亮着时连量两次 90 秒，墙上与单调时钟只差 ±0.07%。（先量到的 1.5% 是在 dex2oat 占满 CPU 时量的，是调度延迟造成的假象。）

### 回路是什么

把源码跟完之后，**它不是一个代码环**：

- `checkAndGetTimeLocked()` 一共只有五个调用点——`reportEvent()`（每次应用使用事件）和 `queryUsageStats` / `queryConfigurationStats` / `queryEvents` 三个查询接口，加上备份还原。
- 判定成立 → `onTimeChanged()` → `persistActiveStats()` + `loadActiveStats()` → `mListener.onStatsReloaded()` → `postOneTimeCheckIdleStates()` → `checkIdleStates()`。
- 而 **`checkIdleStates()` 根本不读墙上时钟**，它只用 `elapsedRealtime()`；它发出的 `MSG_INFORM_LISTENERS` 在这个进程里只有两个订户（`AppIdleController`、`NetworkPolicyManagerService`），两个都只问 `isAppIdle` / `isAppIdleParoleOn`，都不碰 `checkAndGetTimeLocked`。

所以回路是**经过 CPU 争用的正反馈**：

> 误判 → 整份使用统计落盘再重载（昂贵）→ 单核被饿 → 两次时钟读之间的调度间隙变大 → 误判

**这条回路的全部增益都在判定器里。** frameworks/base 补丁 0050 让判定器不再把调度间隙当成改时，之后无论 CPU 被饿到什么程度都不会产生误判，回路增益为零、点不着。这不是"点火源没了但回路还在"，而是**没有增益的回路不是回路**。

### 那些偏差里有没有真实的改时

原代码**先读墙上时钟、后读单调时钟**，所以间隙 g 产生约 −g 的偏差；它随后用这对偏了的值重置快照，于是**下一次的偏差是 `g_前 − g_本次`**——两次调度间隙之差。这推出三件可核对的事：

1. 第一次报出的是 −（首个间隙）；
2. 之后每一次的量级被**间隙的波动**夹住，而不是被任何与时钟有关的东西夹住；
3. 连报时倾向**反号**，因为每报一次就重置一次。

按 dex2oat 占满单核时的间隙量级（2.2–6.0 秒）照原样跑一遍：300 次调用报出 76 次，量级 **±2/±3/±4 秒**、符号 `-+-+-+-+…`。**日志里记的就是 `+2/+3/+4/-3`——形状、量级、符号全部对上。**

而真实被拨动过的时钟是另一个形状：补丁后拨 +10 / −10 / +60 / −3600 / +86400 秒，每次**恰好报一次**，报出的偏差等于跳变量（保守地扣掉快照宽度），之后安静。持续刷几秒的流不是改时能产生的。

**所以结论是：解释这条日志流不需要任何真实改时。** 形状、量级、符号都由间隙单独复现得出来，而达到阈值的真实改时会表现成单独一次、量级等于跳变量、随后安静——日志里没有那样的东西。

这句话有意比"全部来自调度间隙"弱，因为后者是证据支持不了的。原代码报出的量是 `上次间隙 − 本次间隙 + 这期间的真实改时`，三项叠在一起：一次 +1 秒的真实改时配上 4 秒的本次延迟同样报 −3，下一次零延迟的采样再报 +4。所以**反号的流不排除其中夹着一次阈值以下的真实改时**。排除掉的是"有一次值得报的改时"，而那恰好是这条问题关心的全部——补丁和结论都不因此改变。

### 守卫

`tools/tests/test_usage_time_check.py`（`run-tests.py` 里的 `usage-time-check`）把上面全部跑成可重复的检查：

- 四档间隙（0–50 / 2200–6000 / 0–30000 / 0–600000 毫秒）各两千次，**补丁后 0 次误报**；
- 十种真实改时（含 ±10 秒到一整天）各**恰好报一次**，报出的量级不超过真实跳变；
- **检测发生在宽采样里的最坏情形**单独一组（宽 6/20/120 秒的间隙上出现真实跳变，紧接着间隙为零）。补丁最初的写法正是错在这里：只给本次采样算区间、把快照当精确值，于是在一次正确判定之后制造一次误判。现在两边都带宽度；
- 五种阈值以下的改动（±1.9 秒以内）**一次都不报**，所以 1 秒分辨率的 RTC 在唤醒时和单调时钟之间的小漂移不会惊动它；
- 原代码在同样的间隙上**复现出日志里那个签名**，这是上面那个结论的依据本身。

模型只在和 Java 同一套算术时才有意义，所以测试最后把补丁里的表达式逐条钉住，并且核对**墙上时钟那次读必须落在两次单调读之间**。**七种反转逐一确认会失败**，其中一种就是"不带快照宽度"。

### 剩下的

没有了。真实改时仍然会触发一次重载，那是对的，而且在这台机器上不会连发——NTP 的轮询间隔是一天量级，`SET_TIME` 要权限。

**同一句日志还有第二个来源，是我们自己的缺陷，2026-10-31 修掉了。**本节讲的是判定器把调度间隙读成了改时；另一条是内核真的报错了数：`get_monotonic_boottime()` 把三个 `long` 纳秒字段在 32 位里连加而溢出，所以每次真正的休眠／唤醒之后，一部分 `CLOCK_BOOTTIME` 读数少 2³² 纳秒（4.295 秒），而 `elapsedRealtime` 走的就是它。补丁 `patches/android-core/0103` 把第一个操作数转成 `s64`，和上游 3.4.y 的回移同一个改法，设备上跨三次真实挂起量过。**所以看到 `Time changed in UsageStats` 时先分清是哪一种**：本节这种偏差是秒级且与负载相关，那一种是固定的 4.295 秒且只在睡醒之后出现。

## 十、`PW3_ANDROID_START` 这一行能说明什么，不能说明什么

**在哪看**：`android/logs/<时间>/launcher.log` 的末尾，紧跟 `PW3_ANDROID_REPORT`。

**它读的是什么**：报告区（`android/report.bin`）里的启动计数。那个数**只在 Android 跑到 `pw3-boot-report` 并且那次写入成功时才增加**。启动器把这次读到的数和此前任何一次点击记下的最大数比一比，打印四种结果之一：

| 这一行 | 确定的事 | **不**确定的事 |
|---|---|---|
| `reported previous_boot=N this_boot=M` | 内核起来了、init 跑了、存储就绪、`pw3-boot-report` 执行了、报告区可写 | **框架有没有起来**。服务是在 `sys.pw3.storage_ready=1` 就启动的，不是 `sys.boot_completed`——这是故意的，没走完的那次启动才是值得要报告的那次 |
| `no_new_report previous_boot=N this_boot=N` | 两次读到同一份报告 | **是不是什么都没跑**。也可能跑进了 Android 却没来得及写（写入被拒、掉电、提前重启）。这两种情形这个数分不开 |
| `no_previous_report this_boot=M` | 这是第一次读到报告的那次点击 | — |
| `unknown this_boot=M errno=N` | `android/logs/` 没能读完，所以没有做任何比较 | 和"里面什么都没有"不是一回事 |

**为什么写成这样**：这一行曾经印 `started` / `not_started`，而那正好是上表右栏的两件事——它把这个数办不到的两个判断都断言了。所以现在只说观察到的事实。

**`no_new_report` 之后往哪看**：同一个文件夹里的 `previous-kernel.txt`。报告写入失败的原因由 `pw3-report-area:` 这个标签打进内核日志（`BootReport.cpp` 的 `KernelLine`），内容只有本项目自己的固定文字、路径和错误码，所以启动器会把它留在交给使用者的日志里。但内核环形缓冲只有 32 KB，**开机那一段一分钟后就被绕掉**，所以这条线索要尽早取。

## 十一、换过签名密钥的那一次升级：首次开机多花约 20 秒，并重置系统设置

**什么时候会遇到**：装上来的系统和机器上原来那个系统**用不同的密钥签名**时。公开发行版从 v0.1.0 起都用本项目同一把发行密钥（`ro.build.tags=dev-keys`），公开版本之间的升级不会遇到。会遇到的是密钥不同的两种构建互相覆盖：自己构建的系统用的不是本项目的发行密钥（没设 `PW3_RELEASE_KEY_DIR` 时是 AOSP 公开的 `test-keys`，设了就是自己的密钥，见 `device/amazon/pw3/pw3.mk`），所以把自己构建的系统装到装过发行版的机器上、或者反过来，都会换一次密钥。设备上量过的是从 `test-keys` 换到发行密钥这个方向。

**密钥不变的升级（例如两个公开版本之间）不会触发它**，这一条在 AOSP 的代码里是写明的，不是推断。`PackageManagerService.scanPackageDirtyLI` 对"系统包签名变了"有两条出路，分水岭是这个包有没有共用身份（`sharedUserId`）：

```java
// The signature has changed, but this package is in the system image... let's recover!
pkgSetting.signatures.mSignatures = pkg.mSignatures;
// However...  if this package is part of a shared user, but it
// doesn't match the signature of the shared user, let's fail.
if (signatureCheckPs.sharedUser != null) {
    if (compareSignatures(...) != PackageManager.SIGNATURE_MATCH) {
        throw new PackageManagerException(INSTALL_PARSE_FAILED_INCONSISTENT_CERTIFICATES,
                "Signature mismatch for shared user: " + pkgSetting.sharedUser);
```

设备上那次开机的日志正好分成互不相交的两份名单，和这段代码一一对上：**没有共用身份的 18 个包**（EinkBro、Launcher3、PackageInstaller、文档界面、输入法、WebView、下载器自己……）写的是"签名变了、保留数据"；**带共用身份的 13 个包**（`android` 自己、settings、systemui、shell、keychain、inputdevices，以及 settings／telephony／media／downloads／userdictionary 几个 provider）扫描直接失败，于是被后面那一轮判成"系统包不存在了"。

而进这条分支的前提是 `compareSignatures(...) != SIGNATURE_MATCH`——**只有换密钥才会不匹配**。指纹变化（每次升级都变）根本走不到这里：签名校验通过，既不抛异常，也不会有包被判定不存在。所以这 20 秒只在换密钥的那一次付。

**会看到什么**：那一次开机比平常多约 20 秒；系统设置回到默认（亮度、输入法选择、下载记录、用户词典这些），**使用者自己装的应用和书不受影响**。

**里面发生了什么**（2026-10-22 在设备上取到的证据：一台由 `test-keys` 构建生成过数据的设备，升到发行密钥签名的构建）：

Android 的包管理服务开机时发现系统镜像的签名变了，于是对**共用 `android.uid.system` 这个身份的那一批系统包**——平台包 `android` 本身，加上 settings、systemui、keychain、inputdevices、几个 provider——判定"扫不到"，把它们的数据清掉。平台包被当成不存在之后，紧接着那一步要查"卸载器是哪个"，它拿平台包的解析活动去比名字，而那个名字此刻是空的，于是 system_server 抛空指针退出：

```
System package android no longer exists; it's data will be wiped
java.lang.NullPointerException: ... String.equals ...
  at com.android.server.pm.PackageManagerService.getRequiredUninstallerLPr(PackageManagerService.java:3499)
```

init 随后重启 zygote，**第二次**起来的 system_server 从被重建的记录出发，一切正常——所以除了多等一会和设置回默认，不留后遗症。代价有精确的数：事件日志里第一次 `boot_progress_start` 在 35.8 秒，第二次在 55.4 秒，**白走 19.5 秒**。

**为什么不去"修"它**：这是 AOSP 自己的行为（共用身份的签名必须一致，否则整组视为不可用），触发条件是密钥变化。能做的是不让公开用户碰上：发行密钥不更换，公开发行版从第一个版本起就用它（`design/release-build.md`）。上游那个空指针本身也值得一提——它把"平台包不见了"这种情形报成了空指针而不是一句能看懂的话；但改它要动启动类路径上的 `framework.jar`，那会让 45 个 ART 启动文件全部变化，升级后 `/data` 里的应用都要重编（单是微信读书就要 60–93 秒），为一句报错的措辞让每台机器升级后多等一两分钟，不值得。


## 十二、微信读书打开一本书要几十秒

**现象**（使用者 2026-10-28 报告）：点一本书要等约 20 秒才有反应；进了书里还在下载，而网络并不慢；速度很不稳定。

**量法**。每一轮都先确认停在书架（`dumpsys activity` 的 `mResumedActivity` 是
`WeReadFragmentActivity`），再点同一本书，然后按进程的 `utime`/`stime` 判定
「完全安静」（连续两个 2 秒区间用掉不到 0.15 秒 CPU）。要注意：**微信读书冷启动会直接恢复到
上次读的书，而不是书架**；不先确认停在书架，点击就会落在阅读页里，那样的轮次不能用。

**量到的**（开发期间，`speed-profile` 机器码，应用已在运行，连点三轮）：

| 轮次 | 阅读器出现 | 完全安静 | 其中 CPU | CPU 占墙时间 | 该进程主缺页 |
|---|---|---|---|---|---|
| 1 | 11.4 s | 77.1 s | 52.5 s | 68% | 4288 |
| 2 | 4.5 s | 62.6 s | 45.8 s | 73% | 2845 |
| 3 | 5.2 s | 43.6 s | 28.4 s | 65% | 610 |

表里的 CPU 是把 `/proc/<pid>/task/*/stat` 里的线程累加得到的，会漏掉运行期间已经退出的线程，
把应用的 CPU 少算约三成。按 `/proc/<pid>/stat` 和 `/proc/stat` 在同一窗口对账，**整核
94%–96% 是忙的，微信读书自己占 69%–78%，空转 4%–6%，iowait 为 0**（`design/performance.md`
第十节）。

**是 CPU。** 热点线程是 `TypeSetting #1`（排版）、`ent.weread.eink`（主线程）、`WRNetworkPool`、
`WRCommonPool` 和 `HeapTaskDaemon`（GC）；JIT 编译线程单次开书只用 0.0–0.6 秒。合成
（`surfaceflinger` 3%）和分词进程（2%）也都排除了。频率全程 **996 MHz 占 100%**，`ondemand`
没有把它压到 396 MHz（翻页时量的是 996 MHz 占 67%，其余是两次翻页之间的空闲）。
内核里没有编 `interactive` 调速器，但这个负载是持续的、不是短促的，编了也用不上。

**不是网速。** 链路：RSSI −51 dBm、协商 72 Mbps、2.4 GHz、丢包 0%，到 `weread.qq.com`
往返 8.4–88.9 毫秒（平均 46.6）。单独量过一次开书，整个过程 uid 10022 只收发了
**4 KB / 9 个包**；另一次是一段约 270 KB、两秒的突发（127 KB/s）。按实测往返，9 次发送
最多解释 0.4 秒。等待不在传输上。

**也不是内存。** 开机以来的累计量（换出 924 MB、主缺页 60 万次）看上去很大，但这台机器当时
刚被 `dex2oat` 连续编译了二十多分钟，绝大部分计在那上面，不能拿来判断开书。按单次操作量：

- 开一本书，该进程主缺页 610–4288 次（约 2.4–17 MB），系统 `pswpout` 在窗口内**为 0**；
- 空闲 15 秒里 `pswpin` 只动 1 页；
- `am kill-all` 之后 `MemFree` 一点没变（32 MB → 32 MB），因为**根本没有后台应用可杀**；
  而 239 MB 是页缓存，本来就可回收。这台机在开书那一刻并不缺内存。

所以换页最多解释几秒，不是几十秒的来源。

**其中一半是解释执行。** 同一台设备、同一个账号、同一个书架，把微信读书编到 `speed`（全量
机器码），开一本书从 **95.6 秒降到 51.4 秒**，进程 CPU 从 73.0 降到 30.2 秒，四轮对两轮
不重叠；垃圾回收几乎没变，省掉的 43 秒全在应用自己的 CPU 里——**那就是解释执行**。在登录前、
书架空的冷启动上比较 `quicken`、`speed`、`speed-profile`，三者差不出统计意义（约 37 秒，记在
`pw3.mk`），那是另一种工作量：要跑的代码少，多半已在使用画像里；加载书架、打开一本书要跑的
EPub 解析、CSS 解析和逐字排版，画像里没有。详见 `design/performance.md` 第十二节。

**现在的做法**：安装时用 `quicken`，设备空闲时由后台编译任务编到 `speed`（`pw3.mk` 的
`pm.dexopt.bg-dexopt=speed`）。frameworks/base 补丁 0066 让这个任务等"设备空闲、电量不低"，
而不是 AOSP 原来的"设备空闲、插着充电器"；frameworks/native 补丁 0009 让编译先写到旁边的
`.staging` 文件、成功后再原子换上，编译期间旧的机器码一直可用。所以新装的微信读书要等设备
第一次空闲够久（屏幕灭约 71 分钟）、再编约二十分钟，开书才降到 50 秒上下；在那之前大部分
代码靠解释执行。

**为什么剩下的不改**：微信读书是上游签名的发行件，改它就破坏了这台机器信任它的理由（和 EinkBro
那个崩溃同理，见第一节）。系统这边能做的已经做了：频率已满、机器码编到 `speed`、系统常驻已
精简且开书时并不缺内存。编到 `speed` 之后剩下的约 30 秒 CPU，是应用自己在 1 GHz 单核
Cortex-A9 上做的解析、排版和同步。系统侧唯一还有数的杠杆是垃圾回收的余量（上限约 6%–7%），
配对实测分不出差别，所以保持现状（`design/performance.md` 第十节）。

**绕法**：阅读页在 5 秒左右就出来，后面那几十秒是在后台把书排完，**期间可以先读**，只是翻页
会迟钝。而且**越读越快**：在 `speed-profile` 下同一本书连开三次，完全安定的时间从 77 秒降到
44 秒、CPU 从 52 秒降到 28 秒。

**第一天的数（2026-11-04，发行版 h，使用者报"加载新书等了 1 分多钟"之后量的）**。这次量的是
**没下载过的书**：从书架点开、下章节、解析、排版到完全安静。先在刚装上的 `quicken` 状态量两本，
再用 `cmd package compile -m speed -f com.tencent.weread.eink`（后台任务做的就是这件事，实测
**21 分 41 秒**）编到 `speed`、重启进程后量两本。四本书不同，所以只能当量级看，不是配对比较：

| 状态 | 书 | 下载 | 阅读器出现 | 完全安静 | 进程 CPU |
|---|---|---|---|---|---|
| `quicken` | 《必然》 | 0.9 MB | 5.4 s | ≤ 46 s（采样器丢了时间线） | 27.8 s |
| `quicken` | 《别让慢病找上你》（25 章） | 7.3 MB | 2.7 s | **57.4 s** | **35.2 s** |
| `speed` | 《宝贵的人生建议》 | 0.4 MB | 9.6 s | 41.4 s | 25.9 s |
| `speed` | 《读写拥有》（26 章） | 0.7 MB | 2.4 s | **23.8 s** | **15.9 s** |

量法和上面一样（进程 `utime+stime`，连续两个 2 秒区间少于 0.15 秒算安静），采样器是一个只用
内建命令的 shell（`read` 读 `/proc/<pid>/stat`，定时用 `read -t` 读被宿主撑着的 stdin——mksh 的
coprocess 第一次超时就没了，`mkfifo` 在 shell 域建不了），全程不起进程。下载量按
`/proc/net/xt_qtaguid/stats` 里 uid 的 `rx_bytes` 在采样前后各读一次算。使用者遇到的"一分多钟"
对应的是第二行：刚装上、解释执行、又是一本带图的大书。**Wi-Fi 和内存这次也不是瓶颈**：
−48 dBm / 72 Mbps，空闲内存 225 MB；那一分钟里有 114 条 `Deoptimizing … JIT inline cache`、
每 5–6 秒一次 0.6–2.9 秒的 GC，单核 5 分钟负载 4.2——全是解释执行和 JIT 的样子。

所以"第一天慢"是设计内的：后台任务要等设备熄屏空闲约 71 分钟才编，之后就是下面两行的水平。
（11-04 实测了一次：微信读书编回 `quicken`、屏幕 09:07 灭、插着电，任务 10:23 自己启动，编了 21.5 分钟，10:44 变成 `speed`。插着电不妨碍。）
量完没有编回 `quicken`，设备留在 `speed`；**要量"真实第一夜"的耗电，先 `cmd package compile -m quicken -f`
编回去再拔线**，否则那一夜少了这 21 分钟的编译。

**屏幕上看到的**（2026-10-28 的记录）：开书过程中用 `screencap` 截屏，会截到二十多秒整屏纯黑；
当时写的是「那只在帧缓冲里，面板上看不到」，因为看着屏幕的人（那时微信读书还没登录）看到的是
先一段白屏、正文出来时黑闪一下。那一下是 `Panel.h` 的 `replaced()`，设计内的一次 GC16。**2026-11-02 更正**：
登录之后微信读书画第一帧要 12–30 秒，撞上应用过渡的 5 秒超时，那段黑**是真的送上了面板**
（`design/performance.md` 第十一节）。现在的镜像（补丁 0067–0073）让上一屏留到应用画出来为止，
启动时系统先画微信读书自己的启动页（白底黑徽标），不再出现那段黑；熄屏打断过渡的情况见第十六节。

## 十三、系统向不存在的服务要东西，每次都在 dropbox 里留一条 wtf

**现象**（开发期间，2026-10-27 量到）：设备上一天的 dropbox 里有六条 `system_server_wtf`。

- 开机一条：`UsageStatsService.onBootPhase()` 要 `appwidget`。这套系统没有它——桌面不放
  小部件——于是 `SystemServiceRegistry.onServiceNotFound()` 记一条，1718 字节，在
  `SystemServer.startOtherServices()` 的路径上。
- 设置里五条：`DevelopmentSettings.showEnableOemUnlockPreference()` 要 `oem_lock`。这台
  机器没有持久数据块、也不做 bootloader 解锁，于是同样记一条——一次来自搜索索引线程
  （`DatabaseIndexingManager`），其余来自打开开发者选项。

**两处都不是故障。** 调用方都判了 null，所以界面是对的（「OEM 解锁」那一项根本不显示），
没有崩溃、没有 ANR，使用者看不见任何东西。

**为什么不改。** 改法只有一条是对的：让 `UsageStatsService` 和 `DevelopmentSettings` 容得下
一个不存在的服务。那是 `services.jar` 和 Settings 的 Java 改动，换来的是每次开机少构造一个
异常、少写 1718 字节。**这个代价没有量过**，而它在 1 GHz 单核上是几十毫秒的量级。

**什么情况下要改**：真要动的那天先量——如果开机路径上那一次异常构造被证明值几百毫秒，
就该把它当成要修的故障；或者 dropbox 的写入在长期使用中累积到占盘。现在两者都没有证据。

## 十四、浏览器开着就占掉单核的六分之一，重页面加载时能把系统界面饿到 ANR

**量到的**（2026-11-02 在 v0.1.0 上）：EinkBro 停在一个页面上什么都不做，**稳定占单核的
17%–18%**，而且和页面无关——纯文本页（`example.com`）18%，本项目官网安定之后 16%。
所以这不是某个页面的开销，是浏览器开着本身的开销。

**2026-11-02 晚补了三件，前两件把"常驻"这个说法改掉了**（做法是前后各取一次
`/proc/<pid>/task/*/stat` 再相减，设备侧几乎没有开销）：

| 情形 | 进程占单核 | 整机 idle |
|---|---|---|
| 前台、停在 `about:blank` | **22.9%** | 59.4% |
| 前台、停在一个真实页面 | 20.0% | 58.4% |
| **退到后台（回桌面，进程还活着）** | **0.0%**（21.5 秒里 1 个 jiffy） | **92.5%** |

1. **空白页和真实页一样贵**：`about:blank` 上 22.9%，比真实页还高一点。所以确实与页面无关。
2. **它不是常驻开销，只在前台可见时付。** 退到后台整个进程是 0.0%，整机 92.5% idle——
   Chromium 在渲染器不可见时会把定时任务节流掉。所以本节原来写的"浏览器开着就占掉"
   要理解成"**看着网页的时候**占掉"；真正常驻的是零。
3. **这些 CPU 没有变成任何一帧**：同一段 20 秒里面板侧 `cpu_frames` 只 +1、
   `presents` 只 +1（那一次是状态栏时钟的 16,384 像素）。**零出帧**，所以不是软件
   合成或刷新循环在烧，是 Chromium 自己的周期性工作。按线程看：
   `Chrome_InProcRe`（WebView 渲染主线程）**10.0%**，三个 `ThreadPoolForeg` 合 5.4%，
   EinkBro 自己的 UI 线程 3.2%，`Compositor` 1.9%，`Chrome_IOThread` 1.4%。
   **具体是哪一项还没查出来**——要在 `userdebug` 上对这个进程做一次 profile
   （正式版镜像上它不是 debuggable，`simpleperf` 要 root）。在那之前不去猜着改
   WebView 的开关。

**加载重页面那一段要贵得多**：打开本项目官网（整屏插画加一段入场动效）的那一分钟里，系统
记下的归因是 `89% info.plateaukao.einkbro: 88% user + 1.2% kernel`，一分钟负载冲到 9.11，
`com.android.systemui` 因此在一条 `TIME_TICK` 广播上 ANR，屏幕上弹出「系统界面没有响应」。
点「等待」就恢复，没有东西崩溃，`logcat -b crash` 全程 0 行。

**为什么不改**：这是上游应用加一个 1 GHz 单核的事实，不是这套系统的缺陷——系统该做的
（SwiftShader 软件合成、频率已满、`speed` 机器码）都做了，ANR 对话框本身是 Android 在
告诉使用者"有人占着主线程"，它工作正常。EinkBro 是上游签名的发行件，改它就破坏了这台机器
信任它的理由（同第一节）。

**什么情况下要改**：如果要让官网在这台机器上打开得更轻，那是**官网那一侧**的事（入场动效、
整屏插画），不是系统这一侧；判据是同一台设备上打开官网那一分钟的 CPU 归因。
~~另外，量耗电之前要把浏览器关掉——17% 的常驻足以让"空闲耗电"量的是浏览器而不是系统。~~
**这一句 2026-11-02 作废**：退到后台之后它是 0.0%，不会污染空闲耗电。量耗电时把它关掉
仍然是好习惯（省得它恰好在前台），但理由不是"常驻 17%"。

## 十五、Wi-Fi「打不开」有两种，先分清是哪一种

**一、屏幕灭着的时候发出「打开」**（adb 能走到，手指走不到）。屏幕灭后一分钟
`WifiController` 把 `mDeviceIdle` 置真、进 `NoLockHeldState`，`WifiStateMachine` 被设成
`DISABLED_MODE`——这是本项目要的策略（`frameworks/opt/net/wifi` 补丁 0002：Wi-Fi 随屏幕睡，
和原厂一致）。此时来一条「打开」，`ApStaDisabledState` 走 `mDeviceIdle == true` 那一支，
**不发 `CONNECT_MODE`**，又回到 `NoLockHeldState`：父状态把 `wpa_supplicant` 拉起来、
子状态同时是 `DISABLED_MODE`，`SupplicantStartedState.enter()` 立刻把整条链拆掉。于是开关看着
拨过去了、模块也插上了，网络一直不来。`mDeviceIdle` 只由 `CMD_SCREEN_ON` 清零，所以**唤醒屏幕
就好，不用重启**。手指碰不到这一种：要点开设置就得先亮屏。

**二、屏幕亮着却打不开**。使用者 2026-11-02 报的是这一种，**2026-11-04 找到了根因，和 SDIO 无关**：
补丁 0002 把"灭屏一分钟"的闹钟改成了非唤醒闹钟，而 AlarmManager 对非唤醒闹钟在灭屏期间最多压
15 分钟、并在设备变为交互（亮屏）那一刻投递。于是 `CMD_DEVICE_IDLE` 和 `CMD_SCREEN_ON` 相隔 3 ms
到达：`WifiController` 先进 `NoLockHeldState`（发 `DISABLED_MODE`）再回 `DeviceActiveState`（发
`CONNECT_MODE`），`WifiStateMachine` 在 `SupplicantStoppingState` 收到 `CONNECT_MODE` 只记了个模式，
最后停在 `InitialState`，没人再把 supplicant 拉起来；而控制器仍在 `StaEnabledState`，所以设置里每次
"打开"都被当作"已经开着"忽略——开关拨上去、页面重开又是关。11-02 那次的 `wifi module clean unload
failed` 正是唤醒瞬间的那条 `DISABLED_MODE` 在卸模块。现场恢复：`svc wifi disable` 再 `svc wifi enable`。
修在 `frameworks/opt/net/wifi` 补丁 0004（v0.1.0 之后的镜像）：空闲计时改成状态机自己的延时消息
（只数"系统醒着、屏幕灭着"的时间），亮屏即取消；亮着屏到达的空闲消息一律当过期丢掉。k 版实测：
灭屏 63 s 后关、亮屏 25 s 内连上。

**两种共用一个放大器**：Wi-Fi 一关就卸内核模块，是本项目的策略。要根治第二种，方向是关 Wi-Fi
时不卸模块、只给卡下电，代价是常驻功耗，要用一次耗电实测来定——没量之前不动。

**怎么量**（2026-11-02 定的，之前按别的办法量错过）：

- **先亮屏**：`input keyevent 224`（`26` 是开关，会把亮着的屏关掉），否则量到的是第一种假故障。
- **先读状态，再读日志**：`dumpsys wifi` 里的 `mScreenOff`、`mDeviceIdle`、`curState`。不要从内核
  日志的 insmod/rmmod 往上猜。
- **等待写成"轮询 + 上限"，不要写固定 sleep**：固定等 10 秒曾让 10 次健康的开关全报"失败"，而同一批
  日志里 `wpa_supplicant` 是在请求后 0.2 秒就关联上的。改成轮询之后量到的是：**空闲时 5–6 秒连上，
  有负载时超过 10 秒，5 次全部成功**。
- **接口要看 `ip addr show wlan0` 的原文**：`ip link show wlan0` 在 `user` 版上报
  `Permission denied`，那不是"接口不存在"。

## 十六、过渡还在等的时候熄屏，唤醒后是两条栏夹黑，直到应用画出来

**什么时候会碰上**：点开一个应用，它还没画出第一帧（屏幕上是上一屏被撑着，或是它自己的启动页），
这时按电源键熄屏，再唤醒。唤醒后屏幕中间是黑的，只有状态栏和导航栏，直到那个应用画出来为止。

**量到的**（2026-11-04，f 版的 userdebug 孪生版，X5b/X5c/X5e；记录在接管文档 3a 节）：把微信读书
用 `kill -STOP` 冻住让它画不出来，开书、熄屏、唤醒——熄屏那一刻书架图层被提交成隐藏
（`dumpsys SurfaceFlinger` 里 flags `0x3`、可见区为空），唤醒后保持记录还在，但没有人把它显示回来；
面板计数器显示唤醒时确实全刷了一次。**把保持旧屏整个关掉（X5e，只剩上游行为）也一样黑**，
所以这不是 0069–0073 带来的，是"过渡等待中熄屏"本身：休眠路径把关闭方的可见性直接提交成隐藏，
上游 5 秒到期也会黑，这一版不比上游差。

**为什么不改**：要改的是休眠路径里对关闭方可见性的处理，不是保持逻辑；而能撞上它的窗口很小——
发行默认开着启动页，装着的四个应用都有启动页，过渡在一秒内就结束。真遇到了，等那个应用画出来
就正常，或者按 Home 回桌面（桌面正常）。按 X5c 的观察，冻着的应用解冻画出后放手也正常。

## 十七、卸载微信读书之后，首页那一格不回来（v0.1.0；下一版放回去）

**现象**（使用者 2026-11-04 报）：在"设置 > 应用"里卸载了微信读书，首页上原来"安装微信读书"那一格
不再出现；要重新装，只能在"全部应用"里找到安装器。

**发生了什么**：安装器装完微信读书那一刻，把首页上自己那一格**改写**成微信读书的图标（同一行记录，
位置不变），再停用自己的桌面入口，所以装好之后不会出现两格。卸载微信读书时，启动器照例删掉所有指向
它的记录——那一格就空了。安装器**有**在听"微信读书被卸载"这件事（`RemovalReceiver`，开机时
`BootReceiver` 也查一遍）：收到后把自己的入口重新启用，所以它回到了"全部应用"；但 v0.1.0 里没有
再往首页加回一条记录，于是格子不回来。安装器本身一直在（`/system/priv-app/WeReadInstaller`），在
"全部应用"里叫"安装微信读书"，点它就重新下载安装，装完直接进微信读书。

**下一版镜像里改了**（2026-11-04，`HomeScreenCell.takeBack`）：交格子的时候记下它在哪一屏哪一格；
微信读书被卸载、入口重新启用时，把入口加回原来那一格（被占了就放首屏第一个空格）。只在"从停用
回到启用"这一步做，所以使用者自己把这一格从首页拖掉的，不会每次开机被放回来。

## 十八、换版本后的第一次开机，屏幕黑约一分半（v0.1.0；下一版不黑）

**现象**（使用者 2026-11-04 升级到 g 版时报"系统卡在黑屏好久"）：装完换了版本的系统，启动画面出现
之后屏幕变黑，黑到桌面出现为止；升级开机实测 g 版 85 秒、h 版 80 秒（这一段在重新编译微信读书，
见第十一节）；平常开机也有十几秒。

**原因**：交接程序确实把 `BOOT` 画面画上了（内核日志 `pw3-handoff: SCREEN up`），但 SurfaceFlinger
一起来就开始合成，第一扇窗口出现之前它的每一帧都只有自己那块**清成黑色**的目标缓冲，本机的合成器
照样把它贴到面板上，画面就被黑覆盖了（合成器的计数 `empty_frames=6` 就是这几帧）。

**下一版镜像里改了**（2026-11-04，`composer.cpp`）：没有任何图层的帧不再贴到面板，面板保持它已有的
画面——墨水屏不推新帧就不变，所以启动画面一直留到第一扇窗口画出来。那几帧照旧计数，只是不送。

**现在能做的**：不用做什么，黑屏那一分多钟就是在编译，等桌面出来即可；长按电源键只会打断它。

## 十九、Voyage 在模拟器上的限制，以及哪些还不是结论

- **QEMU 上原厂 suspend 需要先卸载 fsr_keypad**：模拟器没有实现 fsr_keypad 的 kl05 I2C，它的 suspend 回调会等一个 mutex，直到内核 hung task 检测把调用栈写进日志；`tools/emulator/prepare-voyage-suspend.sh` 先停 `lab126_gui`/`x`/`lab126` 再卸载 `fsr_keypad`（连同 `drv26xx_haptics`）后，真实 suspend 才能完成。真机不需要这一步。
- **Cortex-A9 TLB lockdown 的 QEMU 模型与真机不同**：QEMU fork 会把锁定位期间的任意 TLB 填表记进 lockdown 项，transition 之后的新内核在 `bus_freq` 探测里读回探测函数自己的代码页而死循环；发行补丁序列的 1060 用 `readom.emulator=1` token 门控跳过探测，token 只出现在测试入口 cmdline，真机/发行不受影响。
- **imx_epdc 的固定扫描地址看不到 Android 合成结果**：QEMU 机器给 EPDC 写死 `fb-addr=0x81000000`，而 Android 合成到 update buffer；QEMU 侧新增默认关闭的 `update-source` 属性（`tools/emulator/start-voyage.sh --epdc-update-source` 会加上长格式 `-global driver=imx.epdc,property=update-source,value=on`）才能读到桌面。打开 `--gui` + Xvfb 后已验证 Android 桌面完整可读、触摸可操作；默认关闭时与改动前逐像素一致。
- **触摸注入需要显示 console**：模拟器用 `-display none` 启动时无法注入触摸；用 `-display gtk`（Xvfb :9 + openbox）后靠 QMP `input-send-event` 驱动 cyttsp4，已验 tap 设置/返回，但 `xdotool click` 对 GTK 窗口无效。坐标换算与面板位置写在 `tools/emulator/README.md`。
- **bootstrap system 镜像不是 AOSP 构建产物**：它由官方 v0.1.0 的 PW3 镜像换入 Voyage 模块、改写身份属性得到，只用于模拟器验证；正式发行必须用 `PW3_AOSP_PRODUCT=voyage` 的 AOSP 全量构建替换。
- **installer 端到端在模拟器上已通过，真机未跑**：release-v2 完成安装（产物哈希与预期一致、report area 创建、payload 删除），严格模块在模拟器硬件门正确地拒绝跳转并清理，安装产物用 v3 链路启动到 `boot_completed=1`；真机安装、HTTPS 下载路径和真机看门狗/FSP 时序仍未验证。
- **TCG 大写入会触发 imx2 watchdog 复位（installer 安装时）**：内核 timer 每 15 s 喂狗，但 FSP/vfat 大量写会让 timer softirq 长时间得不到调度。模拟器专用规避：`devmem 0x020bc000 16 0xFF3D` 把 WCR timeout 从 60 s 调到 128 s，并在 guest 内每 4 s 用户态喂 WSR；`/mnt/us` 余量 2.7 GiB 下写 2.3 GiB 不再复位。真机 eMMC 写入路径不同，不适用这个规避。
