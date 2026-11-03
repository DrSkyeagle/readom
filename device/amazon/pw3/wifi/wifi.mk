# Change PW3_WIFI_CANDIDATE_PATH when copying this candidate into the device tree.
PW3_WIFI_CANDIDATE_PATH := device/amazon/pw3/wifi
PRODUCT_PACKAGES += wificond wpa_supplicant
PRODUCT_COPY_FILES += \
    $(PW3_WIFI_CANDIDATE_PATH)/wpa_supplicant.conf:$(TARGET_COPY_OUT_VENDOR)/etc/wifi/wpa_supplicant.conf \
    $(PW3_WIFI_CANDIDATE_PATH)/init.pw3-wifi.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/init.pw3-wifi.rc \
    frameworks/native/data/etc/android.hardware.wifi.xml:system/etc/permissions/android.hardware.wifi.xml
PRODUCT_PROPERTY_OVERRIDES += wifi.interface=wlan0
# Merge manifest-wifi.xml's supplicant HAL element into the device manifest.
# Do not install android.hardware.wifi@1.0-service with libwifi-hal-fallback.

# No RTT implementation is provided without a vendor IWifi HAL.
PRODUCT_PROPERTY_OVERRIDES += config.disable_rtt=true
