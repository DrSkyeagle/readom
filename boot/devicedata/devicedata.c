/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "devicedata.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* PW3 (Wario), stock firmware 5.16.2.1.1: its /proc/cmdline, user FAT mount
 * and the Wi-Fi files its startup script selects (/etc/upstart/wifim; the
 * running ath6kl_sdio board_data_file parameter names the same file). */
const struct dd_profile dd_pw3_5_16_2_1_1 = {
    "log_pos_goal=0x8fff0000 console=ttymxc0,115200 ro ip=off quiet video=mxcepdcfb:E60,bpp=8,x_mem=3M",
    "/mnt/base-us",
    "/opt/ar6k/target/AR6003/hw2.1.1/bin/AR6003_wfo_calfile.bin",
    "/opt/ar6k/target/AR6003/hw2.1.1/bin/fw-4.bin",
    "3.0.35-lab126",
    "#8 PREEMPT Tue Aug 1 12:49:59 UTC 2023",
};

void dd_free(struct dd_buf *b) { free(b->data); b->data = NULL; b->size = b->cap = 0; }

static int fail(struct dd_why *why, const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(why->text, sizeof(why->text), format, args);
    va_end(args);
    return -1;
}
/* The same, for the checks that say which Kindle and which firmware this is. */
static int fail_other_device(struct dd_why *why, const char *text) {
    why->other_device = 1;
    return fail(why, "%s", text);
}

