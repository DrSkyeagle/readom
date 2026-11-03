/* SPDX-License-Identifier: GPL-2.0-only */
#include "pw3_plan.h"

static pw_u32 get32(const pw_u8 *p)
{
    return (pw_u32)p[0] | ((pw_u32)p[1] << 8) |
           ((pw_u32)p[2] << 16) | ((pw_u32)p[3] << 24);
}
static void put32(pw_u8 *p, pw_u32 v)
{
    unsigned i;
    for (i = 0; i < 4; ++i) p[i] = (pw_u8)(v >> (8 * i));
}
static void zero(void *p, pw_u32 n)
{
    pw_u8 *d = p;
    while (n--) *d++ = 0;
}
static void copy(pw_u8 *d, const pw_u8 *s, pw_u32 n)
{
    while (n--) *d++ = *s++;
}
static pw_u64 end(struct pw_range r)
{
    return (pw_u64)r.start + r.size;
}
int pw_range_valid(struct pw_range r)
{
    return r.size != 0 && end(r) <= 0x100000000ull;
}
int pw_ranges_overlap(struct pw_range a, struct pw_range b)
{
    return (pw_u64)a.start < end(b) && (pw_u64)b.start < end(a);
}
static int contains(struct pw_range outer, struct pw_range inner)
{
    return inner.start >= outer.start && end(inner) <= end(outer);
}
static int inside_any(struct pw_range r, const struct pw_range *map, pw_u32 n)
{
    pw_u32 i;
    for (i = 0; i < n; ++i) if (contains(map[i], r)) return 1;
    return 0;
}
static int validate_map(const struct pw_range *r, pw_u32 n, int disjoint)
{
    pw_u32 i, j;
    for (i = 0; i < n; ++i) {
        if (!pw_range_valid(r[i])) return PW_E_RANGE;
        for (j = 0; disjoint && j < i; ++j)
            if (pw_ranges_overlap(r[i], r[j])) return PW_E_OVERLAP;
    }
    return PW_VALID_DATA_ONLY;
}
static int buffers_overlap(const void *a, pw_u32 an, const void *b, pw_u32 bn)
{
    pw_uptr x = (pw_uptr)a, y = (pw_uptr)b;
    /* Subtraction avoids pointer-end wrap and ordering unrelated pointers. */
    return x <= y ? y - x < an : x - y < bn;
}

int pw_atags_parse(const pw_u8 *data, pw_u32 bytes, struct pw_atags *out)
{
    struct pw_atags a;
    pw_u32 offset = 0, words, tag, len, i;
    if (!data || !out || buffers_overlap(data, bytes, out, sizeof(*out)))
        return PW_E_ARGUMENT;
    zero(out, sizeof(*out));
    zero(&a, sizeof(a));
    if (bytes < 16 || bytes > PW_ATAG_CAP || (bytes & 3)) return PW_E_LENGTH;
    while (bytes - offset >= 8) {
        words = get32(data + offset);
        tag = get32(data + offset + 4);
        if (tag == PW_ATAG_NONE) {
            if (words || !a.tag_count || !a.mem_count) return PW_E_FORMAT;
            a.bytes = offset + 8;
            /* A bounded page dump may include only zero padding after NONE. */
            for (i = a.bytes; i < bytes; ++i)
                if (data[i]) return PW_E_FORMAT;
            *out = a;
            return PW_VALID_DATA_ONLY;
        }
        if (words < 2 || words > (bytes - offset) / 4) return PW_E_LENGTH;
        len = words * 4; /* multiplication follows bounded division */
        if (!a.tag_count && tag != PW_ATAG_CORE) return PW_E_FORMAT;
        if (++a.tag_count > 128) return PW_E_CAPACITY;
        switch (tag) {
        case PW_ATAG_CORE:
            if (offset) return PW_E_DUPLICATE;
            if (words != 2 && words != 5) return PW_E_FORMAT;
            break;
        case PW_ATAG_MEM:
            if (words != 4) return PW_E_FORMAT;
            if (a.mem_count == PW_MAX_RANGES) return PW_E_CAPACITY;
            a.mem[a.mem_count].size = get32(data + offset + 8);
            a.mem[a.mem_count].start = get32(data + offset + 12);
            ++a.mem_count;
            if (validate_map(a.mem, a.mem_count, 1)) return PW_E_RANGE;
            break;
        case PW_ATAG_INITRD2:
            if (words != 4) return PW_E_FORMAT;
            if (a.has_initrd++) return PW_E_DUPLICATE;
            a.initrd.start = get32(data + offset + 8);
            a.initrd.size = get32(data + offset + 12);
            if (!pw_range_valid(a.initrd)) return PW_E_RANGE;
            break;
        case PW_ATAG_CMDLINE:
            if (a.has_cmdline++) return PW_E_DUPLICATE;
            for (i = 8; i < len && data[offset + i]; ++i) {}
            if (i == len || i - 8 >= PW_CMDLINE_CAP) return PW_E_FORMAT;
            break;
        default:
            ++a.opaque_tag_count;
            break;
        }
        offset += len;
    }
    return PW_E_LENGTH; /* absent or truncated NONE */
}

