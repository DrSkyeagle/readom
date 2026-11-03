/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef PWS_ALLOCATION_RANGES_H
#define PWS_ALLOCATION_RANGES_H
#include "pw3_plan.h"
/* Exclude every byte the pinned decompressor or copy destinations may write.
 * Physical pages still come exclusively from the live kernel allocator. */
static int pws_allocation_allowed(pw_u32 base,pw_u32 footprint_end,
        pw_u32 kernel_bytes,pw_u32 initrd_bytes,pw_u32 address,pw_u32 bytes)
{
    struct pw_range candidate={address,bytes},reserved[3];
    pw_u64 ram_end=(pw_u64)base+512u*1024u*1024u;
    unsigned int i;
    if((base!=0x80000000u && base!=0x60000000u) || !bytes ||
       (address&4095u) || (bytes&4095u) || address<base ||
       (pw_u64)address+bytes>ram_end || footprint_end<=base+0x8000u ||
       footprint_end>base+64u*1024u*1024u || (footprint_end&4095u) ||
       kernel_bytes<48u || kernel_bytes>8u*1024u*1024u ||
       !initrd_bytes || initrd_bytes>40u*1024u*1024u)return 0;
    /* Prefix includes ATAGs, decompressor page tables, kernel/BSS, relocation,
     * malloc area and stack, as derived from the exact ELF and gzip bytes. */
    reserved[0].start=base;reserved[0].size=footprint_end-base;
    reserved[1].start=base+0x8000u;reserved[1].size=(kernel_bytes+4095u)&~4095u;
    reserved[2].start=base+64u*1024u*1024u;reserved[2].size=(initrd_bytes+4095u)&~4095u;
    if(!pw_range_valid(candidate))return 0;
    for(i=0;i<3;i++)if(!pw_range_valid(reserved[i]) || pw_ranges_overlap(candidate,reserved[i]))return 0;
    return 1;
}
/* How many pages pws_allocation_allowed() can refuse, at most: the three
 * reserved ranges added up, counting the kernel's own range even though the
 * footprint normally contains it. A caller that holds every page it is refused
 * cannot be refused more times than this, because it would have to be refused
 * the same page twice; that is what makes the holding bounded (stage.c).
 * Pages, not bytes, and an upper bound rather than the exact union, which
 * costs the kernel's own page count in a list and spares the caller a case to
 * get wrong. */
static pw_u32 pws_excluded_page_limit(pw_u32 base,pw_u32 footprint_end,
        pw_u32 kernel_bytes,pw_u32 initrd_bytes)
{
    if(footprint_end<=base || kernel_bytes>8u*1024u*1024u ||
       initrd_bytes>40u*1024u*1024u)return 0;
    return (footprint_end-base)/4096u + ((kernel_bytes+4095u)&~4095u)/4096u +
           ((initrd_bytes+4095u)&~4095u)/4096u;
}
#endif
