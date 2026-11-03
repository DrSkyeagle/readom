LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE := hwcomposer.pw3
LOCAL_MODULE_RELATIVE_PATH := hw
LOCAL_PROPRIETARY_MODULE := true
LOCAL_SRC_FILES := composer.cpp LumaCompose.cpp ../epdc/Luma.cpp
LOCAL_SHARED_LIBRARIES := libhardware liblog libcutils libsync
LOCAL_HEADER_LIBRARIES := libhardware_headers
LOCAL_CPPFLAGS := -std=c++14 -Wall -Wextra -Werror
LOCAL_CFLAGS := -DLOG_TAG=\"PW3Composer\" -Wno-missing-field-initializers
include $(BUILD_SHARED_LIBRARY)
