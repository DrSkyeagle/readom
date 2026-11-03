/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef PW3_CONTROLLED_HANDOFF
#error "Controlled candidate only"
#endif
.syntax unified
.arch armv7-a
.arm
.text
.global pws_call_on_stack
.type pws_call_on_stack,%function
pws_call_on_stack:
    push {r4,lr}
    mov r4,sp
    mov sp,r2
    mov r3,r0
    mov r0,r1
    blx r3
    mov sp,r4
    pop {r4,pc}
.size pws_call_on_stack,.-pws_call_on_stack
.section .note.GNU-stack,"",%progbits
