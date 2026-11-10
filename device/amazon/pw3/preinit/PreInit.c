// SPDX-License-Identifier: Apache-2.0
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include "StorageFormat.h"
#include "../report/ReportStop.h"
#include <openssl/sha.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <linux/fs.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <sys/sysmacros.h>
#include <sys/utsname.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_BLOCKS 32
#define MAX_CONF 8192
#define FAT_MOUNT "/pw3-user"
#define CONF_PATH "/pw3-preinit.conf"
struct block { char name[32]; uint64_t sectors, start; bool has_start; };
struct config {
    char system_path[192], release[128];
    unsigned char system_sha[32], init_sha[32], cmdline_sha[32], adb_sha[32];
    uint64_t system_size, offset, sizelimit;
    unsigned seen, block_count;
    bool adb_key;
    struct block blocks[MAX_BLOCKS];
};
static int console_fd = -1, kmsg_fd = -1;
static bool log_channels_tried = false;
/* The log channels, opened by whoever first wants to say something.
 *
 * The nodes are in the ramdisk (pack.LOG_NODES), not made from sysfs, because
 * the mounts that sysfs needs are themselves among the things that can fail --
 * and a stop() before them used to report to nowhere at all: no serial line,
 * and nothing in the kernel log, which is the only channel that reaches a
 * person, since it sits at the fixed physical address on the command line and
 * the stock side reads it back after the next boot. The nodes made from sysfs
 * later replace these, on the tmpfs, and that is what the steady state uses.
 *
 * report() calls this rather than each main() calling it first, so that there
 * is no ordering left to get wrong: the earliest possible failure opens its own
 * channels on the way out. Opening is tried once; a failed open stays failed
 * rather than being retried on every line.
 *
 * O_NONBLOCK is for the *writes*, and it is never taken off again. The open
 * cannot block whatever we ask for -- the kernel sets O_NONBLOCK itself for
 * /dev/console and then restores the caller's flags (tty_io.c, "Don't let
 * /dev/console block") -- but the console is a serial port whose default
 * termios has IXON (tty_std_termios in tty_io.c). An XOFF on the line, or noise
 * on a floating test pad that decodes as one, sets tty->stopped; uart_start()
 * then never calls start_tx(), the ring fills, and a blocking write sleeps in
 * n_tty_write() with no timeout at all. That would hang PID 1 before it could
 * write the kernel log or reach the restart below, which is worse than the
 * silence this change is fixing. With O_NONBLOCK the write returns EAGAIN and
 * the line is lost. That is the right trade: the serial line is a developer's
 * channel, the kernel log is the person's, and kmsg carries the whole text.
 */
static void open_log_channels(void) {
    if (log_channels_tried) return;
    log_channels_tried = true;
    kmsg_fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    console_fd = open("/dev/console", O_WRONLY | O_CLOEXEC | O_NONBLOCK);
}
/* The first system call of this start that failed, and why.
 *
 * The checks here are written as contracts: a chain of conditions that all have
 * to hold, with one sentence for the chain. What that costs is the reason -- a
 * failed read and a hash that does not match collapse into the same false, and
 * the sentence describes the comparison. A V65 start was refused on 2026-10-21
 * with "extents do not agree" when the truth was a denied stat; the same shape
 * is here, where a read error reads as "SHA256 rejected" and an fstatfs error as
 * "not a FAT filesystem".
 *
 * Only stop() says it. report() is also the progress log, and gluing an earlier
 * harmless failure onto every later line would be worse than saying nothing. */
static const char *fault_call;
static char fault_path[128];
static int fault_why;
static void note(const char *call, const char *path) {
    if (fault_call) return;
    fault_call = call;
    fault_why = errno;
    snprintf(fault_path, sizeof(fault_path), "%s", path ? path : "(a descriptor)");
}
/* Used by the mapper headers, which the host tests compile too; there the hook
 * is the no-op they define for themselves. */
#define PW3_NOTE(call, path) note((call), (path))

static void report(const char *message) {
    open_log_channels();
    /* The kernel log first: it is the one that gets back to a person, and the
     * console write below can be refused or short. */
    if (kmsg_fd >= 0) dprintf(kmsg_fd, "<3>pw3-preinit: %s\n", message);
    if (console_fd >= 0) dprintf(console_fd, "pw3-preinit: %s\n", message);
}
/* How long a failed start stays on the screen before the device restarts
 * itself. Nothing here can draw on the panel, so the last thing the user sees
 * is the launcher's starting screen, which already says the Kindle may restart
 * itself back to its own system within a few minutes. Waiting for ever instead
 * left the only way out as holding the power button for twenty seconds, and
 * someone whose whole toolkit is a USB cable has no reason to know that the
 * wait is pointless. Two minutes is long enough to read the screen and to
 * catch the device over USB while the kernel log is still in memory. */
#define STOP_SECONDS 120
/* The report area's block device, once it has been mapped (set in
 * TestFileInit.h), and empty before that.
 *
 * A start that refuses to go on is the one the owner has no other way to hear
 * about. The launcher's screen says the Kindle may restart itself back to its
 * own system within a few minutes, and that is all it says; the reason is in
 * the kernel log, which goes with the kernel. So the reason is also written
 * into the area, where the next tap reads it (installer/install/install.c) --
 * that tap can install the system image again when that is what was wrong,
 * instead of handing over to the same refusal for ever (docs/known-issues.md,
 * 七).
 *
 * Most of the stops in this program happen before the area is mapped, and
 * those write nothing: a stop with nowhere to write is still a stop, and the
 * one refusal a tap can act on comes after the mapping. */
static char report_node[64];
/* In .bss and not on the stack: this runs on a path that is already failing,
 * and 64 KiB there is not what PID 1 should spend its last call on. */
