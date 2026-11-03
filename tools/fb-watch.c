/* SPDX-License-Identifier: Apache-2.0 */
/* fb-watch SECONDS [INTERVAL_MS]: what the panel is given, sampled from the
 * framebuffer without starting a process per sample. Every INTERVAL_MS (20) it
 * reads the buffer being shown (FBIOGET_VSCREENINFO's yoffset; the compositor
 * pans between buffers) on a grid of every 8th pixel and prints a line when that
 * changes: milliseconds since start (CLOCK_MONOTONIC), yoffset, mean grey
 * (0-255), and the per-mille of the grid below 32 (near black) and above 223
 * (near white). Read-only: mmap PROT_READ, no ioctl that sets anything. For
 * catching a frame such as the black one at a cold start (D5). Run as root. */
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static long long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) { fprintf(stderr, "usage: fb-watch SECONDS [INTERVAL_MS]\n"); return 2; }
    const long seconds = strtol(argv[1], NULL, 10), interval = argc == 3 ? strtol(argv[2], NULL, 10) : 20;
    if (seconds <= 0 || seconds > 600 || interval < 5 || interval > 1000) { fprintf(stderr, "out of range\n"); return 2; }
    int fd = open("/dev/graphics/fb0", O_RDONLY | O_CLOEXEC);
    if (fd < 0) { perror("fb0"); return 1; }
    struct fb_var_screeninfo var; struct fb_fix_screeninfo fix;
    if (ioctl(fd, FBIOGET_FSCREENINFO, &fix) || ioctl(fd, FBIOGET_VSCREENINFO, &var)) { perror("screeninfo"); return 1; }
    if (var.bits_per_pixel != 8) { fprintf(stderr, "bits_per_pixel %u, need 8\n", var.bits_per_pixel); return 1; }
    const unsigned char *fb = mmap(NULL, fix.smem_len, PROT_READ, MAP_SHARED, fd, 0);
    if (fb == MAP_FAILED) { perror("mmap"); return 1; }
    printf("# xres=%u yres=%u stride=%u yres_virtual=%u interval_ms=%ld\n", var.xres, var.yres, fix.line_length, var.yres_virtual, interval);
    printf("# ms yoffset mean black_permille white_permille\n");
    const long long start = now_ms(), end = start + seconds * 1000;
    long last_mean = -1, last_black = -1, last_white = -1; unsigned last_y = ~0u;
    for (long long t = start; t < end; t = now_ms()) {
        if (ioctl(fd, FBIOGET_VSCREENINFO, &var)) { perror("vscreeninfo"); return 1; }
        if ((size_t)(var.yoffset + var.yres) * fix.line_length > fix.smem_len) { fprintf(stderr, "yoffset out of range\n"); return 1; }
        unsigned long sum = 0, n = 0, black = 0, white = 0;
        for (unsigned y = 0; y < var.yres; y += 8) {
            const unsigned char *row = fb + (size_t)(var.yoffset + y) * fix.line_length;
            for (unsigned x = 0; x < var.xres; x += 8) {
                const unsigned v = row[x];
                sum += v; n++; black += v < 32; white += v > 223;
            }
        }
        const long mean = (long)(sum / n), bp = (long)(black * 1000 / n), wp = (long)(white * 1000 / n);
        if (mean != last_mean || bp != last_black || wp != last_white || var.yoffset != last_y) {
            printf("%lld %u %ld %ld %ld\n", t - start, var.yoffset, mean, bp, wp);
            fflush(stdout);
            last_mean = mean; last_black = bp; last_white = wp; last_y = var.yoffset;
        }
        usleep((useconds_t)interval * 1000);
    }
    return 0;
}
