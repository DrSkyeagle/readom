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

# The pieces of the line written on the picture (epdc/SleepText.h).
include $(CLEAR_VARS)
LOCAL_MODULE := pw3-sleep-glyphs
LOCAL_MODULE_CLASS := ETC
LOCAL_MODULE_STEM := sleep-glyphs.pgm
LOCAL_SRC_FILES := sleep-glyphs.pgm
LOCAL_MODULE_PATH := $(TARGET_OUT)/usr/share/pw3
LOCAL_REQUIRED_MODULES := pw3-sleep-glyphs-index
include $(BUILD_PREBUILT)

include $(CLEAR_VARS)
LOCAL_MODULE := pw3-sleep-glyphs-index
LOCAL_MODULE_CLASS := ETC
LOCAL_MODULE_STEM := sleep-glyphs.txt
LOCAL_SRC_FILES := sleep-glyphs.txt
LOCAL_MODULE_PATH := $(TARGET_OUT)/usr/share/pw3
include $(BUILD_PREBUILT)

# The default wallpapers live in wallpapers/ with their own makefile. The build
# finds makefiles with first-makefiles-under (findleaves.py), which stops
# descending as soon as it meets one -- so this directory's Android.mk hides
# wallpapers/ from that scan, and the wallpapers are only defined because they
# are included from here. Without this line the modules exist in no build and
# the image silently ships without them (found in the v8.9 plumbing, 2026-11-10).
include $(call all-makefiles-under,$(LOCAL_PATH))
