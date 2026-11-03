/* PW3 handoff restore - runs once as the first process of the Android test
 * initramfs, in place of the storage entry it then executes.
 *
 * Why: the test kernel is entered from inside the stock kernel's suspend path.
 * The stock suspend callbacks have run, their resume halves never will, and a
 * normal cold boot gets the same things from the boot loader instead:
 *   - PMIC LDO7 ("keeper") is at its suspend level, 1.8 V instead of 3.2 V.
 *     At 1.8 V the panel does not respond correctly to any update (verified on
 *     the device, Handoff Probe 2).
 *   - BUCK1 is still assigned to the suspend power sequencer.
 * This program puts exactly those registers back, and only when they hold the
 * known suspend values.
 *
 * It then loads the EPDC driver from the initramfs and puts the installer's own
 * "正在启动 Android" back on the panel -- the driver's initialisation clears it
 * and shows its own freshly allocated buffer, so without this the reader is
 * left looking at nothing until Android's first frame -- and finally executes
 * the unmodified storage entry as PID 1 with the root file system exactly as it
 * found it.
 *
 * V28 also records the CPU clock state (registers and a measured loop rate)
 * at the start and at the end, and asks cpufreq for its "performance"
 * governor if another one is active: probes after a handoff found the CPU on
 * the 396 MHz step clock with PLL1 off, and nothing here needs power saving.
 *
 * No block device is opened and nothing is written outside RAM.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <linux/fb.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include "cpu-probe.h"
#include "start-picture.h"

#ifndef KERNEL_RELEASE /* overridden only for the emulator self-test */
#define KERNEL_RELEASE "3.0.35-lab126-pw3-android81"
#endif
#define REAL_ENTRY "/pw3-testfile-init.real"
#define REAL_ARGV0 "/pw3-testfile-init"
#define MODULE_FILE "/pw3-handoff/mxc_epdc_fb.ko"
#define FIRMWARE_FILE "/firmware/image/epdc/pw3/waveform.pw3.gz"
#define STATUS_FILE "/pw3-handoff/status.txt"

/* ---- Lab126 framebuffer ABI (include/linux/mxcfb.h, 5.16.2.1.1) ---- */
struct rect { uint32_t top, left, width, height; };
struct update {
	struct rect region;
	uint32_t waveform, mode, marker, hist_bw, hist_gray;
	int32_t temperature;
	uint32_t flags;
	struct { uint32_t address, width, height; struct rect region; } alternate;
};
struct marker { uint32_t value, collision; };
_Static_assert(sizeof(struct update) == 72, "Lab126 update ABI");
#define MXCFB_SET_AUTO_UPDATE_MODE _IOW('F', 0x2D, uint32_t)
#define MXCFB_SEND_UPDATE _IOW('F', 0x2E, struct update)
#define MXCFB_WAIT_FOR_UPDATE_COMPLETE _IOWR('F', 0x2F, struct marker)
#define MXCFB_SET_UPDATE_SCHEME _IOW('F', 0x32, uint32_t)
#define WAVEFORM_MODE_GC16 2u
#define UPDATE_MODE_FULL 1u
#define TEMP_USE_AUTO 0x1001

static int kmsgfd = -1, statusfd = -1;

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static void msleep(unsigned ms)
{
	struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000L };
	while (nanosleep(&t, &t) && errno == EINTR) { }
}

/* One line to the kernel log and to the status file kept in the RAM root. */
static void P(const char *fmt, ...)
{
	char b[600];
	int n = snprintf(b, sizeof(b), "<6>pw3-handoff: ");
	va_list ap;
	va_start(ap, fmt);
	int m = vsnprintf(b + n, sizeof(b) - (size_t)n - 2, fmt, ap);
	va_end(ap);
	if (m < 0) return;
	if ((size_t)m > sizeof(b) - (size_t)n - 2) m = (int)(sizeof(b) - (size_t)n - 2);
	n += m;
	b[n++] = '\n';
	if (kmsgfd >= 0) (void)!write(kmsgfd, b, (size_t)n);
	if (statusfd >= 0) (void)!write(statusfd, b + 3, (size_t)n - 3);
}

