LOCAL_PATH := $(call my-dir)

# The browser is called "EinkBro" by the APK and carries its author's icon. The
# APK is the upstream release signed by him and is not ours to change: editing
# it would break the signature that is the reason this device can trust it.
# Someone who only wants to read something on the web should not have to know
# what EinkBro is, and the icon has to be one of the home screen's four marks
# rather than a fifth drawing, so both are changed the way Android itself is
# meant to change them -- a runtime resource overlay over that package's
# app_name and ic_launcher, which touches nothing inside the APK
# (docs/design/eink-ui.md, launcher-icons/PW3-SOURCE.md).
include $(CLEAR_VARS)
LOCAL_PACKAGE_NAME := PW3BrowserName
LOCAL_MODULE_TAGS := optional
LOCAL_SDK_VERSION := current
LOCAL_CERTIFICATE := platform
# BUILD_RRO_PACKAGE without LOCAL_RRO_THEME: installed in /vendor/overlay and
# applied always, rather than with a theme the device never sets. A static
# overlay (AndroidManifest.xml) is applied at boot with no setting to switch
# it, which is what a name should be.
include $(BUILD_RRO_PACKAGE)
