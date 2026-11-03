# 第三方组件

发行物里每个第三方组件的版本、许可和源码位置。版本以 `source-lock.json` 和 `device/amazon/pw3/apps/prebuilts.json` 为准；仓库内文件的许可见 `REUSE.toml`，CI 每次提交都跑 `reuse lint` 核对。英文版：`THIRD_PARTY.en.md`。

## 系统镜像（`readom-system-<版本>.img.zst`）

| 组件 | 版本 | 许可 | 源码 |
|---|---|---|---|
| Android 开源项目（AOSP） | `android-8.1.0_r81` | 以 Apache-2.0 为主，各项目以自己的 `NOTICE` 为准 | https://android.googlesource.com ，加上 `patches/aosp-*` |
| 内核模块（随镜像安装在 `/vendor/lib/modules`） | Lab126 Linux 3.0.35（固件 5.16.2.1.1 的 GPL 源码） | GPL-2.0-only | Amazon 的 GPL 源码包，加上 `patches/android-core` |
| EinkBro | 16.7.1，未修改 | GPL-3.0-or-later | https://github.com/plateaukao/einkbro ，标签 `v16.7.1` |
| Android System WebView（Chromium） | 128.0.6613.88，未修改的 AOSP 预编译件 | BSD-3-Clause 等；Blink 为 LGPL-2.1 | Chromium 128.0.6613.88；LGPL 部分随发行提供完整源码 |
| Noto Emoji（单色，装在 `/system/fonts`，替代 AOSP 的彩色版） | google/fonts `8b0a1d0f` 的 `ofl/notoemoji/NotoEmoji[wght].ttf`，未修改 | OFL-1.1 | https://github.com/google/fonts ；`device/amazon/pw3/emoji/PW3-SOURCE.md` |
| 拼音输入法（AOSP PinyinIME） | `49aebad1`，已修改 | Apache-2.0 | `device/amazon/pw3/apps/PinyinIME` |
| 拼音词库 | 由 AOSP 词表和 rime-ice `da1fbe60` 生成 | GPL-3.0-only | https://github.com/iDvel/rime-ice ；生成方法 `tools/pinyin-dict/` |
| SwiftShader / Subzero | AOSP 8.1 自带 | Apache-2.0；Subzero 为 NCSA | AOSP `external/swiftshader`，加上 `patches/aosp-swiftshader` |
| 休眠画面插图 | GPT 图像模型生成（2026-10-15） | Apache-2.0，随本项目；生成结果的权利归使用者（OpenAI 使用条款） | `device/amazon/pw3/sleep/` |
| 首页五个图标（Material Symbols） | `@material-symbols/svg-400` 0.37.0 的五个实心图标，未修改；放进黑色圆角底块转成 VectorDrawable | Apache-2.0 | https://registry.npmjs.org/@material-symbols/svg-400 ；`device/amazon/pw3/launcher-icons/PW3-SOURCE.md` |

镜像里"设置 > 关于 > 法律信息"列出每个文件的许可。

## 入口（`Readom.sh`）

| 组件 | 版本 | 许可 | 源码 |
|---|---|---|---|
| Linux 内核（`zImage`） | Lab126 3.0.35 + 本项目补丁 | GPL-2.0-only | 同上 |
| 交接模块、启动程序、辅助程序、安装器 | 本项目 | GPL-2.0-only | `boot/`、`installer/` |
| 公开 initramfs 里的 AOSP 程序（`init` 等） | `android-8.1.0_r81` | Apache-2.0 | AOSP |
| musl（静态链接进辅助程序和 `pw3-unpack`） | 1.2.5 | MIT | https://musl.libc.org/releases/musl-1.2.5.tar.gz |
| zstd 解压库 | 1.5.6 | BSD-3-Clause 或 GPL-2.0-only（本项目按 BSD-3-Clause 使用） | https://github.com/facebook/zstd/releases/tag/v1.5.6 |
| SHA-256 实现 | Brad Conte | 公有领域 | https://github.com/B-Con/crypto-algorithms |
| Mozilla 根证书库（curl 提取版） | 2026-09-25 | MPL-2.0 | https://curl.se/ca/cacert-2026-09-25.pem （文件本身即源码形式） |
| 安装画面里的文字位图 | 由 Noto Sans CJK SC 和 Roboto 渲染 | 字体为 OFL-1.1 和 Apache-2.0；发行物里只有渲染结果，没有字体文件 | AOSP `external/noto-fonts`、`external/roboto-fonts` |

## 系统镜像里的 GPL/LGPL 组件

