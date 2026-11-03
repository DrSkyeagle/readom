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
# 1792 MiB (1.75 GiB), set on 2026-10-15 and kept on 2026-11-03 after the two
# sizes were worked out again against what the device reports. Everything below
# is `df` on the device on 2026-11-03, with the one application that matters
# installed, logged in and a book downloaded:
#
#   the Kindle's own volume   3113.9 MiB, of which 643.0 MiB free
#   /system (576 MiB image)    566.9 MiB, 445.9 MiB used, 79%
#   /data   (1792 MiB image)  1763.8 MiB, 748.7 MiB used, 43%
#
# What fills /data is downloaded books, so the question is not the 43% but what
# is left: 1015.1 MiB, which is hundreds of books of text. That is what makes
# this size sufficient, and it is why 1536 MiB was prepared on 2026-11-02 and
# turned down by the owner on 2026-11-03: it would have taken 256 MiB from the
# one thing /data is for, to buy room on a volume that already has 643 MiB
# free.
#
# 2048 MiB was the other candidate -- a power of two, and the two images would
# then be 2.5 GiB exactly. It was not taken, for two reasons that are both
# arithmetic: a clean install would need 512 + 2048 + 64 MiB = 2.75 GB of a
# 3.27 GB volume, so a reader with more than about 465 MB of their own books
# could not install at all; and 256 MiB more for books that the measurement
# above says are not needed costs every reader that. The room this version
# hands back comes out of the system image instead, which is read-only and
# measured, not guessed: 64 MiB of it, so this device goes from 643.0 MiB free
# to about 707 MiB.
#
# The sizes are 512 MiB and 7 x 256 MiB; together 2304 MiB, which is 9 x 256
# MiB -- whole steps of a quarter of a GiB, with the read-only one a power of
# two.
#
# A size change here has to go with --data-format (installer/build_payload.py),
# because an installer keeps an existing data image only of its own format and
# its own size: a device holding the old one is refused with E304, changes no
# file, and goes on running the version it has until the reader deletes
# data.img themselves -- which resets their applications and books. That is why
# this number moves only in a version that is installed clean anyway, and why
# it did not move here: an upgrade into this version keeps what the reader has.
BOARD_USERDATAIMAGE_PARTITION_SIZE := 1879048192
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
