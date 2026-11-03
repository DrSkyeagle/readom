/* SPDX-License-Identifier: Apache-2.0 */
/* power-log OUT INTERVAL_S PHASE...: battery current against what the device is
 * doing, without starting a process per sample. PHASE is
 * GOVERNOR[+io]:SECONDS[,ACTION]... where GOVERNOR+io is ondemand with
 * io_is_busy=1 (plain ondemand sets it to 0), and each ACTION is one of
 *   freq=KHZ   hold that frequency (the governor is then "userspace")
 *   load=N     keep N threads spinning for the phase (N up to 4)
 *   fl=PERMIL  frontlight at that share of its range (0-1000)
 * At the start of each phase the governor and the actions are applied; every
 * INTERVAL_S a line goes to OUT (flushed and synced, so that it survives a flat
 * battery):
 *   ms phase governor cur_khz current_now_ua current_avg_ua voltage_uv
 *   charge_uah capacity busy_permille charger_online
 * ms is CLOCK_MONOTONIC since start; busy_permille is the CPU's non-idle share
 * since the previous line (/proc/stat). At the end the governor, io_is_busy and
 * frontlight found at start are put back. Only the max77696 fuel gauge and
 * backlight of the PW3 are read and written.
 * Run as root, with the cable out for the numbers to mean anything.
 *
 * The phases of a power_profile.xml measurement are in
 * work/android-port/build-share/p7-power/PLAN.md. */
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define BAT "/sys/class/power_supply/max77696-battery/"
#define GOV "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"
#define CUR "/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq"
#define IOB "/sys/devices/system/cpu/cpufreq/ondemand/io_is_busy"
#define SETSPEED "/sys/devices/system/cpu/cpu0/cpufreq/scaling_setspeed"
#define BL "/sys/class/backlight/max77696-bl/"
#define MAX_LOAD 4

static int read_text(const char *path, char *buf, size_t size) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, size - 1);
    close(fd);
    if (n < 0) return -1;
    buf[n] = 0;
    char *nl = strchr(buf, '\n');
    if (nl) *nl = 0;
    return 0;
}

static long read_long(const char *path) {
    char buf[64];
    return read_text(path, buf, sizeof buf) ? -1 : strtol(buf, NULL, 10);
}

static int write_text(const char *path, const char *text) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = write(fd, text, strlen(text));
    close(fd);
    return n == (ssize_t)strlen(text) ? 0 : -1;
}

static long long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* Non-idle and total jiffies from the first line of /proc/stat. */
static void cpu_jiffies(unsigned long long *busy, unsigned long long *total) {
    char buf[256];
    unsigned long long v[8] = {0};
    *busy = *total = 0;
    if (read_text("/proc/stat", buf, sizeof buf)) return;
    if (sscanf(buf, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3],
               &v[4], &v[5], &v[6], &v[7]) < 7) return;
    for (int i = 0; i < 8; i++) *total += v[i];
    *busy = *total - v[3] - v[4];   /* idle and iowait */
}

/* A spinning thread, so that a phase can ask for a known CPU load without
 * starting a process. The work is integer only: no VFP, so that the frequency
 * is what distinguishes the phases and not which unit the code happens to use. */
static atomic_int load_running;

