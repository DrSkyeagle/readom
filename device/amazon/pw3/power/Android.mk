LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE := power.pw3
LOCAL_MODULE_RELATIVE_PATH := hw
LOCAL_PROPRIETARY_MODULE := true
LOCAL_SRC_FILES := power.c
LOCAL_SHARED_LIBRARIES := liblog libcutils
LOCAL_HEADER_LIBRARIES := libhardware_headers
LOCAL_CFLAGS := -Wall -Wextra -Werror -DLOG_TAG=\"PW3Power\"
include $(BUILD_SHARED_LIBRARY)
