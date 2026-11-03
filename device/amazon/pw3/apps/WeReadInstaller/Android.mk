LOCAL_PATH := $(call my-dir)

# Downloads WeRead from Tencent's official address on the first network
# connection and installs it once it is shown to be Tencent's
# (docs/design/app-install.md, I11).
include $(CLEAR_VARS)
LOCAL_PACKAGE_NAME := WeReadInstaller
LOCAL_MODULE_TAGS := optional
LOCAL_SRC_FILES := $(call all-java-files-under, src)
LOCAL_PRIVILEGED_MODULE := true
LOCAL_SDK_VERSION := current
LOCAL_PROGUARD_ENABLED := disabled
LOCAL_REQUIRED_MODULES := privapp-permissions-wereadinstaller.xml
include $(BUILD_PACKAGE)

include $(CLEAR_VARS)
LOCAL_MODULE := privapp-permissions-wereadinstaller.xml
LOCAL_MODULE_CLASS := ETC
LOCAL_MODULE_PATH := $(TARGET_OUT_ETC)/permissions
LOCAL_SRC_FILES := $(LOCAL_MODULE)
include $(BUILD_PREBUILT)
