/* SPDX-License-Identifier: GPL-2.0-only */
/* Explicit pinned first-kernel candidate. Payload pages and transition state
 * are allocator-owned outside the pinned boot footprint/destinations. Native PM core dry-run
 * and fresh fixed hardware checks must pass before the single pinned jump.
 *
 * What the build pins (target-lock.h): the kernel, the public initramfs, the
 * command line, which ATAGs may be passed and the physical plan. What the
 * device adds, made on it from its own values at each start (boot/devicedata):
 * the ATAGs and an archive appended to the public initramfs. Those are checked
 * by their rules, not by a hash: the build cannot know them. */
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/mutex.h>
#include <linux/capability.h>
#include <linux/uaccess.h>
#include <linux/utsname.h>
#include <asm/memory.h>
#include <linux/preempt.h>
#include "stage_abi.h"
#include "sha256.h"
#include "pw3_plan.h"
#include "identity-attrs.h"
#include "allocation-ranges.h"
#include "target-lock.h"

#ifndef PWS_PROFILE
#define PWS_PROFILE 0
#endif
#if PWS_PROFILE < 0 || PWS_PROFILE > 2
#error "PWS_PROFILE: 0 = insertion disabled, 1 = pinned stock, 2 = virtual test"
#endif
#if !defined(CONFIG_ARM) || defined(CONFIG_SMP) || PAGE_SHIFT != 12
#error "Only UP ARM32 with 4 KiB pages is supported"
#endif
#define MAX_PAYLOAD_PAGES ((48u*1024u*1024u)/PAGE_SIZE)
/* A source page that lies where the copy or the decompressor will write is set
 * aside rather than freed, so that the allocator cannot hand back the same
 * unusable page for ever. How many may be set aside is therefore not a number
 * to pick: it is how many pages those destinations hold. Once every one of
 * them is ours, there is none left for the allocator to give, so the next page
 * it gives is necessarily outside them.
 *
 * It was 2048 until V56, and on 2026-10-19 a start failed because of it: this
 * build's initramfs lands at 64 MiB and is 8699212 bytes, which is 2124 pages,
 * and the stock system happened to have most of them free. All three attempts
 * set aside 2048 and gave up with 592, 592 and 1030 of the roughly 3200 pages
 * they needed, while 89 MB of the device's memory was free. Whether a start
 * worked depended on how much of that 8.3 MB window the stock system was not
 * using -- the same tap failing one hour and working the next.
 *
 * The ceiling below is what the ranges can be at their largest (a footprint of
 * 64 MiB, a kernel of 8 MiB and an initramfs of 40 MiB, the limits
 * allocation-ranges.h enforces), and bounds the array; begin() asks
 * pws_excluded_page_limit() for the real limit of this payload. */
#define MAX_COLLISIONS_CEILING (((64u+8u+40u)*1024u*1024u)/PAGE_SIZE)
#define MAX_WRITE (64u*1024u)
#define LIST_WORDS (PAGE_SIZE/sizeof(u32))
/* The 1 MiB sections the private page table owns. Below this the table is
 * cleared and the control page's one-to-one section is written in; from here
 * up it is copied from the live table word for word, because the module's own
 * code and the kernel are up there and have to keep running across the TTBR0
 * write.
 *
 * It is the plan's identity_limit in sections, and it is derived from it
 * rather than written out again: pw3_plan.c refuses any destination at or
 * above identity_limit, and this is the table that makes those addresses one
 * to one, so the two numbers are one fact. Spelling it a second time is how a
 * port to a machine with another split gets a table that does not match the
 * plan it was checked against, with nothing to say so. */
