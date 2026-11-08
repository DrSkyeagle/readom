# 发行构建：调研与方案

本文写怎么构建一次发行、发行前什么必须为真。发行之前照「发行清单」逐条走；其余各节是这张清单背后的理由和证据。调研从 2026-10-15 开始。

## 要改什么

| 项 | 测试版 | 发行版 | 为什么 |
|---|---|---|---|
| 构建类型 | `userdebug` | `user` | 去掉 `su`、`gdbserver` 等调试组件，`ro.debuggable=0` |
| 签名 | AOSP 公开的 `test-keys` | 项目自己的发行密钥 | 测试密钥人人都有，任何人都能签出带系统权限的应用 |
| adb | 调试版默认 `mtp,adb`（构建自动加 adb），必须授权 | 默认只开 MTP，在开发者选项里打开 adb | 安全；文件传输用 MTP（`usb-mtp.md`） |
| 诊断开关 | 无 | 无 | 开发期间的测试入口（诊断开关、扣住框架等人放行、测试辅助脚本）已在 v0.1.0 之前整套删除，`build_entry.py` 只构建发行入口 |

## 已核实的事实

- **软件绘制不依赖调试版**（2026-10-15 读补丁核实）：
  - 所有应用窗口都用软件 Canvas，靠 `frameworks/base` 0006 和属性 `ro.pw3.software_canvas`，没有调试版条件，`user` 构建同样生效。
  - 原先还有一个只在调试版生效的按包开关（`debug.pw3.canvas_pkg`），作用已被 0006 覆盖。这个属性 2026-10-22 起不再写进 `pw3.mk`：代码有 `Build.IS_DEBUGGABLE` 条件，在 `user` 构建上本来就不生效，而留着就等于让系统镜像的 `build.prop` 里写着一个应用的包名——镜像没有理由知道使用者会装什么。提供它的补丁在 v0.1.0 之前删掉了，同时删掉的还有两个与 0006 重复的切软件绘制补丁。删它们要改 `framework.jar`，45 个 ART 启动文件因此全变，已装的应用在升级后第一次开机时要重编一次（frameworks/base 0026、0047、0051：有使用画像的按画像编，没有的只做 quicken，微信读书单个应用约 1–3 分钟）；公开发行之前删，没有公开用户要付这个代价。
- **换签名密钥会清掉共用身份的系统包数据**（`PackageManagerService.java` 第 11120–11140 行）：
  - 系统应用的签名变了，8.1 会接受新签名、保留数据；
  - 但应用若属于共享 UID（如 `android.uid.system`，框架本身就在其中），新签名与共享 UID 记录的旧签名不符时，直接抛出 "Signature mismatch for shared user"，这个包加载失败，数据被清。
  - 2026-10-22 在设备上实测：用 `test-keys` 生成的数据直接升到发行密钥，带共用身份的 13 个包数据被清，平台包 `android` 也在其中，system_server 第一次启动因此退出一次，第二次正常。代价是那一次开机多约 20 秒、系统设置回到默认；使用者装的应用和书不受影响（`known-issues.md` 第十一节）。

## 方案

1. **发行版从第一个版本起就用发行密钥**，公开用户不会遇到换密钥的问题。
2. **开发期间用 `test-keys` 生成过数据的设备**直接升级，不做迁移，安装器也不为此区分数据格式（`data_format` 仍是 1：它不因密钥而变，而 2026-11-02 一度要随数据镜像改小升到 2，使用者 11-03 否了那次改小，所以格式也没动）。曾考虑过两种做法：
   - 用新的空数据镜像，应用和登录重新来过；
   - 加一个范围很窄的迁移：只有旧签名是 AOSP 公开的测试平台密钥、新签名是本项目发行密钥时，才允许共享 UID 换签名，并让安装器把两种数据区分为不同的 `data_format`。
   - 都没有采用：上面的实测说明直接升级只损失一次系统设置和约 20 秒，而且只发生在换密钥的那一次；前者要使用者重装应用、重新登录，后者要改 `PackageManagerService` 并上机验证。
3. **`user` 构建**：`lunch pw3-user`（`build-aosp-system.sh` 按 `PW3_BUILD_VARIANT` 选）；检查 SELinux 中只给调试版的规则（如 `su` 域）是否被本项目的策略引用；`adbd` 服务的 `--root_seclabel` 在 `user` 版里不起作用，可以保留。
4. **adb 默认关闭**：`user` 构建不会给 `persist.sys.usb.config=mtp` 加 adb；开发者选项里的"USB 调试"照常可用，电脑第一次连接时要在 Kindle 上授权（`usb-mtp.md`）。

## 第二条路：bootstrap 镜像（验证用，不是发行版）

除了 AOSP 全量构建，还有一条**只用于验证**的路线：从官方 v0.1.0 的发布物出发，换入 Voyage 内核的模块、
改写 `build.prop` 的身份属性，得到 Voyage 身份的 system 镜像（工具 `tools/build-voyage-release.py`）。
它让内核、交接、preinit 和 Android 启动可以在真机固件上先跑通，而不必等 AOSP 构建；但它**不是发行版**：
用户空间仍是发布版 PW3 的字节。

| 项目 | AOSP 全量构建 | bootstrap 重打包 |
|---|---|---|
| 输入 | 本仓库 + AOSP 树 + 内核产物 | 官方 v0.1.0 的 system.img/data 模板 + Voyage 内核模块 |
| 身份来源 | `build.prop` 由 AOSP 构建生成（`voyage.mk`） | 只改写 7 类身份属性，其余字节仍是 PW3 |
| 用途 | 正式发行 | 模拟器验证；不能作为发行镜像 |

发行物要逐字节可复现，还有一条工程约束：3.0.35 内核把源码的**绝对路径**写进镜像（`__FILE__`，约 583 条），
所以只有在规范路径 `/root/readom-kernel` 下构建，zImage/uImage/modules 才与历史发行物逐字节一致；
换个路径（如 `/home/builder/readom-kernel`）哈希就会变，并且会波及任何嵌入内核或模块的产物。
复现步骤（bind mount 到该路径）见 `readom-port` 的 `kernel-fix-report` 第 4.3 节。

## 随发行附源码

`tools/collect-copyleft-sources.py`，在构建虚拟机里、导出镜像（`export-aosp-images.py`）之后运行，生成每个 Release 要附的源码文件。

**哪些文件要附源码，从镜像本身读**：解析镜像里的 `/etc/NOTICE.html.gz`，凡许可文本含 GPL、LGPL、MPL、EPL、CDDL、EUPL 的，逐个文件追溯：
- 文件 → 模块：`module-info.json`（`make out/target/product/pw3/module-info.json`，只写表、不编译）；静态库按模块名查。
- 模块 → 源码：
  - AOSP 项目：`config/aosp-8.1.0_r81-resolved.xml`（同步时的 manifest）；
  - 内核：`vendor/amazon/pw3-hardware` 和 `/kernel`；
  - 本仓库的第三方应用：`device/amazon/pw3/apps/sources.json`。
- 追溯不到的文件（如 `prebuilts/` 下的预编译二进制）一个都不放过，脚本列出全部后停止，什么都不写。

**源码就是构建用的那份**，脚本逐项核对，不符就停：
- 每个 AOSP 项目的 HEAD 等于 manifest 记的版本；
- 本项目给它打的补丁，与镜像构建时记录的补丁（导出目录的 `build-result.json`）一致；
- 构建树里除了这些补丁没有别的改动或多余文件（`git status`）；
- 镜像里的内核模块来自的那次内核构建，用的是 `source-lock.json` 里的内核压缩包和本仓库现在的内核补丁。

**2026-10-19 演练**：拿当天的测试镜像跑了一遍，停在两处，两处都查清了：

- `/system/vendor/lib/modules/g_android.ko`："没有模块装它"。**不是源码缺失，是追溯用的元数据过期**：`module-info.json` 是 10-15 生成的，而 USB gadget 模块在 10-17 从 `g_ffs.ko` 改名成了 `g_android.ko`。所以脚本不靠说明里一句"请先生成"，而是自己检查：`module-info.json` 比镜像旧就停下，并说该跑什么。一份描述别的构建的元数据，会让"这些源码就是这个镜像的源码"这句话失去依据，而这正是整个脚本要保证的事。
- `/system/bin/gdbserver`：AOSP 的预编译件（在 `prebuilts/` 下），源码不在 AOSP 树里。它的构建标签是 `debug`，`user` 构建不装，所以发行版镜像里没有这个文件。**测试镜像停在这里是对的**：测试镜像本来就不该发行。

换上新的 `module-info.json` 后，除 `gdbserver` 外再无一处追溯不到。

**输出**（2026-10-15 演练）：

| 文件 | 大小 | 内容 |
|---|---|---|
| `<名称>-aosp-sources.tar.xz` | 28.5 MB | 14 个 AOSP 项目：art、libcore、external 下的 e2fsprogs、iptables、iproute2、selinux、libnl、libcap、libexif、fec、dnsmasq、f2fs-tools、gptfdisk、mmc-utils。按 manifest 版本取出，打上本项目补丁；同一输入两次生成逐字节相同 |
| `linux-3.0.35-amazon-5.16.2.1.1.tar.gz` | 119 MB | 亚马逊发布的内核源码原样；补丁、配置、构建脚本在仓库标签里 |
| `einkbro-16.7.1-source.tar.gz` | 14.5 MB | EinkBro 上游源码（GPL-3.0-or-later） |
| `rime-ice-da1fbe6-source.tar.gz` | 16.8 MB | 拼音词库的来源（GPL-3.0-only） |
| `SOURCES.md`、`sources.json` | — | 镜像里 54 个 copyleft 文件各自的源码在哪里；书面要约 |

- **Chromium（WebView 的 LGPL 部分）用书面要约**：源码包 6.7 GB，超过 GitHub 单个发行文件 2 GiB 的上限。要约写明自发布起至少三年、只收复制成本、通过 `--contact` 指定的渠道索取，并给出 Google 公开的同一份源码地址和 MD5。
- 用时约 1 分 45 秒。测试：`tools/tests/test_collect_sources.py`（12 项）。

**2026-11-01：v0.1.0 真的生成了一次，六个文件都挂到 Release 上了。** 过程里有一件不明显的事，以后每次都会遇到：

脚本只看 `out/`，而**两个变体各有自己的输出树**——最后构建的那个叫 `out/`，另一个叫 `out-<变体>`，`build-aosp-system.sh` 靠改名来换（见它开头的注释）。10-31 最后一次构建是 `userdebug`，所以 v0.1.0 的 `user` 树完好地留在 `out-user/` 里：`system.img` 是 `08a4d46f…`、469,244,836 字节，和 `export-v010-20261031/build-result.json` 的 `files["system.img"].sha256` 逐字一致，`module-info.json` 还比它新一分钟。两道按内容的拒绝因此都过得去。

**办法是把 `out-user` 临时遮在 `out` 上，不改名、不写树**（改名要动 `.pw3-variant` 的簿记，遮盖只要解除挂载）：

