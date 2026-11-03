/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef PW3_TRANSITION_ABI_H
#define PW3_TRANSITION_ABI_H
#define PT_LIST 0
#define PT_ENTRY 4
#define PT_MACHINE 8
#define PT_ATAGS 12
#define PT_RAM_START 16
#define PT_RAM_END 20
#define PT_BUDGET 24
#define PT_CONTROL 28
#define PT_NR_SEGMENTS 32
#define PT_SEGMENTS 36
#define PT_EXPECTED_PAGES 60
#define PT_COPIED_PAGES 64
#define PT_STATUS 68
#define PT_BYTES 72

#ifndef __ASSEMBLER__
typedef unsigned int tr_u32;
struct tr_destination { tr_u32 start, end; };
struct tr_parameters {
    tr_u32 list, entry, machine, atags, ram_start, ram_end;
    tr_u32 budget, control, nr_segments;
    struct tr_destination segments[3];
    tr_u32 expected_pages, copied_pages, status;
};
typedef char tr_abi_size[(sizeof(struct tr_parameters) == PT_BYTES) ? 1 : -1];
/* The private table is built by prepare() in stage.c, not by anything declared
 * here: it needs the store's own allocations and this file is shared with
 * transition.S, which runs after the table is already installed. */
void pw3_enter_private_mapping(tr_u32 physical_control, tr_u32 private_ttbr0);
extern unsigned char pw3_transition_start[], pw3_transition_end[], pw3_transition_params[];
#endif
#endif
