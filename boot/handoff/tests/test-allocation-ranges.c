/* SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include "allocation-ranges.h"
static unsigned checks;
#define CHECK(x) do { ++checks; if(!(x)) {printf("PW3_ALLOCATION_RANGES_FAIL line=%u\n",__LINE__);return 1;} } while(0)
int main(void)
{
    unsigned profile,i;pw_u32 bases[2]={0x80000000u,0x60000000u};
    for(profile=0;profile<2;profile++) {
        pw_u32 base=bases[profile],end=base+0x905000u,kernel=3035796u,initrd=1481647u;
        pw_u32 rd_start=base+0x4000000u,rd_end=rd_start+((initrd+4095u)&~4095u);
#define ALLOWED(address,bytes) pws_allocation_allowed(base,end,kernel,initrd,address,bytes)
        CHECK(ALLOWED(base+0x2000000u,4096));
        CHECK(ALLOWED(base+0x2000000u,16384));
        CHECK(ALLOWED(end,4096));
        CHECK(!ALLOWED(end-4096,4096));
        CHECK(!ALLOWED(base-4096,4096));
        CHECK(!ALLOWED(base+0x1000,4096));
        CHECK(!ALLOWED(base+0x4000,16384));
        CHECK(!ALLOWED(base+0x8000,4096));
        CHECK(ALLOWED(rd_start-16384,16384));
        CHECK(!ALLOWED(rd_start-4096,16384));
        CHECK(!ALLOWED(rd_start,4096));
        CHECK(!ALLOWED(rd_end-4096,4096));
        CHECK(ALLOWED(rd_end,4096));
        CHECK(ALLOWED(base+0x20000000u-4096,4096));
        CHECK(!ALLOWED(base+0x20000000u,4096));
        CHECK(!ALLOWED(base+0x20000000u-4096,8192));
        CHECK(!ALLOWED(0xfffff000u,8192));
        CHECK(!ALLOWED(base+0x2000000u,0));
        CHECK(!ALLOWED(base+0x2000001u,4096));
        CHECK(!ALLOWED(base+0x2000000u,4095));
        /* Every page in the actual pinned decompression prefix and initrd
         * remains excluded, while the old 32--40 MiB false exclusions pass. */
        for(i=0;i<end-base;i+=4096)CHECK(!ALLOWED(base+i,4096));
        for(i=rd_start;i<rd_end;i+=4096)CHECK(!ALLOWED(i,4096));
        for(i=0;i<2048;i++)CHECK(ALLOWED(base+0x2000000u+i*4096,4096));
        CHECK(!pws_allocation_allowed(base,base+0x4000000u+4096,kernel,initrd,base+0x8000000u,4096));
        CHECK(!pws_allocation_allowed(base,end,0,initrd,base+0x2000000u,4096));
        CHECK(!pws_allocation_allowed(base,end,kernel,0,base+0x2000000u,4096));
        CHECK(!pws_allocation_allowed(base,end,kernel,40u*1024u*1024u+1,base+0x2000000u,4096));
        CHECK(!pws_allocation_allowed(base,base+0x100000u,8u*1024u*1024u,initrd,base+0x804000u,4096));
        CHECK(pws_allocation_allowed(base,base+0x100000u,8u*1024u*1024u,initrd,base+0x808000u,4096));
        /* The bound stage.c sets aside pages against. It has to be at least
         * the number of pages that are actually refused, or a start fails
         * with memory to spare, which is what happened on 2026-10-19 when the
         * bound was the constant 2048 and the initramfs alone was 2124 pages.
         * Counted here over all 512 MiB, for the pinned ranges and for the
         * case where the kernel's range reaches past the footprint. */
        {
            pw_u32 refused=0,limit=pws_excluded_page_limit(base,end,kernel,initrd);
            for(i=0;i<0x20000000u/4096u;i++)if(!ALLOWED(base+i*4096u,4096))++refused;
            CHECK(refused==((end-base)+((initrd+4095u)&~4095u))/4096u);
            CHECK(limit>=refused);
            /* Not loose either: only the kernel's range, which the footprint
             * already contains here, is counted twice. */
            CHECK(limit==refused+((kernel+4095u)&~4095u)/4096u);
        }
        {
            /* A footprint smaller than the kernel it is for: the kernel's
             * range sticks out of it and those pages are refused too. */
            pw_u32 small=base+0x100000u,big=8u*1024u*1024u,refused=0;
            pw_u32 limit=pws_excluded_page_limit(base,small,big,initrd);
            for(i=0;i<0x20000000u/4096u;i++)
                if(!pws_allocation_allowed(base,small,big,initrd,base+i*4096u,4096))++refused;
            CHECK(refused==(0x8000u+big+((initrd+4095u)&~4095u))/4096u);
            CHECK(limit>=refused);
        }
        CHECK(!pws_excluded_page_limit(base,base,kernel,initrd));
        CHECK(!pws_excluded_page_limit(base,end,8u*1024u*1024u+1,initrd));
        CHECK(!pws_excluded_page_limit(base,end,kernel,40u*1024u*1024u+1));
#undef ALLOWED
    }
    printf("PW3_ALLOCATION_RANGES_PASS checks=%u\n",checks);return 0;
}