static ssize_t slurp(const char *path, char *buf, size_t max)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) return -1;
	size_t used = 0;
	for (;;) {
		ssize_t k = read(fd, buf + used, max - 1 - used);
		if (k < 0 && errno == EINTR) continue;
		if (k <= 0) break;
		used += (size_t)k;
		if (used >= max - 1) break;
	}
	close(fd);
	buf[used] = 0;
	return (ssize_t)used;
}

static unsigned char *load_file(const char *path, size_t *len)
{
	struct stat st;
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0 || fstat(fd, &st) || st.st_size <= 0 || st.st_size > (12 << 20)) { if (fd >= 0) close(fd); return 0; }
	unsigned char *p = malloc((size_t)st.st_size);
	size_t used = 0;
	while (p && used < (size_t)st.st_size) {
		ssize_t k = read(fd, p + used, (size_t)st.st_size - used);
		if (k < 0 && errno == EINTR) continue;
		if (k <= 0) break;
		used += (size_t)k;
	}
	close(fd);
	if (!p || used != (size_t)st.st_size) { free(p); return 0; }
	*len = used;
	return p;
}

static int put_string(const char *path, const char *s)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0) return -1;
	ssize_t k = write(fd, s, strlen(s));
	int e = errno;
	close(fd);
	errno = e;
	return k == (ssize_t)strlen(s) ? 0 : -1;
}

static int node(const char *path, unsigned major_no, unsigned minor_no, int flags)
{
	unlink(path);
	if (mknod(path, S_IFCHR | 0600, makedev(major_no, minor_no))) return -1;
	return open(path, flags | O_CLOEXEC);
}

/* ---- PMIC: MAX77696 core block, I2C bus 0, address 0x3C ---- */
static int i2cfd = -1;

static int pmic_rd(unsigned reg, uint8_t *v)
{
	uint8_t a = (uint8_t)reg;
	struct i2c_msg m[2] = { { 0x3C, 0, 1, &a }, { 0x3C, I2C_M_RD, 1, v } };
	struct i2c_rdwr_ioctl_data d = { m, 2 };
	if (i2cfd < 0) return -1;
	return ioctl(i2cfd, I2C_RDWR, &d) == 2 ? 0 : -1;
}

static int pmic_wr(unsigned reg, uint8_t v)
{
	uint8_t b[2] = { (uint8_t)reg, v };
	struct i2c_msg m = { 0x3C, 0, 2, b };
	struct i2c_rdwr_ioctl_data d = { &m, 1 };
	if (i2cfd < 0) return -1;
	return ioctl(i2cfd, I2C_RDWR, &d) == 1 ? 0 : -1;
}

/* Put one register back to its run value, but only from the exact value the
 * stock suspend callback leaves behind.  Returns 1 when the run value is in place. */
static int restore(const char *name, unsigned reg, uint8_t suspend_value, uint8_t run_value)
{
	uint8_t before = 0, after = 0;
	if (pmic_rd(reg, &before)) { P("PMIC %s reg=%02x unreadable errno=%d", name, reg, errno); return 0; }
	if (before == run_value) { P("PMIC %s reg=%02x already=%02x", name, reg, before); return 1; }
	if (before != suspend_value) { P("PMIC %s reg=%02x unexpected=%02x left alone", name, reg, before); return 0; }
	int w = pmic_wr(reg, run_value);
	msleep(20);
	int r = pmic_rd(reg, &after);
	P("PMIC %s reg=%02x before=%02x wrote=%02x after=%02x rc=%d/%d", name, reg, before, run_value, after, w, r);
	return !w && !r && after == run_value;
}

/* ---- CPU clock ---- */
#define CPUFREQ "/sys/devices/system/cpu/cpu0/cpufreq/"
static int memfd = -1;

