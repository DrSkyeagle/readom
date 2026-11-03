/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef PW3_CONTROLLED_HANDOFF
#error "Explicit controlled-candidate build required"
#endif
#if PWS_PROFILE != 1 && PWS_PROFILE != 2
#error "Pinned physical or virtual profile required"
#endif
#include <linux/io.h>
#include <linux/suspend.h>
#include <linux/syscore_ops.h>
#include <linux/sched.h>
#include <linux/thread_info.h>
#include <linux/kallsyms.h>
#include <linux/kobject.h>
#include <asm/cacheflush.h>
#include <asm/outercache.h>
#include <asm/mach-types.h>
#include "native-abi.h"
#include "engine-guard.h"
#include "target-lock.h"
#include "backend.h"
static struct pws_native_report native_report;
static struct store *native_active;
static struct task_struct *native_owner;
static struct cache_backend native_backend;
#if PWS_PROFILE == 2
static void *native_stack;
#endif
#if PWS_PROFILE == 1
static void __iomem *native_ccm,*native_dcp,*native_usb;
#endif
static int native_commit;
#if PWS_PROFILE == 2
static int native_test_reject;
module_param(native_test_reject,int,0600);
static int native_test_stack;
module_param(native_test_stack,int,0600);
#endif
#if PWS_PROFILE == 1
static ssize_t (*native_show_pm_test)(struct kobject *,struct kobj_attribute *,char *);
static int native_mode_core(void)
{
    char text[128];ssize_t n;
    /* Exact pinned stock getter: six constant labels and sprintf only; it
     * ignores the first two arguments and does not allocate or sleep. */
    n=native_show_pm_test(NULL,NULL,text);
    if(n<=0 || n>=(ssize_t)sizeof(text))return 0;
    text[n]=0;return strstr(text,"[core]")!=NULL;
}
#else
static int native_mode_core(void){return 1;}
#endif
static const unsigned char gate_bank[PWS_GATE_COUNT]={5,3,3,3,3,3,3,1,1,6,6,6,6,6};
static const unsigned char gate_shift[PWS_GATE_COUNT]={6,2,4,10,6,8,0,26,10,0,2,4,6,8};
extern void sim_flush_cache_all(void);
#if PWS_PROFILE == 2
extern int pws_call_on_stack(int (*fn)(const struct cache_backend *),const struct cache_backend *b,void *top);
#endif
static int target_matches(const struct store *s)
{
    return s->device_checked && s->slot[0].bytes==TARGET_KERNEL_BYTES &&
        !memcmp(s->slot[0].expected,target_kernel_sha256,32) &&
        s->slot[2].bytes>TARGET_PUBLIC_INITRAMFS_BYTES &&
        s->slot[2].bytes-TARGET_PUBLIC_INITRAMFS_BYTES<=TARGET_DEVICE_ARCHIVE_MAX;
}
static int native_prepare(struct store *s)
{
    struct tr_parameters *p;void *control;u32 i;
    unsigned int blob=pw3_transition_end-pw3_transition_start;
    unsigned int param=pw3_transition_params-pw3_transition_start;
    if(s->state!=PWS_PREPARED || !target_matches(s))return -EPERM;
    if(TARGET_FOOTPRINT_END>PHYS_OFFSET+64u*1024u*1024u)return -EUCLEAN;
    if(blob>PAGE_SIZE || param+sizeof(*p)>blob || audit_plan(s))return -EUCLEAN;
    if(!outer_cache.flush_all || !outer_cache.disable || !outer_cache.inv_all)return -EOPNOTSUPP;
    control=page_address(s->allocations[s->control_first].page);
#if PWS_PROFILE == 2
    native_stack=page_address(s->allocations[s->control_first+1].page);
#endif
    memcpy(control,pw3_transition_start,blob);p=(void *)((u8 *)control+param);memset(p,0,sizeof(*p));
    p->list=owned_phys(s,s->list_first);p->entry=destination(0);p->machine=TARGET_MACHINE;p->atags=destination(1);
    p->ram_start=PHYS_OFFSET;p->ram_end=PHYS_OFFSET+512u*1024u*1024u;
    p->budget=s->list_entries;p->control=owned_phys(s,s->control_first);p->nr_segments=PWS_SLOTS;p->expected_pages=s->control_first;
    for(i=0;i<PWS_SLOTS;i++){p->segments[i].start=destination(i);p->segments[i].end=destination(i)+s->slot[i].pages*PAGE_SIZE;}
    memset(&native_backend,0,sizeof(native_backend));
    native_backend.physical_control=p->control;native_backend.physical_pgdir=owned_phys(s,s->control_first+2);
    native_backend.private=page_address(s->allocations[s->control_first+2].page);
    native_backend.flush_inner=sim_flush_cache_all;native_backend.flush_outer=outer_flush_all;native_backend.disable_outer=outer_disable;
    native_backend.invalidate_outer=outer_inv_all;
    flush_icache_range((unsigned long)control,(unsigned long)control+blob);
    return 0;
}
static int native_sample(void)
{
    u32 cpsr,midr,sctlr,ttbcr,prrr,nmrr,dacr,ccgr[7],ctrl=0,i,valid=1,clear=1;
    __asm__ volatile("mrs %0,cpsr":"=r"(cpsr));
    __asm__ volatile("mrc p15,0,%0,c0,c0,0":"=r"(midr));
    __asm__ volatile("mrc p15,0,%0,c1,c0,0":"=r"(sctlr));
    __asm__ volatile("mrc p15,0,%0,c2,c0,2":"=r"(ttbcr));
    __asm__ volatile("mrc p15,0,%0,c10,c2,0":"=r"(prrr));
    __asm__ volatile("mrc p15,0,%0,c10,c2,1":"=r"(nmrr));
    __asm__ volatile("mrc p15,0,%0,c3,c0,0":"=r"(dacr));
    native_report.irq_fiq_masked=(cpsr&0xff)==0xd3;
    native_report.cpu_profile_valid=native_report.irq_fiq_masked && pw3_identity_profile(sctlr,prrr,nmrr) && !ttbcr && (sctlr&0x1005)==0x1005 && !(sctlr&0x22000000) && (dacr&3)==1;
#if PWS_PROFILE == 1
    native_report.cpu_profile_valid&=midr==0x412fc09au && prrr==0x000a81a8u && nmrr==0x40e040e0u;
    for(i=0;i<7;i++)ccgr[i]=readl(native_ccm+0x68+i*4);
    ctrl=readl(native_dcp);
    /* Reject absent/unreadable/all-ones decodes. Never read keys, descriptors
     * or cipher buffers. An ungated DCP must additionally have all channels disabled and no activity. */
    valid=(ccgr[0]&3)!=0 && ((ccgr[3]>>22)&3)!=0 && (ctrl&0x20000000u) && !(ctrl&0x0f1ffe00u);
    native_report.dcp_reset=!!(ctrl&0x80000000u);native_report.dcp_clock_gated=!!(ctrl&0x40000000u);
    native_report.dcp_stopped=native_report.dcp_reset || native_report.dcp_clock_gated;
    native_report.hardware_sampled=1;
    native_report.dcp_ctrl=ctrl;
    /* Report only activity/count bits; no keys or descriptor addresses. */
    if(!native_report.dcp_stopped){
        native_report.dcp_stat_activity=readl(native_dcp+0x10)&0x0fff0000u;
        native_report.dcp_channel_enable=readl(native_dcp+0x20)&0xffu;
        native_report.dcp_csc_enable=readl(native_dcp+0x300)&1u;
        for(i=0;i<4;i++)native_report.dcp_sema[i]=(readl(native_dcp+0x110+i*0x40)>>16)&0xffu;
    }
    /* USB OTG and H1 share this gate. Never enable a clock just to inspect it. */
    if(ccgr[6]&3u){
        native_report.usb_sampled=1;
        for(i=0;i<2;i++){
            void __iomem *u=native_usb+i*0x200;
            native_report.usb[i].id=readl(u);
            native_report.usb[i].cap=readl(u+0x100);
            native_report.usb[i].dciversion=readl(u+0x120);
            native_report.usb[i].dccparams=readl(u+0x124);
            native_report.usb[i].command=readl(u+0x140);
            native_report.usb[i].status=readl(u+0x144);
            native_report.usb[i].mode=readl(u+0x1a8);
            native_report.usb[i].prime=readl(u+0x1b0);
            native_report.usb[i].flush=readl(u+0x1b4);
            native_report.usb[i].endpoints=readl(u+0x1b8);
        }
    }
#else
    /* Diskless, networkless VExpress harness. No hardware claim is exported. */
    native_report.cpu_profile_valid&=(midr&0xff0ffff0u)==0x410fc090u;
    for(i=0;i<7;i++)ccgr[i]=0;
    native_report.dcp_stopped=!native_test_reject;
    native_report.dcp_ctrl=native_test_reject?0x20000000u:0xe0000000u;
    native_report.dcp_channel_enable=native_test_reject?1u:0u;
    (void)ctrl;(void)prrr;(void)nmrr;
#endif
    for(i=0;i<PWS_GATE_COUNT;i++){native_report.gate[i]=(ccgr[gate_bank[i]]>>gate_shift[i])&3u;if(native_report.gate[i])clear=0;}
    native_report.registers_valid=valid;native_report.gates_clear=clear;
    __asm__ volatile("dsb\n\tisb":::"memory");
    native_report.usb_idle_observed=pws_usb_idle(&native_report);
    native_report.dcp_idle_observed=pws_dcp_idle(&native_report);
    return native_report.cpu_profile_valid && valid && pws_engine_fields_ok(&native_report);
}
static int native_core_suspend(void)
{
    unsigned long flags;u32 ttbr,ttbcr,i,*pgd;int okay,rc=0;
    if(!native_active || native_owner!=current)return 0;
    native_report.suspend_samples++;
    if(!native_mode_core()){native_report.callback_rejected=1;return -EPERM;}
    local_irq_save(flags);local_fiq_disable();preempt_disable();
    okay=native_sample();
    if(native_commit && (!okay || native_report.suspend_samples!=1)) {native_report.callback_rejected=1;rc=-EBUSY;goto out;}
    __asm__ volatile("mrc p15,0,%0,c2,c0,0":"=r"(ttbr));
    __asm__ volatile("mrc p15,0,%0,c2,c0,2":"=r"(ttbcr));
    if(ttbcr || (ttbr&~16383u)<PHYS_OFFSET || (ttbr&~16383u)>PHYS_OFFSET+512u*1024u*1024u-16384u){rc=-EOPNOTSUPP;goto out;}
    native_backend.original=phys_to_virt(ttbr&~16383u);pgd=(u32 *)native_backend.private;
    for(i=IDENTITY_SECTIONS;i<4096;i++)pgd[i]=native_backend.original[i];
    native_backend.expected_thread_info=(u32)task_thread_info(native_owner);
    native_report.backend_check_performed=1;
#if PWS_PROFILE == 2
    if(native_test_stack)rc=pws_call_on_stack(pw3_cache_backend_check,&native_backend,(u8 *)native_stack+PAGE_SIZE);
    else
#endif
    rc=pw3_cache_backend_check(&native_backend);
    native_report.backend_result=rc;
    if(rc){native_report.callback_rejected=1;if(!native_commit)rc=0;goto out;}
    if(native_commit){rc=pw3_cache_backend(&native_backend);native_report.backend_result=rc;}
out:
    preempt_enable();__asm__ volatile("msr cpsr_c,%0"::"r"(flags):"memory","cc");return rc;
}
static void native_core_resume(void)
{
    if(native_active && native_owner==current)native_report.resume_samples++;
}
static struct syscore_ops native_ops={.suspend=native_core_suspend,.resume=native_core_resume};
static int native_cycle(struct store *s,int commit)
{
    int rc;
    if(!native_mode_core())return -EPERM;
    if(commit && !s->native_ready)return -EPERM;
    s->native_ready=0;
    rc=native_prepare(s);if(rc)return rc;
    memset(&native_report,0,sizeof(native_report));native_report.version=4;native_report.profile=PWS_PROFILE;native_report.commit_requested=commit;
    native_commit=commit;native_owner=current;native_active=s;
    native_report.pm_called=1;rc=pm_suspend(PM_SUSPEND_MEM);
    native_active=NULL;native_owner=NULL;native_report.pm_result=rc;native_report.returned_to_caller=1;
    if(!commit && !rc && native_report.suspend_samples==1 && native_report.resume_samples==1 && native_report.cpu_profile_valid && native_report.registers_valid && native_report.backend_check_performed && !native_report.backend_result && !native_report.callback_rejected && pws_engine_fields_ok(&native_report))s->native_ready=1;
    return rc;
}
static int native_init(void)
{
    BUILD_BUG_ON(sizeof(struct pws_native_report)!=260);
    if(__machine_arch_type!=TARGET_MACHINE)return -ENOEXEC;
#if PWS_PROFILE == 1
    {
        SHA256_CTX hash;u8 actual[32];
        native_show_pm_test=(void *)kallsyms_lookup_name("pm_test_show");
        if(!native_show_pm_test)return -EOPNOTSUPP;
        sha256_init(&hash);sha256_update(&hash,(const u8 *)native_show_pm_test,TARGET_PM_SHOW_BYTES);sha256_final(&hash,actual);
        if(memcmp(actual,target_pm_show_sha256,32))return -ENOEXEC;
    }
    {
        unsigned int i;
        const void *callbacks[3]={(void *)outer_cache.flush_all,(void *)outer_cache.disable,(void *)outer_cache.inv_all};
        for(i=0;i<3;i++){
            const void *symbol=(void *)kallsyms_lookup_name(target_outer_names[i]);SHA256_CTX hash;u8 actual[32];
            if(!symbol || symbol!=callbacks[i])return -ENOEXEC;
            sha256_init(&hash);sha256_update(&hash,symbol,target_outer_bytes[i]);sha256_final(&hash,actual);
            if(memcmp(actual,target_outer_sha256[i],32))return -ENOEXEC;
        }
    }
    native_ccm=ioremap(0x020c4000u,4096);native_dcp=ioremap(0x020fc000u,4096);native_usb=ioremap(0x02184000u,4096);
    if(!native_ccm || !native_dcp || !native_usb){if(native_ccm)iounmap(native_ccm);if(native_dcp)iounmap(native_dcp);if(native_usb)iounmap(native_usb);return -ENOMEM;}
#endif
    if(!mutex_trylock(&pm_mutex)){
#if PWS_PROFILE == 1
        iounmap(native_ccm);iounmap(native_dcp);iounmap(native_usb);
#endif
        return -EBUSY;
    }
    register_syscore_ops(&native_ops);mutex_unlock(&pm_mutex);return 0;
}
static void native_exit(void)
{
    mutex_lock(&pm_mutex);unregister_syscore_ops(&native_ops);mutex_unlock(&pm_mutex);
#if PWS_PROFILE == 1
    iounmap(native_ccm);iounmap(native_dcp);iounmap(native_usb);
#endif
}
