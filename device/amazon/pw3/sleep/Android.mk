LOCAL_PATH := $(call my-dir)
# The sleep pictures (README.md). Modules rather than copied files, so that
# they are listed with their licence (the device's NOTICE) in the image. In
# /system/usr/share rather than /system/media, which the media scanner walks
# at every start.
include $(CLEAR_VARS)
LOCAL_MODULE := pw3-sleep-picture-zh
LOCAL_MODULE_CLASS := ETC
LOCAL_MODULE_STEM := sleep-zh.pgm
LOCAL_SRC_FILES := sleep-zh.pgm
LOCAL_MODULE_PATH := $(TARGET_OUT)/usr/share/pw3
include $(BUILD_PREBUILT)

include $(CLEAR_VARS)
LOCAL_MODULE := pw3-sleep-picture-en
LOCAL_MODULE_CLASS := ETC
LOCAL_MODULE_STEM := sleep-en.pgm
LOCAL_SRC_FILES := sleep-en.pgm
LOCAL_MODULE_PATH := $(TARGET_OUT)/usr/share/pw3
include $(BUILD_PREBUILT)