static void cpu_log(const char *tag)
{
	struct cpu_state s;
	char line[400], governor[32] = "?", current[32] = "?";
	cpu_read(memfd, &s);
	cpu_format(&s, line, sizeof(line));
	if (slurp(CPUFREQ "scaling_governor", governor, sizeof(governor)) > 0) governor[strcspn(governor, "\n")] = 0;
	if (slurp(CPUFREQ "scaling_cur_freq", current, sizeof(current)) > 0) current[strcspn(current, "\n")] = 0;
	P("CPU %s governor=%s cur=%s %s", tag, governor, current, line);
}

static void cpu_setup(void)
{
	char text[200] = "";
	memfd = node("/dev/mem", 1, 1, O_RDONLY | O_SYNC);
	if (slurp(CPUFREQ "scaling_available_frequencies", text, sizeof(text)) > 0) text[strcspn(text, "\n")] = 0;
	P("CPU table=%s mem=%d", text, memfd >= 0);
	cpu_log("begin");
	if (slurp(CPUFREQ "scaling_governor", text, sizeof(text)) > 0 && strncmp(text, "performance", 11)) {
		int rc = put_string(CPUFREQ "scaling_governor", "performance\n");
		P("CPU governor request=performance rc=%d errno=%d", rc, rc ? errno : 0);
		msleep(200);
		cpu_log("after-governor");
	}
}

/* ---- EPDC driver ---- */
static const char *fw_dirs[] = {
	"/sys/devices/platform/imx_epdc_fb/firmware/imx_epdc_fb",
	"/sys/class/firmware/imx_epdc_fb",
};

/* Feed one firmware request through the kernel's sysfs loading interface. */
static int serve_firmware(const unsigned char *data, size_t len, pid_t loader, unsigned timeout_ms)
{
	char loading[160], path[160];
	double end = now() + timeout_ms / 1000.0;
	for (;;) {
		for (unsigned i = 0; i < 2; i++) {
			snprintf(loading, sizeof(loading), "%s/loading", fw_dirs[i]);
			if (access(loading, W_OK)) continue;
			snprintf(path, sizeof(path), "%s/data", fw_dirs[i]);
			if (put_string(loading, "1\n")) { P("FW loading=1 failed errno=%d", errno); return -1; }
			int fd = open(path, O_WRONLY | O_CLOEXEC);
			if (fd < 0) { P("FW data open failed errno=%d", errno); put_string(loading, "-1\n"); return -1; }
			size_t done = 0;
			while (done < len) {
				size_t chunk = len - done > 4096 ? 4096 : len - done;
				ssize_t k = write(fd, data + done, chunk);
				if (k < 0 && errno == EINTR) continue;
				if (k <= 0) break;
				done += (size_t)k;
			}
			close(fd);
			if (done != len) { P("FW short write %zu/%zu errno=%d", done, len, errno); put_string(loading, "-1\n"); return -1; }
			if (put_string(loading, "0\n")) { P("FW loading=0 failed errno=%d", errno); return -1; }
			return 0;
		}
		int status;
		if (loader > 0 && waitpid(loader, &status, WNOHANG) == loader) return 1; /* caller must not wait again */
		if (now() > end) { P("FW no request within %u ms", timeout_ms); return -1; }
		msleep(10);
	}
}