static unsigned char report_area[PW3_REPORT_BYTES];
static void report_stop(const char *message) {
    if (!report_node[0]) return;
    /* Said in the kernel log too, because an area that was not written cannot
     * carry the news that it was not written (the same reason BootReport.cpp
     * has for its own line). */
    report(pw3_report_write_stop(report_node, message, report_area, sizeof(report_area))
           ? "the reason above is also in the report area"
           : "the reason above could not be written to the report area");
}
__attribute__((noreturn)) static void stop(const char *message) {
    if (fault_call) {
        char line[480];
        snprintf(line, sizeof(line), "%s; first failed system call this start: %s %s: %s",
                 message, fault_call, fault_path, strerror(fault_why));
        report(line);
        report_stop(line);
    } else {
        report(message);
        report_stop(message);
    }
    report("STOPPED in the new kernel; the device restarts itself back to the stock system.");
    sync();
    for (unsigned left = STOP_SECONDS; left; --left) sleep(1);
    reboot(RB_AUTOBOOT);
    report("the restart was refused; holding the power button for 20 seconds is the way back.");
    for (;;) pause();
}
static bool decimal(const char *s, uint64_t *out) {
    char *end;
    unsigned long long v;
    if (!*s) return false;
    for (const char *p = s; *p; ++p) if (*p < '0' || *p > '9') return false;
    errno = 0; v = strtoull(s, &end, 10);
    if (errno || *end) return false;
    *out = v; return true;
}
static bool hex_sha(const char *s, unsigned char out[32]) {
    if (strlen(s) != 64) return false;
    for (int i = 0; i < 32; ++i) {
        unsigned v = 0;
        for (int j = 0; j < 2; ++j) {
            unsigned char c = (unsigned char)s[2 * i + j];
            unsigned d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : 16;
            if (d == 16) return false;
            v = (v << 4) | d;
        }
        out[i] = (unsigned char)v;
    }
    return true;
}
static bool safe_component(const char *s) {
    if (!*s || !strcmp(s, ".") || !strcmp(s, "..")) return false;
    for (; *s; ++s) if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
        (*s >= '0' && *s <= '9') || *s == '_' || *s == '-' || *s == '.')) return false;
    return true;
}
static bool safe_release(const char *s) {
    if (!*s) return false;
    for (; *s; ++s) if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
        (*s >= '0' && *s <= '9') || *s == '_' || *s == '-' || *s == '.' || *s == '+')) return false;
    return true;
}
static bool safe_path(const char *s) {
    char copy[192], *save = NULL, *part;
    if (!*s || *s == '/' || strlen(s) >= sizeof(copy) || strstr(s, "//") || s[strlen(s)-1] == '/') return false;
    strcpy(copy, s);
    for (part = strtok_r(copy, "/", &save); part; part = strtok_r(NULL, "/", &save))
        if (!safe_component(part)) return false;
    return true;
}
static bool block_name(const char *s) {
    if (!strcmp(s, "mmcblk0")) return true;
    if (strncmp(s, "mmcblk0", 7)) return false;
    s += 7;
    if (*s == 'p') ++s;
    else if (!strncmp(s, "boot", 4)) s += 4;
    else if (!strcmp(s, "rpmb")) return true;
    else return false;
    if (!*s) return false;
    for (; *s; ++s) if (*s < '0' || *s > '9') return false;
    return true;
}
/* The call that failed is recorded here because this is where it stops being
 * visible: every caller turns `false` into a judgement about the value it was
 * after -- a configuration rejected, a layout that differs, a command line that
 * is not ours (TestFileInit.h) -- and a read that never happened then reads as a
 * value that disagreed. */
static bool read_text(const char *path, char *buf, size_t cap) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    ssize_t n;
    if (fd < 0) { note("open", path); return false; }
    n = read(fd, buf, cap - 1);
    if (n < 0) { note("read", path); close(fd); return false; }
    if ((size_t)n == cap - 1) { close(fd); return false; }
    close(fd); buf[n] = 0;
    while (n && (buf[n-1] == '\n' || buf[n-1] == '\r' || buf[n-1] == ' ')) buf[--n] = 0;
    return true;
}
static bool sysnum(const char *name, const char *field, uint64_t *value) {
    char path[160], buf[64];
    if (snprintf(path, sizeof(path), "/sys/class/block/%s/%s", name, field) >= (int)sizeof(path)) return false;
    return read_text(path, buf, sizeof(buf)) && decimal(buf, value);
}
/* The kernel this storage entry is built for names itself after its model
 * (CONFIG_LOCALVERSION="-lab126-pw3-android81" and "-lab126-voyage-android81").
 * The board name is the product's, and the product's device directory sets
 * TARGET_BOOTLOADER_BOARD_NAME (device/amazon/voyage/BoardConfig.mk); the
 * preinit Android.mk passes it here. One source therefore keeps one check per
 * product: an image built for one model still refuses a kernel built for the
 * other, which is the whole point of this line. The default is the only model
 * this project has run on a device. */
