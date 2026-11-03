/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include "identity-attrs.h"
static unsigned checks;
#define CHECK(x) do{checks++;assert(x);}while(0)
int main(void){unsigned s=0x10c5387du,p=0x000a81a8u,n=0x40e040e0u;unsigned region=((PW3_IDENTITY_SECTION>>12)&1u)*4u+((PW3_IDENTITY_SECTION>>2)&3u);
CHECK(region==1);CHECK(((p>>(region*2))&3)==2);CHECK(((n>>(region*2))&3)==0);CHECK(((n>>(16+region*2))&3)==0);
CHECK((p&3)==0); /* old 0x402 selected Strongly-ordered, not executable Normal */
CHECK(pw3_identity_profile(s,p,n));CHECK(pw3_identity_profile(s,0xff0a81a8u,n));
CHECK(!pw3_identity_profile(s&~0x10000000u,p,n));CHECK(!pw3_identity_profile(s&~0x00800000u,p,n));CHECK(!pw3_identity_profile(s|0x80000,p,n));
CHECK(!pw3_identity_profile(s,p&~12u,n));CHECK(!pw3_identity_profile(s,(p&~12u)|4u,n));CHECK(!pw3_identity_profile(s,p,n|4u));CHECK(!pw3_identity_profile(s,p,n|0x40000u));
printf("PW3_IDENTITY_ATTRS_PASS checks=%u\n",checks);return 0;}
