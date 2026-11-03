/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef PW3_ENGINE_GUARD_H
#define PW3_ENGINE_GUARD_H
/* Fixed observed PW3 controller revision and roles. Read-only predicates;
 * no clock, reset, queue or register mutation is provided here. */
static int pws_usb_idle(const struct pws_native_report *r)
{
    unsigned int i;
    if(r->usb_sampled!=1)return 0;
    for(i=0;i<2;i++){
        if(r->usb[i].id!=0xe401fa05u || r->usb[i].cap!=0x01000040u ||
           r->usb[i].dciversion!=1 || r->usb[i].dccparams!=0x188u ||
           (r->usb[i].command&3u) || r->usb[i].prime ||
           r->usb[i].flush || r->usb[i].endpoints)return 0;
    }
    if(r->usb[0].mode!=0x0au || r->usb[1].mode!=0x13u)return 0;
    /* EHCI: schedules disabled and inactive, controller acknowledges halt. */
    return !(r->usb[1].command&0x30u) && (r->usb[1].status&0xd000u)==0x1000u;
}
static int pws_dcp_ctrl_valid(const struct pws_native_report *r)
{
    return (r->dcp_ctrl&0x20000000u) && !(r->dcp_ctrl&0x0f1ffe00u);
}
static int pws_dcp_idle(const struct pws_native_report *r)
{
    unsigned int i;
    if(!pws_dcp_ctrl_valid(r) || (r->dcp_ctrl&0xc0000000u) ||
       r->dcp_channel_enable || r->dcp_stat_activity || r->dcp_csc_enable)return 0;
    for(i=0;i<4;i++)if(r->dcp_sema[i])return 0;
    return 1;
}
static int pws_engine_fields_ok(const struct pws_native_report *r)
{
    unsigned int i;
    for(i=0;i<PWS_GATE_COUNT;i++)if(i!=9 && r->gate[i])return 0;
    if(r->gate[9] && (r->gate[9]!=3 || !pws_usb_idle(r)))return 0;
    return pws_dcp_ctrl_valid(r) && ((r->dcp_ctrl&0xc0000000u) || pws_dcp_idle(r));
}
#endif