static int reserve(struct dd_buf *b, size_t more) {
    if (more > SIZE_MAX - b->size) return -1;
    if (b->size + more <= b->cap) return 0;
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < b->size + more) { if (cap > SIZE_MAX / 2) return -1; cap *= 2; }
    unsigned char *data = realloc(b->data, cap);
    if (!data) return -1;
    b->data = data; b->cap = cap;
    return 0;
}
static int append(struct dd_buf *b, const void *data, size_t size) {
    if (reserve(b, size)) return -1;
    if (size) memcpy(b->data + b->size, data, size);
    b->size += size;
    return 0;
}
static int append_zero(struct dd_buf *b, size_t size) {
    if (reserve(b, size)) return -1;
    memset(b->data + b->size, 0, size);
    b->size += size;
    return 0;
}
static int append_text(struct dd_buf *b, const char *format, ...) {
    char line[256];
    va_list args;
    va_start(args, format);
    int n = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    return n < 0 || (size_t)n >= sizeof(line) ? -1 : append(b, line, (size_t)n);
}
static void put32(unsigned char *p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = (unsigned char)(v >> (8 * i)); }
static uint32_t get32(const unsigned char *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* A whole regular file (or a /proc or /sys file, which reports no size) of at most `limit` bytes. */
static int read_file(const char *root, const char *path, size_t limit, struct dd_buf *out, struct dd_why *why) {
    char full[512];
    if (snprintf(full, sizeof(full), "%s%s", strcmp(root, "/") ? root : "", path) >= (int)sizeof(full))
        return fail(why, "path too long: %s", path);
    int fd = open(full, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    if (fd < 0) return fail(why, "cannot open %s (errno %d)", path, errno);
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) { close(fd); return fail(why, "not a regular file: %s", path); }
    out->size = 0;
    for (;;) {
        if (reserve(out, 65536)) { close(fd); return fail(why, "out of memory reading %s", path); }
        ssize_t n = read(fd, out->data + out->size, 65536);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { close(fd); return fail(why, "cannot read %s (errno %d)", path, errno); }
        if (!n) break;
        out->size += (size_t)n;
        if (out->size > limit) { close(fd); return fail(why, "%s exceeds %zu bytes", path, limit); }
    }
    close(fd);
    return 0;
}
/* A short text file as a string without its final newline. */
static int read_text(const char *root, const char *path, char *text, size_t size, struct dd_why *why) {
    struct dd_buf b = {0};
    if (read_file(root, path, size - 1, &b, why)) { dd_free(&b); return -1; }
    while (b.size && b.data[b.size - 1] == '\n') --b.size;
    int nul = b.size && memchr(b.data, 0, b.size) != NULL;
    if (b.size) memcpy(text, b.data, b.size);
    text[b.size] = 0;
    dd_free(&b);
    return nul ? fail(why, "NUL in %s", path) : 0;
}
static int all_of(const char *s, size_t n, int (*accept)(int)) {
    if (strlen(s) != n) return 0;
    for (size_t i = 0; i < n; ++i) if (!accept((unsigned char)s[i])) return 0;
    return 1;
}
static int decimal(const char *s, uint64_t *value) {
    if (!*s) return 0;
    uint64_t v = 0;
    for (; *s; ++s) {
        if (*s < '0' || *s > '9' || v > (UINT64_MAX - 9) / 10) return 0;
        v = v * 10 + (uint64_t)(*s - '0');
    }
    *value = v;
    return 1;
}

/* ---- ATAGs ---- */

/* The value of the one line that starts with `key`; NULL if none or several. */
static const char *line_value(const char *text, const char *key) {
    const char *found = NULL;
    size_t n = strlen(key);
    for (const char *line = text; *line;) {
        if (!strncmp(line, key, n)) { if (found) return NULL; found = line + n; }
        const char *end = strchr(line, '\n');
        if (!end) break;
        line = end + 1;
    }
    return found;
}

#define ATAG_CORE 0x54410001u
#define ATAG_MEM 0x54410002u
#define ATAG_REVISION 0x54410007u
#define ATAG_CMDLINE 0x54410009u
#define ATAG_REVISION16 0x5441000bu   /* Lab126: board ID */
#define ATAG_MACADDR 0x5441000du      /* Lab126: MAC address, then a 20-byte secret */
#define ATAG_BOOTMODE 0x5441000fu     /* Lab126: boot mode, then the post mode */
#define ATAG_INITRD2 0x54420005u

static int tag(struct dd_buf *b, uint32_t kind, const void *payload, size_t size) {
    unsigned char head[8];
    size_t padded = (size + 3) & ~(size_t)3;
    put32(head, (uint32_t)(2 + padded / 4));
    put32(head + 4, kind);
    return append(b, head, 8) || append(b, payload, size) || append_zero(b, padded - size);
}

/* The banks of System RAM in /proc/iomem (top level, page aligned), merged. */
static int ram_banks(const char *iomem, uint32_t start[16], uint32_t size[16], unsigned *count) {
    unsigned n = 0;
    for (const char *line = iomem; *line;) {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        char text[128];
        unsigned long long a, b;
        char name[64];
        if (len < sizeof(text) && line[0] != ' ') {
            memcpy(text, line, len); text[len] = 0;
            if (sscanf(text, "%llx-%llx : %63[^\n]", &a, &b, name) == 3 && !strcmp(name, "System RAM")) {
                if (b < a || b >= 0x100000000ull || a % 4096 || (b + 1) % 4096) return -1;
                if (n && a < (unsigned long long)start[n - 1] + size[n - 1]) return -1;
                if (n && a == (unsigned long long)start[n - 1] + size[n - 1]) size[n - 1] += (uint32_t)(b + 1 - a);
                else { if (n == 16) return -1; start[n] = (uint32_t)a; size[n] = (uint32_t)(b + 1 - a); ++n; }
            }
        }
        line += len + (end != NULL);
    }
    *count = n;
    return n ? 0 : -1;
}

/* Like the stock boot loader's tags, as the stock kernel keeps them in /proc:
 * CORE, REVISION, REVISION16, MACADDR, BOOTMODE, the RAM banks, then the
 * build's CMDLINE and INITRD2. Identity-service fields are not passed on: the
 * serial number, Bluetooth address, POST result and DDR vendor are left out,
 * the MAC secret and post mode are zero (nothing in the kernel reads them for
 * the hardware). Unknown or inconsistent values stop the start. */
/* Which Kindle and which firmware this is, and nothing else: the two values a
 * build is made for. Read on their own, and before anything is downloaded or
 * written, so that someone whose Kindle this build is not for is told so while
 * their storage is still untouched. Both are also checked again where they are
 * used, since the answer is read again there. */
int dd_device_matches(const char *root, const struct dd_profile *profile, struct dd_why *why) {
    char live[1024], stock[1024] = "", *save = NULL;
    struct dd_buf cpu = {0};
    if (read_text(root, "/proc/cmdline", live, sizeof(live), why)) return -1;
    for (char *token = strtok_r(live, " ", &save); token; token = strtok_r(NULL, " ", &save)) {
        if (!strcmp(token, "rootwait") || !strncmp(token, "root=", 5)) continue;
        if (strlen(stock) + strlen(token) + 2 > sizeof(stock)) return fail(why, "stock command line too long");
        if (*stock) strcat(stock, " ");
        strcat(stock, token);
    }
    if (strcmp(stock, profile->stock_cmdline))
        return fail_other_device(why, "stock command line is not this model's");
    /* The kernel the handover will call into. The command line above comes from
     * the boot loader, so a firmware that replaced only the kernel would pass
     * it and be stopped later by a check that cannot say why -- and "restart
     * and tap again" is no use to someone whose Kindle updated itself. */
    if (read_text(root, "/proc/version", live, sizeof(live), why)) return -1;
    {
        char expected[256];
        int n = snprintf(expected, sizeof(expected), "Linux version %s ", profile->kernel_release);
        if (n <= 0 || (size_t)n >= sizeof(expected)) return fail(why, "kernel release too long");
        if (strncmp(live, expected, (size_t)n) || !strstr(live, profile->kernel_version))
            return fail_other_device(why, "stock kernel is not this firmware's");
    }
    if (read_file(root, "/proc/cpuinfo", 65536, &cpu, why) || append_zero(&cpu, 1)) { dd_free(&cpu); return -1; }
    const char *hw = line_value((char *)cpu.data, "Hardware\t: ");
    const char *hw_end = hw ? strchr(hw, '\n') : NULL;
    int rc = 0;
    if (!hw) rc = fail(why, "no single Hardware line in cpuinfo");
    else if (!memmem(hw, hw_end ? (size_t)(hw_end - hw) : strlen(hw), "Wario", 5))
        rc = fail_other_device(why, "not a Wario board");
    dd_free(&cpu);
    return rc;
}

int dd_atags(const char *root, const struct dd_profile *profile, const char *cmdline,
             uint32_t initrd_start, uint32_t initrd_size, struct dd_buf *out, struct dd_why *why) {
    char board[64], mac[64], mode[64], live[1024];
    struct dd_buf cpu = {0}, iomem = {0};
    int rc = -1;
    out->size = 0;
    if (read_text(root, "/proc/board_id", board, sizeof(board), why) ||
        read_text(root, "/proc/mac_addr", mac, sizeof(mac), why) ||
        read_text(root, "/proc/bootmode", mode, sizeof(mode), why) ||
        read_text(root, "/proc/cmdline", live, sizeof(live), why)) return -1;
    if (!all_of(board, 16, isalnum)) return fail(why, "board ID is not 16 letters and digits");
    if (!all_of(mac, 12, isxdigit)) return fail(why, "MAC address is not 12 hex digits");
    if (strcmp(mode, "main")) return fail(why, "boot mode is not main");
    /* The stock command line, without what selected the stock root. */
    char stock[1024] = "", *save = NULL;
    for (char *t = strtok_r(live, " ", &save); t; t = strtok_r(NULL, " ", &save)) {
        if (!strcmp(t, "rootwait") || !strncmp(t, "root=", 5)) continue;
        if (strlen(stock) + strlen(t) + 2 > sizeof(stock)) return fail(why, "stock command line too long");
        if (*stock) strcat(stock, " ");
        strcat(stock, t);
    }
    if (strcmp(stock, profile->stock_cmdline))
        return fail_other_device(why, "stock command line is not this model's");
    size_t n = strlen(profile->stock_cmdline);
    if (strncmp(cmdline, profile->stock_cmdline, n) || cmdline[n] != ' ' || strlen(cmdline) >= 1024)
        return fail(why, "build command line does not extend the stock one");

    if (read_file(root, "/proc/cpuinfo", 65536, &cpu, why) || append_zero(&cpu, 1)) goto done;
    const char *hw = line_value((char *)cpu.data, "Hardware\t: "), *rev = line_value((char *)cpu.data, "Revision\t: ");
    if (!hw || !rev) { fail(why, "no single Hardware and Revision line in cpuinfo"); goto done; }
    const char *hw_end = strchr(hw, '\n');
    if (!memmem(hw, hw_end ? (size_t)(hw_end - hw) : strlen(hw), "Wario", 5)) { fail_other_device(why, "not a Wario board"); goto done; }
    char *end = NULL;
    errno = 0;
    unsigned long revision = strtoul(rev, &end, 16);
    if (errno || end == rev || (*end && *end != '\n') || revision > UINT32_MAX) { fail(why, "board Revision unreadable"); goto done; }

    uint32_t bank_start[16], bank_size[16];
    unsigned banks = 0;
    if (read_file(root, "/proc/iomem", 65536, &iomem, why) || append_zero(&iomem, 1)) goto done;
    if (ram_banks((char *)iomem.data, bank_start, bank_size, &banks) || bank_start[0] != 0x80000000u) {
        fail(why, "System RAM in iomem is not the Wario layout"); goto done;
    }
    if (initrd_size == 0 || initrd_start % 4096) { fail(why, "invalid initramfs range"); goto done; }

    unsigned char word[32] = {0};
    put32(word, 1); put32(word + 4, 4096); put32(word + 8, 0);
    if (tag(out, ATAG_CORE, word, 12)) goto oom;
    put32(word, (uint32_t)revision);
    if (tag(out, ATAG_REVISION, word, 4) || tag(out, ATAG_REVISION16, board, 16)) goto oom;
    memset(word, 0, sizeof(word)); memcpy(word, mac, 12);
    if (tag(out, ATAG_MACADDR, word, 32)) goto oom;
    memset(word, 0, sizeof(word)); memcpy(word, "main", 4);
    if (tag(out, ATAG_BOOTMODE, word, 32)) goto oom;
    for (unsigned i = 0; i < banks; ++i) {
        put32(word, bank_size[i]); put32(word + 4, bank_start[i]);
        if (tag(out, ATAG_MEM, word, 8)) goto oom;
    }
    if (tag(out, ATAG_CMDLINE, cmdline, strlen(cmdline) + 1)) goto oom;
    put32(word, initrd_start); put32(word + 4, initrd_size);
    if (tag(out, ATAG_INITRD2, word, 8) || append_zero(out, 8)) goto oom;
    if (out->size > 4096) { fail(why, "ATAGs exceed one page"); goto done; }
    rc = 0;
    goto done;
oom:
    fail(why, "out of memory");
done:
    dd_free(&cpu); dd_free(&iomem);
    if (rc) out->size = 0;
    return rc;
}

/* ---- waveform envelope ---- */

uint32_t dd_crc32(uint32_t crc, const unsigned char *data, size_t size) {
    static uint32_t table[256];
    if (!table[1]) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    }
    crc = ~crc;
    while (size--) crc = table[(crc ^ *data++) & 0xff] ^ (crc >> 8);
    return ~crc;
}