#ifndef PW3_BOARD_NAME
#define PW3_BOARD_NAME "pw3"
#endif
static bool release_is_this_board(const char *s) {
    char expected[40];
    int n = snprintf(expected, sizeof(expected), "-%s-android81", PW3_BOARD_NAME);
    return n > 0 && (size_t)n < sizeof(expected) && strstr(s, expected) != NULL;
}
static bool parse_config(const char *path, struct config *cfg) {
    char data[MAX_CONF], *save = NULL, *line;
    memset(cfg, 0, sizeof(*cfg));
    if (!read_text(path, data, sizeof(data))) return false;
    for (line = strtok_r(data, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *eq = strchr(line, '='); unsigned bit = 0;
        if (!eq || eq == line) return false;
        *eq++ = 0;
        if (!strcmp(line, "schema")) { bit = 1U; if (strcmp(eq, "1")) return false; }
        else if (!strcmp(line, "system_path")) { bit = 2U; if (!safe_path(eq)) return false; strcpy(cfg->system_path, eq); }
        else if (!strcmp(line, "system_size")) { bit = 4U; if (!decimal(eq, &cfg->system_size) || cfg->system_size < (1U<<20) || cfg->system_size > UINT32_MAX || cfg->system_size % 512) return false; }
        else if (!strcmp(line, "system_sha256")) { bit = 8U; if (!hex_sha(eq, cfg->system_sha)) return false; }
        else if (!strcmp(line, "init_sha256")) { bit = 16U; if (!hex_sha(eq, cfg->init_sha)) return false; }
        else if (!strcmp(line, "cmdline_sha256")) { bit = 32U; if (!hex_sha(eq, cfg->cmdline_sha)) return false; }
        else if (!strcmp(line, "kernel_release")) { bit = 64U; if (!safe_release(eq) || strlen(eq) >= sizeof(cfg->release) || !release_is_this_board(eq)) return false; strcpy(cfg->release, eq); }
        else if (!strcmp(line, "fat_offset")) { bit = 128U; if (!decimal(eq, &cfg->offset) || cfg->offset % 512) return false; }
        else if (!strcmp(line, "fat_sizelimit")) { bit = 256U; if (!decimal(eq, &cfg->sizelimit) || cfg->sizelimit % 512) return false; }
        else if (!strcmp(line, "adb_key_sha256")) { bit = 512U; cfg->adb_key = strcmp(eq, "none") != 0; if (cfg->adb_key && !hex_sha(eq, cfg->adb_sha)) return false; }
        else if (!strcmp(line, "block")) {
            char *a = strchr(eq, ':'), *b;
            struct block *v;
            if (!a || cfg->block_count == MAX_BLOCKS) return false;
            *a++ = 0; b = strchr(a, ':'); if (!b) return false; *b++ = 0;
            if (!block_name(eq) || strlen(eq) >= sizeof(cfg->blocks[0].name)) return false;
            for (unsigned i = 0; i < cfg->block_count; ++i) if (!strcmp(eq, cfg->blocks[i].name)) return false;
            v = &cfg->blocks[cfg->block_count++]; strcpy(v->name, eq);
            if (!decimal(a, &v->sectors) || !v->sectors || v->sectors > INT64_MAX / 512) return false;
            v->has_start = strcmp(b, "-") != 0;
            if (v->has_start != (strncmp(eq, "mmcblk0p", 8) == 0)) return false;
            if (v->has_start && !decimal(b, &v->start)) return false;
        } else return false;
        if (bit && (cfg->seen & bit)) return false;
        cfg->seen |= bit;
    }
    if (cfg->seen != 1023U) return false;
    bool disk = false, user = false;
    uint64_t disk_size = 0, user_start = 0, user_size = 0;
    for (unsigned i = 0; i < cfg->block_count; ++i) {
        struct block *b = &cfg->blocks[i];
        if (!strcmp(b->name, "mmcblk0")) { disk = !b->has_start; disk_size = b->sectors; }
        if (!strcmp(b->name, "mmcblk0p4")) { user = b->has_start; user_start = b->start; user_size = b->sectors; }
    }
    if (!(disk && user && user_start < disk_size && user_size <= disk_size - user_start &&
          cfg->offset < user_size * 512 && (!cfg->sizelimit || cfg->sizelimit <= user_size * 512 - cfg->offset)))
        return false;
    /* The partitions of the stock system are set read only, p4 is left writable,
     * and the kernel keeps that flag per block device, not per sector: a p4 that
     * shared sectors with one of them would be a way through the protection,
     * since a write to the image files reaches those sectors by the other name.
     * This device's table does not overlap, which is why it is worth saying so:
     * a layout that does is not one this port will write to. */
    for (unsigned i = 0; i < cfg->block_count; ++i) {
        struct block *b = &cfg->blocks[i];
        if (!b->has_start || !strcmp(b->name, "mmcblk0p4"))
            continue;
        if (b->start < user_start + user_size && user_start < b->start + b->sectors)
            return false;
    }
    return true;
}
static bool only_pseudo_mounts(void) {
    char data[16384], *save = NULL, *line;
    if (!read_text("/proc/self/mountinfo", data, sizeof(data))) return false;
    for (line = strtok_r(data, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char type[32], *sep = strstr(line, " - ");
        if (!sep || sscanf(sep + 3, "%31s", type) != 1) return false;
        if (strcmp(type, "rootfs") && strcmp(type, "ramfs") && strcmp(type, "tmpfs") &&
            strcmp(type, "proc") && strcmp(type, "sysfs") && strcmp(type, "devpts") && strcmp(type, "devtmpfs")) return false;
    }
    return true;
}
static bool mkdir_ram(const char *path, mode_t mode) {
    struct stat st;
    if (mkdir(path, mode) && errno != EEXIST) return false;
    return lstat(path, &st) == 0 && S_ISDIR(st.st_mode);
}
static bool node_from_sysfs(const char *sysdev, const char *node, mode_t type) {
    char buf[64], *colon; uint64_t maj, min; struct stat st;
    if (!read_text(sysdev, buf, sizeof(buf)) || !(colon = strchr(buf, ':'))) return false;
    *colon++ = 0;
    if (!decimal(buf, &maj) || !decimal(colon, &min) || maj > 4095 || min > 1048575) return false;
    dev_t dev = makedev((unsigned)maj, (unsigned)min);
    if (mknod(node, type | 0600, dev) && errno != EEXIST) return false;
    return lstat(node, &st) == 0 && (st.st_mode & S_IFMT) == type && st.st_rdev == dev;
}
static bool block_node(const char *name, char path[128]) {
    char sysdev[160];
    if (snprintf(sysdev, sizeof(sysdev), "/sys/class/block/%s/dev", name) >= (int)sizeof(sysdev) ||
        snprintf(path, 128, "/dev/block/%s", name) >= 128) return false;
    return node_from_sysfs(sysdev, path, S_IFBLK);
}
static bool readonly_fd(int fd) {
    int one = 1, value = 0;
    return ioctl(fd, BLKROSET, &one) == 0 && ioctl(fd, BLKROGET, &value) == 0 && value == 1;
}
/* `read_complete`, when a caller asks for it, says whether the whole of
 * `expected_size` was read and a digest computed: that is, whether a false
 * return means "this is not that image" or "this could not be read".
 *
 * The two have to be told apart now that the stock side acts on the first --
 * a refusal naming the image makes the next tap install it again
 * (installer/install/install.c). Reporting a read error as a mismatch would
 * have it throw away an image that is in fact intact and then ask for 196 MB
 * it may have no way to fetch, where before this the next tap simply read the
 * image again and carried on. An image longer than it should be counts as
 * not-read rather than as a mismatch: the mapped device is exactly
 * `cfg.system_size`, so that says the mapping is wrong, not the content. */
static bool sha_fd(int fd, uint64_t expected_size, const unsigned char expected[32], bool *read_complete) {
    unsigned char buf[65536], digest[32]; SHA256_CTX ctx; uint64_t total = 0;
    if (read_complete) *read_complete = false;
    if (lseek(fd, 0, SEEK_SET) != 0 || !SHA256_Init(&ctx)) return false;
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { note("read", NULL); return false; }
        if (!n) break;
        if (total > expected_size || (uint64_t)n > expected_size - total || !SHA256_Update(&ctx, buf, (size_t)n)) return false;
        total += (uint64_t)n;
    }
    if (total != expected_size || !SHA256_Final(digest, &ctx)) return false;
    if (read_complete) *read_complete = true;
    return !memcmp(digest, expected, 32);
}
static bool hash_regular(const char *path, const unsigned char hash[32]) {
    struct stat st; int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW); bool ok;
    if (fd < 0) return false;
    ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 && sha_fd(fd, (uint64_t)st.st_size, hash, NULL);
    close(fd); return ok;
}
static bool mounted_readonly(const char *path, unsigned long magic, bool noexec, bool nosuid) {
    struct statfs fs; struct statvfs flags;
    return statfs(path, &fs) == 0 && (uint32_t)fs.f_type == magic && statvfs(path, &flags) == 0 &&
        (flags.f_flag & ST_RDONLY) && ((flags.f_flag & ST_NOEXEC) != 0) == noexec &&
        (flags.f_flag & ST_NODEV) && ((flags.f_flag & ST_NOSUID) != 0) == nosuid;
}
/* The one entry this program has. The RAM pre-init main and the read-only
   host-test main that used to stand beside it went with the ram_boot=1 mode
   on 2026-10-29, and with them lock_physical, cmdline_ok, loop_name and
   attach_loop, which nothing else called. Everything above is shared with
   the entry below and stays. */

