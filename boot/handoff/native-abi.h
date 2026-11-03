/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef PW3_NATIVE_ABI_H
#define PW3_NATIVE_ABI_H
#include "stage_abi.h"
#define PWS_NATIVE_PROBE _IO('W',0x71)
#define PWS_NATIVE_COMMIT _IO('W',0x72)
#define PWS_NATIVE_REPORT _IOR('W',0x73,struct pws_native_report)
#define PWS_GATE_COUNT 14
struct pws_native_report {
    pws_u32 version,profile,commit_requested,pm_called;
    pws_u32 suspend_samples,resume_samples,irq_fiq_masked,cpu_profile_valid;
    pws_u32 registers_valid,hardware_sampled,gates_clear,dcp_stopped;
    pws_u32 gate[PWS_GATE_COUNT];
    pws_u32 dcp_reset,dcp_clock_gated,callback_rejected,returned_to_caller;
    signed int pm_result,backend_result;
    pws_u32 dcp_ctrl,dcp_stat_activity,dcp_channel_enable,dcp_csc_enable,dcp_sema[4];
    pws_u32 usb_sampled;
    struct {pws_u32 id,cap,dciversion,dccparams,command,status,mode,prime,flush,endpoints;} usb[2];
    pws_u32 diagnostic_only,usb_idle_observed,dcp_idle_observed;
    pws_u32 backend_check_performed;
};
#endif
