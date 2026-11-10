TARGET_ARCH := arm
TARGET_ARCH_VARIANT := armv7-a-neon
TARGET_CPU_VARIANT := cortex-a9
TARGET_CPU_ABI := armeabi-v7a
TARGET_CPU_ABI2 := armeabi
TARGET_BOARD_PLATFORM := imx6sl
TARGET_BOOTLOADER_BOARD_NAME := pw3
TARGET_NO_BOOTLOADER := true
TARGET_NO_KERNEL := true
TARGET_NO_RECOVERY := true
TARGET_USES_64_BIT_BINDER := false
# SurfaceFlinger's retained HWC1 path supports the framebuffer HAL directly.
TARGET_USES_HWC2 := false
# The legacy framebuffer path has no kernel sync-file/fence implementation.
# Tell DispSync to use the standard no-sync-framework resampling path.
TARGET_RUNNING_WITHOUT_SYNC_FRAMEWORK := true
USE_OPENGL_RENDERER := true
NUM_FRAMEBUFFER_SURFACE_BUFFERS := 2
BOARD_USES_GENERIC_AUDIO := true
# The XML policy exposes only the software silence sink; no capture device.
USE_XML_AUDIO_POLICY_CONF := 1
USE_CAMERA_STUB := true
TARGET_USERIMAGES_USE_EXT4 := true
TARGET_COPY_OUT_VENDOR := system/vendor
# File-image budgets only: no device partition resize or write is defined here.
# The system image holds what is installed (453 MB on Test V36) and little
# more: it was read in full at every start for its SHA256 (1 GiB until Test
# V36, some 20 s spent on empty blocks); since the storage entry V7 only when
# it changed.
#
# 512 MiB (user, 2026-11-02): 576 MiB until then, of which `df` on the device
# showed 445.9 MiB used of the 566.9 MiB it mounts as -- a read-only image, so
# the free part is never written to and buys nothing but room for the next
# version to grow.
#
# 512 MiB is the smallest power of two the build fits in, and the build of
# 2026-11-03 reports the room left rather than leaving it to a subtraction:
# 131,072 blocks of 4 KiB with 15,131 free, so 452.9 MiB in use and 59.1 MiB
# left (`dumpe2fs -h` on the image the build made). A build that no longer fits
# says so and stops; it does not fail quietly, and raising this number again is
# an ordinary upgrade -- it is the data size, not this one, that a reader
# cannot be upgraded across (see below).
#
# What the 64 MiB buys: the version that changes this image reads 64 MiB less
# on its first boot, 2.3 s at the 27.6 MiB/s measured here (576 MiB in 20.9 s),
# and the Kindle's own volume gets those 64 MiB back for books -- this device
# goes from 643.0 MiB free to about 707 MiB.
BOARD_SYSTEMIMAGE_PARTITION_SIZE := 536870912
# 3584 MiB (3.5 GiB) on the private/128gb-storage branch (2026-11-10): twice the
# 1.75 GiB this device shipped with, and as much as a file on the user FAT can
# hold while staying under FAT32's 4 GiB file limit -- the system image is a
# second file on the same volume. The sizing evidence for the 1.75 GiB that
# preceded it is in this file's history.
#
# A size change here has to go with --data-format (installer/build_payload.py),
# because an installer keeps an existing data image only of its own format and
# its own size. This branch does not refuse the old one with E304: an image of
# the previous format and size is grown in place -- the installer extends the
# file on the user store (install.c, grow_data) and the entry grows the
# filesystem inside it before Android mounts it (preinit V13, data image grown
# in place). Nothing the reader put in /data is lost. An image that cannot be
# grown is still refused with E304 and the device keeps running what it has.
BOARD_USERDATAIMAGE_PARTITION_SIZE := 3758096384
BOARD_FLASH_BLOCK_SIZE := 4096
# Keep framework and APK dex preparation off the 512 MB target at first boot.
# The product selects speed compilation only for its core UI applications.
WITH_DEXPREOPT := true
WITH_DEXPREOPT_BOOT_IMG_AND_SYSTEM_SERVER_ONLY := false
BOARD_HAL_STATIC_LIBRARIES := libhealthd.pw3
# The font configuration this build installs instead of AOSP's: the same file
# with the fallback families for scripts that have no locale or keyboard here
# deleted. It is read by frameworks/base/data/fonts/Android.mk (patch 0052),
# which keeps one module on /system/etc/fonts.xml -- a second module installing
# that path does not override the first, it loses to it silently, because kati
# merges the two copy rules into one and keeps the first recipe.
REPLACEMENT_FONTS_FILE := device/amazon/pw3/fonts/fonts.xml
DEVICE_MANIFEST_FILE := device/amazon/pw3/manifest.xml
BOARD_SEPOLICY_DIRS += device/amazon/pw3/sepolicy
BOARD_PLAT_PRIVATE_SEPOLICY_DIR := device/amazon/pw3/sepolicy-private

include device/amazon/pw3/wifi/BoardConfig-wifi.mk

TARGET_ANDROID_FILESYSTEM_CONFIG_H := device/amazon/pw3/android_filesystem_config.h

PW3_USB_COMPONENT_ROOT := vendor/amazon/pw3-hardware
include $(PW3_USB_COMPONENT_ROOT)/userspace-patch/BoardConfig-usb.mk
