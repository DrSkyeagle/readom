LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
# Times SwiftShader's GLES on this CPU for the work SurfaceFlinger gives it
# (N38). Built on request only and installed outside the system image.
LOCAL_MODULE := pw3-gles-bench
LOCAL_MODULE_TAGS := tests
LOCAL_MODULE_PATH := $(TARGET_OUT_DATA_NATIVE_TESTS)/pw3-gles-bench
LOCAL_CLANG := true
LOCAL_CPPFLAGS := -std=c++14 -Wall -Wextra -Werror
LOCAL_SRC_FILES := bench.cpp
LOCAL_SHARED_LIBRARIES := libEGL libGLESv2 libui libutils libnativewindow
include $(BUILD_EXECUTABLE)
