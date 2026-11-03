LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
# Released, signed APK (see PW3-SOURCE.md). Kept as it is: the build only
# aligns it and compiles its code ahead of time next to it.
LOCAL_MODULE := EinkBro
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_CLASS := APPS
LOCAL_MODULE_SUFFIX := $(COMMON_ANDROID_PACKAGE_SUFFIX)
LOCAL_SRC_FILES := EinkBro.apk
LOCAL_CERTIFICATE := PRESIGNED
LOCAL_OVERRIDES_PACKAGES := Browser2
include $(BUILD_PREBUILT)