static void testfile_documents_bind(void);
static void testfile_kindle_root_view(void);
static void testfile_sleepwall_dir(void);
static void testfile_nav_marker(void);
static void preinit_nav_settings(void);
static bool preinit_cmdline_has(const char *token);

/* Readom O6: hand the stock store to Android read-write.
 *
 * This runs as PID 1 before /init execs, so the Android SELinux policy -- which
 * is loaded from inside /init -- is not in force yet. That is why a plain
 * read-write vfat mount is possible here: the init-domain restrictions that
 * would refuse context=/bind/remount in an rc file do not apply, and the mount
 * keeps genfscon's vfat label, which AOSP already lets the app domains write.
 * The android/ directory is then covered by an empty read-only tmpfs: apps see
 * a directory they cannot enter, and the mapped system/data/report files are
 * not reachable as files at all.
 *
 * nls_utf8 is a module (CONFIG_NLS_UTF8=m) and no Android rc loads it before
 * the app-facing mount is useful, so the read-only initramfs carries it and
 * PID 1 loads it here, while module loading is still unrestricted.
 */
static bool pw3_load_nls_utf8(void) {
    int fd = open("/nls_utf8.ko", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat st; unsigned char *bytes; ssize_t got; bool ok;
    if (fd < 0) return false;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > (1 << 20)) {
        close(fd); return false;
    }
    bytes = (unsigned char *)malloc((size_t)st.st_size);
    if (!bytes) { close(fd); return false; }
    got = read(fd, bytes, (size_t)st.st_size);
    close(fd);
    if (got != (ssize_t)st.st_size) { free(bytes); return false; }
    ok = syscall(SYS_init_module, bytes, (unsigned long)st.st_size, "") == 0;
    report(ok ? "nls_utf8 loaded for UTF-8 FAT names"
              : "nls_utf8 could not be loaded; the store uses ISO-8859-1 names");
    free(bytes);
    return ok;
}

static bool testfile_handover_store(const char *fat) {
    bool utf8;
    const char *options;
    if (umount(FAT_MOUNT)) {
        int umount_errno = errno;
        char line[128];
        snprintf(line, sizeof(line),
                 "PW3_STORE rw_mount=-1 umount_errno=%d; the store stays read-only and this "
                 "start will probably return to the stock system", umount_errno);
        report(line);
        return false;
    }
    utf8 = pw3_load_nls_utf8();
    options = utf8
        ? "uid=0,gid=0,umask=0022,utf8=1,codepage=437,shortname=mixed"
        : "uid=0,gid=0,umask=0022,codepage=437,iocharset=iso8859-1";
    if (!mkdir_ram("/kindle-us", 0755) ||
        mount(fat, "/kindle-us", "vfat", MS_NOSUID | MS_NODEV | MS_NOEXEC, options)) {
        report("the read-write hand-over FAT mount failed; the store is not shared");
        return false;
    }
    /* An empty read-only tmpfs covers android/: system.img, data.img,
     * report.bin and state cannot be opened, let alone changed. */
    if (mount("tmpfs", "/kindle-us/android", "tmpfs", MS_RDONLY | MS_NOSUID | MS_NODEV,
              "size=64k,nr_inodes=8")) {
        report("the android/ cover could not be mounted; the store is not shared");
        umount("/kindle-us");
        return false;
    }
    /* Test entry only: prove the rw mount by a real write/read-back while the
     * SELinux policy is not loaded (the release entry never writes). */
    if (preinit_cmdline_has("readom.emulator=1")) {
        const char *path = "/kindle-us/readom-write-test.txt";
        char back[16] = {0};
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        int write_rc = -1, read_rc = -1;
        if (fd >= 0) {
            write_rc = write(fd, "hello\n", 6) == 6 ? 0 : -1;
            close(fd);
        }
        fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            ssize_t got = read(fd, back, sizeof(back) - 1);
            read_rc = got > 0 && !strcmp(back, "hello\n") ? 0 : -1;
            close(fd);
        }
        char line[96];
        snprintf(line, sizeof(line), "PW3_STORE rw_mount=0 umount_errno=0 write=%d readback=%d",
                 write_rc, read_rc);
        report(line);
    }
    testfile_documents_bind();
    testfile_kindle_root_view();
    testfile_sleepwall_dir();
    testfile_nav_marker();
    report("hand-over store mounted read-write at /kindle-us; android/ covered by read-only tmpfs");
    return true;
}


/* Readom PagePress (qemu-research task-29 spec).
 *
 * The Android rc files load neither module, and the keys do nothing until the
 * keypad misc device has its event generation enabled and its key mapping set
 * (left page up, right page down). This runs after /system is mounted and
 * checked, before execve("/init"), so the modules can be read from the checked
 * image and loaded while module loading is still unrestricted. Best effort:
 * every failure is one report() line and boot continues. readom.no-fsr=1 skips
 * the whole block (the emulator's kl05 I2C is absent and its suspend path can
 * wait on a mutex; the touch/power/display paths must stay clean).
 */
static bool preinit_cmdline_has(const char *token) {
    char buf[1024];
    ssize_t n;
    int fd = open("/proc/cmdline", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return false;
    buf[n] = 0;
    for (char *p = strtok(buf, " \t\n"); p; p = strtok(NULL, " \t\n"))
        if (!strcmp(p, token)) return true;
    return false;
}

static int preinit_init_module(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat st;
    unsigned char *bytes;
    ssize_t got;
    int rc;
    if (fd < 0) return -1;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > (1 << 20)) {
        close(fd); return -1;
    }
    bytes = (unsigned char *)malloc((size_t)st.st_size);
    if (!bytes) { close(fd); return -1; }
    got = read(fd, bytes, (size_t)st.st_size);
    close(fd);
    if (got != (ssize_t)st.st_size) { free(bytes); return -1; }
    rc = (int)syscall(SYS_init_module, bytes, (unsigned long)st.st_size, "");
    free(bytes);
    return rc;
}

static bool preinit_write_text(const char *path, const char *text, size_t len) {
    int fd = open(path, O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    bool ok;
    if (fd < 0) return false;
    ok = write(fd, text, len) == (ssize_t)len;
    close(fd);
    return ok;
}

static bool preinit_read_small(const char *path, char *out, size_t cap) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    ssize_t got;
    if (fd < 0 || cap == 0) return false;
    got = read(fd, out, cap - 1);
    close(fd);
    if (got <= 0) return false;
    out[got] = 0;
    return true;
}