#define IDENTITY_SECTIONS (TARGET_IDENTITY_LIMIT>>20)
struct allocation { struct page *page; unsigned int order; };
struct slot { unsigned int bytes, written, first, pages; u8 expected[32]; };
struct store {
    struct mutex lock;
    unsigned int state, count, collisions, collision_limit;
    unsigned int control_first, list_first, list_pages, list_entries;
    struct allocation *allocations, *rejected;
    struct slot slot[PWS_SLOTS];
    unsigned int native_ready;
    unsigned int device_checked;   /* seal(): device ATAGs and archive keep the build's rules */
};
static atomic_t opened=ATOMIC_INIT(0);
static unsigned long live_pages;
module_param(live_pages,ulong,0444);
/* Preserve allocation failure evidence after owned pages are discarded.
 * 1=excluded-page limit, 2=allocator returned NULL, 3=metadata allocation,
 * 4=virtual fault injection. No physical addresses are exposed. */
static unsigned long allocation_failure_reason,allocation_failure_order;
static unsigned long allocation_failure_accepted,allocation_failure_rejected;
/* How many could have been set aside, so that a failure record says by itself
 * whether the limit was reached or the device had no memory left. */
static unsigned long allocation_failure_limit;
module_param(allocation_failure_reason,ulong,0444);
module_param(allocation_failure_order,ulong,0444);
module_param(allocation_failure_accepted,ulong,0444);
module_param(allocation_failure_rejected,ulong,0444);
module_param(allocation_failure_limit,ulong,0444);
#if PWS_PROFILE == 2
static int fail_alloc_after=-1;
module_param(fail_alloc_after,int,0600);
static int plan_fault;
module_param(plan_fault,int,0600);
#endif

