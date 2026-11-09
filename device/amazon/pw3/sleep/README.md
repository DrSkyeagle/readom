# 休眠画面

屏幕关闭时，显示 HAL 先把一张图片刷到屏幕上，再给面板断电（`gralloc/framebuffer.cpp` 的 `opsSleep`）。

## 为什么要画

墨水屏断电后仍保留画面。Android 熄屏的本意是让内容消失，这在液晶屏上是自然发生的；在墨水屏上，不主动画点什么，正在看的网页或书页就会一直留在屏幕上。另外，看着正常却点了没反应的屏幕，像是死机了。

## 图片

| 文件 | 何时用 | 内容 |
|---|---|---|
| `sleep-zh.pgm` | 系统语言为中文（`persist.sys.locale`，未设置时看 `ro.product.locale`，以 `zh` 开头） | `art.png`，标题换成"读书无禁区" |
| `sleep-en.pgm` | 其他语言 | `art.png` 原样，标题是 "Reading knows no forbidden zones" |

- **`art.png`**：用户于 2026-10-15 用图片生成模型制作。画面是一只独角兽趴在地球上读书，四周是飞舞的书和各地地标；独角兽恰好与输入法同名。原图 1086×1448，取中间 1072 列。
  - 许可：用户用 OpenAI 的 GPT 图像模型生成（2026-10-15 确认）。OpenAI 的使用条款把生成结果的权利归使用者，所以这张图和由它做出的两张休眠图随本项目按 Apache-2.0 发布。
- **生成方法**：
  ```
  make-sleep-picture.py pictures art.png LXGWWenKai-Regular.ttf .
  make-sleep-picture.py glyphs LXGWWenKai-Regular.ttf sleep-glyphs.pgm sleep-glyphs.txt
  ```
  - 中文版的做法：把原标题所在方框（`ART_TITLE`）填成周围纸面的灰度，再用霞鹜文楷排"读书无禁区"。
  - 字体：霞鹜文楷 v1.522（LXGW WenKai，SIL OFL 1.1，https://github.com/lxgw/LxgwWenKai）。只用来生成图片，不随系统分发；用字体排出的图片不属于字体本身。
- 构建时装到 `/system/usr/share/pw3/`（`/system/media` 每次开机都会被媒体扫描遍历，不放那里）。

画面平均亮度约 239/255，四分之三是纸白。

## 用户自己的壁纸（`/kindle-us/sleepwall/`）

除了上面两张随镜像分发的图，用户可以放自己的图：**把 PNG 放进 Kindle 盘根目录下的 `sleepwall/` 文件夹**（和安装时放 `documents/Readom.sh` 是同一块盘、同一套 USB 操作）。这个文件夹由 preinit 在开机时建好（空目录，vfat 上是 `root:root 0755`），用户直接往里拷图即可；发行镜像**不带示例图**。每次熄屏随机挑一张；PNG 的透明像素不画，露出**熄屏前最后一帧的画面**。

- **为什么透明像素能"露出来"**：墨水屏断电后保留画面，所以透明处根本不用画——显示 HAL 把那里写成面板此刻已有的灰度级（`Panel::shown()`），GC16 再驱动一遍同样的值，画面上这一块就和睡前完全一样，不是"抖动过的近似"。半透明像素（0 < alpha < 255）在 16 级灰阶上没法真的半透明：它们和睡前一帧的亮度做 alpha 混合后再误差扩散（`epdc/SleepWallpaper.h`）。
- **尺寸**：任意尺寸都行。按"铺满屏幕、居中裁剪"缩放（bilinear）：比例不同时裁掉两边，不会留黑边。上限：边长 4096、像素 400 万、文件 16 MB（`epdc/Png.h`）；超过的图直接跳过（换下一张，或回退自带图）。上限是内存预算：显示进程总共 512 MB，4 MP 是"够用且不会把 SurfaceFlinger 顶掉"的折中（4000×3000 的手机照片会被拒，屏幕不会因此留在书页上，会显示自带的 `sleep-*.pgm`）。
- **随机规则**：每次熄屏重新挑，且**不连续重复**（有多张时）。上一次选中的文件名记在进程里，所以重启后的第一次可能重复；文件夹里只有一张时每次都显示它。
- **回退**（任何一步失败都不会让屏幕留在书页上）：文件夹不存在/没有 PNG → 用镜像里分发的 `sleep-*.pgm`；某张 PNG 解码失败（拷了一半、格式不支持）→ 试下一张；面板状态未知、没有"上一帧"可露出（`Panel::needsFull()`）→ 用 `sleep-*.pgm`；连 PGM 都读不到 → 老行为：把还在第一遍里的灰度级刷出来。
- **休眠文字**（`epdc/SleepText.h`，`休眠于 21:47 · 10月15日 · 电量 63%`）照旧叠在图片顶部，叠在壁纸之后；文字占的那几行不保留背景（否则字压在旧画面上看不清）。
- **配置**（`debug.` 属性，发行版上默认值即产品值；调试构建可用 `setprop` 改）：
  | 属性 | 默认 | 作用 |
  |---|---|---|
  | `debug.pw3.sleepwall.enabled` | `true` | `false`：完全回到镜像自带的图 |
  | `debug.pw3.sleepwall.dir` | `/kindle-us/sleepwall` | 换一个文件夹（例如调试时 `/data/local/tmp/sleepwall`） |
  该属性读不到时还会再看 `/system/usr/share/pw3/sleepwall/`（装机/构建时预置的副本），两处都没有才回退到 PGM。
- **权限**：`/kindle-us` 是 `vfat` 标签，SurfaceFlinger 的域默认读不到；`sepolicy/pw3_sleepwall.te` 加了只读的两行 allow。没有这条策略（例如只把 `gralloc.pw3.so` 注入现有 system.img、没重建 ramdisk 里的 policy）时，HAL 打一行 `sleep wallpaper: no .png`（实际是 EACCES）然后读内置目录 `/system/usr/share/pw3/sleepwall/`（`system_file`，策略本来放行）；内置目录也没有才回退 PGM——功能关闭，但不会坏。发行镜像**不含**内置目录，只在"只注入 HAL"的镜像里用它放用户/测试图（换图要重注入）。
- **代价**：每次熄屏要解一张 PNG 再缩放（Cortex-A9 上约 100–300 ms，6 MB 左右临时内存），发生在按电源键之后、面板断电之前；屏幕已经关了，用户看不到，但关机动画会稍慢一点。图片超大时会先被尺寸上限挡掉。

## 刷新

- **熄屏：** 整屏 GC16，会黑闪一下。不用 GL16，是因为 GL16 会在图片里留下前一页文字的淡影，等于没有遮住。
- **唤醒：** HAL 记得屏上是这张图（`Panel::assumeShown`），第一帧按普通翻页处理，只改不同的像素（GL16，不闪），并计入每 20 页一次的全刷。画面里的地球、书和云是中灰，唤醒后可能留下淡影，要上机看；如果淡影明显，改为唤醒时全刷一次，并先请用户看过。
  - 用壁纸时同一套逻辑：透明处本来就是前帧的灰度级，唤醒后第一帧与它相同，不会动；壁纸本身的中灰/深色区域可能留淡影，靠 `fullEvery` 的全刷清。

**格式：** 8 位二进制 PGM（P5），尺寸必须与屏幕完全相同（`epdc/Picture.h`）。HAL 逐行读取，用误差扩散把灰度分到面板的 16 级（`epdc/Dither.h`）。