```sh
SRC=/home/builder/android-src                      # 在构建虚拟机里，不进 chroot
sudo mount --bind "$SRC/out-user" "$SRC/out"       # 跑完一定要解除
sudo runuser -u builder -- env PATH=/usr/sbin:/usr/bin:/bin HOME=/home/builder \
  python3 /mnt/port/tools/collect-copyleft-sources.py \
    --export /mnt/build-share/export-v010-20261031 \
    --entry /mnt/build-share/entry-v010-20261101 \
    --name Readom --tag v0.1.0 \
    --contact https://github.com/hbyxabc/readom/issues \
    --output /home/builder/sources-v0.1.0
sudo umount "$SRC/out"
```

以 `builder` 的身份跑：那棵树是 `builder` 的，而 `git` 不接受别人名下的树（`safe.directory`）。`PATH` 要带 `/usr/sbin`，脚本调 `debugfs` 读镜像里的 `NOTICE`。输出先写 `builder` 的家目录再拷进 `/mnt/build-share`。跑完复核了两件：`out/.pw3-variant` 还是 `userdebug`，`out-user` 里的 `system.img` 还是 `08a4d46f…`——树没被动过。

产出和 10-15 那张表一样，大小是：AOSP 包 28,506,024，内核包 119,420,718，EinkBro 14,534,339，rime-ice 16,782,935，`SOURCES.md` 5,517，`sources.json` 13,742。每个压缩包的 SHA-256 在 `sources.json` 的 `assets` 里。`SOURCES.md` 和 10-29 演练那份**逐字节相同**（文件清单和要约都没变），`sources.json` 和 AOSP 包不同——它们绑的是这一次的构建。

**下一次 `user` 构建会把 `out-user` 覆盖掉**，之后要重做 v0.1.0 的源码包就只能先把那次构建逐字节复现（`PW3_BUILD_DATETIME` 用发行记录里的值）。所以顺序是定的：**发行镜像导出之后、下一次 `user` 构建之前**就把源码包生成出来。

**2026-11-03 又跑了一次，结论是"不用换"。** 换镜像就要重跑这一步，但不一定要重传。当天最后一版
（带 0072）的 `user` 树就在 `out/` 里（最后一次构建就是它，所以不用遮盖：`system.img` =
`e7f574be…` 与导出回执逐字一致，`module-info.json` 还新一分钟，两道按内容的拒绝都过），
跑出来六个文件与 11-01 挂在 Release 上的那一份**逐字节相同**。合理：这一天的补丁全在
`frameworks/base`（Apache-2.0），镜像里 copyleft 文件的集合没动。所以 **GPL 义务对新镜像仍然成立，
源码附件不用重传**——但这句话只有在**跑过并逐字节比过**之后才能说，不能从"改的是 Apache 的代码"直接推出来。

## 发行流水线演练（2026-10-19）

在做发行工程之前，先把发行要走的每一步在当天的开发构建上空跑一遍，只为找毛病，不发布任何东西：`build_payload.py` → `render_screens.py` → `build_entry.py --installer`，再用造出来的载荷跑 `test_unpack.py`。找到三处毛病，现在的做法如下：

1. **空数据镜像不是空的**（最要紧的一处）。见 `installer/README.md`：开发期间早先发出的数据模板里夹带 4.2 MB AOSP 测试程序，按同样办法再做还会多一个 `/data/app/PW3FormatTest.apk`。现在模板由 `build_payload.py` 对空目录造，并按超级块核对。
2. **画面放不下**，而且是算法算错。见 `installer/README.md` 的"区域高度按每个画面真正要画的算"。渲染和 `test_screen.py` 都在 `tools/run-tests.py` 里跑。
3. **导出回执可能记的是另一次构建**。`export-aosp-images.py` 的 `built_utc` 和 `build_log_sha256` 取自 `logs/aosp-system-job.json` 和 `logs/aosp-system-build.log`，而这两个文件只有作业外壳 `run-aosp-build-job.sh` 会写。开发期间有几次构建直接调了 `build-aosp-system.sh`，于是这几次导出的回执都在证明 10-15 的那次构建：演练造出的载荷清单里 `built` 写着 `2026-10-15T16:05:09Z`。这个时间会进入安装器"设备时钟不得早于构建时间"的判断（早于就无法校验 TLS）。
   - 曾考虑用"回执不能比镜像旧"来防，**这个办法不成立**（2026-10-19 对抗式审查查出）：本项目切换构建变体就是把整棵输出树 `mv` 过去，而 `mv` 保留时间戳；从副本恢复也一样。"回执比镜像新"因此根本不能证明它是这批镜像的回执，本项目自己的构建流程就会绕过它。
   - 现在的做法：**构建作业把它造出来的两个镜像按内容记进回执**（`logs/aosp-system-job.json` 的 `images` 字段），导出时逐一核对 SHA256，对不上就拒绝。构建日志由同一次运行写出，因而被同一条绑定覆盖。
   - **因此发行版的那次构建必须用 `tools/run-aosp-build-job.sh` 启动**，不能直接调 `build-aosp-system.sh`。

实测（2026-10-19、构建机 6 核）：载荷 46 秒（系统镜像 196 MB、数据模板 59 KB），画面 10 秒，入口 17 秒，`Readom.sh` 7.8 MB。`test_unpack.py` 15 项通过，其中真实载荷在 QEMU 下展开系统镜像 25 秒、数据镜像 23 秒。

GitHub 上传与下载不在这次演练里：Release 的地址通不通是仓库上的事实，构建机上验证不了，所以放到发行之后做（发行清单第 13、14 步）。

### 第四处毛病：下载地址把文件名写了两遍（2026-10-21 发现）

`build_launcher.py` 收的 `--release-url` 是**目录**，文件名由它自己接在后面。开发期间曾给成完整文件地址，于是入口里写着

```
…/releases/download/<ver>/readom-system-<ver>.img.zst/readom-system-<ver>.img.zst
```

这个地址必然 404。没人发现，是因为上机测的那条路径安装包已经在 Kindle 盘上，根本不去取这个地址——而它恰好只对**唯一需要它的人**失效：只会下载的那一位。现在 `build_launcher.py` **先把 authority 定下来再分类**（小写、禁用户信息、禁端口、禁百分号编码、禁空标签和尾部点、至少两段、每段不超过 63 字节且整体不超过 253 字节），**路径里整个禁用百分号编码**——不是解码后再比，因为一次解码答不了服务器会怎么解（`%2e%2e` 解一次是 `..`，`%252e%252e` 解一次还是 `%2e%2e`），而发行地址是一个版本号加一个目录名，用不到这个机制。定下来之后**对两类地址宽严不同**（2026-10-30 使用者定了线路之后改的）：

- **github.com 上的地址必须正好是** `https://github.com/<所有者>/<仓库>/releases/download/<标签>` 这个形状。GitHub 有形状，形状里就有两个各坑过一次发行的陷阱——附件自己的地址、标签页——所以在那里按形状卡。
- **其他域名没有形状可卡**，就只检查任何地址都成立的那些：https、没有查询串和片段、路径里没有空段和 `.`／`..`、以及**路径里没有哪一段正好等于载荷名**。最后这条就是 V65 那个守卫，它不认域名，照样能抓住"把附件地址当目录传进来"。它按段比，不按子串数：数路径里有几个 `/<载荷名>` 会把只是以载荷名开头的段也数进去——叫 `<载荷名>-backup` 的目录、叫 `<载荷名>-rc` 的 GitHub 标签，都是合法的一段，却被拒了（2026-10-30 Codex 复审指出）。
- **反过来也卡**：带着 GitHub 那个形状却不在 github.com 上（`githubb.com/...`），判为域名打错，拒绝。

对不上就让构建失败（`tools/tests/test_release_url.py`，9 个接受、43 个拒绝）。

### 下载线路：入口里一个有序的地址表，外加服务器侧的路由

**2026-10-11-01 改成多地址，因为单地址被实测否掉了。**原先（2026-10-30 定）入口只写
`https://readom.app/dl/<版本>`，路由全交给官网的 Worker。那个判断的前提是"可达性由服务器侧
决定"——而 2026-10-31 整机测试证明它不成立：本项目自己的设备 26 分钟只拿到 191 MB 里的
515,554 字节，最后两次尝试连 HTTP 应答都没有，而同样的字节从另一条网络回来是 390 KB/s。
`readom.app` 在 Cloudflare 的计划上没有境内节点，国内访客一律被引到境外 PoP，这件事服务器侧
改不了。**可达性不是发行版的属性，是读者和某一家供应商之间的属性**，所以地址表必须在设备手里。

`RELEASE-PLAN.md` 2026-10-24 就定过"多地址逐个试 + 手拷贝兜底"（G2），当时没做，理由是
"要重排安装器的失败分类"。那个理由是对的，而重排之后的分类**比原来更对**：

- 404／410／416／长度不对，原来直接判"发行文件有问题"。现在改成**这个源没有这个文件**，
  换下一个；只有每个源都这么说，才是发行版真的不在（E303）。这不只是为了镜像——
  2026-10-31 Worker 的 GitHub 回源分支就挂着上一版的载荷（短 497 字节），单地址的分类会让
  一个好的发行版被一个陈旧的镜像否掉。
- 一个字节都没来，原来累计到两次就停。现在**先换源**：取不到的源只花一次尝试，而不是花光全部额度。
- **低于速度下限也算这个源不行**（2026-11-01 加）。下载给 curl 的下限是
  `--speed-limit 1024 --speed-time 60`，低于 1 KB/s 满一分钟 curl 退出 100。原来"有字节就是有进展"
  会把这种尝试算成进展、守着同一个源不放——10-31 那 26 分钟正是这么花掉的：四次尝试各拿到几百 KB，
  每一次都"比刚才多"，于是速率从头到尾不是问题。按 1 KB/s 算 191 MB 要两天多，而那一屏的建议本来
  就是用电脑下，所以一分钟低于下限就是关于这条路的答案：换下一个源，并且**同时计入 idle**——
  只有一个源时，两次这样的尝试就报 E201（原来要六次），分片照样留着、再点一次接着下。
- `refused`（"这个源没有这个文件"的计数）只要有字节到就归零，**不看速率**。它数的是"说文件不在"的源，
  而字节是一个源在说相反的话；并进上一条会让"一个源慢慢供着、另一个源连着 404"报成 E303。
- 两个额度都按源计：`DOWNLOAD_ATTEMPTS * 源数`、`DOWNLOAD_IDLE_ATTEMPTS * 源数`。
  `installer` 那套测试现在 171 项全过，新加的三项各有变异测试：去掉"低速换源"挂两项，
  去掉"有字节就清零 `refused`"挂一项。

**换源时 `.part` 不丢，这一点和 2026-10-24 的设想相反，而且是故意的。**理由：各个源装的是
同一个对象（发行时同一份字节传到每一个源），而载荷是**整份到齐之后按 `payload.sha256` 准入**的，
所以跨源拼出来的分片和单源下完的分片受同一道校验。某个源给了不同的字节，哈希会拒（E302、删掉载荷、
下次重下），这和任何一次损坏的下载是同一条路。代价说清楚：被坏镜像污染的情况下，使用者白下一次。
换成"每次换源都丢分片"则意味着慢网上永远装不完——那个代价更大，而且天天都要付。

路由仍然留在服务器侧，两者不冲突：Worker 先取 R2、不在就 302 去 GitHub，而**那整条链只是地址表
里的一个条目**。设备侧的 curl 本来就跟重定向，所以服务器侧加镜像、按地区分流照旧不需要重发入口；
而地址表让"某一家整条不可达"也有出路。

