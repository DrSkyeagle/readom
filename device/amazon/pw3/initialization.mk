# Include from pw3.mk. The storage contract check and the boot report.
PRODUCT_PACKAGES += pw3-init-check
# Not PRODUCT_PACKAGES_DEBUG, which is where this sat from the first import
# until 2026-10-23. That variable is added to the image only for a userdebug or
# eng build, so the sampler was in every build that already has adb and root,
# and in none of the builds it was written for: a release has no adb, its
# kernel ring is 32 KB and gone within hours, and the report area is the only
# thing its reader can send (device/amazon/pw3/report/ReportArea.h). The V78
# release was checked on the device and had neither /system/bin/pw3-boot-report
# nor its .rc; nothing said so, because an absent service leaves no .rc behind
# to dangle, and init.svc.pw3-boot-report simply never existed.
PRODUCT_PACKAGES += pw3-boot-report
PRODUCT_DEFAULT_PROPERTY_OVERRIDES += ro.adb.secure=1 ro.secure=1
# USB is MTP (docs/design/usb-mtp.md). A debuggable build gets ",adb" added by
# the build (post_process_props.py); a release only when developer options
# turn adb on, and every computer must then be authorised.
PRODUCT_DEFAULT_PROPERTY_OVERRIDES += persist.sys.usb.config=mtp
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/rootdir/init.pw3-main.rc:root/init.pw3-main.rc \
    $(LOCAL_PATH)/rootdir/init.pw3.rc:root/init.pw3.rc \
    $(LOCAL_PATH)/rootdir/init.pw3.usb.rc:root/init.pw3.usb.rc \
    $(LOCAL_PATH)/rootdir/ueventd.pw3.rc:root/ueventd.pw3.rc \
    $(LOCAL_PATH)/rootdir/fstab.pw3:root/fstab.pw3 \
    $(LOCAL_PATH)/rootdir/fstab.pw3-testfile:root/fstab.pw3-testfile \
    $(LOCAL_PATH)/rootdir/fstab.pw3-swap:root/fstab.pw3-swap

# The kernel command line this expects is boot/build_entry.py's
# CMDLINE_ANDROID; that list is the one definition, so it is not copied
# here to drift out of date. No host key, private or public, is embedded.
