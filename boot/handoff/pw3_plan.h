/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef PW3_PLAN_H
#define PW3_PLAN_H

/* Pure data validation. No syscalls, physical mapping or execution backend. */
#ifndef PW3_ENABLE_EXECUTE
#define PW3_ENABLE_EXECUTE 0
#endif
#if PW3_ENABLE_EXECUTE
#error "PW3 execute backend is not implemented or device-verified"
#endif

typedef unsigned char pw_u8;
typedef unsigned int pw_u32;
typedef unsigned long long pw_u64;
typedef __UINTPTR_TYPE__ pw_uptr;
typedef char pw_u32_must_be_32_bits[(sizeof(pw_u32) == 4) ? 1 : -1];
typedef char pw_u64_must_be_64_bits[(sizeof(pw_u64) == 8) ? 1 : -1];

#define PW_ATAG_CAP 4096u
#define PW_CMDLINE_CAP 1024u
#define PW_MAX_RANGES 16u
#define PW_MAX_SEGMENTS 3u
#define PW_PAGE_SIZE 4096u
#define PW_ATAG_NONE 0u
#define PW_ATAG_CORE 0x54410001u
#define PW_ATAG_MEM 0x54410002u
#define PW_ATAG_RAMDISK 0x54410004u
#define PW_ATAG_INITRD 0x54410005u
#define PW_ATAG_INITRD2 0x54420005u
#define PW_ATAG_CMDLINE 0x54410009u

enum pw_error {
    PW_VALID_DATA_ONLY = 0,
    PW_E_ARGUMENT = -1, PW_E_LENGTH = -2, PW_E_FORMAT = -3,
    PW_E_DUPLICATE = -4, PW_E_RANGE = -5, PW_E_OVERLAP = -6,
    PW_E_CAPACITY = -7, PW_E_ALIGNMENT = -8, PW_E_CONTRACT = -9,
    PW_E_EXECUTE_DISABLED = -10
};

/* Half-open physical interval, with addition promoted before overflow checks. */
struct pw_range { pw_u32 start, size; };
struct pw_atags {
    pw_u32 bytes, tag_count, opaque_tag_count, mem_count;
    struct pw_range mem[PW_MAX_RANGES];
    pw_u32 has_cmdline, has_initrd;
    struct pw_range initrd;
};

enum pw_kind { PW_ZIMAGE = 1, PW_ATAGS = 2, PW_INITRAMFS = 3 };
struct pw_segment {
    pw_u32 kind;
    struct pw_range destination; /* kexec destination: page aligned/rounded */
    pw_u32 buffer_bytes;         /* exact file/data length */
};
struct pw_plan {
    pw_u32 entry, machine_id, expected_machine_id, boot_ram_base;
    /* Exclusive upper bound of the region that is mapped one to one in the
     * table this will run under -- not "TASK_SIZE", although on this machine it
     * is the same number. Here the module builds that window itself, in the
     * half the stock kernel leaves to user space, because PAGE_OFFSET
     * (0xc0000000) and PHYS_OFFSET (0x80000000) differ and so the kernel's own
     * lowmem mapping is not one to one. On a Kindle whose kernel has a 2G/2G
     * split the two constants are equal, lowmem is already one to one, and the
     * bound is the end of lowmem instead (design/porting-to-other-models.md
     * section 6). That port also has to stop prepare() in stage.c from
     * clearing the module area, which on a 2G/2G kernel is below the bound
     * rather than above it; the module refuses to build there until it does,
     * on the BUILD_BUG_ON beside IDENTITY_SECTIONS. */
    pw_u32 identity_limit;
    pw_u32 ram_count, forbidden_count, footprint_count, segment_count;
    struct pw_range ram[PW_MAX_RANGES];
    /* Caller-derived hardware reservations and still-live relocation state. */
    struct pw_range forbidden[PW_MAX_RANGES];
    /* Explicit decompressed kernel/BSS AND decompressor scratch/relocation. */
    struct pw_range footprint[PW_MAX_RANGES];
    struct pw_segment segments[PW_MAX_SEGMENTS];
};

int pw_range_valid(struct pw_range r);
int pw_ranges_overlap(struct pw_range a, struct pw_range b);
int pw_atags_parse(const pw_u8 *data, pw_u32 bytes, struct pw_atags *out);
/* Input/output must be separate buffers. Old boot-media tags are replaced;
 * all other complete tags are copied exactly, including unknown extensions.
 * 'used' is zero on failure; do not consume partial output after an error. */
int pw_atags_rewrite(const pw_u8 *src, pw_u32 src_bytes,
                    const char *cmdline, pw_u32 cmdline_bytes,
                    struct pw_range initrd,
                    pw_u8 *dst, pw_u32 capacity, pw_u32 *used);
/* Every tag is one of the `kinds` given, and there is exactly one CMDLINE,
 * equal to `cmdline`. For ATAGs made on the device from its own values: the
 * build fixes which tags may be passed and the whole command line. */
int pw_atags_check(const pw_u8 *data, pw_u32 bytes, const pw_u32 *kinds,
                   pw_u32 kind_count, const char *cmdline);
/* Validates FORMAT and declared intervals, never their hardware provenance. */
int pw_plan_validate(const struct pw_plan *plan, const pw_u8 *atags,
                     pw_u32 atags_bytes, const pw_u8 *zimage,
                     pw_u32 zimage_bytes);
int pw_execution_status(void);

#endif