static int load_driver(void)
{
	size_t ko_len = 0, fw_len = 0;
	unsigned char *ko = load_file(MODULE_FILE, &ko_len), *fw = load_file(FIRMWARE_FILE, &fw_len);
	if (!ko || !fw) { P("DRIVER files module=%zu firmware=%zu errno=%d", ko_len, fw_len, errno); return -1; }
	int p[2];
	if (pipe(p)) return -1;
	pid_t id = fork();
	if (id < 0) return -1;
	if (!id) {
		close(p[0]);
		int rc = (int)syscall(SYS_init_module, ko, (unsigned long)ko_len, "");
		int e = rc ? errno : 0;
		(void)!write(p[1], &e, sizeof(e));
		_exit(rc ? 1 : 0);
	}
	close(p[1]);
	double begin = now();
	int served = serve_firmware(fw, fw_len, id, 20000);
	int status = 0, e = -1;
	if (served != 1) {
		double end = now() + 40;
		for (;;) {
			pid_t r = waitpid(id, &status, WNOHANG);
			if (r == id) break;
			if (now() > end) { P("DRIVER load still blocked after 40 s"); close(p[0]); return -2; }
			msleep(20);
		}
	}
	if (read(p[0], &e, sizeof(e)) != (ssize_t)sizeof(e)) e = -1;
	close(p[0]);
	P("DRIVER init_module errno=%d firmware_result=%d module_bytes=%zu firmware_bytes=%zu seconds=%.2f", e, served, ko_len, fw_len, now() - begin);
	/* The driver keeps its own copy and asks for the waveform only while it
	 * is not yet initialised (mxc_epdc_fb_init_hw at probe). The launcher
	 * stores the envelope uncompressed (no compressor on the stock system):
	 * about 7 MB that the root file system would otherwise hold for good. */
	if (e == 0 && unlink(FIRMWARE_FILE)) P("DRIVER firmware unlink errno=%d", errno);
	return e == 0 ? 0 : -1;
}

/* ---- the screen the reader was promised, put back on the other side ---- */
struct screen {
	int fd;
	unsigned char *mem;
	struct fb_var_screeninfo v;
	struct fb_fix_screeninfo f;
};

static void px(struct screen *s, int x, int y, unsigned char c)
{
	if (x < 0 || y < 0 || (unsigned)x >= s->v.xres || (unsigned)y >= s->v.yres) return;
	s->mem[(size_t)((unsigned)y + s->v.yoffset) * s->f.line_length + (unsigned)x + s->v.xoffset] = c;
}

static void fill(struct screen *s, unsigned char c)
{
	for (unsigned y = 0; y < s->v.yres; y++)
		memset(s->mem + (size_t)(y + s->v.yoffset) * s->f.line_length + s->v.xoffset, c, s->v.xres);
}

/* The installer's own "正在启动 Android", run length encoded by
 * make-start-picture.py, placed where the installer places it: across the
 * width, its top at the same proportion of the panel's height. On the panel it
 * was laid out for that comes to the installer's own 64, 176 exactly, and on a
 * panel of another size it is the same placement rather than a second layout.
 *
 * Why it is drawn at all: the kernel that took over allocates its own
 * framebuffer and gets it zeroed, and the display driver clears the panel and
 * then shows that zeroed buffer as it initialises (mxc_epdc_fb_probe ->
 * do_epdc_init_sequence, unconditional). Nothing of what the reader was
 * looking at survives the handover, so leaving the panel alone leaves it dark
 * until Android's first frame. */
static void draw_start(struct screen *s)
{
	int left = ((int)s->v.xres - PW3_START_W) / 2;
	int top = (int)((uint64_t)s->v.yres * PW3_START_TOP / PW3_START_PANEL_H);
	unsigned x = 0, y = 0;
	for (size_t i = 0; i + 1 < sizeof(pw3_start_runs); i += 2)
		for (unsigned n = pw3_start_runs[i + 1]; n; n--) {
			px(s, left + (int)x, top + (int)y, pw3_start_runs[i]);
			if (++x == PW3_START_W) { x = 0; y++; }
		}
	/* The count tells the whole truth about the decoding: the runs are the
	 * picture exactly when it comes to PW3_START_PIXELS, with no row left
	 * half drawn. */
	P("SCREEN picture %dx%d at %d,%d pixels=%lu/%u rows=%u over=%u", PW3_START_W, PW3_START_H,
	  left, top, (unsigned long)y * PW3_START_W + x, PW3_START_PIXELS, y, x);
}