#define ENVELOPE_HEADER 64u
#define ENVELOPE_MAX (10u * 1024u * 1024u)
#define WBF_MAX (0x70000u - 0x886u)

/* The PW3WFP1 envelope the kernel's EPDC driver takes (android-core patch
 * 0700): the panel's own WBF, as its flash holds it (the first `size` bytes of
 * the panel export), and the WRF the stock system made from it. The checks
 * are the driver's: a WBF with a valid checksum and dimensions, and a WRF that
 * begins with the same header and temperature table. */
int dd_envelope(const unsigned char *panel, size_t panel_size, const unsigned char *wrf, size_t wrf_size,
                struct dd_buf *out, struct dd_why *why) {
    static const unsigned char versions[] = {7, 18, 19, 24, 25};
    out->size = 0;
    if (panel_size < 48) return fail(why, "panel waveform truncated");
    uint32_t size = get32(panel + 4);
    uint32_t limit = panel[19] == 0x15 || panel[19] == 0x2b ? 0x30000u - 0x886u :
                     panel[19] == 0x3c || panel[19] == 0x4b || panel[19] == 0x4c || panel[19] == 0x50 ? WBF_MAX : 0;
    if (size <= 48 || size > limit || size > panel_size || !memchr(versions, panel[16], sizeof(versions)))
        return fail(why, "panel waveform header unsupported");
    if (panel_size != size && panel_size != limit) return fail(why, "unexpected panel waveform export size");
    unsigned char zero4[4] = {0};
    if (dd_crc32(dd_crc32(0, zero4, 4), panel + 4, size - 4) != get32(panel))
        return fail(why, "panel waveform checksum mismatch");
    unsigned modes = panel[37] + 1u, temps = panel[38] + 1u, advanced = panel[39];
    size_t prefix = 48 + temps + 1;
    if (modes < 3 || temps < 2 || advanced > 3 || prefix >= size) return fail(why, "panel waveform dimensions invalid");
    size_t offset = ENVELOPE_HEADER + ((size + 7u) & ~7u);
    if (wrf_size <= prefix || wrf_size > ENVELOPE_MAX - offset || memcmp(wrf, panel, prefix))
        return fail(why, "stock waveform does not belong to this panel");
    unsigned char head[ENVELOPE_HEADER] = "PW3WFP1";
    put32(head + 8, 1); put32(head + 12, ENVELOPE_HEADER); put32(head + 16, size); put32(head + 20, (uint32_t)wrf_size);
    put32(head + 24, (uint32_t)offset); put32(head + 28, dd_crc32(0, panel, size)); put32(head + 32, dd_crc32(0, wrf, wrf_size));
    put32(head + 36, dd_crc32(0, head, ENVELOPE_HEADER));
    if (append(out, head, ENVELOPE_HEADER) || append(out, panel, size) ||
        append_zero(out, offset - ENVELOPE_HEADER - size) || append(out, wrf, wrf_size))
        return fail(why, "out of memory");
    return 0;
}

