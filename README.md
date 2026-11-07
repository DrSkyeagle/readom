<h1 align="center">
  <a href="https://readom.app">
    <picture>
      <source media="(prefers-color-scheme: dark)" srcset="docs/img/wordmark-reverse.svg">
      <img src="docs/img/wordmark.svg" alt="Readom" width="280">
    </picture>
  </a>
</h1>

<p align="center"><b>唤醒你的 Kindle。</b><br>把 Android 装进 Kindle，点亮一个全新的阅读世界。</p>

<br>

<!-- 链接指 /releases 而不是 /releases/latest：GitHub 的 latest 只认正式版，某一版先以预发行
     出来时它还指着上一个正式版（一个正式版都没有时是 404）。徽章带 include_prereleases，预发行也算。 -->
<p align="center">
  <a href="https://github.com/hbyxabc/readom/releases"><img src="https://img.shields.io/github/v/release/hbyxabc/readom?include_prereleases&label=%E7%89%88%E6%9C%AC" alt="最新版本"></a>
  <a href="https://github.com/hbyxabc/readom/actions/workflows/tests.yml"><img src="https://github.com/hbyxabc/readom/actions/workflows/tests.yml/badge.svg" alt="测试"></a>
  <a href="#许可"><img src="https://img.shields.io/badge/%E8%AE%B8%E5%8F%AF-Apache--2.0%20%2F%20GPL--2.0-blue" alt="许可：Apache-2.0 / GPL-2.0"></a>
</p>

<p align="center">
  <a href="https://readom.app/#pick">官网下载</a> ·
  <a href="https://readom.app/#guide">安装教程</a> ·
  <a href="https://github.com/hbyxabc/readom/releases">GitHub Releases</a> ·
  <a href="README.en.md">English</a>
</p>

<br>

<p align="center"><a href="https://readom.app"><img src="docs/img/hero.jpg" alt="Readom 在 Kindle Paperwhite 3 上的首页" width="340"></a></p>

<br>

<p align="center">演示、下载和图文安装教程都在官网：<b><a href="https://readom.app">readom.app</a></b><br>暂时只支持 Kindle Paperwhite 3（第 7 代），同时保留 Kindle 原厂系统。</p>

<br>

## 最新进展

<!-- 每次发行时更新：版本号、发布日期、这一版做了什么。发布当天把「待发布」换成日期。 -->

| 版本 | 发布日期 | 做了什么 |
|---|---|---|
| v0.1.0 | 2026‑11‑04 | 第一个公开版本：把 Android 8.1 适配到 Kindle Paperwhite 3，带上微信读书一键安装、EinkBro 浏览器、拼音输入法和休眠画面，在 Kindle 上就能安装和升级 |

