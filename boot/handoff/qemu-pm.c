/* SPDX-License-Identifier: GPL-2.0-only */
/* Synthetic PM provider for the diskless VExpress integration harness ONLY. */
#include <linux/module.h>
#include <linux/suspend.h>
#include <linux/utsname.h>
#include <linux/kallsyms.h>
#include <asm/mach-types.h>
static int entered;
static void (*set_ops)(struct platform_suspend_ops *);
static int valid(suspend_state_t state){return state==PM_SUSPEND_MEM;}
module_param(entered,int,0444);
static int test_enter(suspend_state_t state){if(state!=PM_SUSPEND_MEM)return -EINVAL;entered++;return 0;}
static struct platform_suspend_ops ops={.valid=valid,.enter=test_enter};
static int __init start(void){if(__machine_arch_type!=2272 || strcmp(init_uts_ns.name.release,"3.0.35-pw3-core-smoke"))return -ENOEXEC;set_ops=(void *)kallsyms_lookup_name("suspend_set_ops");if(!set_ops)return -ENOENT;set_ops(&ops);return 0;}
static void __exit stop(void){set_ops(NULL);}
module_init(start);module_exit(stop);MODULE_LICENSE("GPL");
