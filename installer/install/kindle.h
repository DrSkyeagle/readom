/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The installer's surroundings on the Kindle's stock system (install.h, inst_ops):
 * battery, Wi-Fi and the screen saver through the stock lipc tools, downloads
 * through the stock curl, progress and errors on the screens of screen.h.
 */
#ifndef PW3_KINDLE_H
#define PW3_KINDLE_H

#include <signal.h>

#include "install.h"
#include "screen.h"

struct kindle {
	int awake_changed;
	char awake_before[16];
	struct screen *screen;
	long long shown_at;	/* monotonic milliseconds of the last progress screen */
	int shown_screen, shown_permille;
};

void kindle_ops(struct kindle *k, struct screen *screen, struct inst_ops *ops);

/* The installer's lock: the android folder of the user store, locked for as long as
 * the returned descriptor stays open. -1 with errno EWOULDBLOCK: another tap holds it. */
int kindle_lock(const char *user);

/* Keeps the stock system from starting its screen saver (and stopping Wi-Fi) while
 * installing, and puts the setting back. */
void kindle_keep_awake(struct kindle *k);
void kindle_restore_awake(struct kindle *k);

/* Shows the result of an install that stopped: the code and what to do. */
/* The screen for this result, and whether it reached the panel: the caller's
 * exit status says whether the launcher must add its own line, so it cannot be
 * guessed (helper.c, stop_now). */
int kindle_show_error(struct kindle *k, const struct inst_result *r);

/* Keeps a screen that ends the run on the panel until the reader has seen it.
 * The library's scriptlet launcher repaints the whole screen with the library
 * about a second after the script exits (measured, 2026-11-04), so a run that
 * stopped with a reason on the screen waits here, with the touch screen taken
 * from the stock UI, until the reader taps -- or ten minutes pass, the Kindle
 * goes to sleep, or *stop is set by a signal. Returns at once, saying why, when
 * there is no touch screen it can take. */
void kindle_hold_screen(volatile sig_atomic_t *stop);

/* Takes the touch screen from the stock UI for the rest of the run, so that
 * nothing it opens -- quick settings, a book under a tap the reader did not
 * mean for it -- lands on the installer's screens. Called at every screen;
 * does nothing once taken, and asks again while a finger is down. */
void kindle_take_touch(void);

#endif
