# 第三方：stb_image

`stb_image.h` 来自 <https://github.com/nothings/stb>，版本 **v2.30**，文件原样使用、未修改。

- 许可：**public domain（公有领域）/ MIT 二选一**，许可全文在文件头里（"public domain image loader … ALTERNATIVE B — MIT License"）。随本项目按 Apache-2.0 分发不冲突；`LICENSES/MIT.txt` 已在本仓库里。
- 用途：`epdc/Png.cpp` 是唯一编译它的翻译单元，且只用 `STBI_ONLY_PNG`（加 `STBI_NO_STDIO`：文件由我们自己读、先查大小）。息屏壁纸是用户放进文件夹的 PNG，显示 HAL 需要能读它。
- 为什么内嵌而不是链 `libpng`：显示 HAL 再多一个运行时依赖就要跟着这个移植走到底，而这是单头文件、无 ABI 可对；代价是 283 KB 的源码，一次编译进 `gralloc.pw3.so`。
- 取值记录（2026-11-09 取）：283,010 B，`sha256 594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3`。

`epdc/Png.h` 在把数据交给它之前先设上限（边长 4096、像素数 1200 万、文件 32 MB，见该头文件），所以一个损坏或过大的文件不会被解码出一张本机放不下的图。

重新获取同一版本（需要网络）：

```sh
curl -o device/amazon/pw3/sleep/third_party/stb_image.h \
  https://raw.githubusercontent.com/nothings/stb/v2.30/stb_image.h
```

`THIRD_PARTY.md` 尚未登记（本任务写入范围之外）；随 PR 合并时由文档任务补一行。