/* readom.conf in the store root may carry keymap=N for field experiments; a
 * missing file, a failed read or an out-of-range value all mean the default. */
static int preinit_conf_keymap(void) {
    char buf[512];
    if (!preinit_read_small("/kindle-us/readom.conf", buf, sizeof(buf))) return 0;
    for (char *p = strtok(buf, " \t\r\n"); p; p = strtok(NULL, " \t\r\n")) {
        if (strncmp(p, "keymap=", 7)) continue;
        char *end = NULL;
        long value = strtol(p + 7, &end, 10);
        if (end && *end == 0 && value >= 1 && value <= 4) return (int)value;
    }
    return 0;
}

/* The stock /etc/upstart/haptics recipe (icewine branch), best effort: lock the
 * KL05 out of the way, program the DRV2604 waveform for the board's actuator,
 * then unlock. A failure here only means no vibration — the key path continues. */
static void preinit_program_haptics(void) {
    static const char *const id_pin = "/sys/devices/system/drv26xx_haptics/drv26xx_haptics0/id_pin";
    static const char *const gain_setting = "/sys/devices/system/drv26xx_haptics/drv26xx_haptics0/gain_setting";
    static const char *const programming = "/sys/devices/system/drv26xx_haptics/drv26xx_haptics0/programming_interface";
    char buf[512];
    int idpin = -1, gain = -1, program_rc = -1, lock_rc, unlock_rc;
    const char *cfg = "none";
    char line[160];

    lock_rc = preinit_write_text("/proc/keypad", "lock\n", 5) ? 0 : -1;
    if (preinit_read_small(id_pin, buf, sizeof(buf))) idpin = atoi(buf);
    /* id_pin 0 = AAC (production EVT3+); anything else selects SEMCO. */
    gain = idpin == 0 ? 1 : 2;
    if (!(idpin == 0 ? preinit_write_text(gain_setting, "1\n", 2)
                     : preinit_write_text(gain_setting, "2\n", 2))) gain = -1;
    if (idpin == 0) {
        cfg = "haptics0.config";
        if (preinit_read_small("/haptics0.config", buf, sizeof(buf)))
            program_rc = preinit_write_text(programming, buf, strlen(buf)) ? 0 : -1;
    } else {
        cfg = "haptics1.config";
        if (preinit_read_small("/haptics1.config", buf, sizeof(buf)))
            program_rc = preinit_write_text(programming, buf, strlen(buf)) ? 0 : -1;
    }
    unlock_rc = preinit_write_text("/proc/keypad", "unlock\n", 7) ? 0 : -1;
    snprintf(line, sizeof(line), "PW3_HAPTICS id_pin=%d gain=%d cfg=%s program=%d lock=%d unlock=%d",
             idpin, gain, cfg, program_rc, lock_rc, unlock_rc);
    report(line);
}

static void testfile_pagepress(void) {
    int haptics = -1, fsr = -1, node = -1, event_enable = -1, keymap = -1, mode_ok = -1;
    int keymap_value;
    const char *keymap_src;
    char line[200];
    if (strcmp(PW3_BOARD_NAME, "voyage")) return;
    if (preinit_cmdline_has("readom.no-fsr=1")) {
        report("PW3_PAGEPRESS skipped by command line (readom.no-fsr=1)");
        return;
    }
    /* fsr_keypad depends on symbols exported by drv26xx_haptics: order matters. */
    haptics = preinit_init_module("/system/vendor/lib/modules/drv26xx_haptics.ko");
    fsr = preinit_init_module("/system/vendor/lib/modules/fsr_keypad.ko");
    preinit_program_haptics();
    keymap_value = preinit_conf_keymap();
    keymap_src = keymap_value ? "conf" : "default";
    if (!keymap_value) keymap_value = 1; /* stock: TOP contact = prev, BOTTOM = next */
    if (!node_from_sysfs("/sys/class/misc/keypad/dev", "/dev/keypad", S_IFCHR)) node = -1;
    else {
        int fd;
        node = 0;
        fd = open("/dev/keypad", O_RDWR | O_CLOEXEC);
        if (fd >= 0) {
            unsigned enable = 1;
            event_enable = ioctl(fd, 0x40044203, &enable) == 0 ? 0 : -1;
            close(fd);
        }
        fd = open("/sys/devices/platform/fsr_keypad.0/key_mapping", O_WRONLY | O_CLOEXEC);
        if (fd >= 0) {
            char value[4];
            int len = snprintf(value, sizeof(value), "%d\n", keymap_value);
            keymap = write(fd, value, (size_t)len) == len ? 0 : -1;
            close(fd);
        }
    }
    /* mode_ok cannot be observed without the kl05 controller; -1 means unknown. */
    snprintf(line, sizeof(line),
             "PW3_PAGEPRESS haptics=%d fsr=%d node=%d event_enable=%d keymap=%d src=%s mode_ok=%d",
             haptics, fsr, node, event_enable, keymap, keymap_src, mode_ok);
    report(line);
}


/* Readom v8.5: make the stock documents directory visible to Android apps as
 * /storage/emulated/0/documents. vold exports /data/media/0 through its FUSE
 * volume, so a bind mount placed there before execve (no SELinux policy yet)
 * is what the storage service and MediaProvider end up serving. The source is
 * the read-write vfat store mounted fmask/dmask=0022 with root ownership, so
 * apps can read and cannot write it; the FAT is not modified by Android.
 * v8.8: the bind is remounted read-only as well. The masks are what make it
 * read-only today, and they are a mount option somebody can change; the
 * per-mount flag is the property the product promised.
 * Best effort: a failure is one log line and boot continues.
 */
static void testfile_documents_bind(void) {
    int mkdir_rc = -1, mount_rc = -1, ro_rc = -1, mount_errno = 0;
    if (mkdir_ram("/data/media", 0775) && mkdir_ram("/data/media/0", 0775) &&
        mkdir_ram("/data/media/0/documents", 0775))
        mkdir_rc = 0;
    if (mount("/kindle-us/documents", "/data/media/0/documents", NULL, MS_BIND, NULL)) {
        mount_rc = -1;
        mount_errno = errno;
    } else {
        mount_rc = 0;
        if (!mount(NULL, "/data/media/0/documents", NULL, MS_REMOUNT | MS_BIND | MS_RDONLY, NULL))
            ro_rc = 0;
        else
            mount_errno = errno;
    }
    char line[160];
    snprintf(line, sizeof(line), "PW3_STORAGE documents_bind=%d mkdir=%d mount=%d ro=%d errno=%d",
             (mkdir_rc == 0 && mount_rc == 0 && ro_rc == 0) ? 0 : -1,
             mkdir_rc, mount_rc, ro_rc, mount_errno);
    report(line);
}


