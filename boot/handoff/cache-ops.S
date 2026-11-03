/*
 *  linux/arch/arm/mm/cache-v7.S
 *
 *  Copyright (C) 2001 Deep Blue Solutions Ltd.
 *  Copyright (C) 2005 ARM Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 *  This is the "shell" of the ARMv7 processor support.
 */
/* Standalone ARM-only UP test extraction. Caller has already masked IRQ/FIQ.
 * The public ABI wrapper saves every callee-saved register used by the body.
 */
.syntax unified
.arch armv7-a
.arm
.text
.global sim_flush_cache_all
.type sim_flush_cache_all, %function
sim_flush_cache_all:
    push {r4-r11, r12, lr}
    bl sim_flush_dcache_all
    mov r0, #0
    mcr p15, 0, r0, c7, c5, 0
    dsb
    isb
    pop {r4-r11, r12, pc}
.size sim_flush_cache_all, . - sim_flush_cache_all
.type sim_flush_dcache_all, %function
sim_flush_dcache_all:
	dmb					@ ensure ordering with previous memory accesses
	mrc	p15, 1, r0, c0, c0, 1		@ read clidr
	ands	r3, r0, #0x7000000		@ extract loc from clidr
	mov	r3, r3, lsr #23			@ left align loc bit field
	beq	finished			@ if loc is 0, then no need to clean
	mov	r10, #0				@ start clean at cache level 0
loop1:
	add	r2, r10, r10, lsr #1		@ work out 3x current cache level
	mov	r1, r0, lsr r2			@ extract cache type bits from clidr
	and	r1, r1, #7			@ mask of the bits for current cache only
	cmp	r1, #2				@ see what cache we have at this level
	blt	skip				@ skip if no cache, or just i-cache
	mcr	p15, 2, r10, c0, c0, 0		@ select current cache level in cssr
	isb					@ isb to sych the new cssr&csidr
	mrc	p15, 1, r1, c0, c0, 0		@ read the new csidr
	and	r2, r1, #7			@ extract the length of the cache lines
	add	r2, r2, #4			@ add 4 (line length offset)
	ldr	r4, =0x3ff
	ands	r4, r4, r1, lsr #3		@ find maximum number on the way size
	clz	r5, r4				@ find bit position of way size increment
	ldr	r7, =0x7fff
	ands	r7, r7, r1, lsr #13		@ extract max number of the index size
loop2:
	mov	r9, r4				@ create working copy of max way size
loop3:
    orr	r11, r10, r9, lsl r5	@ factor way and cache number into r11
    orr	r11, r11, r7, lsl r2	@ factor index number into r11
	mcr	p15, 0, r11, c7, c14, 2		@ clean & invalidate by set/way
	subs	r9, r9, #1			@ decrement the way
	bge	loop3
	subs	r7, r7, #1			@ decrement the index
	bge	loop2
skip:
	add	r10, r10, #2			@ increment cache number
	cmp	r3, r10
	bgt	loop1
finished:
	mov	r10, #0				@ swith back to cache level 0
	mcr	p15, 2, r10, c0, c0, 0		@ select current cache level in cssr
	dsb
	isb
	mov	pc, lr
.size sim_flush_dcache_all, . - sim_flush_dcache_all

/* No stack or data writes occur here. r6/r8 are untouched by the reviewed
 * raw set/way routine. All Linux callbacks have already returned while L1
 * was still enabled. Last-clean, disable and enter form one non-returning
 * instruction path, so no stale cached frame can overwrite an uncached one. */
.balign 32
.global pw3_cache_finish_and_enter
.type pw3_cache_finish_and_enter, %function
pw3_cache_finish_and_enter:
    mov r6, r0
    mov r8, r1
    bl sim_flush_dcache_all
    mov r0, #0
    mcr p15, 0, r0, c7, c5, 0
    mcr p15, 0, r0, c7, c5, 6
    dsb
    isb
    mrc p15, 0, r2, c1, c0, 0
    bic r2, r2, #0x1000
    bic r2, r2, #0x0006
    mcr p15, 0, r2, c1, c0, 0
    isb
    mov r0, r6
    mov r1, r8
    b pw3_enter_private_mapping
.size pw3_cache_finish_and_enter, . - pw3_cache_finish_and_enter
.section .note.GNU-stack,"",%progbits
