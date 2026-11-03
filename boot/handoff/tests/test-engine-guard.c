/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include "native-abi.h"
#include "engine-guard.h"
#include "observed-engine-fixture.h"
static unsigned checks;
#define PASS(x) do{checks++;assert(x);}while(0)
#define REJECT(change) do{r=observed;change;PASS(!pws_engine_fields_ok(&r));}while(0)
int main(void){struct pws_native_report r=observed;unsigned i;PASS(sizeof(r)==260);PASS(pws_usb_idle(&r));PASS(pws_dcp_idle(&r));PASS(pws_engine_fields_ok(&r));
    for(i=0;i<PWS_GATE_COUNT;i++)if(i!=9){REJECT(r.gate[i]=1);REJECT(r.gate[i]=3);}
    REJECT(r.gate[9]=1);REJECT(r.gate[9]=2);REJECT(r.usb_sampled=0);
    for(i=0;i<2;i++){
        REJECT(r.usb[i].id^=1);REJECT(r.usb[i].cap^=1);REJECT(r.usb[i].dciversion=0);REJECT(r.usb[i].dccparams=0);
        REJECT(r.usb[i].command|=1);REJECT(r.usb[i].command|=2);REJECT(r.usb[i].prime=1);REJECT(r.usb[i].flush=1);REJECT(r.usb[i].endpoints=1);REJECT(r.usb[i].mode^=1);
    }
    REJECT(r.usb[1].command|=0x10);REJECT(r.usb[1].command|=0x20);REJECT(r.usb[1].status&=~0x1000u);REJECT(r.usb[1].status|=0x4000);REJECT(r.usb[1].status|=0x8000);
    REJECT(r.dcp_ctrl=0);REJECT(r.dcp_ctrl=~0u);REJECT(r.dcp_channel_enable=1);REJECT(r.dcp_stat_activity=0x10000);REJECT(r.dcp_stat_activity=0x1000000);REJECT(r.dcp_csc_enable=1);
    for(i=0;i<4;i++){REJECT(r.dcp_sema[i]=1);}
    r=observed;r.gate[9]=0;r.usb_sampled=0;PASS(pws_engine_fields_ok(&r));PASS(!pws_usb_idle(&r));
    r=observed;r.dcp_ctrl|=0x40000000;PASS(pws_engine_fields_ok(&r));PASS(!pws_dcp_idle(&r));
    r=observed;r.dcp_ctrl|=0x80000000;PASS(pws_engine_fields_ok(&r));
    printf("PW3_ENGINE_GUARD_PASS checks=%u observed_fixture=1\n",checks);return 0;}