static void wipe(void *p, size_t n)
{
    volatile u8 *v=p;
    while(n--) *v++=0;
}
static void free_record(struct allocation *a)
{
    if(a->page) {
        wipe(page_address(a->page),PAGE_SIZE<<a->order);
        __free_pages(a->page,a->order);
        live_pages-=1u<<a->order;
        a->page=NULL;
    }
}
static void discard(struct store *s)
{
    unsigned int i;
    for(i=0;i<s->count;++i)free_record(&s->allocations[i]);
    for(i=0;i<s->collisions;++i)free_record(&s->rejected[i]);
    vfree(s->allocations);vfree(s->rejected);
    s->allocations=NULL;s->rejected=NULL;s->count=0;s->collisions=0;s->collision_limit=0;
    wipe(s->slot,sizeof(s->slot));s->state=PWS_EMPTY;s->native_ready=0;s->device_checked=0;
    s->control_first=s->list_first=s->list_pages=s->list_entries=0;
}
static int allocation_failure(struct store *s,unsigned int order,unsigned int reason)
{
    allocation_failure_reason=reason;allocation_failure_order=order;
    allocation_failure_accepted=s->count;allocation_failure_rejected=s->collisions;
    allocation_failure_limit=s->collision_limit;
    return -ENOMEM;
}
static int allocate_owned(struct store *s,unsigned int order)
{
    struct page *page;
    unsigned long address,bytes=PAGE_SIZE<<order;
    for(;;) {
        /* Unreachable by construction: see MAX_COLLISIONS_CEILING. Kept so
         * that a destination range which grew without this growing with it
         * stops here instead of looping. */
        if(s->collisions==s->collision_limit)return allocation_failure(s,order,1);
        #if PWS_PROFILE == 2
        if(fail_alloc_after==0)return allocation_failure(s,order,4);
        if(fail_alloc_after>0)--fail_alloc_after;
#endif
        page=alloc_pages(GFP_KERNEL|__GFP_ZERO|__GFP_NORETRY|__GFP_NOWARN,order);
        if(!page)return allocation_failure(s,order,2);
        live_pages+=1u<<order;
        address=page_to_phys(page);
        if(address<(unsigned long)PHYS_OFFSET ||
           address>(unsigned long)PHYS_OFFSET+512u*1024u*1024u-bytes) {
            __free_pages(page,order);live_pages-=1u<<order;return -ERANGE;
        }
        if(pws_allocation_allowed(PHYS_OFFSET,TARGET_FOOTPRINT_END,
            s->slot[0].bytes,s->slot[2].bytes,address,bytes))break;
        /* Hold rejected allocations so the allocator cannot give us the same
         * unsuitable page repeatedly. These pages remain exclusively ours.
         */
        s->rejected[s->collisions].page=page;
        s->rejected[s->collisions++].order=order;
    }
    s->allocations[s->count].page=page;
    s->allocations[s->count++].order=order;
    return 0;
}
static int begin(struct store *s,void __user *arg)
{
    struct pws_begin b;
    unsigned int i,j,pages=0;
    int rc;
    if(s->state!=PWS_EMPTY)return -EBUSY;
    allocation_failure_reason=allocation_failure_order=0;
    allocation_failure_accepted=allocation_failure_rejected=allocation_failure_limit=0;
    if(copy_from_user(&b,arg,sizeof(b)))return -EFAULT;
    if(b.version!=PWS_VERSION || b.reserved)return -EINVAL;
    if(b.images[0].bytes<48 || b.images[0].bytes>8u*1024u*1024u ||
       b.images[1].bytes<16 || b.images[1].bytes>4096 || (b.images[1].bytes&3) ||
       !b.images[2].bytes || b.images[2].bytes>40u*1024u*1024u)return -EINVAL;
    for(i=0;i<PWS_SLOTS;++i)pages+=DIV_ROUND_UP(b.images[i].bytes,PAGE_SIZE);
    if(pages>MAX_PAYLOAD_PAGES)return -E2BIG;
    /* Each list page reserves its final word for a link when necessary. */
    s->list_pages=DIV_ROUND_UP(pages+PWS_SLOTS+1,LIST_WORDS-1);
    /* Every page allocation-ranges.h can refuse, which is every page that can
     * be set aside: a page held is never offered twice. */
    s->collision_limit=pws_excluded_page_limit(PHYS_OFFSET,TARGET_FOOTPRINT_END,
                                               b.images[0].bytes,b.images[2].bytes);
    if(!s->collision_limit || s->collision_limit>MAX_COLLISIONS_CEILING)return -E2BIG;
    /* __GFP_NORETRY for the same reason the pages themselves take it: the
     * stock system is running, and a start that cannot be prepared has to
     * fail rather than push that system into reclaiming for us. */
    s->allocations=__vmalloc((pages+3+s->list_pages)*sizeof(*s->allocations),
        GFP_KERNEL|__GFP_HIGHMEM|__GFP_ZERO|__GFP_NORETRY|__GFP_NOWARN,PAGE_KERNEL);
    s->rejected=__vmalloc(s->collision_limit*sizeof(*s->rejected),
        GFP_KERNEL|__GFP_HIGHMEM|__GFP_ZERO|__GFP_NORETRY|__GFP_NOWARN,PAGE_KERNEL);
    if(!s->allocations || !s->rejected){rc=allocation_failure(s,0,3);goto fail;}
    for(i=0;i<PWS_SLOTS;++i) {
        struct slot *slot=&s->slot[i];
        slot->bytes=b.images[i].bytes;
        slot->pages=DIV_ROUND_UP(slot->bytes,PAGE_SIZE);
        memcpy(slot->expected,b.images[i].sha256,32);
    }
    /* Know all destination extents before allocating the first source page. */
    for(i=0;i<PWS_SLOTS;++i) {
        struct slot *slot=&s->slot[i];slot->first=s->count;
        for(j=0;j<slot->pages;++j) {
            rc=allocate_owned(s,0);if(rc)goto fail;
        }
    }
    /* Owned control page, dedicated stack and aligned 16 KiB private PGD.
     * They remain zero data; this module never populates executable code.
     */
    s->control_first=s->count;
    rc=allocate_owned(s,0);if(rc)goto fail;
    rc=allocate_owned(s,0);if(rc)goto fail;
    rc=allocate_owned(s,2);if(rc)goto fail;
    s->state=PWS_WRITING;
    wipe(&b,sizeof(b));return 0;
fail:
    discard(s);wipe(&b,sizeof(b));return rc;
}
static int put(struct store *s,void __user *arg)
{
    struct pws_put p;
    struct slot *slot;
    unsigned int done=0;
    if(s->state!=PWS_WRITING)return -EPERM;
    if(copy_from_user(&p,arg,sizeof(p)))return -EFAULT;
    if(p.slot>=PWS_SLOTS || p.reserved || !p.bytes || p.bytes>MAX_WRITE ||
       p.user_buffer>0xffffffffull || p.user_buffer+p.bytes>0x100000000ull)return -EINVAL;
    slot=&s->slot[p.slot];
    if(p.offset!=slot->written || p.bytes>slot->bytes-slot->written)return -EINVAL;
    while(done<p.bytes) {
        unsigned int offset=slot->written+done;
        unsigned int take=min_t(unsigned int,p.bytes-done,PAGE_SIZE-(offset&(PAGE_SIZE-1)));
        void *dest=page_address(s->allocations[slot->first+offset/PAGE_SIZE].page);
        if(copy_from_user((u8 *)dest+(offset&(PAGE_SIZE-1)),
              (void __user *)(unsigned long)(p.user_buffer+done),take)) {
            /* A partial user copy never leaves a resumable mixed snapshot. */
            discard(s);return -EFAULT;
        }
        done+=take;
    }
    slot->written+=done;return 0;
}
/* Fixed payload destinations are excluded individually by the allocation policy. */
static u32 destination(unsigned int slot)
{
    static const u32 offset[PWS_SLOTS]={0x8000u,0x1000u,64u*1024u*1024u};
    return (u32)PHYS_OFFSET+offset[slot];
}
static u8 slot_byte(struct store *s,unsigned int slot,unsigned int offset)
{
    return ((const u8 *)page_address(s->allocations[s->slot[slot].first+offset/PAGE_SIZE].page))[offset%PAGE_SIZE];
}
/* The initramfs slot: the public archive, zero padding to a multiple of four,
 * then the device's uncompressed newc archive (the kernel unpacks one after
 * the other and skips the padding). */
