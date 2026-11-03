# EinkBro

`EinkBro.apk` is the unmodified release asset `app-armeabi-v7a-release.apk` of EinkBro 16.7.1
(`https://github.com/plateaukao/einkbro/releases/tag/v16.7.1`, package `info.plateaukao.einkbro`,
minSdk 24), sha256 `4bf5a7245141022c0bc29434afc81e7665ea3d55af718aefe3e9e12417d673b6`.
Downloaded 2026-10-13 with the user's approval.

Why: the user asked for a browser. Chrome needs a GPU and Google services; the AOSP image only had
the WebView test shell (Browser2). EinkBro is a browser made for e-paper screens (paged scrolling,
no animations) on top of the system WebView. On Test V32 it installed, started and showed
www.baidu.com with the image's WebView (Chromium 61, software drawing).

Not kept in the source tree: `tools/fetch-prebuilts.py` downloads it from the address above and checks
its size and SHA-256 (list: `apps/prebuilts.json`); the AOSP build refuses to start without it.
