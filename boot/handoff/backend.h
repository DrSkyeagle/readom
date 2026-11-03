/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef PW3_CACHE_BACKEND_H
#define PW3_CACHE_BACKEND_H
#include "transition_abi.h"
struct cache_backend {
    tr_u32 physical_control, physical_pgdir, expected_thread_info;
    const tr_u32 *original, *private;
    void (*flush_inner)(void);
    void (*flush_outer)(void);
    void (*disable_outer)(void);
    void (*invalidate_outer)(void);
};
/* All kernel callbacks run on the owning task stack with caches enabled.
 * The final cache-disable/entry path is stackless assembly and never returns. */
int pw3_cache_backend_check(const struct cache_backend *b);
int pw3_cache_backend(const struct cache_backend *b);
void pw3_cache_finish_and_enter(tr_u32 control,tr_u32 pgdir) __attribute__((noreturn));
#endif
