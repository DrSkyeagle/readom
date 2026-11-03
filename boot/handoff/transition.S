/* SPDX-License-Identifier: GPL-2.0-only */
/* Host-review ARMv7 candidate. NOT linked into any device loader/module.
 * Caller prerequisites: one core, SVC ARM, IRQ/FIQ masked, caches cleaned
 * and disabled, outer cache disabled, TTBCR=0, private 16KiB table owned,
 * immutable/validated page list, all hardware DMA quiesced.
 */
#ifndef PW3_CONTROLLED_HANDOFF
#error "Transition is review-only; no device execution build is supplied"
#endif
#include "transition_abi.h"
.syntax unified
.arch armv7-a
.arm
.text
.balign 32
.global pw3_enter_private_mapping
.type pw3_enter_private_mapping, %function
pw3_enter_private_mapping:
    /* No stack is needed after this point. Failure deliberately cannot return
     * to a supposedly live OS after shutdown. Real recovery is a separate gate. */
    mrc p15, 0, r2, c2, c0, 2
    cmp r2, #0
    bne .Lswitch_halt
    mrc p15, 0, r2, c1, c0, 0
    tst r2, #4
    bne .Lswitch_halt
    tst r2, #0x1000
    bne .Lswitch_halt
    cpsid if
    dsb
    mcr p15, 0, r1, c2, c0, 0
    isb
    mov r2, #0
    mcr p15, 0, r2, c8, c7, 0
    dsb
    isb
    bx r0
.Lswitch_halt:
    b .Lswitch_halt
.size pw3_enter_private_mapping, . - pw3_enter_private_mapping

/* Everything from start through end is copied as a position-independent
 * control-page blob. No external calls, literal addresses, stack or SP use. */
.balign 32
.global pw3_transition_start
pw3_transition_start:
    adrl r12, pw3_transition_params
    mov r1, #0xa1
    str r1, [r12, #PT_STATUS]
    mov r1, #0
    str r1, [r12, #PT_COPIED_PAGES]
    mrc p15, 0, r1, c1, c0, 0
    tst r1, #4
    movne r1, #0xe1
    bne .Lfail
    tst r1, #0x1000
    movne r1, #0xe1
    bne .Lfail
    /* This instruction stream is already identity mapped at physical PC.
     * Linux v3.4 cpu_v7_reset uses the same M-bit clear + ISB contract. */
    dsb
    bic r1, r1, #1
    mcr p15, 0, r1, c1, c0, 0
    isb
    mov r1, #0xa2
    str r1, [r12, #PT_STATUS]
    ldr r7, [r12, #PT_RAM_START]
    ldr r8, [r12, #PT_RAM_END]
    ldr r9, [r12, #PT_BUDGET]
    ldr r11, [r12, #PT_CONTROL]
    ldr r0, [r12, #PT_LIST]
    mov r4, #0
    /* Head is a page-aligned physical indirection page. */
    mov r3, r0
    b .Lindirection
.Lnext:
    cmp r9, #0
    moveq r1, #0xe2
    beq .Lfail
    sub r9, r9, #1
    cmp r0, r10
    movhs r1, #0xe3
    bhs .Lfail
    ldr r3, [r0], #4
    mov r2, r3, lsl #20
    mov r2, r2, lsr #20
    cmp r2, #4
    beq .Ldone
    sub r3, r3, r2
    cmp r2, #1
    beq .Ldestination
    cmp r2, #2
    beq .Lindirection
    cmp r2, #8
    beq .Lsource
    mov r1, #0xe4
    b .Lfail
.Lindirection:
    /* Reject malformed page addresses, controller alias and RAM escape. */
    mov r1, r3, lsl #20
    cmp r1, #0
    movne r1, #0xe5
    bne .Lfail
    cmp r3, r7
    movlo r1, #0xe5
    blo .Lfail
    sub r1, r8, #4096
    cmp r3, r1
    movhi r1, #0xe5
    bhi .Lfail
    cmp r3, r11
    moveq r1, #0xe5
    beq .Lfail
    mov r0, r3
    add r10, r3, #4096
    b .Lnext
.Ldestination:
    mov r4, r3
    b .Lnext
.Lsource:
    cmp r3, r7
    movlo r1, #0xe6
    blo .Lfail
    sub r1, r8, #4096
    cmp r3, r1
    movhi r1, #0xe6
    bhi .Lfail
    cmp r3, r11
    moveq r1, #0xe6
    beq .Lfail
    cmp r4, r7
    movlo r1, #0xe7
    blo .Lfail
    cmp r4, r1
    movhi r1, #0xe7
    bhi .Lfail
    cmp r4, r11
    moveq r1, #0xe7
    beq .Lfail
    ldr r6, [r12, #PT_NR_SEGMENTS]
    cmp r6, #0
    moveq r1, #0xe8
    beq .Lfail
    cmp r6, #3
    movhi r1, #0xe8
    bhi .Lfail
    add r5, r12, #PT_SEGMENTS
.Lfind_segment:
    ldmia r5!, {r1, r2}
    cmp r4, r1
    blo .Lsegment_next
    add r1, r4, #4096
    cmp r1, r2
    bls .Lcopy
.Lsegment_next:
    subs r6, r6, #1
    bne .Lfind_segment
    mov r1, #0xe8
    b .Lfail
.Lcopy:
    ldr r1, [r12, #PT_COPIED_PAGES]
    ldr r2, [r12, #PT_EXPECTED_PAGES]
    cmp r1, r2
    movhs r1, #0xe9
    bhs .Lfail
    add r1, r1, #1
    str r1, [r12, #PT_COPIED_PAGES]
    mov r6, #1024
.Lcopy_word:
    ldr r5, [r3], #4
    str r5, [r4], #4
    subs r6, r6, #1
    bne .Lcopy_word
    b .Lnext
.Ldone:
    cmp r3, #4
    movne r1, #0xea
    bne .Lfail
    ldr r1, [r12, #PT_COPIED_PAGES]
    ldr r2, [r12, #PT_EXPECTED_PAGES]
    cmp r1, r2
    movne r1, #0xeb
    bne .Lfail
    mov r1, #0xa3
    str r1, [r12, #PT_STATUS]
    dsb
    ldr lr, [r12, #PT_ENTRY]
    mov r0, #0
    ldr r1, [r12, #PT_MACHINE]
    ldr r2, [r12, #PT_ATAGS]
    bx lr
.Lfail:
    str r1, [r12, #PT_STATUS]
    dsb
.Lhalt:
    b .Lhalt
.balign 4
.global pw3_transition_params
pw3_transition_params:
    .space PT_BYTES
.global pw3_transition_end
pw3_transition_end:
.section .note.GNU-stack,"",%progbits
