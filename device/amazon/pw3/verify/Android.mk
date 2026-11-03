LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE := pw3-system-verify
LOCAL_SRC_FILES := SystemVerify.cpp
LOCAL_SHARED_LIBRARIES := libbase libcrypto liblog
LOCAL_CFLAGS := -Wall -Wextra -Werror
LOCAL_CPPFLAGS := -std=c++14
LOCAL_INIT_RC := pw3-system-verify.rc
include $(BUILD_EXECUTABLE)
