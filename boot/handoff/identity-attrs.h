/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef PW3_IDENTITY_ATTRS_H
#define PW3_IDENTITY_ATTRS_H
/* Short-descriptor section, privileged RW, executable, domain 0, TEX:CB=001.
 * This is Normal non-cacheable only under the verified TRE/PRRR/NMRR profile. */
#define PW3_IDENTITY_SECTION 0x406u
static inline int pw3_identity_profile(unsigned int sctlr,unsigned int prrr,unsigned int nmrr)
{
    return (sctlr&0x10800000u)==0x10800000u && !(sctlr&0x00080000u) &&
        ((prrr>>2)&3u)==2u && ((nmrr>>2)&3u)==0u && ((nmrr>>18)&3u)==0u;
}
#endif