把路由放到服务器侧，设备侧一行 C 代码都不用动，因为下载用的 curl 本来就跟重定向：

```
curl --fail --location --max-redirs 5 --proto =https --proto-redir =https
```

（`installer/install/kindle.c`）。`--proto-redir =https` 保证整条重定向链不会掉到 http 上。于是 Worker 可以先回源 R2、不在就 302 去 GitHub Release，**以后换线路、加镜像、按地区分流都不用重新发布入口**——已经装在使用者手里的那一份跟着一起变。

代价要说清楚：每多一个源，就多一份"它必须装着同一个对象"的发行工程责任，而且不可达的源会花掉
一次尝试（最坏情况约 90 秒：30 秒连接超时加 60 秒低速判定）。所以上限是 4 个，而不是一个会长的表。
线路全断时的兜底仍然是"用电脑下载载荷、拷进 `documents/`"（那条路早就实现，失败屏上写着文件名）。

**回源那条线上，慢网可能要几次尝试才下完**。整条是 `readom.app 302 → github.com 302 → release-assets.githubusercontent.com 206`，两跳，在 `--max-redirs 5` 之内，全程 https；**续传跨得过这个跨主机的 302**（官网会话 2026-10-30 实测）。GitHub 的附件签名地址带着**约 30 分钟**的有效期（同一次实测，看的是 URL 里的 `se=` 参数），而 Kindle 的 Wi-Fi 慢到 100 KB/s 时 190 MB 要三十多分钟。

**没实测的那一半要说清楚**：已经开始的传输会不会在到期那一刻被掐断，没有量过——那次只取了 100 字节。S3 的文档说到期是在请求时判定、已开始的下载可以跨过到期点继续，这说明"必然被掐断"不是通用规则，但也不足以证明 GitHub 当前后端的行为（2026-10-30 Codex 复审指出，原文把这件事写成了确定的）。

两种结果这边都接得住，所以**不需要为签名到期写任何代码**：真被掐断，那是一次有进展的失败，`install.c` 的 `idle` 只在这一次一个字节都没多下来、或者整段都低于速度下限时才累加；传得动而且不是低速退出就归零，下一次尝试重走 302 拿新签名、从断点接着下。签名到期掐断的是一条正在以正常速率传的流，curl 的退出码不是 100，所以这条接力不受上面那条新规则影响。**"六次尝试"因此是接力，不是六次重试同一件事**，但不能把"一次成功"当成基准去估重试次数：若每次都被 1800 秒截断，六次总共能拿下 190 MB 的最低速度约 **17.7 KB/s**；低于这个速度六次用完还没下完，此时报 E201、**分片留着**，使用者再点一次又是六次额度、从当前分片继续。R2 命中那条没有签名、没有第二跳、`cache-control: immutable`，设备正常走的是这一条。

还有一条分工随之定下来：既然 416 在设备侧只剩"服务器上的对象比清单短"一种意思、而且**直接报 E303 不重试**，那么**传上去一个被截断的对象，在设备上就表现为一次假的发行版故障**。所以上传之后逐个 HEAD 回读 `content-length` 与本地字节数比对，是挡住这种假故障的那一步，不是冒烟测试，不能为了省时间跳过（官网那边自己记着这一条）。另一半是跨产物约定：Worker 对"起点正好等于对象长度"必须按 RFC 回 416（`>=`，不能松成 `>`），也不能改成"越界回整份 200"。后一种会怎样，这里原来写成"设备把整份接在旧分片后面、变成假的 E303"，**那不对**（2026-10-30 Codex 复审指出，并对照了 curl 7.86 的 `http.c`）：分片短于清单、服务器把正确的完整对象以 200 回过来时，curl 走 `CURLE_RANGE_ERROR`（33）——请求带着非零续传起点而应答没有有效的 `Content-Range`——而安装器对 33 的处置是删掉分片重下。所以代价是白下一遍 190 MB，不是一次假故障。（这条不是"非零起点加没有 `Content-Range` 就一定 33"：应答长度恰好等于已有偏移时 curl 可以返回成功，所以说的是上面那个具体场景。）约定仍然别改，只是理由要说准。

版本号写错不另设检查：载荷是按 SHA256 认的（`install.h`），地址指向别的版本会被拒绝，不会把旧系统装上去。

**构建时查不出来的那一半**，必须发行后补：Release 的附件上传完，按入口回执里的 `installer.payload_url` 真去取一次，核对 `installer.payload.sha256` 和字节数。这一步进发行清单，不是可选项——地址对不对是仓库上的事实，构建机上问不出来。

## `user` 构建演练（2026-10-19）

用一次性密钥（证书主题写明 "PW3 rehearsal only"，只在构建机里，签出来的东西一律不发行）跑 `pw3-user`，目的是找构建层面的毛病，与发行密钥的保管无关。

**第一处卡住**：`insertkeys.py` 要 `$PW3_RELEASE_KEY_DIR/testkey.x509.pem`，构建在编译任何东西之前就停了。

原因是 `system/sepolicy/private/keys.conf` 里 `@RELEASE` 这一项写死了 `testkey.x509.pem`——AOSP 自己就是用 `build/target/product/security/testkey` 签名的。本项目的 `user` 构建改用自己的发行密钥（`pw3.mk`，没有密钥直接报错），那个目录里的名字是 `releasekey`，于是找不到文件。

改法不动 AOSP：`BOARD_PLAT_PRIVATE_SEPOLICY_DIR`（`device/amazon/pw3/sepolicy-private`）里的 `keys.conf` 会被接在 AOSP 那份后面，同名小节后写的覆盖先写的。只覆盖 `USER` 一行指向 `releasekey.x509.pem`；`eng`/`userdebug` 仍用 AOSP 的目录，那里有 `testkey` 没有 `releasekey`。

**代价**：构建时就用发行密钥签（`PRODUCT_DEFAULT_DEV_CERTIFICATE`），所以**密钥在整个构建过程中都要在构建机上**，不是只在签名那几分钟。AOSP 的另一条路是先用 testkey 构建、再用 `sign_target_files_apks` 重签，密钥只在重签时需要。这条路没有走：它要改成 target-files 流程（`make target-files-package`、`img_from_target_files`），导出路径也要跟着改；而且本项目依赖 dexpreopt 和逐字节不变的启动镜像，重签对 odex 的影响尚未验证。

**改完之后构建通过**（2026-10-19 17:10，无一处 FAILED）。镜像核对：

| 项目 | 结果 |
|---|---|
| `ro.build.type` / `ro.debuggable` / `ro.secure` / `ro.adb.secure` | `user` / `0` / `1` / `1` |
| `persist.sys.usb.config` | `default.prop` 里是 `mtp`（`user` 构建不接 `,adb`）。开机后机器上读到的是空的——`UsbDeviceManager` 会把 `mtp` 剥掉——此时 `getDefaultFunctions()` 答 `mtp`，gadget 就是 MTP、没有 adb，与 `usb-mtp.md` 一致 |
| `su`、`gdbserver`、`strace`、`logpersist.start` | 都不在镜像里 |
| 本项目的系统程序 | 当时列着的都在。核对标准是 `initialization.mk`、`pw3.mk` 的 `PRODUCT_PACKAGES`，现在列的是 `pw3-init-check`、`pw3-boot-report`、`pw3-fg-state`、`pw3-system-verify`、`pw3-testfile-init`（装在 ramdisk 里），加上 USB 组件带进来的 `pw3-usb-check` |
| 文件数 | 1616（userdebug 是 1669；少的全是调试工具和它们专用的 `libchrome`、`libevent`） |
| 45 个 ART 启动镜像文件（`boot*.art`、`.oat`、`.vdex`） | 与 userdebug **逐字节相同**——也就是说从测试版升到发行版不会触发应用重编 |
| system.img | 463 MB（userdebug 472 MB） |
| 签名 | `SystemUI.apk` 的 `META-INF/CERT.RSA` 指纹 `13:E2:88:…`，主题 "PW3 rehearsal only"；userdebug 那份是 `C8:A2:E9:…`，主题 `O=Android`（AOSP 公开的平台密钥）。**换密钥这件事确实生效了**——这正是 `user` 构建存在的理由：AOSP 的平台密钥是公开的，拿它签的应用能拿到系统权限 |

两处要记住的：

- **`ro.build.tags=dev-keys`**。AOSP 只在默认证书正是 `build/target/product/security/testkey` 时写 `test-keys`，其余一律 `dev-keys`；`release-keys` 是 `sign_target_files_apks` 那条路才会写的。本项目没走那条路，所以这个值是实话，不改。
- **发行构建必须带 `PW3_AOSP_INSTALLCLEAN=1`**。这次没带，镜像里就留着 `etc/init/wifi-events.rc`——那是 10-17 已经从本项目删掉的文件（本机内核没有 ftrace，开机时 init 会在它的 25 条命令上全部报错），文件日期停在 10-16，是上一次往这个输出树里装东西时留下的。**模块不再安装，已经装进暂存树的文件不会自己消失，镜像就把它带上。** 构建作业回执（`logs/aosp-system-job.json`）记下变体和是否 installclean，导出回执照抄，这样"这个镜像是哪种构建"有据可查。
  - 带上重建后：`wifi-events.rc` 没有了，文件 1615 个（少的正是它），暂存树里**没有一个文件早于这次构建**，启动镜像 45 个仍逐字节相同。代价只有 **5 分钟**（objects 保留，只是重装一遍），对发行版可以忽略。

## 发行清单（按顺序走，每一步都有可核对的产出）

写成一张单子是为了两件事：**一次发行能被重复**，以及**别人也能发**——后者是开源的前提之一。每一步后面是它的证据，不是"做了就行"。

