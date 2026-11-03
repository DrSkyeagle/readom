LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE := libhealthd.pw3
LOCAL_SRC_FILES := healthd.cpp
LOCAL_HEADER_LIBRARIES := libhealthd_headers
LOCAL_STATIC_LIBRARIES := libbase
LOCAL_CFLAGS := -Wall -Wextra -Werror
include $(BUILD_STATIC_LIBRARY)
