LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE := gralloc.pw3
LOCAL_MODULE_RELATIVE_PATH := hw
LOCAL_PROPRIETARY_MODULE := true
LOCAL_SRC_FILES := gralloc.cpp mapper.cpp framebuffer.cpp ../epdc/Panel.cpp ../epdc/Tone.cpp ../epdc/Luma.cpp \
    ../epdc/Dither.cpp ../epdc/Picture.cpp ../epdc/SleepText.cpp
LOCAL_SHARED_LIBRARIES := liblog libcutils
LOCAL_HEADER_LIBRARIES := libhardware_headers
# LOCAL_CPP_FEATURES is an NDK-build variable; AOSP 8.1 needs the flag here.
LOCAL_CPPFLAGS := -std=c++11 -fexceptions
LOCAL_CFLAGS := -DLOG_TAG=\"PW3Gralloc\" -Wno-missing-field-initializers
include $(BUILD_SHARED_LIBRARY)