/* A gzip member of stored (uncompressed) deflate blocks: no compressor on the
 * device, and nothing to keep afterwards (the restore program unlinks the file
 * once the driver has its copy). */
int dd_gzip_stored(const unsigned char *data, size_t size, struct dd_buf *out) {
    static const unsigned char head[10] = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 0xff};
    unsigned char block[5], tail[8];
    if (size > UINT32_MAX || append(out, head, sizeof(head))) return -1;
    size_t at = 0;
    do {
        size_t n = size - at > 65535 ? 65535 : size - at;
        block[0] = at + n == size;
        block[1] = (unsigned char)n; block[2] = (unsigned char)(n >> 8);
        block[3] = (unsigned char)~n; block[4] = (unsigned char)(~n >> 8);
        if (append(out, block, 5) || append(out, data + at, n)) return -1;
        at += n;
    } while (at < size);
    put32(tail, dd_crc32(0, data, size));
    put32(tail + 4, (uint32_t)size);
    return append(out, tail, 8);
}

/* ---- cpio (newc) ---- */

int dd_newc(struct dd_buf *out, const char *name, uint32_t mode, const unsigned char *data, size_t size) {
    static uint32_t inode = 300000;   /* above the public archive's; links are never formed (nlink 1 for files) */
    char head[111];
    size_t namesize = strlen(name) + 1;
    if (size > UINT32_MAX || namesize > 4096) return -1;
    snprintf(head, sizeof(head), "070701%08X%08X%08X%08X%08X%08X%08X%08X%08X%08X%08X%08X%08X",
             inode++, mode, 0u, 0u, (mode & 0170000) == 0040000 ? 2u : 1u, 0u, (uint32_t)size,
             0u, 0u, 0u, 0u, (uint32_t)namesize, 0u);
    return append(out, head, 110) || append(out, name, namesize) ||
           append_zero(out, (4 - (110 + namesize) % 4) % 4) ||
           append(out, data, size) || append_zero(out, (4 - size % 4) % 4);
}
int dd_newc_end(struct dd_buf *out) { return dd_newc(out, "TRAILER!!!", 0, NULL, 0); }

