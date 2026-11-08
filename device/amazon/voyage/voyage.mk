# Kindle Voyage (7th generation, 2015; Wario, machine 4091) on stock firmware
# 5.13.6: the same machine as the Paperwhite 3 in everything this system is
# built from, so the product is the pw3 one with its identity replaced
# (docs/design/voyage-port.md).
#
# Why inherit instead of a second copy: Amazon builds both boards from one
# kernel configuration (imx60_wario_defconfig, BUILD_HOW-TO.txt in the stock
# source archive), the panel is the same 1072x1448 at the same 300 ppi, the RAM
# is the same 512 MB, and the SoC, PMIC, touch controller, Wi-Fi chip and hall
# sensor are the same parts (docs/design/porting-to-other-models.md sections 1
# and 6). A copied tree would be 300 files that have to be changed in pairs, and
# the first time they are not, the two models quietly build different systems.
#
# What is deliberately NOT overridden, and why:
#
#   * ro.hardware.gralloc/hwcomposer/lights/power = pw3, and the module names
#     gralloc.pw3, hwcomposer.pw3, lights.pw3, power.pw3, libhealthd.pw3: these
#     name the hardware implementation, not the product. Voyage runs the same
#     implementation -- same EPDC, same PXP, same max77696 PMIC -- so a second
#     copy under another name would be the same code twice, and the two copies
#     would then be able to disagree.
#   * init.pw3*.rc, ueventd.pw3.rc, fstab.pw3*, pw3-preinit.conf, and
#     androidboot.hardware=pw3 in boot/build_entry.py: those files are the device
#     tree, and they are the ones this product installs (initialization.mk,
#     inherited below). androidboot.hardware is the name init looks the rc files
#     up by, so it has to stay "pw3" even though the product is not.
#   * everything under overlay/, fonts/, apps/, sepolicy/, epdc/, gralloc/,
#     composer/, sleep/, input/: same panel, same density, same keys, same
#     policy. A Voyage-only difference here would need a measurement on a Voyage
#     to justify it, and none has been made.
#
# What is overridden: the four product identity variables, and the board name in
# BoardConfig.mk. That is the whole of the port on this side.

LOCAL_PATH := device/amazon/voyage

$(call inherit-product, device/amazon/pw3/pw3.mk)

# pw3.mk sets LOCAL_PATH to its own directory at its top (it uses it for its
# PRODUCT_COPY_FILES and for initialization.mk). Everything below is this
# directory's, so it is set again rather than inherited.
LOCAL_PATH := device/amazon/voyage

PRODUCT_NAME := voyage
PRODUCT_DEVICE := voyage

# The name users see is Readom (Kindle library entry, installer); the model is
# what Windows shows for the MTP device. Test builds keep a development name so
# that a test build does not look like a release to an application that is
# logged in -- the same rule, and the same reason, as pw3.mk.
ifeq ($(TARGET_BUILD_VARIANT),user)
PRODUCT_BRAND := Readom
PRODUCT_MODEL := Kindle Voyage
else
PRODUCT_BRAND := VoyageLocalPort
PRODUCT_MODEL := Kindle Voyage development port
endif

# Nothing else: no Voyage-only property has been measured or is known to be
# needed. When one is, it goes here with the reading that justifies it, not into
# pw3.mk (which would then claim a fact about a machine it was not measured on).
