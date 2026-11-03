/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The installer's surroundings on the Kindle's stock system (install.h, inst_ops):
 * battery, Wi-Fi and the screen saver through the stock lipc tools, downloads
 * through the stock curl, progress and errors on the screens of screen.h.
 */
#ifndef PW3_KINDLE_H
#define PW3_KINDLE_H

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

#endif