/* Readom v8.8: the stock store's root, visible to Android apps as
 * /storage/emulated/0/kindle. Same mechanism as documents/ above: vold's FUSE
 * volume serves what is under /data/media/0, and /data is mounted by now, so a
 * bind placed there before execve is what the storage service ends up
 * exporting. A bind mount does not carry the submounts of its source, so the
 * empty read-only tmpfs over android/ has to be bound again into the new view:
 * without that, system.img, data.img and state would be reachable as files.
 * The view is then remounted read-only per mount, so the store cannot be
 * written through it whatever the FAT masks say. Any failure withdraws the
 * whole view -- a root view whose android/ is not covered is worse than none,
 * and there is nothing the user asked for that a half-view would serve. */
#define ROOT_VIEW_MOUNT "/data/media/0/kindle"
static void testfile_kindle_root_view(void) {
    char cover[160], line[192];
    int mkdir_rc = -1, bind_rc = -1, cover_rc = -1, ro_rc = -1, saved_errno = 0;
    if (mkdir_ram(ROOT_VIEW_MOUNT, 0775)) mkdir_rc = 0;
    if (mkdir_rc != 0) {
        saved_errno = errno;
    } else if (mount("/kindle-us", ROOT_VIEW_MOUNT, NULL, MS_BIND, NULL)) {
        saved_errno = errno;
    } else {
        bind_rc = 0;
        if (snprintf(cover, sizeof(cover), "%s/android", ROOT_VIEW_MOUNT) < (int)sizeof(cover) &&
            mount("/kindle-us/android", cover, NULL, MS_BIND, NULL) == 0) {
            cover_rc = 0;
        } else {
            saved_errno = errno;
        }
        if (cover_rc == 0) {
            if (mount(NULL, ROOT_VIEW_MOUNT, NULL, MS_REMOUNT | MS_BIND | MS_RDONLY, NULL) == 0)
                ro_rc = 0;
            else
                saved_errno = errno;
        }
        if (cover_rc || ro_rc) {   /* fail closed: withdraw the whole view */
            if (cover_rc == 0) umount(cover);
            umount(ROOT_VIEW_MOUNT);
        }
    }
    snprintf(line, sizeof(line),
             "PW3_STORAGE root_view=%d mkdir=%d bind=%d cover=%d ro=%d errno=%d",
             (mkdir_rc == 0 && bind_rc == 0 && cover_rc == 0 && ro_rc == 0) ? 0 : -1,
             mkdir_rc, bind_rc, cover_rc, ro_rc, saved_errno);
    report(line);
}


/* Readom sleep wallpaper (T10): an empty folder for the user's own PNGs, which
 * the display HAL reads when the screen goes off
 * (device/amazon/pw3/sleep/README.md). Made here because this is the only point
 * where the store is mounted read-write and nothing needs a permission to
 * create it. On vfat the mode is not stored: the mount's umask=0022 is what the
 * directory ends up with (0755, root:root). Best effort -- the HAL falls back to
 * the picture that ships in the image, so a failure is one log line. */
static void testfile_sleepwall_dir(void) {
    char line[64];
    snprintf(line, sizeof(line), "PW3_SLEEPWALL mkdir=%d",
             mkdir_ram("/kindle-us/sleepwall", 0775) ? 0 : -1);
    report(line);
}


/* readom.conf's nav=hide|show (default hide) selects whether the boot service
 * re-shows the navigation bar through policy_control. The marker is a plain
 * rootfs file: Android's rc starts a oneshot shell service after
 * sys.boot_completed, and that service only reads this two-word file. */
static bool nav_wanted_show(bool *from_conf) {
    char buf[512];
    bool show = false;
    if (from_conf) *from_conf = false;
    if (!preinit_read_small("/kindle-us/readom.conf", buf, sizeof(buf))) return false;
    for (char *p = strtok(buf, " \t\r\n"); p; p = strtok(NULL, " \t\r\n")) {
        if (!strcmp(p, "nav=show")) { show = true; if (from_conf) *from_conf = true; }
        else if (!strcmp(p, "nav=hide")) { show = false; if (from_conf) *from_conf = true; }
    }
    return show;
}

/* The marker is a diagnostic only: v8.6 found it is never written unless initramfs carries the
 * file already, and a preference routed through it could be lost silently. So the marker is
 * created here when it is missing, and the preference itself is read straight from the store. */
static void testfile_nav_marker(void) {
    bool from_conf = false;
    bool show = nav_wanted_show(&from_conf);
    int fd = open("/readom-nav", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd >= 0) {
        const char *text = show ? "show\n" : "hide\n";
        write(fd, text, 5);
        close(fd);
    }
    char line[96];
    snprintf(line, sizeof(line), "PW3_NAV nav=%s src=%s", show ? "show" : "hide",
             from_conf ? "conf" : "default");
    report(line);
}


/* Readom v8.7 / T09: navigation-bar preference.
 *
 * Two SettingsProvider rows tell the Android side what to do with the navigation bar:
 *
 *   policy_control      "immersive.navigation=*" hides the bar (nav=hide); absent leaves the
 *                       system's own behaviour (nav=show).
 *   readom_nav_locked   "1" asks the framework patch (aosp-frameworks-base 0076) to let the bar
 *                       back only for a deliberate swipe up from the bottom edge, and to refuse
 *                       the power-button panic gesture and every other transient request; "0"
 *                       restores AOSP's reveal rules. A boot without the framework patch simply
 *                       ignores the row.
 *
 * The rc-service route does not work on this image: a service whose script lives on rootfs has no
 * SELinux domain ("does not have a SELinux domain defined"), and even the shell domain cannot read
 * /readom-navbar.sh. So the preference is applied where the policy is not loaded yet: PID 1
 * rewrites SettingsProvider's global database before the service starts. The file is owned
 * system:system (1000:1000); the original is backed up once, the replacement is written to a
 * temporary file, fsynced, has the original mode and security.selinux copied onto it, and is
 * renamed over the original. Any failure leaves the original untouched and is only a log line --
 * the navigation bar simply keeps its previous state.
 */
static const char *const kSettingsGlobal = "/data/system/users/0/settings_global.xml";
static const char *const kNavPolicyName = "policy_control";
static const char *const kNavLockedName = "readom_nav_locked";
static const char *const kNavPolicyHidden = "immersive.navigation=*";