| # | 做什么 | 证据 |
|---|---|---|
| 1 | 设备侧问题清零 | `docs/open-items.md` 没有剩项；上一版在设备上装过并跑过 `tools/verify-device.sh`，而且它以 0 退出——退出码 2 表示有几项没能查，那一轮不算验收通过；标 `~~` 的是这个镜像上按设计查不了的（发行构建上要 root 的那几项），不挡 |
| 2 | 密钥就位 | 四把齐全，放在仓库之外（构建时由 `PW3_RELEASE_KEY_DIR` 指向），**且已有离线备份**（这一条只有人能确认） |
| 3 | 工作树干净、测试全绿 | `git status` 无改动；`tools/run-tests.py --cc gcc --zstd-archive <zstd 归档>` 在构建机上全过：0 失败、0 项未跑（第 3a、3b 两步就是为了让需要 QEMU 内核和 userdebug 导出的那几项也能跑） |
| 3a | 两棵 QEMU 测试内核是**这一套**补丁的 | `tools/build-kernel-smoke.sh` 再 `tools/run-kernel-smoke.sh`（内核接口测试：network、process-vm、binder、filecaps、aslr…），然后 `tools/build-loader-stage-kernel.sh`。后者自带闸门：smoke 的结果必须描述同一套补丁，否则停下。`handoff-in-qemu` 和 `handoff-client` 读的就是这棵 loader-stage 内核，套件里那两项能跑就说明这一步做过 |
| 3b | 三个 QEMU 冒烟测试（ART／zygote／系统服务）| 它们在发行镜像上跑不起来：AOSP 三条 `neverallow` 封住了「init 把载荷拷进 `/data`、再以 shell 域运行」这条路（细节在 `tools/aosp-runtime-smoke/README.md`，以及 zygote 和 service 两份同名文件里）。所以用**同一棵源码树**另构一次 userdebug、导出、做文件清单，再跑套件，这三项就会跑（`PW3_EXPORT_DIR` 指向那份导出，或者把它命名成 `aosp-images-device-test-*` 让套件自己找到）。证据：2026-10-22 实测，变体切换只用了 28 秒（树是热的），三项全过，且**两种变体的 45 个 ART 启动文件逐字节相同**——所以在 user 变体上做的"升级不触发重编译"的判断对 userdebug 那棵同样成立。另外 `data-image` 要的种子镜像来自这次构建自己的载荷，所以第 6 步做完之后它才会跑 |
| 4 | 构建 | **必须经 `tools/run-aosp-build-job.sh`**（回执把镜像按内容绑定，直接调 `build-aosp-system.sh` 会让导出用上别次构建的回执）：`PW3_BUILD_VARIANT=user`、`PW3_AOSP_INSTALLCLEAN=1`、`PW3_RELEASE_KEY_DIR=/release-keys`、`PW3_BUILD_VERSION="Readom v0.1.0"`（写进 `ro.build.display.id`，就是「设置 > 关于平板电脑 > 版本号」那一行；不给就是 AOSP 的 `OC dev-keys`，说不出这是哪一版） |
| 5 | 导出 | `export-aosp-images.py`：回执里 `variant=user`、`installclean=true`，两个镜像的 SHA256 与构建回执一致。**再跑一次 `sudo tools/inspect-system-image.py --image <导出>/system.raw.img --output <导出>/filesystem-inspection.json`**：导出本身不写这个文件，而第 11 步要读它 |
| 6 | 载荷 | `build_payload.py`：清单 schema 4，`built` 是这次构建的时间 |
| 7 | 画面 | `render_screens.py --profile boot/profiles/<机型>.json`：机型名与入口一致 |
| 8 | 入口 | `build_entry.py --installer`（**不带 `--test`**，两者互斥）；**连建两次，`Readom.sh` 逐字节一致** |
| 9 | 不触发重编译 | `/system/framework/arm/` 下所有 `boot*.art`／`.oat`／`.vdex`（今天 45 个，这个数字只作参照——按集合数，别按记忆）与上一版逐字节比对：导出回执 `build-result.json` 里的 `boot_image.sha256` 按每个文件的名字和内容算出，与上一版的相同即可；变了就要在发行说明里写明"首次开机要重编译" |
| 10 | 不含私有数据 | `tools/check-private-data.py`：黑名单 0 命中，机器生成字节之外 0 发现（摘要会把这两个数分开报，见下）；`Readom.sh` 里不含波形、校准、ATAGs、序列号、adb 公钥 |
| 10b | **要上传的那个文件解回去，等于入口烘进去的期望** | 不用上传、不用设备，几秒钟：`zstd -dc <发行目录>/readom-system-<版本>.img.zst \| sha256sum`，结果要等于入口 `launcher/release.h` 里 `.system` 那个字节数组，解出的字节数要等于那里的长度（2026-11-03 起 512 MiB）；再把上传文件自己的 SHA-256 与 `.payload` 对一遍。**挡的是"入口配的是另一份载荷"**——这个项目已经栽过一次（漏带 `--release-url` 的那份恰好叫 `ship`），而这类错在设备上要等整整 190 MB 下完才暴露。`release.h` 里是十进制字节数组，对照时转成十六进制。**2026-11-03 当天建了四版**（补丁 0069、0070、0071 各要一次上机才查出下一处），这一步每版都核过。**现在线上的那一版**（2026-11-04 的 h：f 加安装器修复；f 是 11-03 带 0073 的那版）：`.payload` = 190824820 / `c82dbe9f…`，`.system` = 536870912 / `e2bf999c…`，入口 `Readom.sh` 7,759,418 字节 / **`7319e813…`**；被它换掉的 f 是 190824793 / `3b292432…`、`9f268e2c…`、`a5827a94…`（都记在 `readom-site/site-deploy/releases/v0.1.0.json` 的 `superseded` 里，以它为准），连建两遍一致，两个源、GitHub 在前，`build_incremental` = `eng.builde.20261103.110643`，启动镜像仍是 `1cdfe692…`（0073 只改 `services.jar`，不动 ART 启动镜像，所以升级不会让 `/data` 里的机器码失效）。随发行的源码包在这一版的树上也跑了一遍，六个文件与线上那份**仍然逐字节相同**，所以源码附件不用重传。上一版（带 0072，一度发出去过）：`.payload` = 190823293 / `6376f975…`，`.system` = 536870912 / `d3d15306…`，入口 7,759,418 字节 / `249dd8a7…`。**这一次是分两步核的**：先把重建后的 `release.h` 和重建前的逐行比，**只有 `payload_urls` 和 `payload_origins` 两处不同**（1 → 2），`.payload`、`.system`、`.template_file`、`.built` 全部逐字节相同——所以第 10b 步的结论原样成立；再独立跑一遍 `zstd -dc | sha256sum` 和 `zstd -dc | wc -c` 两趟（**不要用 `tee >(sha256sum)` 一趟做完**：进程替换的输出落进同一条管道，`wc -c` 把那 88 个字节也数了，536,870,912 当场变成 536,871,000），得到 536,870,912 / `d3d15306…`，与 `.system` 那个字节数组逐字相同。重建前那一份入口是 `ac8a4db0…`，它地址表只有一个源，不发。上一版（带 0071）：`.payload` = 190823888 / `673b7a7c…`，`.system` = 536870912 / `05f7560c…`，入口 7,759,418 字节 / `24c9f6d6…`。**这里有个容易上当的地方**：四版的入口都是 **7,759,418 字节**，和线上那一份也一样——大小一个字节都不差，内容每版都不同。所以核对上传了哪一份只能按内容（R2 看 ETag＝MD5，GitHub 那份下回来重算），长度什么都不证明。以下是当天第一版的记录：`.payload` = 190830823 / `6ca56d38…`，`.system` = 536870912 / `b3213388…`（`zstd -dc | sha256sum` 与入口烘进去的那个字节数组逐字相同），入口 `Readom.sh` 7,759,418 字节 / `acfc1a3a…`，连建两遍一致。启动镜像仍是 `1cdfe692…`，所以升级不会让 `/data` 里的机器码失效。**2026-11-03 14:20 两处线上附件都换成了这一版，并按内容核过**：GitHub 那三个附件真下回来重算——入口 `249dd8a7…`、载荷 `6376f975…`，`SHA256SUMS` 的内容对，且 `shasum -a 256 -c` 对着下回来的两个文件两行都 OK；Release 正文读回来与上传的逐字相同，两处哈希都换了。R2 那三个对象按 **ETag（单次 PUT 的 R2 对象 ETag 就是 MD5）**与本地逐个相同（`22ba1a46…` / `279c9ec7…` / `6b3239aa…`）——**载荷 190 MB 不从 `readom.app` 下回来重算**，因为那条线在这个网络上实测 26 分钟只给 515 KB。下面留着上一版的记录：**现在发出去的这一版**（载荷 2026-10-31 建、入口 2026-11-01 建）：`.payload` = 190816894 / `ff432b02…`，`.system` = 603979776 / `d3e68e62…`——后者和设备 `state` 里记下的那个值逐字相同，11-01 的干净安装就是按它准入的。（2026-10-30 那一版核的是 `.payload` = 190816397 / `32813650…`、`.system` = 603979776 / `367fa7ef…`，官网会话先做了一遍、这边独立复算一致；那两个值已经不在线上，留在这里是为了说明这一步长什么样。）顺带一个容易混的：导出回执里的 `system.img` 哈希（`51b48b9e…`）和 `.system`（`367fa7ef…`）不矛盾——前者是 AOSP 的构建产物，后者是补齐到 576 MiB、设备真正写入的那一份，清单里记的是后者 |
| 11 | 源码包 | `tools/collect-copyleft-sources.py --export <导出> --entry <入口目录> --name <名称> --tag <仓库标签> --contact <索取渠道> --output <目录>`：GPL 部分的源码与镜像里的二进制对得上。它要读这次构建的 `module-info.json`（哪个模块装了哪个文件），而那个文件的 make 规则没有依赖项、只要存在就被当成最新，所以 `build-aosp-system.sh` 先删再做；开发期间就有一次捡到一周前的旧文件，被脚本直接拒绝。内核按内容绑定三样东西——入口回执里的 `kernel_sha256`（`Readom.sh` 里的 zImage）、`epdc_module_sha256`，以及镜像里每个 `.ko`（从第 5 步的 `filesystem-inspection.json` 读），都要对上这次内核构建的产物；产物自己先与内核构建回执核对过。不比内核回执文件本身的哈希：回执里有编译时间，内核一重编就会被判成另一次构建，哪怕 zImage 和模块逐字节相同 |
| 11b | **上传的字节就是上面验过的那一份** | 第 8–11 步验的全是**构建目录**，而 `release.py --upload` 传的字节就是命令行上 `--entry` / `--system` 指的那两个路径（`--out` 只决定 `SHA256SUMS` 写哪儿）。所以这一步要挡的不是"暂存链错了"，而是**整对构建目录都传错**。盘上的候选多到必然出事：2026-10-30 有 10 份 `readom-system-v0.1.0.img.zst`，大小从 190,808,562 到 190,817,497 挤在一起；`Readom.sh` 有 12 份都是 7,697,433 字节、分属 6 次构建（数字会过期，重算的办法不会：`for f in */launcher/Readom.sh; do printf "%s %s %s\n" "$(stat -f %z "$f")" "$(shasum -a 256 "$f" \| cut -c1-12)" "$f"; done \| sort`）。而名字骗得最狠的那个就叫 `ship`。`release.py` 现在内建三条检查，不带 `--upload` 也照跑（只报告不拦，否则就成了"只有做了不可逆的事才能验"的检查），任一条不过就 `exit 1`、一个字节不传：<br>（1）**入口自验**：解开内嵌 helper，比脚本自己 `[ "$hash" = … ]` 那一行，并确认是 ELF。手工等价命令 `sed '1,/^__PW3_FIRST_KERNEL_HELPER_BASE64__$/d' Readom.sh \| base64 -d \| shasum -a 256`，离开构建机之后仍然能验，所以 GitHub Release 那一份也该这么验——它不走 R2。<br>（2）**载荷配不配这个入口**：对着载荷回执的 `installer.payload.sha256`（没有就退到 `release.h` 的 `.payload`）。**按回执认，不按名字认。**<br>（3）**入口本身对不对**：`--expect-entry <已公布的 SHA-256>`，带 `--upload` 时必填。**这一条不可省，而且只能从外部给。**实测：`entry-v010-ship` 配它自己的 `payload-v010-ship`，前两条**全过**——一对自洽的旧构建，内部怎么比都比不出来，只有外部锚拦住。把哈希写在命令行上是安全的，因为脚本是**核对**它而不是相信它：值过期只会拒绝上传。这和"文档里写死的数字"正好相反——那种过期了没人发现，这种过期了当场拦住。<br>四种真实违规都试过会拦：换成 `payload-rc-v0.1.0-final`、期望值填旧的、自洽旧一对、入口截断到 7,000,000 字节；带 `--upload` 退出码 1 且桶里仍为空，漏了 `--expect-entry` 也是退出码 1。<br>我手工交付的两份副本（拷进设备 `documents/` 的那一份、GitHub Release 的那一份）不经过 `release.py`，所以它们另外要 `cmp <我的副本> <入口目录>/launcher/Readom.sh`。2026-10-30 核过：暂存件与 `entry-v010-rc/launcher/Readom.sh` 同一个 inode，helper 5,695,724 字节、`0a305222…` 与脚本内钉的一致，入口 7,697,433 字节、`46577424…`。顺带核过一条以前没有的源码↔产物绑定：`boot/launcher/launcher-template.sh` 加上发行参数能**逐字节重现**出货脚本的头部（3172 字节，base64 分隔符之前）。<br>**入口的哈希在回执里本来就有**（2026-11-04 更正：这里原来写「留给下一版：入口回执缺 `launcher_sha256`」，不对）。顶层的 `build-result.json`（schema 3）里 `launcher` 确实只是一个路径，但它指向的入口目录里 `launcher/build-result.json`（`boot/build_launcher.py` 写的）一直带着 `launcher_sha256` 和 `helper_sha256`，h 版那份的 `launcher_sha256` 就是 `7319e813…`，与 `shasum -a 256 launcher/Readom.sh` 一致。它仍然代替不了第（3）条的外部锚：回执和字节是同一次运行写在同一个目录里的，只能回答"这次构建产出了这些字节吗"，回答不了"这是要发的那一份吗" |
| 12 | 上传**两处** | **R2**（设备要用的那条）：`<版本>/Readom.sh`、`<版本>/readom-system-<版本>.img.zst`、`<版本>/SHA256SUMS`，官网那套工具里的上传脚本一条命令（**`--expect-entry` 带 `--upload` 时必填**，见第 11b 步；少了它退出码 1）。**GitHub Release**（人要用的、也是 Worker 回源的去处）：`Readom.sh`、`readom-system-<版本>.img.zst`、源码包；发行说明里写两个文件的 SHA256、证书指纹、以及"要拷哪两个文件"。**两处都传完才算发行**——只传一处，官网按钮好用而设备装不了，或者反过来 |
| 13 | **发行后必做**：两条分支都真的通 | 从入口回执里的 `payload_url`（`https://readom.app/dl/<版本>/…`）真下载，核对 `installer.payload.sha256` 和字节数。**两条分支各走一次**：桶里有对象时的命中分支，以及把那个对象临时改名之后的回源分支（302 到 GitHub）。其中至少一次要带 `--continue-at` 从非零偏移续传，确认回的是 206 和正确的 `Content-Range`——断点续传是断网之后才会走的路，平时测不出来。**直接请求 GitHub 那一份不能替代其中任何一条**：它只证明那个对象可取，不证明设备从原始地址拿到的 302、`Location` 和 Range 续传整条链有效 |
| 14 | **发行后必做**：走一遍干净安装 | 预置条件要**可观察**，不能只说"`documents/` 里只有 `Readom.sh`"——那不足以保证会下载：已经装好且状态对得上就不下载，`android/download/<名字>` 里有完整载荷就不下载，`.part` 已经完整也会跳过 curl。所以开始前确认：`android/` 文件夹不存在（或 `state` 里没有 `system=ready`）、`android/download/` 里既没有完整载荷也没有 `.part`、`documents/` 里只有入口。验收证据同样要可观察：**日志里真的发生了下载、真的新建了 `data.img`、真的走完了首次 Android 初始化**——"最后进了桌面"不足以证明这些分支都走过 |