每一版做了什么、在设备上验到哪里，完整的一份在 [CHANGELOG.md](CHANGELOG.md)；官网的[更新日志](https://readom.app/#log)和 [GitHub Releases](https://github.com/hbyxabc/readom/releases) 都从它来。

## Readom 是什么

<img src="docs/img/why.webp" alt="独角兽吹掉抽屉里一台旧阅读器上的灰，屏幕亮了起来" width="100%">

Readom 是为 Kindle 定制的 Android 系统。它把 Android 8.1（AOSP）适配到 Kindle Paperwhite 3 上，按墨水屏和这台机器的硬件重做了刷新、界面、输入法和安装方式。装上之后，Kindle 能用浏览器上网、装 Android 应用、用官方墨水屏版微信读书看书；原来的 Kindle 系统还在，随时切得回去。

名字是 Read 和 Freedom 拼起来的：读什么书、用哪个应用、在哪台设备上读，由你自己决定。

做它，起因是我想在 Kindle 上看正版的微信读书，不想一本本导书，也不想找盗版。越狱后试过 KOReader 加微信读书插件，体验很差；腾讯有专门的墨水屏版，但要 Android 7.0 以上，Kindle 上现有的安卓方案都太老。

于是我一个人，花了几周的 Token 用量，把一个完整的 Android 系统适配到了 Kindle 上。我相信在 AI 时代，积了灰的旧设备都有机会重新用起来，Readom 先从这台 Kindle 开始。

## Readom 的特点

<!-- 这三张是 2026-11-02 在设备上一口气拍的，和官网那十张同一批（原始 PNG 在 readom-site 的
     _lab/shots/reshoot-2026-10-28/）。都按 device/amazon/pw3/epdc/ 的 Luma.h 与 Tone.cpp
     转成了面板实际显示的 16 级灰（readom-site/website/tools/panelise.py），状态栏左上角那两个
     只有连着电脑才有的图标（下载、adb）已抹掉。要重拍就照那边的作业单走。 -->

<p align="center">
  <img src="docs/img/shot-home.webp" width="32%" alt="Readom 首页：搜索框，微信读书、浏览器、文件、设置、全部应用五个图标">
  <img src="docs/img/shot-browser.webp" width="32%" alt="浏览器里打开的微信读书网页版主页">
  <img src="docs/img/shot-reader.webp" width="32%" alt="微信读书里《论语·为政》的正文页">
</p>

<sub>截图里的微信读书界面和内容归腾讯所有，这里只用来展示它在 Readom 上的实际效果。</sub>

### 功能

- **一键安装微信读书**<br>
  连上 Wi‑Fi 后，系统从腾讯官方地址下载墨水屏版，核对腾讯的签名再安装，不用电脑。

- **升级网页浏览**<br>
  预装为墨水屏做的开源浏览器 EinkBro，网页内核从 Android 8.1 自带的 Chromium 61（2017 年）升到 Chromium 128（2024 年）。AOSP 后来还发布过仍支持 Android 8 的官方版本，最高到 Chromium 138（2025 年）；这一版保持 128。

- **拼音首选更准**<br>
  用 rime-ice 把词库从 6.5 万条扩到 14 万多条，同一组 110 个测试输入里，第一个候选就对的从 84 个升到 97 个；界面为墨水屏重画。

- **完美适配原厂硬件**<br>
  前光、电源键、电量、Wi‑Fi、USB 传文件都能用；合上磁吸保护套自动休眠，打开就唤醒。

- **随时回到 Kindle**<br>
  长按电源键，选「重启回 Kindle 系统」；系统反复出错时也会自己退回 Kindle，不清你的数据。

- **休眠画面**<br>
  熄屏时换成一幅独角兽读书的画。先是为了看着舒服，让人每次都更愿意拿起 Kindle；也顺带遮住了刚才读的内容，墨水屏断电后画面不会自己消失。画的主题来自我很喜欢的一句话：「读书无禁区」。

<p align="center"><img src="docs/img/sleep-zh.webp" width="300" alt="休眠画面：一只独角兽趴在地球上看书，四周飞着书，上面写着「读书无禁区」"></p>

### 性能

- **装应用快了十几倍**<br>
  重调了应用的编译方式，装微信读书这样的大应用，从二十多分钟降到一分半左右。

- **为 512 MB 内存精简**<br>
  从 AOSP 最小的产品配置起步，打开 Android Go 的低内存模式，不带电话、相机这类用不上的应用，没有的硬件对应的设置项也都去掉；字体只留中英文，从 173 个文件 43 MB 减到 37 个 33 MB，每次开机要加载的字体组从 116 组减到 15 组。

- **升级只要 5 分钟**<br>
  升级只替换系统镜像，已装的应用和数据原样保留；首次安装约 15 分钟，之后每次升级约 5 分钟。

### 界面

- **按墨水屏重画**<br>
  用黑色细线代替阴影和渐变，对话框背后不压暗，去掉所有动画。

- **翻页不黑闪**<br>
  用不闪的刷新方式翻页，每 20 页才整屏清一次残影。

- **页面更白更干净**<br>
  灰阶按阅读研究和这块屏的实测重新定，11 个常用页面里，纯白占到 92% 到 100%。

- **常用的都在手边**<br>
  首页一屏放下常用入口，上滑打开全部应用，搜索框直接搜网页；下拉一次就能调前光。

## 安装 Readom

| 条件 | 怎么查 |
|---|---|
| Kindle Paperwhite 3（第 7 代，2015 年） | 设置 → 设备选项 → 设备信息 |
| 系统版本 5.16.2.1.1 | 同上。版本更早的，先按安装说明升级 |
| 已越狱，且书库能显示 `.sh` 小程序 | 按 [kindlemodding.org](https://kindlemodding.org/jailbreaking/) 的教程，5.16.2.1.1 首选 WinterBreak2。越狱过、书库却显示不了或运行不了 `.sh` 的，怎么确认、找谁处理，见[安装说明](docs/install.md#越狱里有没有-sh_integration)。越狱不属于本项目，请自行判断风险 |
| 2.6 GB 空闲空间，电量 40% 以上 | 接电脑看 Kindle 盘 |

机型或系统版本不对时，安装器会显示 E101 并停下，不改动任何文件。

1. 下载 `Readom.sh`（约 8 MB）：官网 [readom.app](https://readom.app/#pick) 和 [GitHub Releases](https://github.com/hbyxabc/readom/releases) 上是同一份文件。

2. 用数据线连电脑，把它拷进 Kindle 盘的 `documents` 文件夹，弹出后拔线。

3. 在 Kindle 书库里点「Readom」。

4. 检查、下载、校验、写入都在 Kindle 上完成。首次安装约 15 分钟，装完自动进入 Android。

安装器自己带着官网和 GitHub 两个下载地址，装的时候依次试。两条都下不动的话，在电脑上多下一个离线包（两处也都有），和 `Readom.sh` 一起拷进去。

官网上有图文版的安装教程，也可以让 AI 带你一步步装：[readom.app/#guide](https://readom.app/#guide)。升级、离线包、出错代码，都在[安装说明](docs/install.md)里；拷进去后书库里找不到「Readom」，或者装完 KUAL 用不了，也先看那里的[书库里找不到 Readom](docs/install.md#书库里找不到-readom)和[装完后 KUAL 用不了](docs/install.md#装完后-kual-用不了)。

### 回到 Kindle、卸载

- **回到 Kindle**<br>
  长按电源键，选「重启回 Kindle 系统」。想再进 Android，就在书库里点「Readom」。

- **卡住了**<br>
  按住电源键不放，直到屏幕变化（最多约 40 秒），就会回到 Kindle，数据不会丢。

- **卸载**<br>
  删掉 Kindle 盘上的 `android` 文件夹和 `documents/Readom.sh`。原厂系统和你的书都不受影响。

## 构建与移植

### 技术架构

Readom 不刷机，整套系统由下面几块拼起来：

- **借原厂系统启动**<br>
  越狱后，书库里多一个「Readom」条目。点它时，原厂内核先加载我们的交接模块，由它把 Readom 的内核和启动参数装进内存，再跳过去。不写分区、不改引导，所以随时能回到原厂系统。

- **内核**<br>
  在亚马逊公开的 Kindle GPL 源码（Linux 3.0.35）上打补丁，补上 Android 8.1 需要的 binder、ashmem、SELinux 等，并修好 USB、电源和屏幕驱动。

- **系统是两个镜像文件**<br>
  `system.img` 和 `data.img` 放在 Kindle 盘上，开机时由 Readom 自己的启动程序找到、校验并挂载。

- **Android 8.1（AOSP）**<br>
  一个设备目录 `device/amazon/pw3/`，加上对 AOSP 各项目的补丁系列。

- **墨水屏显示**<br>
  没有 GPU，界面由 CPU 合成；自己写的显示部分决定刷哪一块、用哪种波形、16 级灰阶怎么映射，直接驱动 i.MX 的 EPDC 屏幕控制器。

- **设备专属数据不打包**<br>
  屏幕波形、Wi‑Fi 校准、MAC 这些每台都不一样的东西，每次启动时从这台机器的原厂系统现场读取。

- **安装在 Kindle 上完成**<br>
  安装器检查机型和空间，下载、按 SHA-256 校验、写入镜像，断电后能接着装。

构建环境和步骤见 [CONTRIBUTING.md](CONTRIBUTING.md)，各部分的设计记录见 [docs/README.md](docs/README.md)。

### 移植到其他 Kindle

<img src="docs/img/porting.webp" alt="独角兽用角点亮一排不同年代的阅读器，后面几台还蒙着灰" width="100%">

Readom 现在只跑在 PW3 上，但大部分代码和机型无关：内核补丁里约三分之二不碰硬件代码，AOSP 一侧的改动大多能直接沿用，启动交接按「机型 + 固件」各写一份配置。换一台 Kindle，要重做的是内核里碰硬件的那些补丁、一份启动配置和一个设备目录，再在真机上重新测一遍。

| 机型 | 离 PW3 多远 |
|---|---|
| Voyage | 最近：亚马逊给它和 PW3 用同一份内核配置，内核已经能编出来 |
| Oasis 1 | 内核已经能编出来，差 Wi‑Fi 驱动 |
| Kindle 8、PW4 等 | 要换内核，显示部分能复用 |
| PW5、Kindle 11 代、Scribe 等联发科机型 | 显示、Wi‑Fi、电源都要重写 |

具体要做什么，见[移植到其他机型](docs/design/porting-to-other-models.md)。

## 许可

- 本项目的代码和文档：Apache-2.0，全文在顶层 [LICENSE](LICENSE)。

- 内核、内核补丁，以及 `boot/` 下交接启动的那一整条链和内核配置（`config/android-core.config`）：GPL-2.0-only。这是概述，`boot/` 里个别文件另有许可（例如 `boot/handoff/sha256.c` 是公有领域）。

- 逐个文件以它自己的 SPDX 标记和 `REUSE.toml` 为准（本仓库遵循 [REUSE](https://reuse.software/) 规范，许可全文在 `LICENSES/`）。

- 预装的第三方组件（EinkBro、Chromium WebView、拼音词库等）沿用各自的许可，清单见 [THIRD_PARTY.md](THIRD_PARTY.md)。

- 仓库和发行包都不含亚马逊的私有文件（屏幕波形、Wi‑Fi 校准、原厂私有模块）。它们只在你自己的设备上读取和使用。

<!-- 发行时在这里写发行证书的 SHA-256 指纹，供使用者核对。 -->

Readom 的名字和标志归本项目所有，Readom 是非官方项目，与 Amazon、Google、腾讯均无关联。

### 致谢

Readom 建立在这些项目和社区的工作上：

- **系统**：[AOSP](https://source.android.com/)、亚马逊公开的 Kindle GPL 源码

- **浏览与输入**：[EinkBro](https://github.com/plateaukao/einkbro)、[Chromium](https://www.chromium.org/)、[rime-ice](https://github.com/iDvel/rime-ice)（拼音词库）

- **图标与字体**：[Material Symbols](https://github.com/google/material-design-icons)（首页图标）、[Noto Emoji](https://github.com/googlefonts/noto-emoji)、[霞鹜文楷](https://github.com/lxgw/LxgwWenKai)（休眠画面的标题）

- **社区**：[贡献者公约](https://www.contributor-covenant.org/)、[KindleModding](https://kindlemodding.org/) 和整个 Kindle 越狱社区

## 一起唤醒吃灰的 Kindle

<img src="docs/img/star.webp" alt="独角兽把一颗金色的星推进 Star 按钮" width="100%">

如果 Readom 让你的 Kindle 重新用起来了，请给它点个 Star，也告诉身边有 Kindle 的朋友。

想一起做，可以从这几处开始：

- **用的时候遇到问题**<br>
  先看看[已知问题](docs/known-issues.md)，没有的话开一个 [Issue](https://github.com/hbyxabc/readom/issues)，写上「设置 > 关于平板电脑 > 版本号」那一行。

- **有想法或疑问**<br>
  到 [Discussions](https://github.com/hbyxabc/readom/discussions) 里聊。

- **想改代码**<br>
  从 [Issues](https://github.com/hbyxabc/readom/issues) 里挑一条，标着 `good first issue` 的适合上手；读一下 [CONTRIBUTING.md](CONTRIBUTING.md)，然后提 Pull Request。

- **手里有别的 Kindle**<br>
  fork 这个仓库，照着[移植文档](docs/design/porting-to-other-models.md)把它移植到你的设备上，发布你自己的版本；做稳定了，欢迎合回来，让更多人用上。

- **安全问题**<br>
  见 [SECURITY.md](SECURITY.md)。

在这里交流和贡献，请遵守[行为准则](CODE_OF_CONDUCT.md)。