/* Returns the offset of the first byte of the <setting> element that carries name="<name>", or
 * (size_t)-1. The element is expected to be self-closing, which is how SettingsProvider writes it
 * in Android 8.1. */
static size_t nav_find_setting(const char *data, size_t size, const char *name, size_t *end) {
    (void)size;
    char needle[96];
    int needle_len = snprintf(needle, sizeof(needle), "name=\"%s\"", name);
    if (needle_len <= 0 || (size_t)needle_len >= sizeof(needle)) return (size_t)-1;
    const char *found = strstr(data, needle);
    if (!found) return (size_t)-1;
    const char *start = found;
    while (start > data && start[-1] != '<' && start[-1] != '\n') --start;
    if (start > data && start[-1] == '\n') { /* include the line start below */ }
    if (*start != '<') start = found;
    const char *close = strstr(found, "/>");
    if (!close) close = strstr(found, "</setting>");
    if (!close) return (size_t)-1;
    *end = (size_t)(close - data + (close[1] == '>' ? 2 : 10));
    /* trim leading whitespace/newline so the whole line is replaced */
    while (start > data && (start[-1] == ' ' || start[-1] == '\t')) --start;
    if (start > data && start[-1] == '\n') --start;
    return (size_t)(start - data);
}

static int nav_max_id(const char *data) {
    int max = 0;
    const char *p = data;
    while ((p = strstr(p, "id=\"")) != NULL) {
        int value = atoi(p + 4);
        if (value > max) max = value;
        p += 4;
    }
    return max;
}

static bool nav_has_setting(const char *data, size_t size, const char *name) {
    size_t end = 0;
    return nav_find_setting(data, size, name, &end) != (size_t)-1;
}

/* Builds the file's contents with name="<name>" set to value, or with the element removed when
 * remove is true, in *out. Returns false when no buffer could be built; the caller then keeps the
 * original file. Every buffer here is one byte longer than its length, NUL-terminated. */
static bool nav_edit_setting(const char *data, size_t size, const char *name,
                             const char *value, bool remove, char **out, size_t *out_size) {
    *out = NULL;
    *out_size = 0;
    size_t end = 0;
    size_t start = nav_find_setting(data, size, name, &end);
    if (remove) {
        if (start == (size_t)-1) {           /* nothing to remove */
            char *copy = (char *)malloc(size + 1);
            if (!copy) return false;
            memcpy(copy, data, size);
            copy[size] = 0;
            *out = copy;
            *out_size = size;
            return true;
        }
        size_t after = end;
        while (after < size && data[after] == '\n') ++after;
        size_t out_len = size - (after - start);
        char *next = (char *)malloc(out_len + 1);
        if (!next) return false;
        memcpy(next, data, start);
        memcpy(next + start, data + after, size - after);
        next[out_len] = 0;
        *out = next;
        *out_size = out_len;
        return true;
    }
    if (start != (size_t)-1) {
        /* replace the value attribute of the existing element */
        const char *elem = data + start;
        const char *elem_end = data + end;
        const char *vp = strstr(elem, "value=\"");
        if (vp && vp < elem_end) {
            const char *vend = memchr(vp + 7, '"', (size_t)(elem_end - (vp + 7)));
            if (vend) {
                size_t prefix = (size_t)(vp + 7 - data);
                size_t suffix = size - (size_t)(vend - data);
                size_t out_len = prefix + strlen(value) + suffix;
                char *next = (char *)malloc(out_len + 1);
                if (!next) return false;
                memcpy(next, data, prefix);
                memcpy(next + prefix, value, strlen(value));
                memcpy(next + prefix + strlen(value), vend, suffix);
                next[out_len] = 0;
                *out = next;
                *out_size = out_len;
                return true;
            }
        }
    }
    const char *close = strstr(data, "</settings>");
    if (!close) return false;
    char entry[192];
    int entry_len = snprintf(entry, sizeof(entry),
        "    <setting id=\"%d\" name=\"%s\" value=\"%s\" package=\"com.android.shell\" />\n",
        nav_max_id(data) + 1, name, value);
    if (entry_len <= 0 || (size_t)entry_len >= sizeof(entry)) return false;
    size_t offset = (size_t)(close - data);
    size_t out_len = size + (size_t)entry_len;
    char *next = (char *)malloc(out_len + 1);
    if (!next) return false;
    memcpy(next, data, offset);
    memcpy(next + offset, entry, (size_t)entry_len);
    memcpy(next + offset + (size_t)entry_len, data + offset, size - offset);
    next[out_len] = 0;
    *out = next;
    *out_size = out_len;
    return true;
}

