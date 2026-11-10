# 默认息屏壁纸（baked）

Android.mk 把下面三张图装到 **`/system/usr/share/pw3/sleepwall/`**，也就是显示 HAL 的
`kBakedSleepWallpaperDir`（`gralloc/framebuffer.cpp`）：用户的 `/kindle-us/sleepwall/`
没有图、读不到、或图全都解不出来时，HAL 从这里随机挑一张；用户自己的图优先，可以随时覆盖
默认。选择规则与透明像素行为见 `sleep/README.md`。

**来源与许可**：三张图由本设备的所有者提供（2026-11-10），仅用于本移植的实验包；不是本项目
Apache-2.0 源码的一部分，也没有授权在其它构建里复用。许可说明见同目录 `NOTICE`；
`REUSE.toml` 用 `LicenseRef-UserProvided-Wallpaper` 标注这三个 PNG。

| 安装名 | 原文件名（`readom-port/assets/sleepwall/`） | 尺寸 | 字节 | sha256 |
|---|---|---|---|---|
| `wallpaper-01.png` | `壁纸4_6寸300ppi_较小.png` | 1072×1448 | 424,353 | `c49b4f61d1ddcaf6f89f68895737c005a7d4f638d6bb194d6172377ec08f65fd` |
| `wallpaper-02.png` | `猫耳女孩透明背景.png` | 1272×1696 | 1,030,685 | `b3815127022854225a48a62fb4ef5fe98488ed4c5a1e6a67d5c0600745116553` |
| `wallpaper-03.png` | `rb-ad6b8959d849bcc-(1).png` | 1274×1680 | 1,861,230 | `48009d4c084f3c11593eba903df45b46deec828bc26d8e0ff4b12b423648da96` |

三张都是 8 位 RGBA、带 alpha 通道（透明像素占比分别约 85.3% / 54.0% / 59.6%），尺寸与体积都在
`epdc/Png.h` 的上限内（边长 ≤4096、像素 ≤400 万、文件 ≤16 MB），解码后由 `SleepWallpaper`
按“铺满屏幕、居中裁剪”缩放，透明处保留睡前最后一帧。合计给 system.img 增加约 **3.3 MB**。