static int refresh(struct screen *s, uint32_t id, const char *tag)
{
	struct update u;
	memset(&u, 0, sizeof(u));
	u.region = (struct rect){ 0, 0, s->v.xres, s->v.yres };
	u.waveform = u.hist_bw = u.hist_gray = WAVEFORM_MODE_GC16;
	u.mode = UPDATE_MODE_FULL;
	u.marker = id;
	u.temperature = TEMP_USE_AUTO;
	double t0 = now();
	int rc = ioctl(s->fd, MXCFB_SEND_UPDATE, &u), e = rc < 0 ? errno : 0;
	struct marker m = { id, 0 };
	int wrc = -1, we = 0;
	if (!rc) {
		do { wrc = ioctl(s->fd, MXCFB_WAIT_FOR_UPDATE_COMPLETE, &m); } while (wrc < 0 && errno == EINTR);
		we = wrc < 0 ? errno : 0;
	}
	P("SCREEN %s send=%d/%d wait=%d/%d total_ms=%.0f", tag, rc, e, wrc, we, (now() - t0) * 1000);
	return rc || wrc < 0;
}

static void show_start(int power_restored)
{
	struct screen s;
	memset(&s, 0, sizeof(s));
	s.fd = -1;
	for (int i = 0; i < 50 && s.fd < 0; i++) {
		s.fd = node("/dev/fb0", 29, 0, O_RDWR);
		if (s.fd >= 0 && ioctl(s.fd, FBIOGET_FSCREENINFO, &s.f)) { close(s.fd); s.fd = -1; }
		if (s.fd < 0) msleep(100);
	}
	if (s.fd < 0 || ioctl(s.fd, FBIOGET_VSCREENINFO, &s.v)) { P("SCREEN framebuffer unavailable errno=%d", errno); return; }
	/* Every byte written below has to be inside the mapping. The visible
	 * window starts at (xoffset, yoffset) and both offsets count; the sums
	 * are widened before they are added, because adding them as they come is
	 * a 32-bit addition that a large offset wraps -- and a wrapped sum is a
	 * check that passes on a window nowhere near the mapping. */
	if (s.v.bits_per_pixel != 8 ||
	    (uint64_t)s.v.xoffset + s.v.xres > (uint64_t)s.f.line_length ||
	    ((uint64_t)s.v.yoffset + s.v.yres) * (uint64_t)s.f.line_length > (uint64_t)s.f.smem_len) {
		P("SCREEN geometry not usable x=%u+%u stride=%u y=%u+%u len=%u",
		  s.v.xoffset, s.v.xres, s.f.line_length, s.v.yoffset, s.v.yres, s.f.smem_len);
		close(s.fd);
		return;
	}
	s.mem = mmap(0, s.f.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, s.fd, 0);
	if (s.mem == MAP_FAILED) { P("SCREEN mmap failed errno=%d", errno); close(s.fd); return; }
	uint32_t mode = 0;
	int a = ioctl(s.fd, MXCFB_SET_AUTO_UPDATE_MODE, &mode);
	mode = 2;
	int b = ioctl(s.fd, MXCFB_SET_UPDATE_SCHEME, &mode);
	int c = ioctl(s.fd, FBIOBLANK, FB_BLANK_UNBLANK);
	P("SCREEN fb=%ux%u stride=%u rotate=%u auto=%d scheme=%d unblank=%d", s.v.xres, s.v.yres, s.f.line_length, s.v.rotate, a, b, c);
	fill(&s, 255);
	draw_start(&s);
	/* One update for the whole panel: the driver's own initialisation has just
	 * cleared it and shown its zeroed buffer, so there is nothing on the glass
	 * worth keeping and nothing to be gained by a partial one. */
	/* The result is said, not discarded. This line is the only thing a reader's
	 * report has about whether the start card reached the glass, and "SCREEN
	 * up" printed after a refresh that failed is the kind of lie a diagnostics
	 * channel must not tell (a Codex review, 2026-10-30). Nothing is retried on
	 * a failure: the buffer is already white and the boot goes on either way. */
	int shown = refresh(&s, 1, "start");
	P("SCREEN %s, keeper_restored=%d", shown ? "refresh failed" : "up", power_restored);
	munmap(s.mem, s.f.smem_len);
	close(s.fd);
}