> **第 12 步的工具不在这个仓库里。**官网（站点本身、上传脚本、发行记录 `releases/<版本>.json`、
> Worker）2026-10-31 已经移出这棵树，是独立的一个项目——它自己的 `node_modules` 一个就 193 MB，而使用者的 Cloudflare 账户名只出现在那两个目录里。所以上传与回读是**仓库拥有者在官网那套工具里跑**的，公开仓库里看不到它们，也不需要：开源仓库给的是怎么构建出这两个文件，怎么把它们放到某个地址上是发布者的事。

## 第 4–8 步的命令

每一条都在构建虚拟机里跑。除了明写 `sudo` 的几条（用 systemd 起构建作业、清点文件系统），都以 `builder` 身份跑，**不要加 `sudo`**：`export-aosp-images.py`、`build_payload.py`、`render_screens.py`、`build_entry.py` 第一件事就是拒绝 root。`<ver>` 是发行标签，例如 `v0.1.0`；`PW3_BUILD_VERSION` 写成 `Readom <ver>`。

```sh
# 4 构建（环境变量见上表；PW3_BUILD_VERSION 要靠 aosp-build-make/0004 才生效）
# 上一次同名作业失败过时，先清掉它的状态；没有可清的，这一条会报 not loaded，可以忽略
sudo systemctl reset-failed pw3-aosp-system.service
sudo systemd-run --unit=pw3-aosp-system --service-type=exec \
  --uid=builder --gid=builder --setenv=HOME=/home/builder \
  --setenv=PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
  --setenv=PW3_AOSP_INSTALLCLEAN=1 --setenv=PW3_BUILD_VARIANT=user \
  --setenv=PW3_RELEASE_KEY_DIR=/release-keys \
  "--setenv=PW3_BUILD_VERSION=Readom <ver>" \
  /bin/bash /mnt/port/tools/run-aosp-build-job.sh
# 构建日志在 /mnt/build-share/logs/aosp-system-build.log；
# /mnt/build-share/logs/aosp-system-job.json 的 state 变成 completed 才往下走

# 5 导出，再补一次文件系统清点（导出自己不写它，第 11 步要读）
python3 /mnt/port/tools/export-aosp-images.py --output /mnt/build-share/export-rc-<ver>
sudo python3 /mnt/port/tools/inspect-system-image.py \
  --image /mnt/build-share/export-rc-<ver>/system.raw.img \
  --output /mnt/build-share/export-rc-<ver>/filesystem-inspection.json

# 6 载荷
python3 /mnt/port/installer/build_payload.py --export /mnt/build-share/export-rc-<ver> \
  --data-format 1 --version <ver> --output /mnt/build-share/payload-rc-<ver>

# 7 画面（字体在 AOSP 树里）
S=/home/builder/android-src
python3 /mnt/port/installer/screens/render_screens.py \
  --manifest /mnt/build-share/payload-rc-<ver>/manifest.json \
  --profile /mnt/port/boot/profiles/pw3-5.16.2.1.1.json \
  --cjk-font $S/external/noto-fonts/cjk/NotoSansCJK-Regular.ttc \
  --latin-font $S/external/roboto-fonts/Roboto-Regular.ttf \
  --latin-title-font $S/external/roboto-fonts/Roboto-Medium.ttf \
  --output /mnt/build-share/screens-rc-<ver>

# 8 入口，连建两次核对
python3 /mnt/port/boot/build_entry.py \
  --export /mnt/build-share/export-rc-<ver> --kernel /mnt/build-share/android-core-kernel \
  --system-path android/system.img --data android/data.img:1879048192 \
  --name Readom.sh --title Readom \
  --installer /mnt/build-share/payload-rc-<ver> --screens /mnt/build-share/screens-rc-<ver> \
  --release-url https://github.com/hbyxabc/readom/releases/download/<ver> \
  --release-url https://readom.app/dl/<ver> \
  --ca-bundle /mnt/build-share/third-party/ca-bundle-2026-09-25/cacert-2026-09-25.pem \
  --zstd-archive /mnt/build-share/third-party/zstd-1.5.6/zstd-1.5.6.tar.gz \
  --output /mnt/build-share/entry-rc-<ver>
# 同一条命令把 --output 换成 /mnt/build-share/entry-rc-<ver>-second 再跑一遍，然后：
cmp /mnt/build-share/entry-rc-<ver>/launcher/Readom.sh \
    /mnt/build-share/entry-rc-<ver>-second/launcher/Readom.sh
```

> **`--release-url` 要给两次，顺序就是设备尝试的顺序。** 这一行在 2026-11-03 之前只写了
> `readom.app` 那一个，而发行记录（官网那套工具里的 `releases/v0.1.0.json`）写的是
> **GitHub 在前、`readom.app` 在后**。照这里抄的那天建的四版入口里，地址表全只有一个地址——
> 11-01 才做好的"一个源取不到就问下一个"被悄悄丢掉了，而且**下游没有任何东西会发现**：
> 每个哈希都对，载荷也是对的那一份，只是使用者的 Kindle 去问一个不会回答的主机。
> 拦住它的是官网那套工具里 `release.py` 的 `origins` 检查（它把入口回执的
> `installer.payload_urls` 和发行记录逐项比），所以**第 11b 步不是走过场**：
> 这一次就是它报的 `BAD origins: the entry was built for https://readom.app/...`。

三个容易走错的地方：

- **`--installer` 指的是载荷目录，不是仓库里的 `installer/`**。它要读那个目录里的
  `manifest.json`，而 `installer/` 下没有这个文件，报错是 `FileNotFoundError`。
