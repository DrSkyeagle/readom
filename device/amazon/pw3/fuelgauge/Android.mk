LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE := pw3-fg-state
LOCAL_VENDOR_MODULE := true
LOCAL_SRC_FILES := main.cpp FgState.cpp
LOCAL_CPPFLAGS := -std=c++11 -Wall -Wextra -Werror
LOCAL_SHARED_LIBRARIES := liblog
LOCAL_INIT_RC := pw3-fg-state.rc
include $(BUILD_EXECUTABLE)
