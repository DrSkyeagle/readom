/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef PW3_CONTROLLED_HANDOFF
#error "Only the controlled handoff candidate may link this file"
#endif
#include <linux/thread_info.h>
#include "backend.h"
#include "identity-attrs.h"
int pw3_cache_backend_check(const struct cache_backend *b)
{
    tr_u32 sctlr, ttbcr, ttbr, cpsr, dacr, prrr, nmrr, pc, sp, i;
    if (!b || !b->original || !b->private || !b->flush_inner ||
        !b->flush_outer || !b->disable_outer ||
        !b->invalidate_outer) return -1;
    if ((b->physical_pgdir & 16383) || (b->physical_control & 4095) ||
        !b->physical_pgdir || !b->physical_control) return -2;
    __asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(sctlr));
    __asm__ volatile("mrc p15, 0, %0, c2, c0, 2" : "=r"(ttbcr));
    __asm__ volatile("mrc p15, 0, %0, c2, c0, 0" : "=r"(ttbr));
    __asm__ volatile("mrc p15, 0, %0, c3, c0, 0" : "=r"(dacr));
    __asm__ volatile("mrc p15, 0, %0, c10, c2, 0" : "=r"(prrr));
    __asm__ volatile("mrc p15, 0, %0, c10, c2, 1" : "=r"(nmrr));
    __asm__ volatile("mrs %0, cpsr" : "=r"(cpsr));
    __asm__ volatile("mov %0, pc" : "=r"(pc));
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    if(!b->expected_thread_info || (sp & ~(THREAD_SIZE-1u))!=b->expected_thread_info)return -6;
    /* Preserve high mappings and map only the control section as executable
     * Normal non-cacheable memory. Strongly-ordered instruction fetch is
     * architecturally unpredictable. The new TTBR uses uncached walks. */
    if (ttbcr || ((ttbr & 0x3fff)!=0 && (ttbr & 0x3fff)!=0x59) ||
        (sctlr & 0x22000000) || !pw3_identity_profile(sctlr,prrr,nmrr) ||
        (dacr & 3) != 1 || (cpsr & 0xff) != 0xd3) return -3;
    if ((sctlr & 0x1005) != 0x1005) return -4;
    if (pc < 0xbf000000u || sp < 0xbf000000u ||
        ((b->original[pc >> 20] & 3) != 1 && (b->original[pc >> 20] & 3) != 2) ||
        ((b->original[sp >> 20] & 3) != 1 && (b->original[sp >> 20] & 3) != 2)) return -5;
    for (i=0xbf0;i<4096;++i)
        if (b->private[i]!=b->original[i]) return -5;
    /* Tables must keep the identity control address valid when TTBR changes.
     * The caller separately owns all pages and checks the complete copy plan.
     */
    i=b->physical_control >> 20;
    if (i>=0xbf0 || b->private[i]!=((i<<20)|PW3_IDENTITY_SECTION)) return -5;
    return 0;
}
int pw3_cache_backend(const struct cache_backend *b)
{
    int rc=pw3_cache_backend_check(b);
    if(rc)return rc;
    /* Linux cache callbacks touch current_thread_info and may write the
     * stack. Keep the real task stack and L1 enabled until they all return. */
    b->flush_inner();
    b->flush_outer();
    b->disable_outer();
    b->invalidate_outer();
    /* This assembly entry performs the final L1 clean before disabling it.
     * It never reads/writes a stack or returns through a C epilogue. */
    pw3_cache_finish_and_enter(b->physical_control,b->physical_pgdir);
}