- **`--ca-bundle` 要给到文件**，而那个文件叫 `cacert-<日期>.pem`，不叫 `ca-bundle.crt`。
- **`check-private-data.py` 不要喂压缩过的载荷**。190 MB 的 zstd 块里随机字节会撞上邮箱正则，
  一次实测撞出四条。第 10 步要查的是仓库和入口，都在仓库目录下跑：`python3 tools/check-private-data.py`
  （不带参数，查仓库）和 `find <入口目录> -type f -print0 | xargs -0 python3 tools/check-private-data.py`。

  **入口那一跑会偶发报出"邮箱"，这是预期的，不是放行借口。**入口里有约 7.7 MB 压缩字节
  （`Readom.sh` 嵌着数据镜像和画面，`public-initramfs.cpio.gz` 是 gzip），模式类规则按原始字节跑
  ——藏在 blob 里的密钥或 MAC 正是它要抓的，代价就是每次重新压缩都可能掷出一个假阳性。所以这一步
  的判据要分两层：

  **2026-11-03 起这两层由工具自己分，不再靠读的人分。**每条发现后面会带
  `[in machine-generated bytes]`，摘要那行会写「其中几条在机器生成的字节里」。判据是字节本身有没有
  NUL、是不是合法 UTF-8（`is_text()`），launcher 在 helper 标记处切开——上面是脚本，算文本；下面是
  程序，算机器生成。黑名单命中**永远不带**这个标记。
  在这之前工具是**反过来**做的：尾巴整条是 base64 就不扫原文。那会藏住东西——序列号是 16 个字母
  数字，全在 base64 字母表里，所以一个序列号放到标记之后就消失，连黑名单的精确比对也一起取消
  （实测，同日）。而它换来的几乎是零：**邮箱形状要 `@` 和 `.`，这两个字符都不在 base64 字母表里**，
  所以编码区本来就产生不了邮箱命中，拿掉压制之后 `entry-v010-20261103e2` 仍然是 32 文件 0 发现。

  - **黑名单命中必须为零**，没有例外。那是本机真实值的 SHA-256 精确比对，不可能误报，而且允许清单
    压不住它（它对整个文件照跑）。
  - **模式类命中要逐条解压核实**，核实完**不要**往 `private-data-allow.txt` 里加条目：那里的条目是
    路径加文本，而入口每次重建、撞出的字节每次不同，加进去下一版就失效，只会在清单里烂掉。

  核实办法是看压缩包里的真内容，而不是看撞出来的那几个字节：

  ```sh
  gzip -dc <入口目录>/public-initramfs.cpio.gz > /tmp/x.cpio
  mkdir -p /tmp/initrd && cd /tmp/initrd && cpio -idm --quiet < /tmp/x.cpio
  find . -type f -print0 | xargs -0 python3 <仓库>/tools/check-private-data.py
  ```

  **2026-10-30 这一版核实的结果，记下来免得重做。**入口那一跑报了**三条**，`Readom.sh`、
  `launcher/helper`、`public-initramfs.cpio.gz` 各一条，值都一样（八个字节的乱码；那几个字节这里
  不照抄——下一版撞出的会是另一串，照抄只会让这份文档自己被扫描抓一次）。

  **三条其实是同一处字节，透过三层各报一次**，这一点用偏移算得上：那串字节在
  `public-initramfs.cpio.gz` 里的偏移是 275534，而这个 gzip 整个被嵌在 `launcher/helper` 的偏移
  3322336 处，两者相加正好等于它在 `helper` 里的偏移 3597870。`Readom.sh` 那一条则是工具自己
  解出来的——`check_file()` 对每个文件都跑一遍 `decoded_helper()`，把入口脚本里嵌的 `helper`
  还原再扫。（`helper` 同样整份嵌着 `zImage` 和 `screens.bin`，所以入口里任何一处压缩字节都会
  在三个文件里各报一次。）

  **它是 gzip 压缩产生的，不是内容里的**：把那个 cpio 解开、扫全部文件，这串字节一处都没有，
  真内容里只有三处 `appro@openssl.org`——OpenSSL 上游作者的地址，编进 AOSP 的 `init` 二进制里的
  第三方署名，公开的，不是使用者的数据。同一个 gzip 在之前五轮构建里都是 0 发现，只是压缩输出的
  字节不同。两层判据都过：黑名单 0 命中。

  **2026-10-31 重建入口那一跑又撞了一次，形状完全一样，所以这件事是常态、不是意外。**这次是
  **两条**（`launcher/helper` 偏移 3606832、`public-initramfs.cpio.gz` 偏移 275540，差值 3331292
  正是这个 gzip 嵌在 helper 里的位置），值同样是八个字节的乱码，周围 400 字节里只有三成可打印。
  **怎么判，一条就够**：把命中处前后各几十字节 dump 出来——真地址周围是可读文本，压缩噪声周围
  不是。**不要往 `tools/private-data-allow.txt` 里加**：那串字节每次构建都变，加进去只会让清单
  长出永远不再匹配的条目。入口确实不含设备数据这件事，靠的是回执里的 `contains_device_data: false`
  加上波形、校准、ATAGs、序列号、adb 公钥那几项单独的检查，不是靠这个正则。

## 发行密钥（2026-10-24 以 Readom 之名重新生成）

四把（`releasekey`/`platform`/`shared`/`media`，RSA 2048、SHA-256 证书、10000 天）已经用 `tools/make-release-keys.sh` 在构建机里生成，主题 `O=Readom`。私钥存在**仓库之外**，以只读方式交给构建环境，构建时由 `PW3_RELEASE_KEY_DIR` 指定位置。

**为什么换过一次**（2026-10-24）：项目改名为 Readom，而证书主题里写着旧名。换密钥的代价是升级那一次共用身份的系统包数据被清（系统设置回默认、多约 20 秒，第三方应用不受影响），所以这件事只在"还没有别人装"的时候做才便宜——当时全世界只有开发者这一台机器装着，而且还没登录任何账号。

**为什么不放在仓库里**（2026-10-23 搬出来的）：放在工作树里时，它不进提交靠的只是 `.gitignore`，而过滤器比位置弱——`git add -f`、把工作树打包、按目录整个拷走，三样都会带上它。`build-aosp-system.sh` 直接拒绝落在 `/port` 或 `/build-share` 下的密钥目录，也拒绝 AOSP 公开的 `testkey`（它的私钥是公开的，用它签出来的镜像谁都能替换，而构建本身不会察觉）。

**保管**：私钥静态加密存放，解密口令和私钥分开保管；另外至少要有一份离线副本，只放在云端的副本，拿到任一台登录设备的人就能恢复。

**轮换的代价**：换一把密钥，升级路径上带 `sharedUserId` 的系统应用会被判签名不符、数据被清。所以泄露时的处置是"发布新指纹 + 提供一次干净安装"，不是静默换钥。证书指纹是**公开**的，发行时写进 README，供人核对自己装的是不是这个项目签的：

| 密钥 | 证书 SHA-256 |
|---|---|
| releasekey | `0F:61:83:01:7D:E5:17:B7:E7:4E:BD:8D:90:03:F2:A7:64:4E:80:9E:16:85:DD:55:2D:8D:6D:AC:4E:CF:8C:C0` |
| platform | `7A:31:A4:EA:1B:24:31:80:C0:1F:5B:AB:7E:70:D9:BA:DF:2E:46:13:B5:3C:B4:6F:58:57:51:3B:C8:08:B4:8E` |
| shared | `FA:6A:B5:7A:BB:CF:8A:D3:6A:28:DF:A6:63:B3:B0:D4:73:75:91:D7:47:6A:91:A6:B5:67:1A:77:2D:8F:80:75` |
| media | `FB:CE:D1:B0:8E:06:90:59:A2:41:48:5C:F2:E9:58:C9:DD:D8:57:29:3A:F2:4C:D7:99:48:93:48:20:0C:D5:C6` |

**离线备份**：密钥丢了，就没法再为已发出的版本出新版本（用户只能换数据），所以必须拷一份到离线介质上。这件事构建替不了人做，发行清单第 2 步要求人来确认。

## 安全复核

发行前对这套系统做的复核（2026-10-17 起）：内核、SELinux 策略、系统镜像、本项目加的程序。
下面每条都写清楚**查了什么、结论、以及没做什么**。

### 一、内核来源

- 基线：Amazon 为 Kindle Paperwhite 3 公布的 `5.16.2.1.1` 源码包里的 `linux-3.0.35`。
- **5.16.2.1.1 是这个机型的最后一版固件**，Amazon 没有更新的源码可比对，所以"拿厂商新版本 diff 出他们修了什么"这条路走不通。
- 抽查过的两项：
  - Dirty COW（`CVE-2016-5195`）：Amazon 已修补，`mm/memory.c` 里有 `FOLL_COW` 的判断。
  - RFC 5961 的 challenge ACK：这版内核没有。所以 `CVE-2016-5696`（利用 challenge ACK 计数器做旁路推断）不适用；代价是也没有 RFC 5961 对盲注入的防护。要补就要改 TCP 接收路径的状态判断，属于对一个老协议栈的中等规模改动；内容本身由 HTTPS 保护，盲注入能做的主要是打断连接，因此**记为已知限制，不改**。

### 二、内核里拿掉的东西（2026-10-17 起）

SELinux 本来就拒绝这些接口，但"编译进去"和"根本没有"是两回事：

| 开关 | 变化 | 理由 |
|---|---|---|
| `CONFIG_DEVKMEM` | 关 | `/dev/kmem` 把内核虚拟内存当文件映射，本机没有任何东西用它 |
| `CONFIG_USER_NS` | 关 | Android 不创建用户命名空间；3.0 的实现本来也只做了一半 |
| `CONFIG_SYSCTL_SYSCALL` | 关 | 废弃的二进制 `sysctl()` 接口，bionic 不用 |
| `CONFIG_STRICT_DEVMEM` | 开 | `/dev/mem` 只剩设备寄存器（读 i.MX6 时钟寄存器的工具还能用），不再能读内存 |

Android 对新内核还要求 `CC_STACKPROTECTOR` 和 `DEBUG_RODATA`，**3.0 的 ARM 两者都没有**（`HAVE_CC_STACKPROTECTOR` 是后来才加到 `arch/arm` 的），所以做不到。

另外内核补丁 `1054` 补上了 802.11 的 hole-196 防护（同一无线网内任何人都能用共享的广播密钥伪造单播帧）和 ARP/NA 欺骗过滤，wpa_supplicant 熄屏时会打开它们。

### 三、SELinux

- 设备上实测：`getenforce` = `Enforcing`。
- 从运行中的内核导出策略，用 `sepolicy-analyze` 查：
  - 宽容（permissive）域只有一个：`su`。它只存在于 `userdebug` 构建，发行用的 `user` 构建不包含。
  - `unconfined_domain_type` 属性下没有任何域。
- 本项目自己写的策略（`device/amazon/pw3/sepolicy/*.te`）逐条看过：没有 `permissive`、没有通配，能力只给了三处且都带说明（`init` 的 `sys_module` 用于加载固定的驱动包、`healthd`/`pw3-init-check` 的 `wake_alarm`、`pw3-init-check` 的 `sys_admin` 仅因为 3.0 查 DM 表也要它，并且 ioctl 用 `allowxperm` 限定到两个命令号）。
- 每台设备私有的校准与波形文件有 `neverallow`：应用域和 shell 都不能打开。

### 四、系统镜像

在导出的 `system.raw.img` 上查（2026-10-17，`userdebug` 镜像）：

- 没有 world-writable 文件。
- 唯一的 setuid 文件是 `/system/xbin/su`（04750，`userdebug` 构建的标准做法）。**发行用的 `user` 构建里它不存在**，这一条写进发行检查表。
- `/system/xbin` 里还有 AOSP 的一批调试工具（`micro_bench`、`librank`、`procmem` 等），同样只在 `userdebug` 里。
- 可执行文件里没有本项目不认识的东西。

### 开机校验保证什么、不保证什么

入口从 Kindle 用户区上的镜像文件启动 Android（`pw3-testfile-init`），并不是每次开机都重算整镜像的 SHA‑256。第一次校验通过后，`/data/pw3/system-verified` 里会记下一条印记：镜像的大小、mtime、簇分配（extents），以及这个入口配置里的预期哈希。下次开机四项全同就跳过重算（`TestFileInit.h`、`SystemStamp.h`）。代价是 512 MiB ÷ 27.6 MiB/s ≈ 19 秒（按 576 MiB 的 20.9 秒实测外推，未重测），所以这条缓存值一次版本更新的等待。

印记只比对元数据，不重算内容，所以它本身答不了"内容在这三项不变的情况下被改了"。为此另有两道补充，都不花开机时间：

