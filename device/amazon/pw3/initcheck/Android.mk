LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE := pw3-init-check
LOCAL_SRC_FILES := InitCheck.cpp
LOCAL_SHARED_LIBRARIES := libbase liblog
LOCAL_CFLAGS := -Wall -Wextra -Werror
LOCAL_CPPFLAGS := -std=c++14
include $(BUILD_EXECUTABLE)

include $(CLEAR_VARS)
LOCAL_MODULE := pw3-boot-report
LOCAL_MODULE_TAGS := optional
LOCAL_SRC_FILES := BootReport.cpp
LOCAL_SHARED_LIBRARIES := libbase liblog
LOCAL_CFLAGS := -Wall -Wextra -Werror
LOCAL_CPPFLAGS := -std=c++14
LOCAL_INIT_RC := pw3-boot-report.rc
include $(BUILD_EXECUTABLE)
