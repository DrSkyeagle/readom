# Include from this PW3 device's BoardConfig.mk after applying both AOSP patches.
BOARD_WLAN_DEVICE := ath6kl
BOARD_WPA_SUPPLICANT_DRIVER := NL80211
WPA_SUPPLICANT_VERSION := VER_0_8_X
BOARD_PW3_WIFI_MODULE_CONTROL := true
# No private vendor driver-command library; COUNTRY uses standard nl80211 patch.
# No WIFI_DRIVER_FW_PATH_* definitions and no advertised vendor Wi-Fi HIDL HAL.