- **开机时**：查这块盘上有没有第二个东西声称拥有镜像的簇（`FatCrossLink.h`）。两种声称方式，彼此都看不见对方，所以两样都查：
  - **FAT 表**：链是链表，被两条链穿过的簇会被指向两次，被第二条链当作起点的簇会被指向一次（首簇本该没有任何表项指向）。
  - **目录项**：另一个目录项只要把首簇写成我们的某一簇，就从那里开始共用我们整条链，**FAT 表一个字节都不用改**。只有读目录才看得见。所以每次开机把整棵目录树走一遍，任何一个首簇落在我们簇里的条目都当场停机；我们自己那两个条目各应当恰好出现一次，多一个或少一个都停。
  这块盘 3.3 GB、8 KiB 簇，表 1.6 MB，按 eMMC 的 29 MB/s 约 60 ms，目录树再多几百 KB。交叉链接是"内容变了而大小、mtime、簇分配都没变"最现实的来源——拔盘中断的写入、猜错了的修复工具都会造出来——而且它还意味着往 `/data` 写会写进用户的书里，用户删掉那本书就会把 Android 正在跑的链释放掉。

  **这道检查覆盖不到安装那一次写**（2026-10-30 Codex 第五轮指出，已核实）。它跑在新内核的 preinit 里，也就是交接**之后**；而安装器是在原厂系统里跑的，它原地重写 `android/system.img`（`installer/unpack/unpack.c` 的 `open(O_WRONLY|O_CREAT)` 加 `write()`），并且**不读 FAT**——安装器里和 FAT 有关的只有簇大小的算术。所以**安装器即将覆盖的那段写入范围与一本书共用簇时**，书的内容会在安装阶段被覆盖，preinit 随后拒绝启动也已经晚了（范围就是这一段）。前提是一个已经损坏的文件系统（正常 FAT 上分配器不会把同一簇给两个文件），所以这不是"已复现丢书"，但**保护的边界要说清楚：安装阶段没有这道检查**。详见 `docs/open-items.md`。
- **开机之后**：跳过完整校验的那次开机会留下 `/pw3-system-recheck`（设备节点、长度、预期哈希）。`pw3-system-verify` 在 `boot_completed` 后等 90 秒，以最低调度和 I/O 优先级把整块 dm-1 读一遍，每读一块就从页缓存里丢掉。对得上就置 `sys.pw3.system_verified=1`；对不上就删掉印记，下次开机做完整校验、校验失败、拒绝启动，也就是安装器"系统镜像损坏"那张屏说的路径。

因此准确的说法是：

- **保证**：这一版镜像在本机至少完整校验过一次；开机当时，这块盘上没有第二条链、也没有第二个目录项声称拥有它的任何一簇。
- **不是保证的是"每次开机都复核"**（2026-10-30 Codex 复审指出）：后台复核在 `boot_completed` 之后才启动、再等 90 秒，用的是低 CPU 和低 I/O 优先级，没有完成时限；请求文件不在、读不了、解析失败，或者设备读取出错，它都可能不完成。正常情况下它会在开机后几分钟内读完整个镜像，但那是**实测的常态，不是代码的承诺**。
- **不保证**：从开机到后台复核读完的那一两分钟里，一个被改过的镜像正在运行。

对"有人在 Kindle 侧执行代码"这个威胁，这几条都不改变攻击面——他同样能换掉启动器本身，而启动器才是整条哈希链的锚。它们针对的是损坏。另一条路是移植 dm-verity 按块惰性校验，没有选：3.0.35 要回移驱动，而且本机 512 MB 内存下代码页反复被挤出再读回，每次读都要重算，摊下来比开机读一遍更贵。

### 五、本项目加的程序

- `WeReadInstaller`：只从腾讯官方地址下载（HTTPS，重定向链全程 HTTPS），装之前核对包名和腾讯的签名证书 SHA‑256。它持有 `INSTALL_PACKAGES`，由 `privapp-permissions` 文件明确授予。两条停下来的规则，各自管的事不同（2026-10-30 Codex 复审后写准）：
  - **装过一次就不自动再装**（`DONE` 这个闩）。使用者卸载之后，自动的那条路不会把它弄回来；但首页入口上的「现在下载」会**显式清掉这个闩**（`Installer.scheduleNow`）——那是使用者自己要求的，这条规则从来不是为了拦这个。
  - **下到的不是腾讯的，三次之后不再自动尝试**（`MAX_REJECTED`）。只数身份核对不通过的次数；网络或读写失败不计入，它们照常按十分钟起的退避重试。
- 电源 HAL 新加的写入只有一个文件（`scaling_min_freq`），有单独的 SELinux 类型，只有它能写。
- 合成器、gralloc、lights 等都不读写设备私有标识。
- 入口写在用户区的日志（`android/logs/<时间>/`）是会被当事人原样转交出去的东西，所以其中的**内核日志按允许清单筛**（`boot/launcher/userlog.c`）：原厂内核日志里确实带设备 MAC（本机 201 份快照里 40 处，其中 20 处是 `usb0: MAC …`），黑名单漏一种写法就泄露一次，因此只留本项目用到的子系统、内核出事的那些行，以及本项目自己打的 `pw3-` 开头的行（见 `userlog.c` 的注释：清单只列标签的话，后加的诊断会被默默丢掉——开发期间上机时真丢过，少的正好是内核补丁 1046 打的线缆与 pull-up 状态），其余一律不写；按前缀放行的代价是**我们自己的标签也要分类**：`pw3-boot-report` 进 `DENY`，先查 DENY 再查允许清单。它当初进 DENY 是因为会把 logcat 的告警与错误正文转进内核日志（正文是应用写的，可能有文件名、地址）；2026-10-29 起它只按固定的十三个标签名计数（`BootReport.cpp` 的 `Relevant()`），而它写的那个内核日志出口也随 `androidboot.pw3.diag` 一起去掉了，于是这个标签已经到不了任何内核日志。**它留在 DENY 里是因为不需要放进来，不是因为内容还私密**（`userlog.c` 的注释说了这件事）。`tools/tests/test_klog_tags.py` 保证树里每个 `pw3-` 标签都被分过类、新加的标签不分类就构建不过；筛完再查一遍 MAC、长十六进制串、`ssid`/`serial`/`command line`，中招的也不写。实测保留 10.2%，筛后 96,545 行里带标识符的为 0。测试入口不筛——那些目录只走数据线。

### 六、特权应用的权限白名单（查过，保持不启用）

Android 8.1 可以要求"特权应用拿 `signature|privileged` 权限必须在
`/system/etc/permissions/privapp-permissions-*.xml` 里列明"（`ro.control_privapp_permissions=enforce`）。
本机没有设这个属性，也就是不强制：没列明的照样授予，只记一行警告。

2026-10-19 把 `user` 镜像里 21 个特权应用的 APK 逐个解出请求的权限，与平台声明的 178 个
`signature|privileged` 权限和白名单对了一遍。**只有两处没被白名单覆盖，而且都是 AOSP 自己的疏漏**：

| 应用 | 权限 | 为什么 |
|---|---|---|
| `android.ext.services` | `PROVIDE_RESOLVER_RANKER_SERVICE` | 白名单里写的是 `com.google.android.ext.services`，而 AOSP 构建装的是 `android.ext.services`，包名对不上 |
| `com.android.systemui` | `com.android.permission.WRITE_EMBEDDED_SUBSCRIPTIONS` | eUICC 相关，本机没有基带 |

**不启用 `enforce`，理由**：这个白名单防的是"特权应用悄悄多拿权限"，而本机的特权应用要么是 AOSP 的、要么是本项目自己的，`/system` 只读、并且这一版镜像在本机至少完整校验过一次（不是每次启动都重算，见上面"开机校验保证什么、不保证什么"），第三方没有办法把自己变成特权应用。启用它换来的检测能力，用一次静态核对就能得到（上面那张表就是），而代价是运行时可能拒掉某个权限、让应用以说不清的方式坏掉。**检测放在构建侧，不放在运行时。**

要改的情况：哪天引入了不是我们写的特权应用，或者要按 CDD 送检。

### 七、每次发行前要核对的

1. 用 `user` 变体构建，确认：没有 `su`、没有 `/system/xbin` 的调试工具、`ro.debuggable=0`、adb 默认关闭。2026-10-19 的做法和结果见上文「`user` 构建演练」。
2. 在 `user` 构建上重跑本文第三、四节的检查。
3. GPL 源码包（`tools/collect-copyleft-sources.py`）与镜像一起发布。
4. `tools/check-image-references.py`：镜像里每个 init `.rc` 点名的程序都在、每个动态库都在。2026-10-19 的 `user` 镜像上 59 个程序、4311 处库引用，一个不缺。
5. 入口回执里 `"entry"` 是 `release`，命令行里只有 `boot/build_entry.py` 的 `CMDLINE_ANDROID` 列出的参数。测试入口和它的脚手架已经删除，`build_entry.py` 不再接受 `--test`；核对时确认命令行里没有任何 `androidboot.pw3.` 开头的参数。

### 八、使用者交出去的那个文件夹里有什么（2026-10-20 实测）

发行入口在每次点击时留下 `android/logs/<时间>/`，设计上它就是"出了问题把整个文件夹发出来"的那个东西。里面只有一半是被筛过的：内核日志走 `userlog.c` 的允许列表加标识符检查，报告区走标识符检查，而 **`launcher.log`（helper 的标准输出，由 shell 重定向）和 `write_result()` 写的那些记录是我们自己代码的自由文本，一个字都没筛**。安全靠的是"我们自己从不打印标识符"，这类不变量会随着加代码无声腐烂——`pw3-` 前缀当初就是这么漏的（见 `test_klog_tags.py`）。

所以查了两遍，都是实证不是推断：

1. **拿设备上真实取回的日志夹跑隐私检查**（`tools/check-private-data.py`，带本机的 denylist，也就是用使用者自己的序列号、MAC、网络名当判据）：两个文件夹、26 个文件，**0 条发现**。其中一个还是测试入口的文件夹，里面有一份**未经筛查的原始内核环形缓冲**（`previous-printk-buffer.txt`），同样干净——这与 `userlog.h` 记的"189 份日志里有 40 份带 MAC"不矛盾：那条规则防的就是会带的那些份，这一份恰好没带。
2. **逐条读设备数据的失败文本**（`devicedata.c` 里的每一处 `fail(why, ...)`，会被 `PW3_DEVICE_DATA %s` 原样写进 `launcher.log`）：全部是固定文字加路径、字节数、errno。没有一处把值插进去——板号和 MAC 那两条只说"不是 16 位字母数字"、"不是 12 位十六进制"，不说读到了什么（`devicedata.c:236`、`237`）。下载那行也只有 curl 返回码、HTTP 码和字节数，没有地址。

结论：2026-10-20 交出去的文件夹是干净的。这是一次实测，不是一个机制；真正的机制只有内核日志和报告区那两道闸，所以**新加任何写进那个文件夹的输出时，要么是固定文字，要么过一遍 `pw3_report_keep`**。

### 附：发行版日志里还会出现的报错，各自为什么留着

下面这几条发行版的日志里还会出现，都查清楚了，每条都有理由不改：

