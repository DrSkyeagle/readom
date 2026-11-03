/* CPU clock state of the i.MX6SL: what the clock registers say, and how fast
 * code really runs.  Read by the handoff restore program (PID 1, before
 * Android).  It had a second reader, pw3-peek, which ran later from the Android
 * test script so the two readings could be compared; both went with the test
 * scaffolding on 2026-10-29.  Read-only. */
#ifndef PW3_CPU_PROBE_H
#define PW3_CPU_PROBE_H
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <sys/mman.h>

struct cpu_state {
	int regs;		/* 1 when the registers could be read */
	uint32_t ccsr, cacrr, cbcdr, cbcmr, pll_arm, pll_sys, pfd528, reg_core;
	int step;		/* 1: CPU clock comes from the step clock, not PLL1 */
	unsigned mhz;		/* CPU clock implied by the registers */
	double loops_per_us;	/* measured: iterations of a fixed loop per microsecond */
};

/* Best of several short runs of a fixed register-only loop; short enough that
 * at least one run normally falls between two scheduler ticks. */
static double cpu_bench(void)
{
	double best = 0;
	for (int run = 0; run < 9; run++) {
		struct timespec a, b;
		uint32_t x = 12345u + (uint32_t)run;
		clock_gettime(CLOCK_MONOTONIC, &a);
		for (uint32_t i = 0; i < 400000u; i++) x = x * 1664525u + 1013904223u + (x >> 13);
		clock_gettime(CLOCK_MONOTONIC, &b);
		volatile uint32_t keep = x;
		(void)keep;
		double us = (double)(b.tv_sec - a.tv_sec) * 1e6 + (double)(b.tv_nsec - a.tv_nsec) / 1e3;
		if (us > 0 && 400000.0 / us > best) best = 400000.0 / us;
	}
	return best;
}

/* memfd: an open /dev/mem, or -1. */
static void cpu_read(int memfd, struct cpu_state *s)
{
	*s = (struct cpu_state){ 0 };
	if (memfd >= 0) {
		volatile uint32_t *ccm = mmap(0, 4096, PROT_READ, MAP_SHARED, memfd, 0x020C4000);
		volatile uint32_t *ana = mmap(0, 4096, PROT_READ, MAP_SHARED, memfd, 0x020C8000);
		if (ccm != MAP_FAILED && ana != MAP_FAILED) {
			s->regs = 1;
			s->ccsr = ccm[0x0C / 4]; s->cacrr = ccm[0x10 / 4]; s->cbcdr = ccm[0x14 / 4]; s->cbcmr = ccm[0x18 / 4];
			s->pll_arm = ana[0x00 / 4]; s->pll_sys = ana[0x30 / 4]; s->pfd528 = ana[0x100 / 4]; s->reg_core = ana[0x140 / 4];
			unsigned source;
			s->step = (s->ccsr >> 2) & 1;
			if (!s->step) source = 24u * (s->pll_arm & 0x7F) / 2;		/* PLL1 */
			else if (!(s->ccsr & 0x100)) source = 24;			/* oscillator */
			else if (s->ccsr & 0x8) source = 528;				/* PLL2 */
			else {								/* PLL2 PFD2 */
				unsigned frac = (s->pfd528 >> 16) & 0x3F;
				source = frac ? 528u * 18 / frac : 0;
			}
			s->mhz = source / ((s->cacrr & 7) + 1);
		}
		if (ccm != MAP_FAILED) munmap((void *)ccm, 4096);
		if (ana != MAP_FAILED) munmap((void *)ana, 4096);
	}
	s->loops_per_us = cpu_bench();
}

static int cpu_format(const struct cpu_state *s, char *out, size_t max)
{
	if (!s->regs) return snprintf(out, max, "regs=unreadable bench=%.1f", s->loops_per_us);
	return snprintf(out, max, "mhz=%u step=%d bench=%.1f ccsr=%08x cacrr=%08x cbcdr=%08x cbcmr=%08x pll_arm=%08x pll_sys=%08x pfd528=%08x reg_core=%08x",
		s->mhz, s->step, s->loops_per_us, s->ccsr, s->cacrr, s->cbcdr, s->cbcmr, s->pll_arm, s->pll_sys, s->pfd528, s->reg_core);
}
#endif
