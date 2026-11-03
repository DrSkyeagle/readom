LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
# The host tests of the panel state, the luma rows and the sleep picture
# (epdc/tests), built for the device: the vector code they check is not the
# same on ARMv7 as on the 64-bit host. Built on request only and installed
# outside the system image.
LOCAL_MODULE := pw3-panel-test
LOCAL_MODULE_TAGS := tests
LOCAL_MODULE_PATH := $(TARGET_OUT_DATA_NATIVE_TESTS)/pw3-panel-test
LOCAL_CLANG := true
LOCAL_CPPFLAGS := -std=c++14 -fexceptions -Wall -Wextra -Werror -UNDEBUG
LOCAL_SRC_FILES := ../../epdc/tests/panel_test.cpp ../../epdc/Panel.cpp ../../epdc/Tone.cpp
include $(BUILD_EXECUTABLE)

include $(CLEAR_VARS)
LOCAL_MODULE := pw3-luma-test
LOCAL_MODULE_TAGS := tests
LOCAL_MODULE_PATH := $(TARGET_OUT_DATA_NATIVE_TESTS)/pw3-panel-test
LOCAL_CLANG := true
LOCAL_CPPFLAGS := -std=c++14 -fexceptions -Wall -Wextra -Werror -UNDEBUG
LOCAL_SRC_FILES := ../../epdc/tests/luma_test.cpp ../../epdc/Luma.cpp
include $(BUILD_EXECUTABLE)

include $(CLEAR_VARS)
LOCAL_MODULE := pw3-compose-test
LOCAL_MODULE_TAGS := tests
LOCAL_MODULE_PATH := $(TARGET_OUT_DATA_NATIVE_TESTS)/pw3-panel-test
LOCAL_CLANG := true
LOCAL_CPPFLAGS := -std=c++14 -fexceptions -Wall -Wextra -Werror -UNDEBUG
LOCAL_SRC_FILES := ../../composer/tests/luma_compose_test.cpp ../../composer/LumaCompose.cpp ../../epdc/Luma.cpp
LOCAL_C_INCLUDES := $(LOCAL_PATH)/../../epdc $(LOCAL_PATH)/../../composer
include $(BUILD_EXECUTABLE)

include $(CLEAR_VARS)
LOCAL_MODULE := pw3-dither-test
LOCAL_MODULE_TAGS := tests
LOCAL_MODULE_PATH := $(TARGET_OUT_DATA_NATIVE_TESTS)/pw3-panel-test
LOCAL_CLANG := true
LOCAL_CPPFLAGS := -std=c++14 -fexceptions -Wall -Wextra -Werror -UNDEBUG
LOCAL_SRC_FILES := ../../epdc/tests/dither_test.cpp ../../epdc/Dither.cpp ../../epdc/Tone.cpp
include $(BUILD_EXECUTABLE)

include $(CLEAR_VARS)
LOCAL_MODULE := pw3-picture-test
LOCAL_MODULE_TAGS := tests
LOCAL_MODULE_PATH := $(TARGET_OUT_DATA_NATIVE_TESTS)/pw3-panel-test
LOCAL_CLANG := true
LOCAL_CPPFLAGS := -std=c++14 -fexceptions -Wall -Wextra -Werror -UNDEBUG
LOCAL_SRC_FILES := ../../epdc/tests/picture_test.cpp ../../epdc/Picture.cpp
include $(BUILD_EXECUTABLE)

include $(CLEAR_VARS)
LOCAL_MODULE := pw3-sleep-text-test
LOCAL_MODULE_TAGS := tests
LOCAL_MODULE_PATH := $(TARGET_OUT_DATA_NATIVE_TESTS)/pw3-panel-test
LOCAL_CLANG := true
LOCAL_CPPFLAGS := -std=c++14 -fexceptions -Wall -Wextra -Werror -UNDEBUG
LOCAL_SRC_FILES := ../../epdc/tests/sleep_text_test.cpp ../../epdc/SleepText.cpp ../../epdc/Picture.cpp
include $(BUILD_EXECUTABLE)

include $(CLEAR_VARS)
# The allocator and the composer have to size a pixel the same; this says so
# for every format either of them names.
LOCAL_MODULE := pw3-format-test
LOCAL_MODULE_TAGS := tests
LOCAL_MODULE_PATH := $(TARGET_OUT_DATA_NATIVE_TESTS)/pw3-panel-test
LOCAL_CLANG := true
LOCAL_CPPFLAGS := -std=c++14 -fexceptions -Wall -Wextra -Werror -UNDEBUG
LOCAL_SRC_FILES := ../../composer/tests/buffer_format_test.cpp ../../composer/LumaCompose.cpp ../../epdc/Luma.cpp
LOCAL_C_INCLUDES := $(LOCAL_PATH)/../../epdc $(LOCAL_PATH)/../../composer $(LOCAL_PATH)/../../gralloc
include $(BUILD_EXECUTABLE)
