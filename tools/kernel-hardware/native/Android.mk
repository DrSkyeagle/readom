LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE := pw3-usb-check
LOCAL_SRC_FILES := main.cpp UsbGate.cpp PosixChecks.cpp
LOCAL_CPPFLAGS := -std=c++11 -Wall -Wextra -Werror
LOCAL_SHARED_LIBRARIES := libbase liblog
include $(BUILD_EXECUTABLE)
