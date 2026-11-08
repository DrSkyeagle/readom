LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE := pw3-testfile-init
LOCAL_CLANG := true
LOCAL_MODULE_TAGS := optional
LOCAL_SRC_FILES := PreInit.c
LOCAL_CFLAGS := -std=gnu11 -Wall -Wextra -Werror -Wno-unused-function -D_FILE_OFFSET_BITS=64
# Which model's kernel this storage entry accepts (PreInit.c, kernel_release).
# The board name is the product's: pw3 for the Paperwhite 3, voyage for the
# Voyage, whose device directory inherits this one.
LOCAL_CFLAGS += -DPW3_BOARD_NAME=\"$(TARGET_BOOTLOADER_BOARD_NAME)\"
LOCAL_FORCE_STATIC_EXECUTABLE := true
LOCAL_STATIC_LIBRARIES := libcrypto libc
LOCAL_SHARED_LIBRARIES :=
LOCAL_SYSTEM_SHARED_LIBRARIES :=
LOCAL_MODULE_PATH := $(TARGET_ROOT_OUT)
include $(BUILD_EXECUTABLE)