static int device_archive_follows(struct store *s)
{
    unsigned int start=(TARGET_PUBLIC_INITRAMFS_BYTES+3u)&~3u,i;
    if(s->slot[2].bytes<start+110u || s->slot[2].bytes-TARGET_PUBLIC_INITRAMFS_BYTES>TARGET_DEVICE_ARCHIVE_MAX)return 0;
    for(i=TARGET_PUBLIC_INITRAMFS_BYTES;i<start;++i)if(slot_byte(s,2,i))return 0;
    for(i=0;i<6;++i)if(slot_byte(s,2,start+i)!=(u8)"070701"[i])return 0;
    return 1;
}
/* The ATAGs name what the build fixed (tags, command line) and the plan: the
 * kernel, ATAGs and initramfs at their destinations, inside the RAM the tags
 * describe, clear of the boot footprint and the forbidden ranges. */
static int device_atags_valid(struct store *s)
{
    struct pw_plan plan;
    const u8 *atags=page_address(s->allocations[s->slot[1].first].page);
    const u8 *kernel=page_address(s->allocations[s->slot[0].first].page);
    unsigned int i;
    if(pw_atags_check(atags,s->slot[1].bytes,target_atag_kinds,ARRAY_SIZE(target_atag_kinds),target_cmdline))return 0;
    memset(&plan,0,sizeof(plan));
    plan.entry=destination(0);plan.machine_id=plan.expected_machine_id=TARGET_MACHINE;
    plan.boot_ram_base=PHYS_OFFSET;plan.identity_limit=TARGET_IDENTITY_LIMIT;
    plan.ram_count=ARRAY_SIZE(target_ram);plan.forbidden_count=ARRAY_SIZE(target_forbidden);
    plan.footprint_count=ARRAY_SIZE(target_footprint);plan.segment_count=PWS_SLOTS;
    for(i=0;i<plan.ram_count;++i)plan.ram[i]=target_ram[i];
    for(i=0;i<plan.forbidden_count;++i)plan.forbidden[i]=target_forbidden[i];
    for(i=0;i<plan.footprint_count;++i)plan.footprint[i]=target_footprint[i];
    for(i=0;i<PWS_SLOTS;++i) {
        plan.segments[i].kind=i==0?PW_ZIMAGE:i==1?PW_ATAGS:PW_INITRAMFS;
        plan.segments[i].destination.start=destination(i);
        plan.segments[i].destination.size=s->slot[i].pages*PAGE_SIZE;
        plan.segments[i].buffer_bytes=s->slot[i].bytes;
    }
    return pw_plan_validate(&plan,atags,s->slot[1].bytes,kernel,s->slot[0].bytes)==PW_VALID_DATA_ONLY;
}
static int seal(struct store *s)
{
    unsigned int i,j;
    SHA256_CTX ctx,head;
    u8 hash[32],public_hash[32];
    int public_seen=0;
    if(s->state!=PWS_WRITING)return -EPERM;
    for(i=0;i<PWS_SLOTS;++i)if(s->slot[i].written!=s->slot[i].bytes)return -ENODATA;
    /* Every slot is what the caller declared; the kernel and the public part
     * of the initramfs are also what the build pinned. */
    for(i=0;i<PWS_SLOTS;++i) {
        struct slot *slot=&s->slot[i];
        unsigned int done=0;
        sha256_init(&ctx);
        for(j=0;j<slot->pages;++j) {
            const u8 *page=page_address(s->allocations[slot->first+j].page);
            unsigned int take=min_t(unsigned int,slot->bytes-done,PAGE_SIZE);
            if(i==2 && !public_seen && done+take>=TARGET_PUBLIC_INITRAMFS_BYTES) {
                unsigned int first=TARGET_PUBLIC_INITRAMFS_BYTES-done;
                sha256_update(&ctx,page,first);
                head=ctx;sha256_final(&head,public_hash);public_seen=1;
                sha256_update(&ctx,page+first,take-first);
            } else sha256_update(&ctx,page,take);
            done+=take;
        }
        sha256_final(&ctx,hash);
        if(memcmp(hash,slot->expected,32) || (i==0 && memcmp(hash,target_kernel_sha256,32)))break;
    }
    if(i<PWS_SLOTS || s->slot[0].bytes!=TARGET_KERNEL_BYTES || !public_seen ||
       memcmp(public_hash,target_public_initramfs_sha256,32)) {
        wipe(&ctx,sizeof(ctx));wipe(&head,sizeof(head));wipe(hash,sizeof(hash));discard(s);return -EBADMSG;
    }
    wipe(&ctx,sizeof(ctx));wipe(&head,sizeof(head));wipe(hash,sizeof(hash));
    if(!device_archive_follows(s) || !device_atags_valid(s)) {discard(s);return -ENOEXEC;}
    s->device_checked=1;s->state=PWS_SEALED;return 0;
}
static u32 owned_phys(struct store *s,unsigned int index)
{
    return (u32)page_to_phys(s->allocations[index].page);
}
static u32 *list_page(struct store *s,unsigned int index)
{
    return page_address(s->allocations[s->list_first+index].page);
}
/* Producer emits a finite, allocator-owned indirection chain. */
static void list_emit(struct store *s,unsigned int *page,unsigned int *word,u32 value)
{
    if(*word==LIST_WORDS-1) {
        list_page(s,*page)[*word]=owned_phys(s,s->list_first+*page+1)|2u;
        ++*page;*word=0;++s->list_entries;
    }
    list_page(s,*page)[(*word)++]=value;++s->list_entries;
}
/* Independent consumer checks every actual stored word and every link against
 * page ownership. It never follows an unverified physical pointer. */
