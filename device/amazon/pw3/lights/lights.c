/* SPDX-License-Identifier: Apache-2.0 */
#include <hardware/lights.h>
#include <log/log.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * The Kindle's own frontlight steps for this board (stock 5.16.2.1.1,
 * libfrontlight, LED type "wo5_4led" as the stock device configuration names
 * it for PW3): index 0..24 are the 25 positions of the Kindle slider, index 25
 * is its separate "max" switch. The values go to the max77696 driver as they
 * are (12 bit, linear current). Position 0 is the faintest light, not dark:
 * the Kindle only switches the light off with the screen.
 *
 * Stock tells two LED lots apart by the PMIC's AIN1 input, read once: at most
 * 99 selects the second table, anything else (also an unreadable input) the
 * first.
 */
#define PW3_FL_STEPS 26
static const uint16_t fl_lot_default[PW3_FL_STEPS] = {
    1, 2, 3, 4, 6, 14, 28, 49, 78, 120, 167, 245, 336, 465, 607, 775, 969,
    1162, 1382, 1627, 1898, 2195, 2531, 2893, 3280, 4095,
};
static const uint16_t fl_lot_low_ain1[PW3_FL_STEPS] = {
    1, 2, 3, 4, 5, 6, 7, 19, 34, 54, 89, 127, 191, 286, 445, 604, 764, 940,
    1089, 1253, 1433, 1627, 1830, 2056, 2291, 2529,
};
#define PW3_FL_LOT_INPUT "/sys/devices/platform/imx-i2c.0/i2c-0/0-003c/max77696-adc.0/ain1"
#define PW3_FL_LOT_LIMIT 99

struct pw3_light {
    struct light_device_t base;
    const uint16_t *steps;
};

static const uint16_t *select_steps(void) {
    char text[16] = "";
    int value = -1;
    int fd = open(PW3_FL_LOT_INPUT, O_RDONLY|O_CLOEXEC);
    /* The default table is a deliberate fallback, so this does not fail -- but
     * "could not read it" and "read it and it was high" both ended up as
     * input=-1 in the log, and only one of them is a device to look at. */
    if (fd < 0) ALOGW("frontlight lot input %s unreadable: %s", PW3_FL_LOT_INPUT, strerror(errno));
    if (fd >= 0) {
        ssize_t count;
        do { count = read(fd, text, sizeof(text) - 1); } while (count < 0 && errno == EINTR);
        if (count > 0) {
            char *end;
            long parsed = strtol(text, &end, 10);
            if (end != text && parsed >= 0 && parsed <= 4095) value = (int)parsed;
        }
        close(fd);
    }
    int low = value >= 0 && value <= PW3_FL_LOT_LIMIT;
    ALOGI("frontlight steps: lot input=%d table=%s", value, low ? "low-ain1" : "default");
    return low ? fl_lot_low_ain1 : fl_lot_default;
}

/*
 * Android hands over 0 for "off" and 1..255 for "on". The 255 "on" values are
 * spread evenly over the 26 Kindle steps, so every value an application or the
 * system slider can ask for lands on a step the Kindle itself has.
 */
static unsigned raw_intensity(const uint16_t *steps, unsigned brightness) {
    if (!brightness) return 0;
    return steps[((brightness - 1) * (PW3_FL_STEPS - 1) + 127) / 254];
}

static int set_light(struct light_device_t *device, const struct light_state_t *state) {
    if (!device || !state || state->flashMode != LIGHT_FLASH_NONE) return -EINVAL;
    unsigned color = state->color;
    unsigned level = (77 * ((color >> 16) & 255) + 150 * ((color >> 8) & 255) + 29 * (color & 255) + 128) >> 8;
    /* The kernel applies its blank state independently of this value. */
    char text[16];
    int count = snprintf(text, sizeof(text), "%u\n", raw_intensity(((struct pw3_light *)device)->steps, level));
    int fd = open("/sys/class/backlight/max77696-bl/brightness", O_WRONLY|O_CLOEXEC);
    if (fd < 0) return -errno;
    ssize_t written;
    do { written = write(fd, text, count); } while (written < 0 && errno == EINTR);
    int status = written == count ? 0 : (written < 0 ? -errno : -EIO);
    close(fd);
    return status;
}
static int close_light(struct hw_device_t *device) { free(device); return 0; }
static int open_light(const struct hw_module_t *module, const char *name, struct hw_device_t **out) {
    if (!out || !name || strcmp(name, LIGHT_ID_BACKLIGHT)) return -EINVAL;
    struct pw3_light *device = calloc(1, sizeof(*device));
    if (!device) return -ENOMEM;
    device->base.common.tag = HARDWARE_DEVICE_TAG;
    device->base.common.version = LIGHTS_DEVICE_API_VERSION_1_0;
    device->base.common.module = (struct hw_module_t*)module;
    device->base.common.close = close_light;
    device->base.set_light = set_light;
    device->steps = select_steps();
    *out = &device->base.common;
    return 0;
}
static struct hw_module_methods_t methods = { .open = open_light };
struct hw_module_t HAL_MODULE_INFO_SYM = {
    .tag = HARDWARE_MODULE_TAG,
    .module_api_version = HARDWARE_MODULE_API_VERSION(1, 0),
    .hal_api_version = HARDWARE_HAL_API_VERSION,
    .id = LIGHTS_HARDWARE_MODULE_ID,
    .name = "PW3 monochrome frontlight",
    .author = "PW3 local port project",
    .methods = &methods,
};
