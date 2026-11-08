/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * What only the device itself has, read from the running stock system at the
 * moment the Android entry starts, and put into the form the new kernel and the
 * storage entry take:
 *
 *   ATAGs     board revision and ID, MAC address, boot mode, RAM banks (the
 *             stock kernel's /proc copies of what its boot loader passed), with
 *             the build's command line and initramfs range;
 *   archive   a cpio (newc) appended to the public initramfs: the panel's
 *             waveform (its own WBF from the panel flash with the WRF the stock
 *             system made from it), the Wi-Fi calibration and firmware from the
 *             stock root file system, and the storage entry's configuration
 *             with this device's eMMC geometry and FAT offset.
 *
 * Nothing is stored: every start reads the device again, so nothing private
 * leaves it or goes stale. Paths are read below `root` ("/" on the device; a
 * directory of captured files in tests). Every function returns 0 or -1 and
 * then names the failed check in `why`.
 */
#ifndef PW3_DEVICEDATA_H
#define PW3_DEVICEDATA_H
#include <stddef.h>
#include <stdint.h>

struct dd_buf { unsigned char *data; size_t size, cap; };
void dd_free(struct dd_buf *b);

/* What one supported model on one stock firmware fixes. */
struct dd_profile {
    const char *stock_cmdline;   /* /proc/cmdline without root= and rootwait */
    const char *user_mount;      /* the stock mount of the user FAT */
    const char *wifi_board;      /* stock path of the Wi-Fi calibration */
    const char *wifi_firmware;   /* stock path of the Wi-Fi firmware */
    /* The stock kernel itself, as /proc/version and uname report it. The
     * handover calls that kernel's own pm_test_show and L2 cache functions and
     * checks their bodies byte for byte (boot/handoff/native-handoff.h), so
     * another build of the same kernel is not a thing this one can be used on.
     * Both strings are in the profile JSON too, and a test keeps them equal
     * (tools/tests/test_profile_matches_devicedata.py). */
    const char *kernel_release;  /* uname -r, and what /proc/version names */
    const char *kernel_version;  /* uname -v: the build number, date and time */
};
extern const struct dd_profile dd_pw3_5_16_2_1_1;
extern const struct dd_profile dd_voyage_5_13_6;

/* Which of the profiles above this build is for. One helper binary serves one
 * model on one firmware, and boot/build_launcher.py sets this from the profile
 * JSON it was given ("device_data_profile"); the default is the one this
 * project has run on a device. A build for another model therefore cannot
 * silently keep checking the PW3's facts -- it either names a struct that
 * exists or it does not compile. */
#ifndef DD_PROFILE
#define DD_PROFILE dd_pw3_5_16_2_1_1
#endif

/* What the build fixes. */
struct dd_build {
    const char *cmdline;         /* complete kernel command line; starts with the profile's stock part */
    const char *preinit_conf;    /* pw3-preinit.conf without the fat_* and block= lines */
    const char *data_path;       /* data image, relative to the user FAT; NULL: no pw3-testdata.conf */
    uint32_t data_size;          /* its size in bytes */
};

/* `other_device` says the failure was the identity check: this build is for one
 * model on one firmware, and the running system is not it. The caller shows a
 * different screen for that, because telling someone with another Kindle to
 * restart and try again is telling them to do it for ever. */
struct dd_why { char text[160]; int other_device; };

/* Whether the running system is the model and firmware this build is for.
 * Cheap and side-effect free, for asking before anything is installed. */
int dd_device_matches(const char *root, const struct dd_profile *profile, struct dd_why *why);
int dd_atags(const char *root, const struct dd_profile *profile, const char *cmdline,
             uint32_t initrd_start, uint32_t initrd_size, struct dd_buf *out, struct dd_why *why);
int dd_archive(const char *root, const struct dd_profile *profile, const struct dd_build *build,
               struct dd_buf *out, struct dd_why *why);

/* Parts, for tests. */
uint32_t dd_crc32(uint32_t crc, const unsigned char *data, size_t size);
int dd_envelope(const unsigned char *panel, size_t panel_size, const unsigned char *wrf, size_t wrf_size,
                struct dd_buf *out, struct dd_why *why);
int dd_gzip_stored(const unsigned char *data, size_t size, struct dd_buf *out);
int dd_newc(struct dd_buf *out, const char *name, uint32_t mode, const unsigned char *data, size_t size);
int dd_newc_end(struct dd_buf *out);
#endif
