# Kindle Voyage (7th generation, 2015). Everything the board is, is inherited
# from the Paperwhite 3: same i.MX6SL, same armv7-a-neon Cortex-A9, same 512 MiB
# system image and 1.75 GiB data image, same SELinux policy, same font
# configuration, same USB component (docs/design/voyage-port.md).
#
# The one thing that changes is the board's name: TARGET_BOOTLOADER_BOARD_NAME is
# the identity of the board this build is for, where everything else in the pw3
# file is the silicon and the partition plan both boards share. It is not the
# hardware implementation's name -- ro.hardware.* and the gralloc/hwcomposer/
# lights/power module names stay "pw3" on purpose (voyage.mk says why).
include device/amazon/pw3/BoardConfig.mk

TARGET_BOOTLOADER_BOARD_NAME := voyage
