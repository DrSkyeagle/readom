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

## 刷新

- **熄屏：** 整屏 GC16，会黑闪一下。不用 GL16，是因为 GL16 会在图片里留下前一页文字的淡影，等于没有遮住。
- **唤醒：** HAL 记得屏上是这张图（`Panel::assumeShown`），第一帧按普通翻页处理，只改不同的像素（GL16，不闪），并计入每 20 页一次的全刷。画面里的地球、书和云是中灰，唤醒后可能留下淡影，要上机看；如果淡影明显，改为唤醒时全刷一次，并先请用户看过。

**格式：** 8 位二进制 PGM（P5），尺寸必须与屏幕完全相同（`epdc/Picture.h`）。HAL 逐行读取，用误差扩散把灰度分到面板的 16 级（`epdc/Dither.h`）。
