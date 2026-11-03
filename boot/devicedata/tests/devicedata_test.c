/* SPDX-License-Identifier: GPL-2.0-only */
/* devicedata.c on a made-up device: a temporary directory laid out like the
 * stock system, with invented values (no device's data). Checks what is
 * produced and that each inconsistency stops the start. */
#define _GNU_SOURCE
#include "../devicedata.h"
#include "../../handoff/pw3_plan.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char root[256];
static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static void put(const char *path, const void *data, size_t size) {
    char full[512], dir[512];
    snprintf(full, sizeof(full), "%s%s", root, path);
    strcpy(dir, full);
    for (char *p = dir + strlen(root) + 1; (p = strchr(p, '/')); ++p) { *p = 0; mkdir(dir, 0700); *p = '/'; }
    FILE *f = fopen(full, "wb");
    assert(f && fwrite(data, 1, size, f) == size && fclose(f) == 0);
}
static void puts_(const char *path, const char *text) { put(path, text, strlen(text)); }
static uint32_t get32(const unsigned char *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void set32(unsigned char *p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = (unsigned char)(v >> (8 * i)); }

#define STOCK "log_pos_goal=0x8fff0000 console=ttymxc0,115200 ro ip=off quiet video=mxcepdcfb:E60,bpp=8,x_mem=3M"
#define RELEASE "3.0.35-lab126"
#define VERSION "#8 PREEMPT Tue Aug 1 12:49:59 UTC 2023"
#define CMDLINE STOCK " rdinit=/pw3-testfile-init androidboot.hardware=pw3"

static unsigned char wbf[4096], wrf[20000];
static void make_device(void) {
    puts_("/proc/board_id", "TESTBOARD0000001");
    puts_("/proc/mac_addr", "0123456789ab");
    puts_("/proc/bootmode", "main");
    puts_("/proc/cmdline", "rootwait " STOCK " root=/dev/mmcblk0p1\n");
    puts_("/proc/version", "Linux version " RELEASE " (build@lab126) (gcc version 4.9.3) " VERSION "\n");
    puts_("/proc/cpuinfo", "Processor\t: ARMv7\nHardware\t: Freescale i.MX 6SoloLite based Wario Board\nRevision\t: 60000\nSerial\t\t: 0\n");
    puts_("/proc/iomem", "02008000-02008fff : imx6q-ecspi.0\n80000000-8fffffff : System RAM\n  80008000-80500000 : Kernel text\n98000000-9fffffff : System RAM\n");  /* a gap: two banks (adjacent ones merge) */
    /* A WBF: checksum over the file with its first four bytes zero; a WRF with the same header and temperature table. */
    memset(wbf, 0x5a, sizeof(wbf));
    set32(wbf + 4, sizeof(wbf));
    wbf[16] = 24; wbf[19] = 0x3c; wbf[37] = 3; wbf[38] = 9; wbf[39] = 1;
    unsigned char zero[4] = {0};
    set32(wbf, dd_crc32(dd_crc32(0, zero, 4), wbf + 4, sizeof(wbf) - 4));
    memcpy(wrf, wbf, 48 + 10 + 1);
    for (size_t i = 59; i < sizeof(wrf); ++i) wrf[i] = (unsigned char)(i * 7);
    put("/proc/eink/panel/waveform/data", wbf, sizeof(wbf));
    put("/proc/eink/waveform/data", wrf, sizeof(wrf));
    puts_("/proc/eink/waveform/source", "stored\n");
    unsigned char board[2048];
    memset(board, 0xb0, sizeof(board));
    put("/opt/ar6k/target/AR6003/hw2.1.1/bin/AR6003_wfo_calfile.bin", board, sizeof(board));
    puts_("/opt/ar6k/target/AR6003/hw2.1.1/bin/fw-4.bin", "firmware");
    /* As on Linux: /proc/mounts is a symbolic link to self/mounts. */
    puts_("/proc/self/mounts", "rootfs / rootfs ro 0 0\n/dev/loop/0 /mnt/base-us vfat rw,noexec 0 0\nfsp /mnt/us fuse.fsp rw 0 0\n");
    char link[512];
    snprintf(link, sizeof(link), "%s/proc/mounts", root);
    assert(symlink("self/mounts", link) == 0);
    puts_("/sys/block/loop0/loop/backing_file", "/dev/mmcblk0p4\n");
    puts_("/sys/block/loop0/loop/offset", "8192\n");
    puts_("/sys/block/loop0/loop/sizelimit", "0\n");
    puts_("/sys/class/block/mmcblk0/size", "7733248\n");
    puts_("/sys/class/block/mmcblk0boot0/size", "4096\n");
    puts_("/sys/class/block/mmcblk0p1/size", "921600\n");
    puts_("/sys/class/block/mmcblk0p1/start", "65536\n");
    puts_("/sys/class/block/mmcblk0p4/size", "6385664\n");
    puts_("/sys/class/block/mmcblk0p4/start", "1249280\n");
    puts_("/sys/class/block/loop0/size", "1\n");   /* not an eMMC device: left out */
    unsigned char super[2048] = {0};
    super[1024 + 0x38] = 0x53; super[1024 + 0x39] = 0xef;
    for (int i = 0; i < 16; ++i) super[1024 + 0x68 + i] = (unsigned char)(0xa0 + i);
    put("/mnt/base-us/android/data.img", super, sizeof(super));
    char full[512];
    snprintf(full, sizeof(full), "%s/mnt/base-us/android/data.img", root);
    assert(truncate(full, 1 << 20) == 0);
}

static int atags(struct dd_buf *out, struct dd_why *why) {
    return dd_atags(root, &dd_pw3_5_16_2_1_1, CMDLINE, 0x84000000u, 123456, out, why);
}

/* The gate every stranger meets first: what puts this file in the library is
 * the jailbreak, not us, so it appears on whatever Kindle someone copies it to,
 * and this runs before anything is downloaded or written. Each refusal that is
 * "another Kindle or another firmware" must say so (`other`), because the
 * screen for those is the one that does not end in "restart and tap again". */
static void test_device_matches(void) {
    struct dd_why why = {{0}, 0};
    CHECK(dd_device_matches(root, &dd_pw3_5_16_2_1_1, &why) == 0 && !why.text[0]);
    struct { const char *path, *text; int other; } bad[] = {
        {"/proc/cmdline", "rootwait console=ttymxc0,115200 ro root=/dev/mmcblk0p1\n", 1},
        {"/proc/cpuinfo", "Processor\t: ARMv7\nHardware\t: Freescale i.MX6 SoloLite Celeste Board\n", 1},
        {"/proc/cpuinfo", "Processor\t: ARMv7\nRevision\t: 60000\n", 0},
        /* A firmware that replaced the kernel and kept the boot loader's
         * command line: the handover calls into that kernel by name and hash,
         * so it is another firmware however familiar everything else looks. */
        {"/proc/version", "Linux version 3.0.35-lab126 (build@lab126) (gcc version 4.9.3) "
                          "#9 PREEMPT Mon Mar 4 09:11:02 UTC 2024\n", 1},
        {"/proc/version", "Linux version 4.9.77-lab126 (build@lab126) (gcc version 4.9.3) " VERSION "\n", 1},
        {"/proc/version", "Linux version\n", 1},
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        char saved[4096], full[512];
        snprintf(full, sizeof(full), "%s%s", root, bad[i].path);
        FILE *f = fopen(full, "rb"); size_t n = fread(saved, 1, sizeof(saved), f); fclose(f);
        puts_(bad[i].path, bad[i].text);
        why.text[0] = 0; why.other_device = 0;
        CHECK(dd_device_matches(root, &dd_pw3_5_16_2_1_1, &why) == -1 && why.text[0]);
        CHECK(why.other_device == bad[i].other);
        put(bad[i].path, saved, n);
    }
    /* And the fixture is itself again, so the tests after this one still run on
     * the device they were written for. */
    why.text[0] = 0;
    CHECK(dd_device_matches(root, &dd_pw3_5_16_2_1_1, &why) == 0);
}

static void test_atags(void) {
    struct dd_buf b = {0};
    struct dd_why why = {{0}, 0};
    CHECK(atags(&b, &why) == 0);
    /* CORE, REVISION, REVISION16, MACADDR, BOOTMODE, two MEM banks, CMDLINE, INITRD2, NONE. */
    static const uint32_t kinds[] = {0x54410001, 0x54410007, 0x5441000b, 0x5441000d, 0x5441000f,
                                     0x54410002, 0x54410002, 0x54410009, 0x54420005, 0};
    size_t pos = 0;
    for (unsigned i = 0; i < sizeof(kinds) / sizeof(kinds[0]); ++i) {
        CHECK(pos + 8 <= b.size);
        uint32_t words = get32(b.data + pos), kind = get32(b.data + pos + 4);
        CHECK(kind == kinds[i]);
        const unsigned char *p = b.data + pos + 8;
        if (kind == 0x54410007) CHECK(get32(p) == 0x60000);
        if (kind == 0x5441000b) CHECK(!memcmp(p, "TESTBOARD0000001", 16));
        if (kind == 0x5441000d) { CHECK(words == 10 && !memcmp(p, "0123456789ab", 12)); for (int k = 12; k < 32; ++k) CHECK(p[k] == 0); }
        if (kind == 0x5441000f) { CHECK(words == 10 && !memcmp(p, "main", 5)); for (int k = 4; k < 32; ++k) CHECK(p[k] == 0); }
        if (kind == 0x54410002) CHECK((get32(p) == 0x10000000 && get32(p + 4) == 0x80000000) || (get32(p) == 0x08000000 && get32(p + 4) == 0x98000000));
        if (kind == 0x54410009) CHECK(!strcmp((const char *)p, CMDLINE));
        if (kind == 0x54420005) CHECK(get32(p) == 0x84000000 && get32(p + 4) == 123456);
        pos += kind ? words * 4 : 8;
    }
    CHECK(pos == b.size);

    /* Each of these stops the start. `other` marks the two that say the running
     * system is another Kindle or another firmware, which the launcher reports
     * differently: telling someone with a different model to restart and try
     * again is telling them to do it for ever. */
    struct { const char *path, *text; int other; } bad[] = {
        {"/proc/board_id", "TESTBOARD00001", 0}, {"/proc/board_id", "TESTBOARD000000!", 0},
        {"/proc/mac_addr", "0123456789zz", 0}, {"/proc/bootmode", "diags", 0},
        {"/proc/cmdline", "rootwait " STOCK " foo=1", 1},
        {"/proc/cpuinfo", "Hardware\t: Other Board\nRevision\t: 60000\n", 1},
        {"/proc/cpuinfo", "Hardware\t: Wario\nRevision\t: 1\nRevision\t: 2\n", 0},
        {"/proc/iomem", "70000000-7fffffff : System RAM\n", 0}, {"/proc/iomem", "80000000-8fffffff : System RAM\n88000000-9fffffff : System RAM\n", 0},
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        char saved[4096]; char full[512];
        snprintf(full, sizeof(full), "%s%s", root, bad[i].path);
        FILE *f = fopen(full, "rb"); size_t n = fread(saved, 1, sizeof(saved), f); fclose(f);
        puts_(bad[i].path, bad[i].text);
        why.text[0] = 0; why.other_device = 0;
        CHECK(atags(&b, &why) == -1 && why.text[0] && b.size == 0);
        CHECK(why.other_device == bad[i].other);
        put(bad[i].path, saved, n);
    }
    CHECK(dd_atags(root, &dd_pw3_5_16_2_1_1, "console=other", 0x84000000u, 1, &b, &why) == -1);
    CHECK(atags(&b, &why) == 0);
    dd_free(&b);
}

static void test_envelope_and_gzip(void) {
    struct dd_buf e = {0}, g = {0};
    struct dd_why why = {{0}, 0};
    CHECK(dd_envelope(wbf, sizeof(wbf), wrf, sizeof(wrf), &e, &why) == 0);
    CHECK(!memcmp(e.data, "PW3WFP1", 8) && get32(e.data + 8) == 1 && get32(e.data + 12) == 64);
    CHECK(get32(e.data + 16) == sizeof(wbf) && get32(e.data + 20) == sizeof(wrf) && get32(e.data + 24) == 64 + sizeof(wbf));
    CHECK(get32(e.data + 28) == dd_crc32(0, wbf, sizeof(wbf)) && get32(e.data + 32) == dd_crc32(0, wrf, sizeof(wrf)));
    unsigned char head[64]; memcpy(head, e.data, 64); set32(head + 36, 0);
    CHECK(get32(e.data + 36) == dd_crc32(0, head, 64));
    CHECK(e.size == 64 + sizeof(wbf) + sizeof(wrf) && !memcmp(e.data + 64, wbf, sizeof(wbf)));
    /* A WRF made from another panel, a damaged WBF. */
    wrf[50] ^= 1; CHECK(dd_envelope(wbf, sizeof(wbf), wrf, sizeof(wrf), &e, &why) == -1); wrf[50] ^= 1;
    wbf[100] ^= 1; CHECK(dd_envelope(wbf, sizeof(wbf), wrf, sizeof(wrf), &e, &why) == -1); wbf[100] ^= 1;

    /* Stored gzip: header, blocks of at most 65535 bytes, CRC and length. */
    static unsigned char big[150000];
    for (size_t i = 0; i < sizeof(big); ++i) big[i] = (unsigned char)(i ^ (i >> 8));
    CHECK(dd_gzip_stored(big, sizeof(big), &g) == 0);
    CHECK(g.data[0] == 0x1f && g.data[1] == 0x8b && g.data[2] == 8 && g.data[3] == 0);
    size_t pos = 10, out = 0; int last = 0;
    while (!last) {
        last = g.data[pos] & 1;
        unsigned len = g.data[pos + 1] | g.data[pos + 2] << 8, nlen = g.data[pos + 3] | g.data[pos + 4] << 8;
        CHECK((g.data[pos] & 6) == 0 && (len ^ nlen) == 0xffff && !memcmp(g.data + pos + 5, big + out, len));
        pos += 5 + len; out += len;
    }
    CHECK(out == sizeof(big) && get32(g.data + pos) == dd_crc32(0, big, sizeof(big)) && get32(g.data + pos + 4) == sizeof(big));
    CHECK(pos + 8 == g.size);
    dd_free(&e); dd_free(&g);
    /* The standard check value of CRC-32. */
    CHECK(dd_crc32(0, (const unsigned char *)"123456789", 9) == 0xcbf43926u);
}

struct entry { char name[128]; uint32_t mode, size; const unsigned char *data; };
static unsigned parse(const struct dd_buf *b, struct entry *e, unsigned max) {
    size_t pos = 0; unsigned n = 0;
    for (;;) {
        if (pos + 110 > b->size || memcmp(b->data + pos, "070701", 6)) return 0;
        uint32_t f[13];
        for (int i = 0; i < 13; ++i) { char hex[9]; memcpy(hex, b->data + pos + 6 + i * 8, 8); hex[8] = 0; f[i] = (uint32_t)strtoul(hex, NULL, 16); }
        const char *name = (const char *)b->data + pos + 110;
        if (strlen(name) + 1 != f[11]) return 0;
        size_t data = (pos + 110 + f[11] + 3) & ~(size_t)3;
        if (!strcmp(name, "TRAILER!!!")) return data == b->size ? n : 0;
        if (n == max) return 0;
        snprintf(e[n].name, sizeof(e[n].name), "%s", name);
        e[n].mode = f[1]; e[n].size = f[6]; e[n].data = b->data + data;
        ++n;
        pos = (data + f[6] + 3) & ~(size_t)3;
    }
}
static const struct entry *find(const struct entry *e, unsigned n, const char *name) {
    for (unsigned i = 0; i < n; ++i) if (!strcmp(e[i].name, name)) return &e[i];
    return NULL;
}

static void test_archive(void) {
    struct dd_buf a = {0};
    struct dd_why why = {{0}, 0};
    struct dd_build build = {CMDLINE, "schema=1\nadb_key_sha256=none\n", "android/data.img", 1 << 20};
    CHECK(dd_archive(root, &dd_pw3_5_16_2_1_1, &build, &a, &why) == 0);
    struct entry e[32];
    unsigned n = parse(&a, e, 32);
    CHECK(n == 12);
    const struct entry *conf = find(e, n, "pw3-preinit.conf"), *data = find(e, n, "pw3-testdata.conf");
    const struct entry *board = find(e, n, "firmware/image/ath6k/AR6003/hw2.1.1/board.bin");
    const struct entry *wave = find(e, n, "firmware/image/epdc/pw3/waveform.pw3.gz");
    CHECK(conf && data && board && wave && find(e, n, "firmware/image/ath6k/AR6003/hw2.1.1/fw-4.bin"));
    CHECK(find(e, n, "firmware") && find(e, n, "firmware")->mode == 040700);
    if (conf) {
        static const char expect[] = "schema=1\nadb_key_sha256=none\nfat_offset=8192\nfat_sizelimit=0\n"
            "block=mmcblk0:7733248:-\nblock=mmcblk0boot0:4096:-\nblock=mmcblk0p1:921600:65536\nblock=mmcblk0p4:6385664:1249280\n";
        CHECK(conf->mode == 0100400 && conf->size == strlen(expect) && !memcmp(conf->data, expect, conf->size));
    }
    if (data) {
        static const char expect[] = "schema=2\ndata_path=android/data.img\ndata_size=1048576\ndata_uuid=a0a1a2a3a4a5a6a7a8a9aaabacadaeaf\n";
        CHECK(data->size == strlen(expect) && !memcmp(data->data, expect, data->size));
    }
    if (board) CHECK(board->mode == 0100600 && board->size == 2048);
    if (wave) CHECK(wave->size > 64 + sizeof(wbf) + sizeof(wrf));
    dd_free(&a);

    /* Each of these stops the start. */
    struct { const char *path, *text; } bad[] = {
        {"/proc/eink/waveform/source", "builtin\n"},
        {"/proc/self/mounts", "/dev/loop/1 /mnt/base-us vfat rw 0 0\n"},
        {"/proc/self/mounts", "/dev/loop/0 /mnt/base-us vfat rw 0 0\n/dev/loop/0 /mnt/base-us vfat rw 0 0\n"},
        {"/sys/block/loop0/loop/backing_file", "/dev/mmcblk0p3\n"},
        {"/sys/block/loop0/loop/offset", "100\n"},
        {"/sys/class/block/mmcblk0p4/start", "7733248\n"},
        {"/opt/ar6k/target/AR6003/hw2.1.1/bin/AR6003_wfo_calfile.bin", "short"},
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        static char saved[4096]; char full[512];
        snprintf(full, sizeof(full), "%s%s", root, bad[i].path);
        FILE *f = fopen(full, "rb"); size_t got = fread(saved, 1, sizeof(saved), f); fclose(f);
        puts_(bad[i].path, bad[i].text);
        why.text[0] = 0;
        CHECK(dd_archive(root, &dd_pw3_5_16_2_1_1, &build, &a, &why) == -1 && why.text[0] && a.size == 0);
        put(bad[i].path, saved, got);
    }
    build.data_size = 4096;   /* not the size of the data image */
    CHECK(dd_archive(root, &dd_pw3_5_16_2_1_1, &build, &a, &why) == -1);
    build.data_path = NULL;   /* without one: no pw3-testdata.conf */
    CHECK(dd_archive(root, &dd_pw3_5_16_2_1_1, &build, &a, &why) == 0 && parse(&a, e, 32) == 11);
    dd_free(&a);
}

/* What the device makes passes what the handoff module checks: the tags and
 * command line it allows, and the physical plan, with the values of
 * profiles/pw3-5.16.2.1.1.json and RAM as the stock kernel reports it. */
static void test_module_rules(void) {
    puts_("/proc/iomem", "80000000-9fffffff : System RAM\n  80008000-80500000 : Kernel text\n");
    struct dd_buf b = {0};
    struct dd_why why = {{0}, 0};
    const uint32_t initramfs = 9u << 20;
    CHECK(dd_atags(root, &dd_pw3_5_16_2_1_1, CMDLINE, 0x84000000u, initramfs, &b, &why) == 0);
    static const pw_u32 kinds[] = {0x54410001, 0x54410002, 0x54410007, 0x54410009, 0x5441000b, 0x5441000d, 0x5441000f, 0x54420005};
    CHECK(pw_atags_check(b.data, (pw_u32)b.size, kinds, 8, CMDLINE) == PW_VALID_DATA_ONLY);
    CHECK(pw_atags_check(b.data, (pw_u32)b.size, kinds, 7, CMDLINE) == PW_E_CONTRACT);   /* INITRD2 not allowed */
    CHECK(pw_atags_check(b.data, (pw_u32)b.size, kinds, 8, STOCK) == PW_E_CONTRACT);
    static unsigned char zimage[4096];
    set32(zimage + 0x24, 0x016f2818); set32(zimage + 0x28, 0); set32(zimage + 0x2c, sizeof(zimage));
    struct pw_plan plan = {.entry = 0x80008000, .machine_id = 4091, .expected_machine_id = 4091, .boot_ram_base = 0x80000000,
                           .identity_limit = 0xbf000000, .ram_count = 1, .forbidden_count = 1, .footprint_count = 1, .segment_count = 3,
                           .ram = {{0x80000000, 0x20000000}}, .forbidden = {{0x88000000, 0x18000000}}, .footprint = {{0x80008000, 0x915000}},
                           .segments = {{PW_ZIMAGE, {0x80008000, 0x1000}, sizeof(zimage)}, {PW_ATAGS, {0x80001000, 0x1000}, (pw_u32)b.size},
                                        {PW_INITRAMFS, {0x84000000, initramfs}, initramfs}}};
    CHECK(pw_plan_validate(&plan, b.data, (pw_u32)b.size, zimage, sizeof(zimage)) == PW_VALID_DATA_ONLY);
    plan.segments[2].buffer_bytes = initramfs - 4;   /* the tags name another initramfs size */
    CHECK(pw_plan_validate(&plan, b.data, (pw_u32)b.size, zimage, sizeof(zimage)) == PW_E_CONTRACT);
    plan.segments[2].buffer_bytes = initramfs;
    /* A bound below the start of memory: no destination inside RAM could be
     * under it, so nothing this plan names could be reached through the table
     * it is about to run under. The refusal is the whole point -- the
     * alternative, clamping a destination into the window, writes a payload
     * through a mapping that does not exist.
     *
     * The number is not a stand-in for a 2G/2G machine, which is what this
     * comment first said. There `PAGE_OFFSET` equals `PHYS_OFFSET`, so the
     * kernel's own lowmem mapping is already one to one and the bound is the
     * end of lowmem rather than TASK_SIZE
     * (design/porting-to-other-models.md section 6). This is a malformed plan,
     * which is a case worth refusing on any machine. */
    plan.identity_limit = 0x7f000000;
    CHECK(pw_plan_validate(&plan, b.data, (pw_u32)b.size, zimage, sizeof(zimage)) == PW_E_CONTRACT);
    plan.identity_limit = 0xbf000000;
    CHECK(pw_plan_validate(&plan, b.data, (pw_u32)b.size, zimage, sizeof(zimage)) == PW_VALID_DATA_ONLY);
    dd_free(&b);
}

int main(void) {
    const char *tmp = getenv("TMPDIR");
    snprintf(root, sizeof(root), "%s/devicedata-test-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    assert(mkdtemp(root));
    make_device();
    test_device_matches();
    test_atags();
    test_envelope_and_gzip();
    test_archive();
    test_module_rules();
    char command[600];
    snprintf(command, sizeof(command), "rm -rf '%s'", root);
    if (system(command)) fprintf(stderr, "could not remove %s\n", root);
    if (failures) { fprintf(stderr, "%d checks failed\n", failures); return 1; }
    puts("devicedata: all checks passed");
    return 0;
}
