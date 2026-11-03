LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
# AOSP's WebView prebuilt from a later release of external/chromium-webview
# (see PW3-SOURCE.md), installed instead of the Chromium 61 one that the
# Android 8.1 tree carries. Same rules as external/chromium-webview/Android.mk:
# signed with the platform's development key, native library kept inside the
# APK (stored, page-aligned) where the WebView loader maps it from.
LOCAL_MODULE := SystemWebView
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_CLASS := APPS
LOCAL_MODULE_SUFFIX := $(COMMON_ANDROID_PACKAGE_SUFFIX)
LOCAL_MODULE_TARGET_ARCH := arm
LOCAL_SRC_FILES := SystemWebView.apk
LOCAL_CERTIFICATE := $(DEFAULT_SYSTEM_DEV_CERTIFICATE)
LOCAL_OVERRIDES_PACKAGES := webview
LOCAL_REQUIRED_MODULES := \
        libwebviewchromium_loader \
        libwebviewchromium_plat_support
LOCAL_PREBUILT_JNI_LIBS_arm := @lib/armeabi-v7a/libwebviewchromium.so
include $(BUILD_PREBUILT)