/* Everything that touches hardware; runs in a child so that it can be bounded in time. */
static int work(void)
{
	cpu_setup();
	i2cfd = node("/dev/i2c-0", 89, 0, O_RDWR);
	P("I2C node=%d errno=%d", i2cfd >= 0, i2cfd >= 0 ? 0 : errno);
	/* 0x70: LDO7 3.2 V; 0x54: 1.8 V.  0x9b / 0x38: run-time sequencer settings; 0x87 / 0x00: suspend. */
	int keeper = restore("LDO7-keeper", 0x4F, 0x54, 0x70);
	restore("FPSBUCK1", 0x8C, 0x87, 0x9B);
	restore("CNFG_FPS2", 0x82, 0x00, 0x38);
	msleep(300); /* let the rail settle before anything drives the panel */
	if (i2cfd >= 0) close(i2cfd);

	char temp[32] = "";
	double start = now();
	ssize_t got = -1;
	while (now() - start < 10) {
		got = slurp("/sys/devices/platform/imx-i2c.0/i2c-0/0-003c/max77696-adc.0/display_temp_c", temp, sizeof(temp));
		if (got > 0) break;
		msleep(200);
	}
	temp[strcspn(temp, "\n")] = 0;
	P("TEMP read=%ld value=%s waited_ms=%.0f", (long)got, temp, (now() - start) * 1000);

	int loaded = !load_driver();
	if (loaded) { msleep(1000); show_start(keeper); }
	cpu_log("end");
	if (memfd >= 0) close(memfd);
	P("RESULT keeper_restored=%d driver_loaded=%d", keeper, loaded);
	return 0;
}

static void hand_over(char **envp)
{
	char *argv[] = { REAL_ARGV0, 0 };
	execve(REAL_ENTRY, argv, envp);
}

int main(int argc, char **argv, char **envp)
{
	(void)argc; (void)argv;
	if (getpid() != 1) return 64; /* only ever the first process of its own initramfs */
	signal(SIGPIPE, SIG_IGN);
	/* /proc, /sys and /dev already exist as empty directories in this initramfs. */
	int mp = !mount("proc", "/proc", "proc", 0, 0);
	int ms = !mount("sysfs", "/sys", "sysfs", 0, 0);
	int md = !mount("tmpfs", "/dev", "tmpfs", 0, "mode=0755,size=64k");
	if (md) kmsgfd = node("/dev/kmsg", 1, 11, O_WRONLY);
	statusfd = open(STATUS_FILE, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0444);
	struct utsname u;
	memset(&u, 0, sizeof(u));
	uname(&u);
	P("BEGIN release=%s uptime=%.2f mounts=%d%d%d", u.release, now(), mp, ms, md);

	if (!strcmp(u.release, KERNEL_RELEASE) && mp && ms && md) {
		pid_t child = fork();
		if (!child) _exit(work());
		double end = now() + 75;
		int status = -1, done = 0;
		while (child > 0 && now() < end) {
			if (waitpid(child, &status, WNOHANG) == child) { done = 1; break; }
			msleep(50);
		}
		if (child > 0 && !done) { kill(child, SIGKILL); waitpid(child, &status, 0); P("RESULT timed out; continuing the boot"); }
		while (waitpid(-1, 0, WNOHANG) > 0) { }
	} else P("RESULT skipped: unexpected kernel or mounts");

	/* Leave the root exactly as found: no mounts, no extra open files. */
	P("END uptime=%.2f handing over to %s", now(), REAL_ENTRY);
	if (statusfd >= 0) { fsync(statusfd); close(statusfd); statusfd = -1; }
	if (kmsgfd >= 0) { close(kmsgfd); kmsgfd = -1; }
	if (md && umount("/dev")) umount2("/dev", MNT_DETACH);
	if (ms && umount("/sys")) umount2("/sys", MNT_DETACH);
	if (mp && umount("/proc")) umount2("/proc", MNT_DETACH);
	hand_over(envp);

	/* The storage entry could not be executed: return to the stock system. */
	sync();
	reboot(RB_AUTOBOOT);
	for (;;) sleep(60);
}