/* ---- storage entry configuration ---- */

static int block_name(const char *n) {
    if (strncmp(n, "mmcblk0", 7)) return 0;
    n += 7;
    if (!*n || !strcmp(n, "rpmb")) return 1;
    if (!strncmp(n, "boot", 4)) n += 4;
    else if (*n == 'p') ++n;
    else return 0;
    return *n && all_of(n, strlen(n), isdigit);
}
static int name_order(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* fat_offset, fat_sizelimit and the eMMC geometry lines: the stock system's
 * user FAT is on loop0 over mmcblk0p4 at an offset (pack.py layout_evidence). */
static int storage_lines(const char *root, const struct dd_profile *profile, struct dd_buf *conf, struct dd_why *why) {
    struct dd_buf mounts = {0};
    char backing[128], offset[32], limit[32], value[32], path[160];
    /* /proc/mounts is a symbolic link to it, which read_file does not follow. */
    if (read_file(root, "/proc/self/mounts", 65536, &mounts, why) || append_zero(&mounts, 1)) { dd_free(&mounts); return -1; }
    int seen = 0, good = 0;
    for (char *save = NULL, *line = strtok_r((char *)mounts.data, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char source[128], target[128], type[32];
        if (sscanf(line, "%127s %127s %31s", source, target, type) == 3 && !strcmp(target, profile->user_mount)) {
            ++seen;
            good = !strcmp(type, "vfat") && (!strcmp(source, "/dev/loop/0") || !strcmp(source, "/dev/loop0"));
        }
    }
    dd_free(&mounts);
    if (seen != 1 || !good) return fail(why, "user FAT is not the one vfat mount on loop0");
    if (read_text(root, "/sys/block/loop0/loop/backing_file", backing, sizeof(backing), why) ||
        read_text(root, "/sys/block/loop0/loop/offset", offset, sizeof(offset), why) ||
        read_text(root, "/sys/block/loop0/loop/sizelimit", limit, sizeof(limit), why)) return -1;
    uint64_t fat_offset, fat_limit;
    if (strcmp(backing, "/dev/mmcblk0p4") || !decimal(offset, &fat_offset) || !decimal(limit, &fat_limit) ||
        fat_offset % 512 || fat_limit % 512) return fail(why, "loop0 is not the user partition at a sector offset");
    if (append_text(conf, "fat_offset=%llu\nfat_sizelimit=%llu\n", (unsigned long long)fat_offset, (unsigned long long)fat_limit))
        return fail(why, "out of memory");

    char full[512];
    snprintf(full, sizeof(full), "%s/sys/class/block", strcmp(root, "/") ? root : "");
    char *names[32];
    unsigned count = 0;
    /* Sorted: the entry does not mind the order, sorting keeps the file the same each start. */
    DIR *dir = opendir(full);
    if (!dir) return fail(why, "cannot list /sys/class/block");
    for (struct dirent *e; (e = readdir(dir));) {
        if (!block_name(e->d_name)) continue;
        if (count == 32 || !(names[count] = strdup(e->d_name))) { closedir(dir); while (count) free(names[--count]); return fail(why, "too many eMMC block devices"); }
        ++count;
    }
    closedir(dir);
    qsort(names, count, sizeof(names[0]), name_order);
    int rc = 0, disk = 0, user = 0;
    uint64_t disk_size = 0, user_start = 0, user_size = 0;
    for (unsigned i = 0; i < count && !rc; ++i) {
        uint64_t sectors, start = 0;
        int partition = !strncmp(names[i], "mmcblk0p", 8);
        snprintf(path, sizeof(path), "/sys/class/block/%s/size", names[i]);
        if (read_text(root, path, value, sizeof(value), why) || !decimal(value, &sectors) || !sectors) { rc = fail(why, "no size for %s", names[i]); break; }
        if (partition) {
            snprintf(path, sizeof(path), "/sys/class/block/%s/start", names[i]);
            if (read_text(root, path, value, sizeof(value), why) || !decimal(value, &start)) { rc = fail(why, "no start for %s", names[i]); break; }
            rc = append_text(conf, "block=%s:%llu:%llu\n", names[i], (unsigned long long)sectors, (unsigned long long)start);
        } else {
            rc = append_text(conf, "block=%s:%llu:-\n", names[i], (unsigned long long)sectors);
        }
        if (!strcmp(names[i], "mmcblk0")) { disk = 1; disk_size = sectors; }
        if (!strcmp(names[i], "mmcblk0p4")) { user = 1; user_start = start; user_size = sectors; }
    }
    while (count) free(names[--count]);
    if (rc) return rc;
    if (!disk || !user || user_start >= disk_size || user_size > disk_size - user_start || fat_offset >= user_size * 512 ||
        (fat_limit && fat_limit > user_size * 512 - fat_offset)) return fail(why, "eMMC geometry inconsistent");
    return 0;
}

/* pw3-testdata.conf: the data image is admitted by its file system UUID. */
static int data_conf(const char *root, const struct dd_profile *profile, const struct dd_build *build,
                     struct dd_buf *conf, struct dd_why *why) {
    char path[512];
    snprintf(path, sizeof(path), "%s%s/%s", strcmp(root, "/") ? root : "", profile->user_mount, build->data_path);
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    unsigned char super[1024];
    struct stat st;
    if (fd < 0) return fail(why, "no data image (errno %d)", errno);
    int ok = !fstat(fd, &st) && S_ISREG(st.st_mode) && (uint64_t)st.st_size == build->data_size &&
             pread(fd, super, sizeof(super), 1024) == (ssize_t)sizeof(super);
    close(fd);
    if (!ok || super[0x38] != 0x53 || super[0x39] != 0xef) return fail(why, "data image is not the expected ext4 file");
    if (append_text(conf, "schema=2\ndata_path=%s\ndata_size=%u\ndata_uuid=", build->data_path, build->data_size))
        return fail(why, "out of memory");
    for (int i = 0; i < 16; ++i) if (append_text(conf, "%02x", super[0x68 + i])) return fail(why, "out of memory");
    return append(conf, "\n", 1) ? fail(why, "out of memory") : 0;
}

int dd_archive(const char *root, const struct dd_profile *profile, const struct dd_build *build,
               struct dd_buf *out, struct dd_why *why) {
    struct dd_buf panel = {0}, wrf = {0}, envelope = {0}, gz = {0}, board = {0}, firmware = {0}, conf = {0}, data = {0};
    char source[32];
    int rc = -1;
    out->size = 0;
    /* The waveform the stock driver runs: "stored" is the one made from this panel's flash. */
    if (read_text(root, "/proc/eink/waveform/source", source, sizeof(source), why)) goto done;
    if (strcmp(source, "stored")) { fail(why, "stock display runs a fallback waveform (%s)", source); goto done; }
    if (read_file(root, "/proc/eink/panel/waveform/data", 1u << 20, &panel, why) ||
        read_file(root, "/proc/eink/waveform/data", ENVELOPE_MAX, &wrf, why) ||
        dd_envelope(panel.data, panel.size, wrf.data, wrf.size, &envelope, why)) goto done;
    if (dd_gzip_stored(envelope.data, envelope.size, &gz)) { fail(why, "out of memory"); goto done; }
    if (read_file(root, profile->wifi_board, 65536, &board, why) ||
        read_file(root, profile->wifi_firmware, 1u << 20, &firmware, why)) goto done;
    if (board.size != 2048) { fail(why, "Wi-Fi calibration is not 2048 bytes"); goto done; }
    if (append(&conf, build->preinit_conf, strlen(build->preinit_conf)) || storage_lines(root, profile, &conf, why)) {
        if (!why->text[0]) fail(why, "out of memory");
        goto done;
    }
    if (build->data_path && data_conf(root, profile, build, &data, why)) goto done;
    static const char *const dirs[] = {"firmware", "firmware/image", "firmware/image/ath6k", "firmware/image/ath6k/AR6003",
        "firmware/image/ath6k/AR6003/hw2.1.1", "firmware/image/epdc", "firmware/image/epdc/pw3"};
    for (unsigned i = 0; i < sizeof(dirs) / sizeof(dirs[0]); ++i)
        if (dd_newc(out, dirs[i], 0040700, NULL, 0)) goto oom;
    if (dd_newc(out, "firmware/image/ath6k/AR6003/hw2.1.1/board.bin", 0100600, board.data, board.size) ||
        dd_newc(out, "firmware/image/ath6k/AR6003/hw2.1.1/fw-4.bin", 0100600, firmware.data, firmware.size) ||
        dd_newc(out, "firmware/image/epdc/pw3/waveform.pw3.gz", 0100600, gz.data, gz.size) ||
        dd_newc(out, "pw3-preinit.conf", 0100400, conf.data, conf.size) ||
        (build->data_path && dd_newc(out, "pw3-testdata.conf", 0100400, data.data, data.size)) ||
        dd_newc_end(out)) goto oom;
    rc = 0;
    goto done;
oom:
    fail(why, "out of memory");
done:
    dd_free(&panel); dd_free(&wrf); dd_free(&envelope); dd_free(&gz);
    dd_free(&board); dd_free(&firmware); dd_free(&conf); dd_free(&data);
    if (rc) dd_free(out);
    return rc;
}
