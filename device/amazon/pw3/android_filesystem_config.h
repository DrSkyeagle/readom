// SPDX-License-Identifier: Apache-2.0
// AOSP8.1 fs_config compares directory prefixes without a trailing slash.
// Keep these private per-device assets root-owned and non-executable.
static const struct fs_path_config android_device_files[] = {
    // mkbootfs resets unknown root files to 0644. This is a real PID1 entry.
    { 0750, AID_ROOT, AID_ROOT, 0, "pw3-testfile-init" },
    { 0600, AID_ROOT, AID_ROOT, 0, "system/vendor/firmware/ath6k/AR6003/hw2.1.1/board.bin" },
    { 0600, AID_ROOT, AID_ROOT, 0, "system/vendor/firmware/ath6k/AR6003/hw2.1.1/fw-4.bin" },
    { 0600, AID_ROOT, AID_ROOT, 0, "system/vendor/firmware/epdc/pw3/waveform.pw3.gz" },
};
static const struct fs_path_config android_device_dirs[] = {
    { 0700, AID_ROOT, AID_ROOT, 0, "system/vendor/firmware/ath6k" },
    { 0700, AID_ROOT, AID_ROOT, 0, "system/vendor/firmware/ath6k/AR6003" },
    { 0700, AID_ROOT, AID_ROOT, 0, "system/vendor/firmware/ath6k/AR6003/hw2.1.1" },
    { 0700, AID_ROOT, AID_ROOT, 0, "system/vendor/firmware/epdc" },
    { 0700, AID_ROOT, AID_ROOT, 0, "system/vendor/firmware/epdc/pw3" },
};
