LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
# Times the CPU steps of one composed frame with the HAL's own code. Built on
# request only and installed outside the system image.
LOCAL_MODULE := pw3-display-bench
LOCAL_MODULE_TAGS := tests
LOCAL_MODULE_PATH := $(TARGET_OUT_DATA_NATIVE_TESTS)/pw3-display-bench
LOCAL_CLANG := true
LOCAL_CPPFLAGS := -std=c++14 -fexceptions -Wall -Wextra -Werror
LOCAL_SRC_FILES := bench.cpp ../../epdc/Panel.cpp ../../epdc/Tone.cpp ../../epdc/Luma.cpp ../../composer/LumaCompose.cpp
LOCAL_C_INCLUDES := $(LOCAL_PATH)/../../epdc $(LOCAL_PATH)/../../composer
include $(BUILD_EXECUTABLE)
