LOCAL_PATH := $(call my-dir)

# Puts up one layer of each shape the CPU composer has to take: a buffer in
# RGB_565, the same in RGBA_8888 as the control, and a surface shown larger
# than its buffer. Built on request only (tests), installed from a shell with
# pm install; it is never part of the system image.
include $(CLEAR_VARS)
LOCAL_PACKAGE_NAME := PW3FormatTest
LOCAL_MODULE_TAGS := tests
LOCAL_SRC_FILES := $(call all-java-files-under, src)
LOCAL_SDK_VERSION := current
LOCAL_PROGUARD_ENABLED := disabled
include $(BUILD_PACKAGE)
