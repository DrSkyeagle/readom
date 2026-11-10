LOCAL_PATH := $(call my-dir)
# The default sleep wallpapers: the pictures the device owner supplied for this
# port's builds (PW3-SOURCE.md), installed where the display HAL looks when the
# user's own folder has nothing to show (gralloc/framebuffer.cpp,
# kBakedSleepWallpaperDir). Modules rather than copied files, so that they are
# listed with their licence in the device's NOTICE; the NOTICE beside this file
# is that licence.
#
# This makefile is included from the parent (../Android.mk, all-makefiles-under):
# the build's own scan stops at a directory that has an Android.mk, so a nested
# one is never found on its own. Each module is also in PRODUCT_PACKAGES
# (device/amazon/pw3/pw3.mk): a prebuilt that PRODUCT_PACKAGES does not name is
# defined and then not installed, which is a build that silently ships no default
# wallpaper (both found in the v8.9 plumbing, 2026-11-10).
include $(CLEAR_VARS)
LOCAL_MODULE := pw3-sleepwall-01
LOCAL_MODULE_CLASS := ETC
LOCAL_MODULE_STEM := wallpaper-01.png
LOCAL_SRC_FILES := wallpaper-01.png
LOCAL_MODULE_PATH := $(TARGET_OUT)/usr/share/pw3/sleepwall
include $(BUILD_PREBUILT)

include $(CLEAR_VARS)
LOCAL_MODULE := pw3-sleepwall-02
LOCAL_MODULE_CLASS := ETC
LOCAL_MODULE_STEM := wallpaper-02.png
LOCAL_SRC_FILES := wallpaper-02.png
LOCAL_MODULE_PATH := $(TARGET_OUT)/usr/share/pw3/sleepwall
include $(BUILD_PREBUILT)

include $(CLEAR_VARS)
LOCAL_MODULE := pw3-sleepwall-03
LOCAL_MODULE_CLASS := ETC
LOCAL_MODULE_STEM := wallpaper-03.png
LOCAL_SRC_FILES := wallpaper-03.png
LOCAL_MODULE_PATH := $(TARGET_OUT)/usr/share/pw3/sleepwall
include $(BUILD_PREBUILT)