static int audit_plan(struct store *s)
{
    unsigned int i,j,page=0,word=0,entries=0,total=0;
    const u32 *table=page_address(s->allocations[s->control_first+2].page);
    /* Recheck every actual source, control, page-table and list allocation
     * against all regions that the copy/decompressor path may overwrite. */
    for(i=0;i<s->count;i++)
        if(!pws_allocation_allowed(PHYS_OFFSET,TARGET_FOOTPRINT_END,
            s->slot[0].bytes,s->slot[2].bytes,owned_phys(s,i),
            PAGE_SIZE<<s->allocations[i].order))return -EUCLEAN;
    for(i=0;i<IDENTITY_SECTIONS;++i) {
        u32 expected=i==(owned_phys(s,s->control_first)>>20)?(i<<20|PW3_IDENTITY_SECTION):0;
        if(table[i]!=expected)return -EUCLEAN;
    }
    for(i=0;i<PWS_SLOTS+1;++i) {
        unsigned int n=i<PWS_SLOTS?s->slot[i].pages+1:1;
        for(j=0;j<n;++j) {
            u32 expected;
            if(word==LIST_WORDS-1) {
                if(page+1>=s->list_pages || list_page(s,page)[word]!=
                   (owned_phys(s,s->list_first+page+1)|2u))return -EUCLEAN;
                ++page;word=0;++entries;
            }
            if(i==PWS_SLOTS)expected=4;
            else if(!j)expected=destination(i)|1u;
            else {expected=owned_phys(s,s->slot[i].first+j-1)|8u;++total;}
            if(list_page(s,page)[word++]!=expected)return -EUCLEAN;
            ++entries;
        }
    }
    for(i=word;i<LIST_WORDS;++i)if(list_page(s,page)[i])return -EUCLEAN;
    if(page+1!=s->list_pages || entries!=s->list_entries || total!=s->control_first)
        return -EUCLEAN;
    return 0;
}
static int prepare(struct store *s)
{
    unsigned int i,j,page=0,word=0;
    u32 ttbr,ttbcr,pa,*pgd;
    const u32 *current_pgd;
    int rc;
    if(s->state!=PWS_SEALED)return -EPERM;
    s->list_first=s->count;
    for(i=0;i<s->list_pages;++i) {rc=allocate_owned(s,0);if(rc)goto fail;}
    for(i=0;i<PWS_SLOTS;++i) {
        list_emit(s,&page,&word,destination(i)|1u);
        for(j=0;j<s->slot[i].pages;++j)
            list_emit(s,&page,&word,owned_phys(s,s->slot[i].first+j)|8u);
    }
    list_emit(s,&page,&word,4);
    pgd=page_address(s->allocations[s->control_first+2].page);
    /* Copy only high kernel/module mappings from this CPU's actual live PGD.
     * No TTBR write, cache maintenance or IRQ masking occurs. */
    preempt_disable();
    __asm__ volatile("mrc p15,0,%0,c2,c0,0":"=r"(ttbr));
    __asm__ volatile("mrc p15,0,%0,c2,c0,2":"=r"(ttbcr));
    pa=ttbr&~16383u;
    if(ttbcr || ((ttbr&0x3fff)!=0 && (ttbr&0x3fff)!=0x59) ||
       pa<(u32)PHYS_OFFSET || pa>(u32)PHYS_OFFSET+512u*1024u*1024u-16384u ||
       !pfn_valid(pa>>PAGE_SHIFT) || !pfn_valid((pa+16383)>>PAGE_SHIFT)) {
        preempt_enable();rc=-EOPNOTSUPP;goto fail;
    }
    current_pgd=phys_to_virt(pa);
    for(i=0;i<IDENTITY_SECTIONS;++i)pgd[i]=0;
    pgd[owned_phys(s,s->control_first)>>20]=(owned_phys(s,s->control_first)&0xfff00000u)|PW3_IDENTITY_SECTION;
    for(;i<4096;++i)pgd[i]=current_pgd[i];
#if PWS_PROFILE == 2
    if(plan_fault==4)pgd[0xc00]^=1;
    if(plan_fault==5)pgd[0x600]^=4;
#endif
    for(i=IDENTITY_SECTIONS;i<4096;++i)
        if(pgd[i]!=current_pgd[i])break;
    preempt_enable();
    if(i!=4096){rc=-EUCLEAN;goto fail;}
#if PWS_PROFILE == 2
    if(plan_fault==1)list_page(s,0)[1]^=PAGE_SIZE;
    if(plan_fault==2 && s->list_pages>1)list_page(s,0)[LIST_WORDS-1]^=PAGE_SIZE;
    if(plan_fault==3)list_page(s,0)[0]=4;
#endif
    rc=audit_plan(s);if(rc)goto fail;
    s->state=PWS_PREPARED;return 0;
fail:
    discard(s);return rc;
}

