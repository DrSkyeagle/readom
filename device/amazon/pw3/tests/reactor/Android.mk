LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE := pw3-reactor-div-test
LOCAL_MODULE_TAGS := tests
LOCAL_VENDOR_MODULE := true
LOCAL_CLANG := true
LOCAL_CPPFLAGS := -std=c++11 -Wall -Wextra -Werror
LOCAL_SRC_FILES := division.cpp
LOCAL_C_INCLUDES := external/swiftshader/src
LOCAL_STATIC_LIBRARIES := swiftshader_top_release libsubzero
LOCAL_SHARED_LIBRARIES := libc++ liblog libcutils libsync
include $(BUILD_EXECUTABLE)

include $(CLEAR_VARS)
LOCAL_MODULE := pw3-reactor-speed
LOCAL_MODULE_TAGS := tests
LOCAL_VENDOR_MODULE := true
LOCAL_CLANG := true
LOCAL_CPPFLAGS := -std=c++11 -Wall -Wextra -Werror -O2
LOCAL_SRC_FILES := speed.cpp
LOCAL_C_INCLUDES := external/swiftshader/src
LOCAL_STATIC_LIBRARIES := swiftshader_top_release libsubzero
LOCAL_SHARED_LIBRARIES := libc++ liblog libcutils libsync
include $(BUILD_EXECUTABLE)