static void preinit_nav_settings(void) {
    bool show = nav_wanted_show(NULL);
    int write_rc = -1, saved_errno = 0, rows = -1, locked_row = -1;
    char line[160];
    struct stat st;
    char *data = NULL;
    ssize_t got = 0;
    int fd = open(kSettingsGlobal, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        snprintf(line, sizeof(line), "PW3_NAV xml=-1 write=-1 rows=-1 locked=-1 errno=%d", errno);
        report(line);
        return;
    }
    if (fstat(fd, &st) || st.st_size <= 0 || st.st_size > (4 << 20)) {
        saved_errno = errno;
        close(fd);
        snprintf(line, sizeof(line), "PW3_NAV xml=-1 write=-1 rows=-1 locked=-1 errno=%d",
                 saved_errno);
        report(line);
        return;
    }
    data = (char *)malloc((size_t)st.st_size + 1);
    if (data) got = read(fd, data, (size_t)st.st_size);
    close(fd);
    if (!data || got != st.st_size) {
        free(data);
        snprintf(line, sizeof(line), "PW3_NAV xml=-1 write=-1 rows=-1 locked=-1 errno=%d", errno);
        report(line);
        return;
    }
    data[got] = 0;

    /* policy_control hides the bar; readom_nav_locked is what keeps it hidden from everything but
     * a deliberate bottom edge swipe. nav=show drops the first and writes 0 for the second. */
    bool ok = true;
    for (int which = 0; ok && which < 2; ++which) {
        const char *name = which == 0 ? kNavPolicyName : kNavLockedName;
        const char *value = which == 0 ? kNavPolicyHidden : (show ? "0" : "1");
        bool remove = which == 0 && show;
        char *next = NULL;
        size_t next_size = 0;
        ok = nav_edit_setting(data, (size_t)got, name, value, remove, &next, &next_size);
        if (ok) {
            free(data);
            data = next;
            got = (ssize_t)next_size;
        }
    }
    if (!ok) {
        free(data);
        snprintf(line, sizeof(line), "PW3_NAV xml=-1 write=-1 rows=-1 locked=-1 errno=%d", errno);
        report(line);
        return;
    }
    rows = nav_has_setting(data, (size_t)got, kNavPolicyName) ? 1 : 0;
    locked_row = nav_has_setting(data, (size_t)got, kNavLockedName) ? (show ? 0 : 1) : -1;

    {
        char tmp_path[256], bak_path[280];
        snprintf(tmp_path, sizeof(tmp_path), "%s.readom-tmp", kSettingsGlobal);
        snprintf(bak_path, sizeof(bak_path), "%s.readom-bak", kSettingsGlobal);
        int out_fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (out_fd < 0) { saved_errno = errno; write_rc = -1; }
        else {
            bool write_ok = write(out_fd, data, (size_t)got) == got;
            if (write_ok) {
                char label[128];
                ssize_t label_len = getxattr(kSettingsGlobal, "security.selinux", label, sizeof(label));
                if (label_len > 0) setxattr(tmp_path, "security.selinux", label, (size_t)label_len, 0);
                write_ok = fchown(out_fd, 1000, 1000) == 0 && fchmod(out_fd, st.st_mode & 07777) == 0;
            }
            if (write_ok) write_ok = fsync(out_fd) == 0;
            if (close(out_fd)) write_ok = false;
            if (write_ok) {
                /* one backup only: keep the first pre-Readom settings file */
                int bak_fd = open(bak_path, O_RDONLY | O_CLOEXEC);
                if (bak_fd < 0) {
                    int src = open(kSettingsGlobal, O_RDONLY | O_CLOEXEC);
                    int dst = src >= 0 ? open(bak_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600) : -1;
                    if (src >= 0 && dst >= 0) {
                        char buf[4096];
                        ssize_t n;
                        while ((n = read(src, buf, sizeof(buf))) > 0) write(dst, buf, (size_t)n);
                    }
                    if (src >= 0) close(src);
                    if (dst >= 0) close(dst);
                } else close(bak_fd);
                write_ok = rename(tmp_path, kSettingsGlobal) == 0;
                if (write_ok) {
                    int dir = open("/data/system/users/0", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
                    if (dir >= 0) { fsync(dir); close(dir); }
                    sync();
                } else saved_errno = errno;
            } else saved_errno = errno;
            if (!write_ok) unlink(tmp_path);
            write_rc = write_ok ? 0 : -1;
        }
    }
    free(data);
    snprintf(line, sizeof(line), "PW3_NAV xml=0 write=%d rows=%d locked=%d errno=%d",
             write_rc == 0 ? 0 : -1, rows, locked_row, saved_errno);
    report(line);
}

/* Readom private/128gb-storage: grow the data filesystem to the size of the
 * image file it is in.
 *
 * The one-way upgrade from the 1.75 GiB data image goes through two programs
 * because neither side can do both halves: the installer, on the stock system,
 * can grow a file on the user store but its kernel cannot read ext4; this
 * entry, after the jump, has ext4 and /system's e2fsprogs but finds the user
 * store read-only until verification is done. So install.c extends the file
 * (ftruncate/append, keeping every byte of what the reader has) and this grows
 * what is inside it, offline, before Android mounts it.
 *
 * A no-op when there is nothing to do: the caller only gets here when the
 * superblock's own block count is smaller than the file.
 *
 * Why offline: resize2fs 1.43.3 resizes a mounted filesystem through
 * EXT4_IOC_RESIZE_FS, which this kernel (3.0.35) does not have -- ext4 online
 * resize landed in 3.7. /data is unmounted by the caller for the whole of it,
 * so there is no second writer and no ioctl to need.
 *
 * Failure is reported, not fatal: an image grown and a filesystem still of its
 * old size is a device that starts with the space it had, and the next start
 * asks again. Nothing here writes outside the image's own clusters.
 *
 * A filesystem left mounted by a power cut carries a journal to replay, and
 * resize2fs refuses that ("please run e2fsck -f"). e2fsck is then run once and
 * resize2fs tried once more; e2fsck is the program the fsck option of every
 * mount table names, and it is the same one the offline resize assumes. */
static int preinit_tool(const char *path, char *const argv[])
{
    int status = 0;
    pid_t pid = fork(), got;
    if (pid < 0) return -1;
    if (pid == 0) {
        /* The tools' own chatter is not a channel this start reads; the report
         * lines below say what happened. */
        int null = open("/dev/null", O_WRONLY | O_CLOEXEC);
        if (null >= 0) { if (dup2(null, 1) < 0 || dup2(null, 2) < 0) _exit(127); if (null > 2) close(null); }
        char *envp[] = {"PATH=/sbin:/system/bin:/system/xbin", "HOME=/", "TERM=linux", NULL};
        execve(path, argv, envp);
        _exit(127);
    }
    do { got = waitpid(pid, &status, 0); } while (got < 0 && errno == EINTR);
    if (got < 0) return -1;
    if (WIFSIGNALED(status)) return -1;
    return WEXITSTATUS(status);
}

static bool testfile_grow_data(const char *device, uint64_t size, uint64_t fs_bytes,
        const unsigned char uuid[16])
{
    char line[192], node[64];
    unsigned char sb[1024];
    uint64_t got = 0;
    bool ok = false;
    int status, check = 0;
    if (snprintf(node, sizeof(node), "%s", device) >= (int)sizeof(node)) return false;
    char *resize[] = {"/system/bin/resize2fs", node, NULL};
    char *fsck[] = {"/system/bin/e2fsck", "-f", "-y", node, NULL};
    status = preinit_tool("/system/bin/resize2fs", resize);
    if (status != 0) {
        report("resize2fs did not grow the data filesystem; checking the filesystem first");
        check = preinit_tool("/system/bin/e2fsck", fsck);
        snprintf(line, sizeof(line), "PW3_STORAGE data_grow=fsck status=%d", check);
        report(line);
        status = preinit_tool("/system/bin/resize2fs", resize);
    }
    int fd = open(node, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd >= 0) {
        if (pread(fd, sb, sizeof(sb), 1024) == (ssize_t)sizeof(sb)) {
            got = pw3_ext4_bytes(sb);
            ok = status == 0 && got == size && !memcmp(sb + 0x68, uuid, 16) &&
                pw3_ext4_in_use(sb, size);
        }
        close(fd);
    }
    snprintf(line, sizeof(line), "PW3_STORAGE data_grow=%d from=%llu to=%llu fs=%llu status=%d",
             ok ? 0 : -1, (unsigned long long)fs_bytes, (unsigned long long)size,
             (unsigned long long)got, status);
    report(line);
    return ok;
}

#include "TestFileInit.h"