int pw_atags_rewrite(const pw_u8 *src, pw_u32 src_bytes,
                    const char *cmdline, pw_u32 cmdline_bytes,
                    struct pw_range initrd, pw_u8 *dst,
                    pw_u32 capacity, pw_u32 *used)
{
    struct pw_atags a;
    pw_u32 in = 0, out = 0, n, tag, cmdtag, need, i;
    int rc;
    if (!used) return PW_E_ARGUMENT;
    *used = 0;
    if (!src || !dst || !cmdline) return PW_E_ARGUMENT;
    if (!cmdline_bytes || cmdline_bytes > PW_CMDLINE_CAP ||
        cmdline[cmdline_bytes - 1]) return PW_E_LENGTH;
    if (buffers_overlap(src, src_bytes, dst, capacity) ||
        buffers_overlap(cmdline, cmdline_bytes, dst, capacity)) return PW_E_ARGUMENT;
    for (i = 0; i + 1 < cmdline_bytes; ++i)
        if (!cmdline[i]) return PW_E_FORMAT;
    if (initrd.size ? !pw_range_valid(initrd) : initrd.start != 0) return PW_E_RANGE;
    rc = pw_atags_parse(src, src_bytes, &a);
    if (rc) return rc;
    cmdtag = 8 + ((cmdline_bytes + 3) & ~3u);
    need = cmdtag + 8 + (initrd.size ? 16 : 0);
    /* Size first: insufficient space never produces a partly valid blob. */
    while (in < a.bytes - 8) {
        n = get32(src + in) * 4;
        tag = get32(src + in + 4);
        if (tag != PW_ATAG_CMDLINE && tag != PW_ATAG_INITRD &&
            tag != PW_ATAG_INITRD2 && tag != PW_ATAG_RAMDISK) need += n;
        in += n;
    }
    if (need > capacity || need > PW_ATAG_CAP) return PW_E_CAPACITY;
    in = 0;
    while (in < a.bytes - 8) {
        n = get32(src + in) * 4;
        tag = get32(src + in + 4);
        if (tag != PW_ATAG_CMDLINE && tag != PW_ATAG_INITRD &&
            tag != PW_ATAG_INITRD2 && tag != PW_ATAG_RAMDISK) {
            copy(dst + out, src + in, n);
            out += n;
        }
        in += n;
    }
    zero(dst + out, cmdtag);
    put32(dst + out, cmdtag / 4);
    put32(dst + out + 4, PW_ATAG_CMDLINE);
    copy(dst + out + 8, (const pw_u8 *)cmdline, cmdline_bytes);
    out += cmdtag;
    if (initrd.size) {
        put32(dst + out, 4); put32(dst + out + 4, PW_ATAG_INITRD2);
        put32(dst + out + 8, initrd.start); put32(dst + out + 12, initrd.size);
        out += 16;
    }
    zero(dst + out, 8);
    *used = out + 8;
    return PW_VALID_DATA_ONLY;
}

int pw_atags_check(const pw_u8 *data, pw_u32 bytes, const pw_u32 *kinds,
                   pw_u32 kind_count, const char *cmdline)
{
    struct pw_atags a;
    pw_u32 offset = 0, words, tag, i, found = 0;
    int rc;
    if (!kinds || !cmdline) return PW_E_ARGUMENT;
    rc = pw_atags_parse(data, bytes, &a);
    if (rc) return rc;
    while (offset + 8 < a.bytes) {
        words = get32(data + offset);
        tag = get32(data + offset + 4);
        for (i = 0; i < kind_count && kinds[i] != tag; ++i) {}
        if (i == kind_count) return PW_E_CONTRACT;
        if (tag == PW_ATAG_CMDLINE) {
            const pw_u8 *text = data + offset + 8;
            for (i = 0; cmdline[i] && text[i] == (pw_u8)cmdline[i]; ++i) {}
            if (cmdline[i] || text[i]) return PW_E_CONTRACT;
            ++found;
        }
        offset += words * 4;
    }
    return found == 1 ? PW_VALID_DATA_ONLY : PW_E_CONTRACT;
}

