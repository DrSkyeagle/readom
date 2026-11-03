/* SPDX-License-Identifier: Apache-2.0 */
#include <hardware/power.h>
#include <linux/fb.h>
#include <log/log.h>
#include <cutils/properties.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int composer_owns_epdc(void) {
    char name[PROPERTY_VALUE_MAX];
    property_get("ro.hardware.hwcomposer", name, "");
    return strcmp(name, "pw3") == 0;
}

static void interactive(struct power_module *module, int on) {
    (void)module;
    /* Android-only backlight blanking patch preserves requested brightness. */
    int fd = open("/sys/class/backlight/max77696-bl/bl_power", O_WRONLY|O_CLOEXEC);
    if (fd < 0) ALOGE("Cannot open frontlight blanking: %d", errno);
    else {
        const char* value = on ? "0\n" : "4\n";
        ssize_t written;
        do { written = write(fd, value, 2); } while (written < 0 && errno == EINTR);
        if (written != 2) ALOGE("Frontlight blanking failed: %d", written < 0 ? errno : EIO);
        close(fd);
    }
    /* With HWC enabled, its serialized blank/post path owns EPDC power.
     * setInteractive is not the display's power state and may arrive before
     * or after HWC's callback. Keep legacy framebuffer control without HWC. */
    if (composer_owns_epdc()) return;
    fd = open("/dev/graphics/fb0", O_RDWR|O_CLOEXEC);
    if (fd < 0) ALOGE("Cannot open EPDC power control: %d", errno);
    else {
        int rc;
        do { rc = ioctl(fd, FBIOBLANK, on ? FB_BLANK_UNBLANK : FB_BLANK_POWERDOWN); }
        while (rc < 0 && errno == EINTR);
        if (rc < 0)
            ALOGE("EPDC blanking failed: %d", errno);
        close(fd);
    }
}
/* PowerHint.LAUNCH: ActivityManager sends 1 when it starts an activity in another process,
 * and 0 (data NULL) once the resumed activities are idle (at the latest IDLE_TIMEOUT, 10 s,
 * after the resume) or before sleep. While it lasts the lowest frequency is the highest one:
 * a scaling governor (ondemand) runs the whole start at full speed; under performance it
 * changes nothing. Touches need no hint: the Lab126 touch and key drivers raise the frequency
 * themselves (cpufreq_override()), before the event reaches user space. */
#define CPUFREQ "/sys/devices/system/cpu/cpu0/cpufreq/"
static char cpu_min[16], cpu_max[16];

static int read_line(const char* path, char* value, size_t size) {
    int fd = open(path, O_RDONLY|O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n;
    do { n = read(fd, value, size - 1); } while (n < 0 && errno == EINTR);
    /* close() may set errno of its own, and the caller reports this one. */
    const int why = n < 0 ? errno : (n == 0 ? ENODATA : 0);
    close(fd);
    if (n <= 0) { errno = why; return -1; }
    value[n] = 0;
    return 0;
}

static void set_min_freq(const char* value) {
    int fd = open(CPUFREQ "scaling_min_freq", O_WRONLY|O_CLOEXEC);
    if (fd < 0) { ALOGE("Cannot open scaling_min_freq: %d", errno); return; }
    size_t len = strlen(value);
    ssize_t written;
    do { written = write(fd, value, len); } while (written < 0 && errno == EINTR);
    if (written != (ssize_t)len) ALOGE("Setting scaling_min_freq failed: %d", written < 0 ? errno : EIO);
    close(fd);
}

static void initialize(struct power_module *module) {
    (void)module;
    if (composer_owns_epdc()) ALOGI("EPDC power owned by HWC; Power HAL controls frontlight");
    /* Which one and why: without it this reads as "this kernel has no cpufreq",
     * and a denial reads the same as a missing file. */
    const char *which = NULL;
    if (read_line(CPUFREQ "cpuinfo_min_freq", cpu_min, sizeof(cpu_min)) < 0) which = "cpuinfo_min_freq";
    else if (read_line(CPUFREQ "cpuinfo_max_freq", cpu_max, sizeof(cpu_max)) < 0) which = "cpuinfo_max_freq";
    if (which) {
        ALOGE("Cannot read %s%s: %s; no launch boost", CPUFREQ, which, strerror(errno));
        cpu_max[0] = 0;
    }
}
static void hint(struct power_module *module, power_hint_t value, void *data) {
    (void)module;
    if (value != POWER_HINT_LAUNCH || !cpu_max[0]) return;
    set_min_freq(data && *(const int32_t*)data ? cpu_max : cpu_min);
}
struct power_module HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = POWER_MODULE_API_VERSION_0_2,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = POWER_HARDWARE_MODULE_ID,
        .name = "PW3 EPDC and frontlight power",
        .author = "PW3 local port project",
    },
    .init = initialize, .setInteractive = interactive, .powerHint = hint,
};