| 报错 | 原因 | 为什么不改 |
|---|---|---|
| `Netd: Error destroying sockets` | `SOCK_DESTROY` 是 Linux 4.5 才有的；网络断开时 netd 用它立刻关掉绑在该网络上的连接 | 没有它，连接要等自己超时（最坏十几分钟）而不是立刻失败。回移要动 `inet_diag` 和 TCP 的中止路径，是对一个老协议栈的中等规模改动；读书设备很少切换网络，先记为已知 |
| `SupplicantStaIfaceHal: setSuspendModeEnabled / setBtCoexistenceMode / stopRxFilter failed` | ath6kl 的 supplicant 不支持这三条命令 | 蓝牙本机没有；熄屏省电本机用的是"熄屏约 60 秒后卸载 Wi‑Fi 驱动"，比 suspend 模式更彻底；接收过滤由内核补丁 1054 的四个开关在 IP 层完成 |
| `UsbPortManager: usb hal service not found` | 没有 `android.hardware.usb` HAL | 这个 HAL 管的是 USB‑C 的角色切换（主机/设备、供电方向），本机是 micro‑USB 只做设备，装一个 HAL 进程只为消掉三行日志不值得 |
| `hw-IPCThreadState: attemptIncStrongHandle: Not supported` | HIDL 直通模式下的已知行为 | AOSP 自身的实现细节，不是本机问题 |
| `wificond: No Offload Service available` | 没有 Wi‑Fi 卸载（offload）HAL，芯片也不支持 | 卸载 HAL 是让芯片在主机睡眠时自己做扫描；ath6kl 没有这个能力 |
| `avc: denied { getattr } ... mediaprovider ... cache_recovery_file / cache_private_backup_file` | 下载管理器的空闲维护（`DownloadIdleService`，日志里紧挨着的上一行是 `Removed 0 stale downloads`）遍历下载缓存目录，对 `/data/cache/recovery` 和 `/data/cache/backup` 调 `stat`。这两个目录属于恢复分区日志和备份暂存，各有自己的 SELinux 标签，`mediaprovider` 读不到 | 不改。两个目录里不可能有下载文件，跳过它们不丢任何东西；为消三行日志而放宽策略，或加 `dontaudit` 把它藏起来，都会让以后真正的拒绝更难发现。每次空闲维护记三行，2026-10-19 实测全天仅此一次 |

另有一条 `iptables-restore: unable to initialize table 'nat'` 不会再出现：netd 每次都要建 IPv4 nat 表，原厂内核配置没有它，现在内核配置（`config/android-core.config`）加了 `CONFIG_NF_NAT` 和 `CONFIG_IP_NF_TARGET_MASQUERADE`（2026-10-17）。

## 系统自带的两份"会过期的数据"：根证书与时区规则

2026-10-16 调研并生成。2026-10-19 在镜像上核对：`/system/usr/share/zoneinfo/tzdata` 是 `tzdata2026e`（原来是 2017b），`/system/etc/security/cacerts` 正好 143 个根证书，与本文的预期一致。下文那 10 个时区的时刻尚未在真机上逐条复核：数据在镜像里是事实，显示正确是推论。

Android 8.1 自带的根证书停在 2017 年，时区规则是 tzdata2017b。这两份数据不随代码老化，随时间过期：
- 根证书：过期的、被撤销信任的根仍被信任；新签发的根不认。
- 时区：各国改了夏令时和时区规则，本机仍按 2017 年的算，显示的本地时间差一小时。

两者都是运行时读取的数据文件，换掉不动启动镜像，已装应用不用重编。

### 根证书

**8.1 自带的**：`/system/etc/security/cacerts` 有 135 个根，属于 2017 年。和 Android 当前的列表比：
- 缺 72 个，如 GTS Root R1–R4（Google）、ISRG Root X2（Let's Encrypt）、Microsoft 2017、Sectigo R46/E46、DigiCert G5，以及 BJCA、GDCA、TrustAsia、UCA、vTrus 等国内 CA；
- 多 64 个已被 Android 去掉的，包括被撤销信任的 Symantec 系（VeriSign、GeoTrust、thawte）、TÜRKTRUST、E-Tugra、Certinomis。

**影响**：
- 用 OpenSSL 按这套根验证 31 个常用网站的证书链，全部能通过，包括微信读书、百度、知乎、B 站、GitHub、Google、微软、维基、Let's Encrypt、MobileRead。原因是网站为兼容老 Android 会带交叉签名。所以眼下不是"打不开网页"。
- 问题在两头：一是被撤销信任的根仍有效，属于安全风险；二是随着老根到期（如 GlobalSign R1 在 2028 年）、网站撤掉交叉签名，2017 年的列表会开始验证失败。要长期使用、开源发行，应跟随 Android 当前的列表。

**做法**：`patches/aosp-ca-certificates/0001`，由 `tools/sync-ca-certificates.py` 生成。
- 来源：`system/ca-certificates` 最新的公开更新（`c5434e0a8490`，"Update certificates to NSS_3_112_RTM"，android16-qpr2-release，143 个根）。
- 固定：脚本固定该提交，并对内容做 SHA-256 校验（按文件名排序的清单），不依赖 Gitiles 打包方式。
- 兼容 8.1：文件名是证书的 OpenSSL `subject_hash_old`，8.1 的 Conscrypt `TrustedCertificateStore` 就按这个查；143 个文件名逐一和证书核对过，没有冲突；文件格式与 8.1 相同（PEM 加文本说明）。
- 新列表下，31 个网站同样全部验证通过。
- 构建注意：删掉的证书文件会残留在 `out/` 的暂存目录里（实测 207 = 143 + 64 个旧文件），所以换证书的那次构建要带 `PW3_AOSP_INSTALLCLEAN=1`，导出后核对镜像里正好 143 个。

### 时区规则

**8.1 自带的**：bionic 和 libcore 读的 `/system/usr/share/zoneinfo/tzdata`，以及 ICU 的 `icudt58l.dat`，都是 2017b。在设备上（装的还是 2017b 数据的开发构建）用 `app_process` 跑一个核对程序，同时读 libcore 和 ICU。10 个用例里 7 个错一小时：

| 时区 | 时刻（UTC 12:00） | 本机（2017b） | 应为 |
|---|---|---|---|
| America/Sao_Paulo | 2026-01-15 | −2 h | −3 h（2019 年取消夏令时） |
| America/Mexico_City | 2026-07-15 | −5 h | −6 h（2022 年取消） |
| Asia/Almaty | 2026-07-15 | +6 h | +5 h（2024 年统一） |
| America/Asuncion | 2026-07-15 | −4 h | −3 h（2024 年） |
| Africa/Cairo | 2026-07-15 | +2 h | +3 h（2023 年恢复夏令时） |
| Asia/Amman | 2026-01-15 | +2 h | +3 h（2022 年） |
| America/Godthab | 2026-07-15 | −2 h | −1 h（2023 年） |
| Asia/Shanghai、Europe/Dublin、America/New_York | — | 正确 | 不变（对照） |

中国的时区不受影响；在上表这些国家和地区的用户会看到错一小时的时间。

**做法**：`patches/aosp-timezone/0001`、`patches/aosp-icu/0001`，由 `tools/update-timezone-data.py` 在构建机里生成，沿用 8.1 自己的 `system/timezone/update-tzdata.py` 的流程，输入换成今天的工具能处理的形式：
- **IANA tzdb 2026e**：用 Paul Eggert 的签名密钥（`7E3792A9…62AA7E34`）验签，脚本固定 SHA-256。
- **bionic、libcore**：
  - 用同一版本自带的 zic，生成 rearguard 形式（夏令时不取负值，8.1 的代码按这个假设写），含全部 backzone，与 8.1 一致；
  - `zic -b fat`：8.1 的 libcore 只读每个时区的 32 位段；
  - 最后由 8.1 自带的 ZoneCompactor 打包。
- **ICU**：
  - 用 ICU 为 2026e 发布的时区资源（unicode-org/icu-data `bae5ca64`，`tzdata/icunew/2026e/44/le` 的四个 `.res`；44 格式适用于 ICU 4.4 及以后），固定 git blob 校验值；
  - 用 8.1 源码里 ICU 58 的 `icupkg` 换进 `icudt58l.dat`。这是 ICU 用户指南里"更新时区数据"的做法。
  - 构建 `icupkg` 时关掉 ICU 58 对 `xlocale.h` 的依赖（glibc 2.26 起没有，ICU 60 才修）。这只影响主机上的工具，设备上的 ICU 不变。
- **生成时的检查**：
  - tzdata 头是 `tzdata2026e`；
  - 8.1 原有的每个时区 ID 都还在。唯一例外是 IANA 2017c 删掉的别名 `Canada/East-Saskatchewan`，此后所有 Android 都没有它，8.1 的设置里也不提供；
  - `icudt58l.dat` 的条目清单不变，换进去的四个资源逐字节等于输入。
- **不重新生成的**：
  - `tzlookup.xml`：8.1 手工维护，只给电话网络按国家选时区用，本机没有电话功能；
  - ICU 的文本源文件：构建只用打包好的 `.dat`；
  - 时区更新包（distro）相关文件：本机没有安装更新包的组件。

### 顺带核对过、不改的两项

**Wi‑Fi 管制域**：
- Wi‑Fi 关着时，cfg80211 是世界默认域 `00`；在那个域里 12、13 信道只能被动扫描。
- Wi‑Fi 起来后，内核日志是 `Regulatory domain changed to country: CN`：无线芯片 EEPROM 里的地区生效，12、13 信道按中国规则开放，和原厂一致。
- 8.1 的国家码本来取自 SIM 卡的移动国家码，本机没有电话功能，`dumpsys wifi` 显示 "CountryCode was not initialized"。但上面这一条已经覆盖，不需要另设。

**浏览器内核（WebView）**：
- 现为 AOSP 官方预编译的 128.0.6613.88。要求是用 AOSP 官方发布的预编译版本（2026-10-13 定）。
- 2026-10-16 查 `external/chromium-webview`：main 上最新的仍是 128.0.6613.88（2024-10）。当时据此写的是「之后官方直接跳到 140、141（android16-qpr2-release，2025-08），而 Chromium 从 139 起要求 Android 10，所以 128 仍是 8.1 能用的最新官方版本，决定不变」。
- **2026-11-04 重查，这个前提不成立。** 同一个仓库的 `android16-qpr2-release` 分支上，128 之后、140 之前还有 133、134、136、138 的官方预编译。8.1 能装的最新一版是 **138.0.7204.67**：提交 `8bf71e6228`（2025-07-17），提交说明写着 "This build IS suitable for preview or public release"，arm、arm64、x86、x86_64 四个 APK 都是 `sdkVersion=26`（minSdk 26，Android 8.0 起）；到 140.0.7339.4（2025-08-06）才变成 `sdkVersion=29`，要 Android 10。所以 Chromium 138 是支持 Android 8/9 的最后一版，这一点没错，错的是「AOSP 没发布过」。核法：读 Gitiles 上 `+log/refs/heads/android16-qpr2-release?format=JSON` 的提交日志，再读每个提交的说明（`+/<提交>?format=JSON`）。**「决定不变」的依据因此没了，换不换要使用者重新定**；定之前镜像仍带 128。138 在这台机器上能不能跑（3.0.35 内核、没有 GPU、512 MB）没试过。
- 已知的剩余风险：129–138 修掉的 V8 等漏洞（其中有在野利用的，如 CVE-2025-6554）在 128 上仍在。原来这里接着写「唯一更新的选择是 LineageOS 自编的 138，不属于官方预编译」，不对：AOSP 自己发布过 138.0.7204.67（上一条）。它也补不全：CVE-2025-6554 是在 138.0.7204.96 修的，按版本号 .67 在它之前，这几版各带了哪些修复没有逐条核。