static void *load_thread(void *arg) {
    volatile unsigned long long x = (unsigned long long)(uintptr_t)arg;
    while (atomic_load_explicit(&load_running, memory_order_relaxed)) {
        for (int i = 0; i < 1000000; i++) x = x * 6364136223846793005ULL + 1442695040888963407ULL;
    }
    return (void *)(uintptr_t)x;
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: power-log OUT INTERVAL_S GOVERNOR[+io]:SECONDS...\n"); return 2; }
    const long interval = strtol(argv[2], NULL, 10);
    if (interval < 1 || interval > 600) { fprintf(stderr, "interval out of range\n"); return 2; }
    char gov0[32], iob0[16] = "", bl0[16] = "";
    if (read_text(GOV, gov0, sizeof gov0)) { perror(GOV); return 1; }
    read_text(IOB, iob0, sizeof iob0);   /* absent until ondemand is first selected */
    read_text(BL "brightness", bl0, sizeof bl0);
    const long bl_max = read_long(BL "max_brightness");
    FILE *out = fopen(argv[1], "a");
    if (!out) { perror(argv[1]); return 1; }
    const long long start = now_ms();
    unsigned long long b0, t0;
    cpu_jiffies(&b0, &t0);
    int status = 0;
    for (int p = 3; p < argc && !status; p++) {
        char gov[32], actions[128] = "";
        long seconds;
        int io = 0, threads = 0;
        if (sscanf(argv[p], "%31[^:]:%ld,%127s", gov, &seconds, actions) < 2 || seconds < 1) {
            fprintf(stderr, "bad phase %s\n", argv[p]); status = 2; break;
        }
        char *plus = strstr(gov, "+io");
        if (plus) { *plus = 0; io = 1; }
        long freq = 0, fl = -1;
        for (char *a = strtok(actions, ","); a; a = strtok(NULL, ",")) {
            if (!strncmp(a, "freq=", 5)) freq = strtol(a + 5, NULL, 10);
            else if (!strncmp(a, "load=", 5)) threads = (int)strtol(a + 5, NULL, 10);
            else if (!strncmp(a, "fl=", 3)) fl = strtol(a + 3, NULL, 10);
            else { fprintf(stderr, "bad action %s\n", a); status = 2; break; }
        }
        if (status) break;
        if (threads < 0 || threads > MAX_LOAD || fl > 1000) {
            fprintf(stderr, "action out of range in %s\n", argv[p]); status = 2; break;
        }
        if (freq) {
            char value[16];
            snprintf(value, sizeof value, "%ld", freq);
            if (write_text(GOV, "userspace") || write_text(SETSPEED, value)) {
                fprintf(stderr, "cannot hold %ld kHz\n", freq); status = 1; break;
            }
            strcpy(gov, "userspace");
        } else if (write_text(GOV, gov)) {
            fprintf(stderr, "cannot select %s\n", gov); status = 1; break;
        }
        if (!strcmp(gov, "ondemand")) write_text(IOB, io ? "1" : "0");
        if (fl >= 0 && bl_max > 0) {
            char value[16];
            snprintf(value, sizeof value, "%ld", bl_max * fl / 1000);
            write_text(BL "brightness", value);
        }
        pthread_t load[MAX_LOAD];
        int started = 0;
        if (threads) {
            atomic_store(&load_running, 1);
            for (int i = 0; i < threads; i++)
                if (!pthread_create(&load[i], NULL, load_thread, (void *)(uintptr_t)(i + 1))) started++;
        }
        const long long end = now_ms() + seconds * 1000LL;
        while (now_ms() < end) {
            sleep((unsigned)interval);
            unsigned long long b1, t1;
            cpu_jiffies(&b1, &t1);
            long busy = t1 > t0 ? (long)((b1 - b0) * 1000 / (t1 - t0)) : -1;
            b0 = b1; t0 = t1;
            fprintf(out, "%lld %d %s%s %ld %ld %ld %ld %ld %ld %ld %ld\n", now_ms() - start, p - 2, gov,
                    io ? "+io" : "", read_long(CUR), read_long(BAT "current_now"),
                    read_long(BAT "current_avg"), read_long(BAT "voltage_now"),
                    read_long(BAT "charge_now"), read_long(BAT "capacity"), busy,
                    read_long("/sys/class/power_supply/max77696-charger/online"));
            fflush(out);
            fsync(fileno(out));
        }
        if (started) {
            atomic_store(&load_running, 0);
            for (int i = 0; i < started; i++) pthread_join(load[i], NULL);
        }
    }
    if (iob0[0]) { write_text(GOV, "ondemand"); write_text(IOB, iob0); }
    write_text(GOV, gov0);
    if (bl0[0]) write_text(BL "brightness", bl0);
    fclose(out);
    return status;
}