从 V40 系统镜像的 `/etc/NOTICE.html.gz` 解析（2026-10-15，121 个许可块）。发行时要随附或书面提供它们的完整对应源码。

| 文件 | 来源（AOSP 项目） | 许可 | 本机是否需要 |
|---|---|---|---|
| `/kernel`、`/vendor/lib/modules/*.ko` | Lab126 内核 + `patches/android-core` | GPL-2.0-only | 需要 |
| `iptables`、`ip6tables` | `external/iptables` | GPL-2.0 | 需要（netd） |
| `ip`、`ss`、`tc` 等 | `external/iproute2` | GPL-2.0 | 需要 |
| `e2fsck`、`mke2fs`、`blkid`、`e2fsdroid` 等 | `external/e2fsprogs` | GPL-2.0（库为 LGPL/BSD） | 需要（ext4） |
| `libnl` | `external/libnl` | LGPL-2.1 | 需要 |
| `libselinux` | `external/selinux` | 库本身为公有领域；项目的 NOTICE 含其他部分的 GPL 文本，按 GPL 处理一并附源码 | 需要 |
| `libexif` | `external/libexif` | LGPL-2.1 | 需要（相册、图片元数据） |
| `libcap` | `external/libcap` | BSD 或 GPL-2.0 | 需要 |
| `core-oj.jar`、`libopenjdk.so`、`libopenjdkjvm*.so` | `libcore`（ojluni）、`art/openjdkjvm*` | GPL-2.0，附 Classpath 例外 | 需要 |
| `dnsmasq` | `external/dnsmasq` | GPL-2.0 | 用不到：只给热点和网络共享用，本机已关热点（`config_wifi_softap_supported=false`），不会运行 |
| `fsck.f2fs`、`make_f2fs` | `external/f2fs-tools` | GPL-2.0 | 用不到：本机只用 ext4，不会运行 |
| `sgdisk` | `external/gptfdisk` | GPL-2.0 | 用不到：只给外接存储分区，本机没有卡槽，不会运行 |
| `mmc_utils` | `external/mmc-utils` | GPL-2.0 | 调试工具，标记为 `debug`，发行用的 `user` 构建不带 |
| `gdbserver` | `prebuilts/misc`（源码为 GDB） | GPL | 调试工具，标记为 `debug`，`user` 构建不带（I8） |
| `libfec_rs.a` | `external/fec` | LGPL-2.1 | 静态库，列在 NOTICE 中 |
| WebView、EinkBro、拼音词库 | 见上文 | LGPL-2.1、GPL-3.0-or-later、GPL-3.0-only | 需要 |

**"用不到"的三项保留**（2026-10-15 评估）：
- 收益为零：系统镜像是定长文件（当时 576 MiB，现为 512 MiB），开机整个读一遍，去掉这约 400 KB 不缩短读取；它们不会运行，不占内存和处理器；压缩后在约 196 MB 的安装包里只占约 0.2 MB。
- 代价：它们由 AOSP 的通用产品定义（`base.mk`、`core_minimal.mk`）引入，8.1 没有从设备一侧删除 `PRODUCT_PACKAGES` 的办法，只能给 `build/make` 加补丁，以后每次同步都要带着。
- 源码义务不因此增加：内核、`iptables`、`iproute2`、`e2fsprogs` 等必需组件本来就要随发行附源码，打包脚本按 NOTICE 列出的组件一并处理即可。
- 调试工具（`gdbserver`、`mmc_utils`，以及 `su`、`strace`、`tcpdump`）都标记为 `debug`，改用 `user` 构建后自动不进镜像，不用另外改。

## 源码提供

- 每个 GitHub Release 附上镜像里全部 copyleft 文件的源码，由 `tools/collect-copyleft-sources.py` 从镜像的 NOTICE 逐个文件追溯生成，并核对就是构建用的那份（`docs/design/release-build.md`「随发行附源码」）：
  - AOSP 项目源码包；
  - 亚马逊内核源码原样；
  - EinkBro、rime-ice 的上游源码（`device/amazon/pw3/apps/sources.json`）；
  - `SOURCES.md`：每个文件对应的源码。
- 本仓库在发行标签下的内容（补丁、配置、构建脚本、交接模块）是上述源码的其余部分。
- Chromium 源码包 6.7 GB，超过 GitHub 单个文件的上限，改用书面要约（有效期至少三年），写在 `SOURCES.md` 里。
- 构建工具链：AOSP `arm-linux-androideabi-4.9`（`0390252b`），musl 1.2.5，构建虚拟机 Ubuntu 22.04。
