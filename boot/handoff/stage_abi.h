/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef PW3_STAGE_ABI_H
#define PW3_STAGE_ABI_H
#ifdef __KERNEL__
#include <linux/ioctl.h>
#else
#include <sys/ioctl.h>
#endif
typedef unsigned int pws_u32;
typedef unsigned long long pws_u64;
typedef unsigned char pws_u8;
#define PWS_VERSION 1u
#define PWS_SLOTS 3u
#define PWS_EMPTY 0u
#define PWS_WRITING 1u
#define PWS_SEALED 2u
#define PWS_PREPARED 3u
/* Fixed slots: zImage, ATAGs, initramfs. No destination addresses are accepted. */
struct pws_image { pws_u32 bytes; pws_u8 sha256[32]; };
struct pws_begin { pws_u32 version, reserved; struct pws_image images[PWS_SLOTS]; };
struct pws_put { pws_u32 slot, offset, bytes, reserved; pws_u64 user_buffer; };
struct pws_status {
    pws_u32 version, state, written[PWS_SLOTS], sizes[PWS_SLOTS];
    pws_u32 payload_pages, control_pages, collision_pages, execute_available;
};
/* Data-only preparation report. No addresses leave the kernel. */
struct pws_plan_status {
    pws_u32 version, state, list_pages, list_entries, source_pages;
    pws_u32 pgd_entries, identity_entries, copy_plan_verified;
    pws_u32 boot_footprint_verified, execute_available;
};
#define PWS_PREPARE _IO('W',0x36)
#define PWS_PLAN_STATUS _IOR('W',0x37,struct pws_plan_status)
#define PWS_BEGIN _IOW('W',0x31,struct pws_begin)
#define PWS_PUT _IOW('W',0x32,struct pws_put)
#define PWS_SEAL _IO('W',0x33)
#define PWS_STATUS _IOR('W',0x34,struct pws_status)
#define PWS_DISCARD _IO('W',0x35)
#endif
