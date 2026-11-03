# System WebView (Chromium 128)

`SystemWebView.apk` is the unmodified file `128.0.6613.88/arm/webview.apk` of AOSP's
`platform/external/chromium-webview` at commit `27ac2bdb9a1a4e55dc65bb6cdc752dbfdea3b9f4` (branch
`main`; git blob `54e5b6f82a2c5677264c2d10b496fd90c2afb923`, 86,760,206 bytes, sha256
`19a2cdf16d451ee1a3dea8e5d5b369dc85e7bbc616701d6a41a51e851cfd37c4`). Package `com.android.webview`,
version 128.0.6613.88, minSdk 26, 32-bit ARM only. Downloaded 2026-10-13 with the user's approval
("我同意用 128，用 AOSP 官方最佳").

Why: the Android 8.1 tree carries Chromium 61 (2017) in the same repository; pages written for
current browsers do not lay out with it (github.com on Test V32). 128.0.6613.88 is the newest
version AOSP published there. Chromium 138 is the last that supports Android 8 at all (139 needs
Android 10); a 138 build exists only outside AOSP.

Measured on the device (Test V32, the APK mounted over the old one, minimal WebView shell,
`work/android-port/build-share/webview-128-eval/`):

| | Chromium 61 | Chromium 128 |
|---|---|---|
| runs on the 3.0.35 kernel (no getrandom, memfd_create) | yes | yes |
| www.baidu.com, CPU seconds until idle / memory | 11.8 / 82 MB | 10.8 / 79 MB |
| news.sina.cn | 21.8 / 95 MB | 21.2 / 94 MB |
| weread.qq.com | 7.8 / 76 MB | 13.8 / 92 MB |
| github.com | 14.1 / 97 MB, layout broken | 38.4 / 146 MB, correct |
| shell start | 1.7-2.0 s | 2.5-2.7 s |
| WeRead cold start, CPU seconds after 30 s | 18.9-21.8 | 15.3-18.7 |

WeRead loads this WebView in its own process; its bookshelf opened with both.

`LOCAL_OVERRIDES_PACKAGES := webview` keeps the Chromium 61 package out of the image.

Not kept in the source tree: `tools/fetch-prebuilts.py` downloads it from the address above and checks
its size and SHA-256 (list: `apps/prebuilts.json`); the AOSP build refuses to start without it.