int pw_plan_validate(const struct pw_plan *p, const pw_u8 *atag,
                     pw_u32 atag_bytes, const pw_u8 *zimage,
                     pw_u32 zimage_bytes)
{
    struct pw_atags a;
    pw_u32 i, j, seen = 0, tags = 0, kernel = 0, initrd = 0;
    struct pw_range d;
    int rc;
    if (!p || !zimage) return PW_E_ARGUMENT;
    if (!p->ram_count || p->ram_count > PW_MAX_RANGES ||
        p->forbidden_count > PW_MAX_RANGES || !p->footprint_count ||
        p->footprint_count > PW_MAX_RANGES || p->segment_count < 2 ||
        p->segment_count > PW_MAX_SEGMENTS) return PW_E_LENGTH;
    if (!p->machine_id || p->machine_id != p->expected_machine_id)
        return PW_E_CONTRACT;
    if (p->identity_limit <= p->boot_ram_base ||
        (p->identity_limit & (PW_PAGE_SIZE - 1))) return PW_E_CONTRACT;
    rc = pw_atags_parse(atag, atag_bytes, &a);
    if (rc) return rc;
    if (!a.has_cmdline) return PW_E_CONTRACT;
    rc = validate_map(p->ram, p->ram_count, 1);
    if (rc) return rc;
    rc = validate_map(p->forbidden, p->forbidden_count, 0);
    if (rc) return rc;
    rc = validate_map(p->footprint, p->footprint_count, 0);
    if (rc) return rc;
    for (i = 0; i < p->ram_count; ++i)
        if (!inside_any(p->ram[i], a.mem, a.mem_count) ||
            p->ram[i].start < p->boot_ram_base) return PW_E_CONTRACT;
    if (p->ram[0].start != p->boot_ram_base) return PW_E_CONTRACT;
    for (i = 0; i < p->segment_count; ++i) {
        const struct pw_segment *s = &p->segments[i];
        if (s->kind < PW_ZIMAGE || s->kind > PW_INITRAMFS) return PW_E_FORMAT;
        if (seen & (1u << s->kind)) return PW_E_DUPLICATE;
        seen |= 1u << s->kind;
        if (s->kind == PW_ZIMAGE) kernel = i;
        if (s->kind == PW_ATAGS) tags = i;
        if (s->kind == PW_INITRAMFS) initrd = i;
        d = s->destination;
        if (!pw_range_valid(d) || !s->buffer_bytes || s->buffer_bytes > d.size)
            return PW_E_RANGE;
        /* The old core computes mend using 32-bit unsigned long. Never pass
         * a mathematically valid interval whose exclusive end would wrap. */
        if (end(d) >= 0xffffffffull || end(d) > p->identity_limit) return PW_E_RANGE;
        if ((d.start | d.size) & (PW_PAGE_SIZE - 1)) return PW_E_ALIGNMENT;
        if (!inside_any(d, p->ram, p->ram_count)) return PW_E_RANGE;
        for (j = 0; j < i; ++j)
            if (pw_ranges_overlap(d, p->segments[j].destination)) return PW_E_OVERLAP;
        for (j = 0; j < p->forbidden_count; ++j)
            if (pw_ranges_overlap(d, p->forbidden[j])) return PW_E_OVERLAP;
    }
    if (!(seen & (1u << PW_ZIMAGE)) || !(seen & (1u << PW_ATAGS)))
        return PW_E_CONTRACT;
    /* This is the stock machine_kexec r2 contract, not a guessed PW3 address. */
    if (p->entry != p->segments[kernel].destination.start || p->entry < 0x7000 ||
        p->segments[tags].destination.start != p->entry - 0x7000 ||
        p->segments[tags].buffer_bytes != a.bytes || a.bytes > PW_PAGE_SIZE)
        return PW_E_CONTRACT;
    if ((pw_u64)p->segments[tags].destination.start >=
        (pw_u64)p->boot_ram_base + 0x100000) return PW_E_CONTRACT;
    if (zimage_bytes < 0x30 || zimage_bytes != p->segments[kernel].buffer_bytes ||
        get32(zimage + 0x24) != 0x016f2818u) return PW_E_FORMAT;
    /* Header load/start fields are NOT treated as a runtime memory footprint. */
    if (get32(zimage + 0x2c) <= get32(zimage + 0x28) ||
        (pw_u64)get32(zimage + 0x2c) - get32(zimage + 0x28) > zimage_bytes)
        return PW_E_FORMAT;
    if (a.has_initrd != !!(seen & (1u << PW_INITRAMFS))) return PW_E_CONTRACT;
    if (a.has_initrd && (a.initrd.start != p->segments[initrd].destination.start ||
                        a.initrd.size != p->segments[initrd].buffer_bytes))
        return PW_E_CONTRACT;
    for (i = 0; i < p->footprint_count; ++i) {
        d = p->footprint[i];
        if (!inside_any(d, p->ram, p->ram_count) || end(d) > p->identity_limit)
            return PW_E_RANGE;
        for (j = 0; j < p->forbidden_count; ++j)
            if (pw_ranges_overlap(d, p->forbidden[j])) return PW_E_OVERLAP;
        /* Compressed input may overlap output: zImage moves itself. Its
         * relocation/scratch range must ALSO be supplied in footprint[]. */
        if (pw_ranges_overlap(d, p->segments[tags].destination) ||
            (a.has_initrd && pw_ranges_overlap(d, p->segments[initrd].destination)))
            return PW_E_OVERLAP;
    }
    return PW_VALID_DATA_ONLY;
}

int pw_execution_status(void)
{
    return PW_E_EXECUTE_DISABLED;
}