#include "native-handoff.h"

static long stage_ioctl(struct file *file,unsigned int command,unsigned long arg)
{
    struct store *s=file->private_data;
    struct pws_status status;
    struct pws_plan_status plan_status;
    unsigned int i;
    int rc;
    if(!capable(CAP_SYS_BOOT))return -EPERM;
    if(mutex_lock_interruptible(&s->lock))return -EINTR;
    switch(command) {
    case PWS_NATIVE_PROBE:
#if defined(PWS_EMULATOR)
        /* The emulator has none of the hardware the device probe samples, so it
         * does not suspend: the report the helper's gates read is written
         * directly (native-handoff.h native_probe_synthetic). The commit path
         * below is the device's own -- real suspend, syscore callback and
         * transition -- because that is what the start has to prove. */
        rc=arg?-EINVAL:native_probe_synthetic(s);
#else
        rc=arg?-EINVAL:native_cycle(s,0);
#endif
        break;
    case PWS_NATIVE_COMMIT:rc=arg?-EINVAL:native_cycle(s,1);break;
    case PWS_NATIVE_REPORT:rc=copy_to_user((void __user *)arg,&native_report,sizeof(native_report))?-EFAULT:0;break;
    case PWS_BEGIN:rc=begin(s,(void __user *)arg);break;
    case PWS_PUT:rc=put(s,(void __user *)arg);break;
    case PWS_SEAL:rc=arg?-EINVAL:seal(s);break;
    case PWS_PREPARE:rc=arg?-EINVAL:prepare(s);break;
    case PWS_PLAN_STATUS:
        memset(&plan_status,0,sizeof(plan_status));
        plan_status.version=PWS_VERSION;plan_status.state=s->state;
        if(s->state==PWS_PREPARED) {
            plan_status.list_pages=s->list_pages;plan_status.list_entries=s->list_entries;
            plan_status.source_pages=s->control_first;plan_status.pgd_entries=4096;
            plan_status.boot_footprint_verified=target_matches(s);plan_status.execute_available=1;
            plan_status.identity_entries=1;plan_status.copy_plan_verified=1;
        }
        rc=copy_to_user((void __user *)arg,&plan_status,sizeof(plan_status))?-EFAULT:0;break;
    case PWS_DISCARD:if(arg){rc=-EINVAL;break;}discard(s);rc=0;break;
    case PWS_STATUS:
        memset(&status,0,sizeof(status));status.version=PWS_VERSION;status.state=s->state;status.execute_available=1;
        for(i=0;i<PWS_SLOTS;++i) {
            status.written[i]=s->slot[i].written;status.sizes[i]=s->slot[i].bytes;
            status.payload_pages+=s->slot[i].pages;
        }
        status.control_pages=s->state==PWS_EMPTY?0:6+(s->state==PWS_PREPARED?s->list_pages:0);
        for(i=0;i<s->collisions;++i)status.collision_pages+=1u<<s->rejected[i].order;
        rc=copy_to_user((void __user *)arg,&status,sizeof(status))?-EFAULT:0;break;
    default:rc=-ENOTTY;
    }
    mutex_unlock(&s->lock);return rc;
}
static int stage_open(struct inode *inode,struct file *file)
{
    struct store *s;
    (void)inode;
    if(!capable(CAP_SYS_BOOT) || !(file->f_mode&FMODE_WRITE))return -EPERM;
    if(atomic_cmpxchg(&opened,0,1))return -EBUSY;
    s=kzalloc(sizeof(*s),GFP_KERNEL);
    if(!s){atomic_set(&opened,0);return -ENOMEM;}
    mutex_init(&s->lock);file->private_data=s;return 0;
}
static int stage_release(struct inode *inode,struct file *file)
{
    struct store *s=file->private_data;
    (void)inode;discard(s);kfree(s);file->private_data=NULL;
    atomic_set(&opened,0);return 0;
}
static const struct file_operations ops={
    .owner=THIS_MODULE,.open=stage_open,.release=stage_release,
    .unlocked_ioctl=stage_ioctl,.llseek=no_llseek,
};
static struct miscdevice device={.minor=MISC_DYNAMIC_MINOR,.name="pw3-first-kernel",.fops=&ops,.mode=0600};
static int __init stage_init(void)
{
    const struct new_utsname *u=init_utsname();
    BUILD_BUG_ON(sizeof(struct pws_begin)!=116);
    BUILD_BUG_ON(sizeof(struct pws_put)!=24);
    BUILD_BUG_ON(sizeof(struct pws_status)!=48);
    BUILD_BUG_ON(sizeof(struct pws_plan_status)!=40);
    /* The private table owns whole sections, so a bound inside one cannot be
     * built: a profile that asks for one would silently get the section below
     * it and a plan checked against a limit the table does not keep. */
    BUILD_BUG_ON(TARGET_IDENTITY_LIMIT&0xfffffu);
    BUILD_BUG_ON(IDENTITY_SECTIONS>=4096u || !IDENTITY_SECTIONS);
    /* Everything below the bound is cleared out of the private table, and
     * between the TTBR0 write and the branch into the control page the code
     * still running is this module's own (cache-ops.S tail, transition.S
     * pw3_enter_private_mapping). So the module area has to be at or above the
     * bound, or the table unmaps the instruction that installs it.
     *
     * It holds here by equality, not by luck of the draw: MODULES_VADDR is
     * PAGE_OFFSET - 16 MiB and this kernel is 3G/1G, so both are 0xbf000000.
     * A 2G/2G Kindle (imx60_duet_defconfig sets CONFIG_VMSPLIT_2G) puts the
     * module area at 0x7f000000 while its bound has to clear the top of RAM,
     * and there the two part company. That port needs the private table to
     * keep a second region, not a wider profile number -- and it should find
     * that out from this line rather than from a device that stops booting.
     * docs/design/porting-to-other-models.md section 6 says what it costs. */
    BUILD_BUG_ON(TARGET_IDENTITY_LIMIT>(unsigned int)MODULES_VADDR);
    if(!PWS_PROFILE)return -EOPNOTSUPP;
#if PWS_PROFILE == 1
    /* The stock kernel this module loads into, and the CPU it runs on: pinned by
     * the build from the profile (target-lock.h), not written here. They were
     * the PW3's firmware strings and MIDR as literals, which made a module for
     * the second model refuse to load on the firmware it was built for -- or,
     * worse, load into another one. The machine id and PHYS_OFFSET stay as the
     * board facts below, because a module never runs on another machine. */
    if(strcmp(u->release,TARGET_STOCK_RELEASE) ||
       strcmp(u->version,TARGET_STOCK_VERSION) ||
       strcmp(u->machine,"armv7l") || PHYS_OFFSET!=0x80000000)return -ENOEXEC;
#elif PWS_PROFILE == 2
    if(!strstr(u->release,"-pw3-core-smoke") || PHYS_OFFSET!=0x60000000)return -ENOEXEC;
#else
    (void)u;
#endif
    { int rc=native_init();if(rc)return rc;rc=misc_register(&device);if(rc)native_exit();return rc; }
}
static void __exit stage_exit(void) { if(PWS_PROFILE){misc_deregister(&device);native_exit();} }
module_init(stage_init);module_exit(stage_exit);
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Private fixed-payload native-PM handoff candidate; dry-run required");
