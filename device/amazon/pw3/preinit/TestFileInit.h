// SPDX-License-Identifier: Apache-2.0
// Compiled only into the separate pw3-testfile-init entry. The default RAM
// This is the one entry there is. The RAM pre-init and the dedicated-partition
// direct init went with their modes on 2026-10-29, and the two command-line
// keys that chose between them went with them.
#include "TestFileFormat.h"
#include "TestFileMapper.h"
#include "FatCrossLink.h"
#include "SystemStamp.h"
#include "../report/ReportArea.h"
#include <time.h>

/* The one place both headers are visible, so the one place the agreement can be
 * made a compile error instead of a comment (TestFileFormat.h). */
_Static_assert(PW3_TEST_REPORT_BYTES == PW3_REPORT_BYTES, "report area size");

/* The record of the last complete check of this system image, if there is one
 * and it is this file's (SystemStamp.h). */
static bool testfile_stamp_matches(const struct config *cfg, const struct stat *file,
        const struct pw3_file_mapping *map) {
    struct pw3_system_stamp *recorded = (struct pw3_system_stamp *)malloc(sizeof(*recorded));
    int fd = open(PW3_SYSTEM_STAMP_PATH, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat st;
    bool ok = recorded && fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
        st.st_uid == 0 && st.st_size == (off_t)sizeof(*recorded) &&
        read(fd, recorded, sizeof(*recorded)) == (ssize_t)sizeof(*recorded) &&
        pw3_system_stamp_matches(recorded, cfg->system_size, (int64_t)file->st_mtim.tv_sec,
                                 (int64_t)file->st_mtim.tv_nsec, cfg->system_sha, map);
    if (fd >= 0) close(fd);
    free(recorded);
    return ok;
}
/* Written only after the complete check passed: a new file, synced, renamed over
 * the old record, the directory synced. */
static bool testfile_stamp_write(const struct config *cfg, const struct stat *file,
        const struct pw3_file_mapping *map) {
    struct pw3_system_stamp *stamp = (struct pw3_system_stamp *)malloc(sizeof(*stamp));
    const char *tmp = PW3_SYSTEM_STAMP_PATH ".new";
    if (!stamp) return false;
    pw3_system_stamp_make(stamp, cfg->system_size, (int64_t)file->st_mtim.tv_sec,
                          (int64_t)file->st_mtim.tv_nsec, cfg->system_sha, map);
    if (mkdir(PW3_SYSTEM_STAMP_DIR, 0700) && errno != EEXIST) { free(stamp); return false; }
    unlink(tmp);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0400);
    bool ok = fd >= 0 && write(fd, stamp, sizeof(*stamp)) == (ssize_t)sizeof(*stamp) && fsync(fd) == 0;
    if (fd >= 0 && close(fd)) ok = false;
    free(stamp);
    ok = ok && rename(tmp, PW3_SYSTEM_STAMP_PATH) == 0;
    int dir = open(PW3_SYSTEM_STAMP_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    ok = ok && dir >= 0 && fsync(dir) == 0;
    if (dir >= 0) close(dir);
    return ok;
}

/* What this start did not read, for pw3-system-verify to read once Android is
 * up (SystemRecheck.h). Written only where the record was trusted, so its
 * absence is "the image was read here". */
static bool testfile_recheck_write(const struct config *cfg, const char *device) {
    struct pw3_system_recheck record;
    char line[PW3_SYSTEM_RECHECK_MAX];
    size_t length;
    memset(&record, 0, sizeof(record));
    if (strlen(device) >= sizeof(record.device)) return false;
    strcpy(record.device, device);
    record.size = cfg->system_size;
    memcpy(record.sha, cfg->system_sha, sizeof(record.sha));
    length = pw3_system_recheck_print(line, sizeof(line), &record);
    if (!length) return false;
    unlink(PW3_SYSTEM_RECHECK_PATH);
    int fd = open(PW3_SYSTEM_RECHECK_PATH, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0400);
    bool ok = fd >= 0 && write(fd, line, length) == (ssize_t)length;
    if (fd >= 0 && close(fd)) ok = false;
    return ok;
}

static bool testfile_cmdline(const struct config *cfg) {
    char buf[4096], copy[4096], *save = NULL, *p;
    const char *values[] = {"rdinit=/pw3-testfile-init", "androidboot.hardware=pw3",
        "androidboot.init_rc=/init.pw3-main.rc"};
    const char *keys[] = {"rdinit=", "androidboot.hardware=", "androidboot.init_rc="};
    unsigned counts[3] = {0}; unsigned char hash[32]; struct utsname un;
    if (!read_text("/proc/cmdline", buf, sizeof(buf)) ||
        !SHA256((const unsigned char *)buf, strlen(buf), hash) || memcmp(hash, cfg->cmdline_sha, 32) ||
        uname(&un) || strcmp(un.release, cfg->release)) return false;
    strcpy(copy, buf);
    for (p = strtok_r(copy, " \t", &save); p; p = strtok_r(NULL, " \t", &save))
        for (unsigned i = 0; i < 3; ++i) if (!strncmp(p, keys[i], strlen(keys[i]))) {
            if (strcmp(p, values[i])) return false;
            ++counts[i];
        }
    for (unsigned i = 0; i < 3; ++i) if (counts[i] != 1) return false;
    return true;
}
static bool testfile_physical(const struct config *cfg) {
    // Never remove an existing read-only protection. The disk and user
    // partition must already be writable; every other observed area is locked.
    static const char *names[] = {"mmcblk0", "mmcblk0p1", "mmcblk0p2", "mmcblk0p3",
        "mmcblk0p4", "mmcblk0boot0", "mmcblk0boot1"};
    if (cfg->block_count != 7) return false;
    for (unsigned i = 0; i < 7; ++i) {
        bool found = false;
        for (unsigned j = 0; j < 7; ++j) if (!strcmp(names[i], cfg->blocks[j].name)) found = true;
        if (!found) return false;
    }
    DIR *dir = opendir("/sys/class/block"); struct dirent *e; unsigned found = 0; bool ok = true;
    if (!dir) return false;
    while ((e = readdir(dir))) {
        if (strncmp(e->d_name, "mmcblk0", 7)) continue;
        ++found;
        const struct block *expected = NULL;
        for (unsigned i = 0; i < cfg->block_count; ++i)
            if (!strcmp(e->d_name, cfg->blocks[i].name)) expected = &cfg->blocks[i];
        char path[128]; uint64_t ro, sectors, start;
        if (!expected || !block_node(e->d_name, path) ||
            !sysnum(e->d_name, "size", &sectors) || sectors != expected->sectors ||
            (expected->has_start && (!sysnum(e->d_name, "start", &start) || start != expected->start))) {
            ok = false; continue;
        }
        int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW), value = -1;
        bool writable = !strcmp(e->d_name, "mmcblk0") || !strcmp(e->d_name, "mmcblk0p4");
        if (fd < 0 || (!writable && !readonly_fd(fd)) || ioctl(fd, BLKROGET, &value) ||
            value != (writable ? 0 : 1) || !sysnum(e->d_name, "ro", &ro) || ro != (writable ? 0U : 1U)) ok = false;
        if (fd >= 0) close(fd);
    }
    closedir(dir);
    return ok && found == 7;
}
static bool testfile_mapped_node(const char *name, unsigned expected_major,
        unsigned expected_minor, uint64_t bytes, bool ro, char path[128]) {
    struct stat st; uint64_t actual = 0; int value = -1;
    if (!block_node(name, path)) return false;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    bool ok = fd >= 0 && fstat(fd, &st) == 0 && S_ISBLK(st.st_mode) &&
        major(st.st_rdev) == expected_major && minor(st.st_rdev) == expected_minor &&
        ioctl(fd, BLKGETSIZE64, &actual) == 0 && actual == bytes &&
        ioctl(fd, BLKROGET, &value) == 0 && value == (ro ? 1 : 0);
    if (fd >= 0) close(fd);
    return ok;
}
static bool testfile_mounted(const char *path, unsigned long magic, bool ro, bool noexec) {
    struct statfs fs; struct statvfs flags;
    return statfs(path, &fs) == 0 && (uint32_t)fs.f_type == magic && statvfs(path, &flags) == 0 &&
        !!(flags.f_flag & ST_RDONLY) == ro && !!(flags.f_flag & ST_NOEXEC) == noexec &&
        (flags.f_flag & ST_NODEV) && (flags.f_flag & ST_NOSUID);
}
/* Copies of this image's applications that something once compiled into /data,
 * removed before Android starts.
 *
 * ART prefers a /data/dalvik-cache copy over the odex beside the APK whenever
 * the APK's directory is read-only, which /system always is, and it does not
 * compare the two compiler filters while doing so (art/runtime/
 * oat_file_assistant.cc, GetBestInfo). Every application of this image is
 * compiled ahead of time to `speed` (pw3.mk), so a copy of one in /data can
 * only have been made by a version that did not, and all it can do now is
 * shadow the image with something compiled worse. Measured on the device under
 * V59: 21 of 31 applications ran from /data, Settings, DocumentsUI and
 * PackageInstaller among them, all three of which the image had just compiled
 * to `speed`.
 *
 * Unconditional, rather than only when the image changed: there is no
 * condition here to get wrong, and once nothing writes these the directory is
 * empty and this is one readdir. `system@framework@` is deliberately not
 * touched -- the boot image is not an application, it costs twenty minutes to
 * make, and a copy of it in /data would be a fault to report rather than a
 * file to delete (InitCheck).
 *
 * A diagnostic, so nothing here stops a start: a name that cannot be removed
 * is counted and said. */
static void testfile_drop_shadowed_apps(void) {
    static const char *const prefixes[] = {"system@app@", "system@priv-app@"};
    DIR *root = opendir("/data/dalvik-cache");
    unsigned removed = 0, failed = 0, unreadable = 0;
    struct dirent *isa;
    /* Absent is the normal case on a start whose /data has none of these.
     * Anything else means the sweep did not happen, and saying nothing about
     * that was how it could silently stop working: stale copies of this
     * image's own applications would go on being preferred out of /data. */
    if (!root) {
        if (errno != ENOENT) report("cannot read /data/dalvik-cache, so /data copies of this "
                                    "image's applications were not looked for");
        return;
    }
    while ((isa = readdir(root))) {
        if (!strcmp(isa->d_name, ".") || !strcmp(isa->d_name, "..")) continue;
        int isafd = openat(dirfd(root), isa->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (isafd < 0) { if (errno != ENOTDIR) ++unreadable; continue; }
        DIR *dir = fdopendir(isafd);
        struct dirent *e;
        if (!dir) { close(isafd); ++unreadable; continue; }
        while ((e = readdir(dir))) {
            struct stat st;
            unsigned i;
            for (i = 0; i < 2; ++i)
                if (!strncmp(e->d_name, prefixes[i], strlen(prefixes[i]))) break;
            if (i == 2) continue;
            if (fstatat(dirfd(dir), e->d_name, &st, AT_SYMLINK_NOFOLLOW) || !S_ISREG(st.st_mode)) continue;
            if (unlinkat(dirfd(dir), e->d_name, 0)) ++failed; else ++removed;
        }
        closedir(dir);
    }
    closedir(root);
    if (removed || failed || unreadable) {
        char text[160];
        snprintf(text, sizeof(text), "removed %u /data copies of this image's applications "
                 "(%u could not be removed, %u subdirectories could not be read)",
                 removed, failed, unreadable);
        report(text);
    }
}
static const char testfile_contract[] __attribute__((used)) =
    "PW3_TESTFILE_INIT_V13_DM_CLUSTER_EXTENTS_FAT_READONLY_VERIFY_THEN_RW_ANDROID_TMPFS_COVERED_KINDLE_ROOT_VIEW_READ_WRITE_DATA_IMAGE_GROWN_IN_PLACE_DATA_SIZED_IMAGES_SYSTEM_STAMP_BACKGROUND_RECHECK_FAT_CROSSLINK_OPTIONAL_REPORT_AREA_IMAGE_OWNS_APP_CODE_THREE_KEY_CMDLINE_NO_PARTITIONING";
/* In .bss and not on the stack: at PW3_FILEMAP_MAX_EXTENTS = 8192 one
 * pw3_file_maps is ~575 KiB, which PID 1 must not put on its (small) stack.
 * File scope here is the same single translation unit; the host tests that
 * include these headers get their own copy per binary. */
static struct pw3_file_maps maps;
int main(void) {
    struct config cfg; struct pw3_test_data data; struct statfs root;
    struct stat before, after; uint64_t p4_size = 0, effective, actual;
    unsigned char sector[512], sb[1024]; char buffer[1024], p4[128], fat[128], system[128], userdata[128], reportnode[128];
    char system_path[256], data_path[256], report_path[256]; int p4fd, systemfd, datafd, reportfd;
    if (getpid() != 1) return 2;
    if (statfs("/", &root) || ((uint32_t)root.f_type != 0x858458f6U && (uint32_t)root.f_type != 0x01021994U))
        stop("testfile entry requires a RAM root");
    if (!mkdir_ram("/proc", 0755) || !mkdir_ram("/sys", 0755) || !mkdir_ram("/dev", 0755) ||
        mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, "hidepid=2,gid=3009") ||
        mount("sysfs", "/sys", "sysfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) ||
        mount("tmpfs", "/dev", "tmpfs", MS_NOSUID, "mode=0755,size=4m") ||
        !mkdir_ram("/dev/block", 0700)) stop("cannot establish testfile pseudo filesystems");
    if (node_from_sysfs("/sys/class/tty/console/dev", "/dev/console", S_IFCHR)) {
        int fd = open("/dev/console", O_WRONLY | O_CLOEXEC | O_NONBLOCK);
        if (fd >= 0) { if (console_fd >= 0) close(console_fd); console_fd = fd; }
    }
    if (node_from_sysfs("/sys/class/mem/kmsg/dev", "/dev/kmsg", S_IFCHR)) {
        /* Only replace a channel that is already working if the new one opens:
         * losing a usable descriptor here would take the reason for every later
         * failure with it. */
        int fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
        if (fd >= 0) { if (kmsg_fd >= 0) close(kmsg_fd); kmsg_fd = fd; }
    }
    /* The memory character devices, for the /system tools this start runs.
     *
     * /system/bin/resize2fs and e2fsck are bionic binaries and call getentropy()
     * in their own startup; bionic implements it with open("/dev/urandom") and
     * aborts through __libc_fatal when that fails. /dev is this start's own
     * tmpfs and used to carry only what was made from sysfs -- console, kmsg and
     * the block nodes -- so the one-way data image growth died on the emulator
     * with SIGABRT from a child that had printed nothing (2026-11-10):
     *
     *     libc: getentropy failed
     *
     * /dev/null and /dev/zero are here as well because a child's chatter is
     * redirected to /dev/null and because tools expect them to exist at all; the
     * nodes are not fatal to make. sysfs names the numbers; the fallback is the
     * kernel's fixed mem major (1) in case a kernel hides the class. Android
     * replaces this /dev with its own later, so nothing here outlives the start. */
    {
        static const struct { const char *sysdev, *node; unsigned minor; } memory_nodes[] = {
            {"/sys/class/mem/null/dev", "/dev/null", 3},
            {"/sys/class/mem/zero/dev", "/dev/zero", 5},
            {"/sys/class/mem/random/dev", "/dev/random", 8},
            {"/sys/class/mem/urandom/dev", "/dev/urandom", 9},
        };
        char node_line[96];
        unsigned made = 0, failed = 0;
        for (unsigned i = 0; i < 4; ++i) {
            if (node_from_sysfs(memory_nodes[i].sysdev, memory_nodes[i].node, S_IFCHR) ||
                !mknod(memory_nodes[i].node, S_IFCHR | 0600, makedev(1, memory_nodes[i].minor)))
                ++made;
            else if (errno != EEXIST)
                ++failed;
        }
        snprintf(node_line, sizeof(node_line), "PW3_STORAGE memory_nodes made=%u failed=%u", made, failed);
        report(node_line);
    }
    report(testfile_contract);
    if (!only_pseudo_mounts() || !parse_config(CONF_PATH, &cfg) || !testfile_cmdline(&cfg) ||
        !read_text("/pw3-testdata.conf", buffer, sizeof(buffer)) || !pw3_test_data_parse(buffer, &data))
        stop("testfile configuration or complete command line rejected");
    // Both files must belong to the same dedicated test directory.
    char expected_system[192]; strcpy(expected_system, data.path);
    strcpy(strrchr(expected_system, '/') + 1, "system.img");
    if (strcmp(expected_system, cfg.system_path)) stop("system and data test directories differ");
    /* The report area sits beside them, under the name the installer makes it
     * with; no configuration names it, because its name and size are fixed. */
    char expected_report[192]; strcpy(expected_report, data.path);
    strcpy(strrchr(expected_report, '/') + 1, PW3_REPORT_NAME);
    if (!pw3_test_size(cfg.system_size, PW3_FILE_SYSTEM)) stop("test system image size outside its limits");
    bool locked = false;
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        if (testfile_physical(&cfg)) { usleep(100000); if (testfile_physical(&cfg)) { locked = true; break; } }
        usleep(100000);
    }
    if (!locked) stop("testfile geometry or protected-area state rejected");
    for (unsigned i = 0; i < cfg.block_count; ++i) if (!strcmp(cfg.blocks[i].name, "mmcblk0p4")) p4_size = cfg.blocks[i].sectors * 512;
    effective = cfg.sizelimit ? cfg.sizelimit : p4_size - cfg.offset;
    if (cfg.offset != 8192 || cfg.sizelimit || !block_node("mmcblk0p4", p4) ||
        !node_from_sysfs("/sys/class/misc/device-mapper/dev", "/dev/device-mapper", S_IFCHR))
        stop("exact test FAT mapping interface unavailable");
    p4fd = open(p4, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (p4fd < 0 || ioctl(p4fd, BLKGETSIZE64, &actual) || actual != p4_size ||
        pread(p4fd, sector, sizeof(sector), cfg.offset) != (ssize_t)sizeof(sector) || !pw3_fat_bounds(sector, effective))
        stop("user FAT backing rejected");
    struct stat parent_st, mapped_st; unsigned dm_major = 0, dm_minor = 0;
    int control = open("/dev/device-mapper", O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (control < 0 || fstat(p4fd, &parent_st) ||
        !pw3_testfat_create(control, major(parent_st.st_rdev), minor(parent_st.st_rdev), 16,
                           effective / 512, &dm_major, &dm_minor) || !block_node("dm-0", fat))
        stop("bounded linear test FAT view rejected");
    int mapped_fd = open(fat, O_RDONLY | O_CLOEXEC | O_NOFOLLOW), mapped_ro = -1;
    if (mapped_fd < 0 || fstat(mapped_fd, &mapped_st) || !S_ISBLK(mapped_st.st_mode) ||
        major(mapped_st.st_rdev) != dm_major || minor(mapped_st.st_rdev) != dm_minor ||
        ioctl(mapped_fd, BLKGETSIZE64, &actual) || actual != effective ||
        ioctl(mapped_fd, BLKROGET, &mapped_ro) || mapped_ro != 0)
        stop("linear test FAT view size or identity rejected");
    close(mapped_fd); close(control); close(p4fd);
    const char *fat_options = "uid=0,gid=0,fmask=0177,dmask=0077,codepage=437,iocharset=iso8859-1";
    if (!mkdir_ram(FAT_MOUNT, 0700) || mount(fat, FAT_MOUNT, "vfat", MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC, fat_options) ||
        !testfile_mounted(FAT_MOUNT, 0x4d44, true, true)) stop("read-only test FAT mount failed");
    snprintf(system_path, sizeof(system_path), "%s/%s", FAT_MOUNT, cfg.system_path);
    snprintf(data_path, sizeof(data_path), "%s/%s", FAT_MOUNT, data.path);
    systemfd = open(system_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat system_stat;
    /* What the image is, not what is inside it. The superblock used to be
     * judged here as well, and that is the one refusal that was never heard:
     * this is before the report area is mapped, so a stop here writes nowhere
     * and the next tap sees a system it believes is fine and hands over to the
     * same refusal again. Worse, it pre-empted the SHA256 check further down,
     * which says the same thing about a damaged image in the sentence the
     * installer acts on. The superblock is judged after that check now. */
    if (systemfd < 0 || fstat(systemfd, &system_stat) || !S_ISREG(system_stat.st_mode) || (uint64_t)system_stat.st_size != cfg.system_size)
        stop("test system image missing, not a regular file, or not the recorded size");
    datafd = open(data_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (datafd < 0 || fstat(datafd, &before) || !S_ISREG(before.st_mode) || before.st_nlink != 1 ||
        (uint64_t)before.st_size != data.size || pread(datafd, sb, sizeof(sb), 1024) != (ssize_t)sizeof(sb) ||
        memcmp(sb + 0x68, data.uuid, 16)) stop("test data image format, size, state or UUID rejected");
    /* The filesystem inside the image may be smaller than the image itself.
     * That is the one-way upgrade from a release whose data image was 1.75 GiB:
     * the installer grew the file in place (it cannot grow what is inside it,
     * the stock kernel has no ext4), and this entry grows the filesystem to the
     * file before Android mounts it. A filesystem larger than its file is still
     * refused -- no cluster of the file holds it -- and so is an image of any
     * other shape. The tool that does the growing comes from /system, so it runs
     * after /system has been checked, below. */
    const uint64_t data_fs_bytes = pw3_ext4_bytes(sb);
    if (!data_fs_bytes || data_fs_bytes > data.size || !pw3_ext4_in_use(sb, data_fs_bytes))
        stop("test data image format, size, state or UUID rejected");
    const bool data_grow = data_fs_bytes != data.size;
    snprintf(buffer, sizeof(buffer), "PW3_STORAGE data_bytes=%llu data_fs_bytes=%llu grow=%d",
             (unsigned long long)data.size, (unsigned long long)data_fs_bytes, data_grow ? 1 : 0);
    report(buffer);
    /* The report area, which is a diagnostic and therefore optional: an
     * installation from before it existed has none, the installer makes none on
     * a store with no room to spare, and a file at that name that is not ours
     * is left alone (install.c). None of that may stop a start, so every
     * failure here leaves the mapping absent and says which one it was.
     *
     * Checked for being ours here as well as in the installer, and by the one
     * rule both share (pw3_report_claim): the installer leaves a file of
     * exactly the right size alone whatever is in it, so without this a
     * stranger's 64 KiB file at that name would be mapped writable and Android
     * would write over it. The file is on the read-only FAT, so reading all of
     * it costs one 64 KiB read. */
    snprintf(report_path, sizeof(report_path), "%s/%s", FAT_MOUNT, expected_report);
    reportfd = open(report_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat report_stat;
    /* Each failure says which one it was. None of them stops a start, so all
     * three used to end in one of three sentences about the file -- "there is
     * none", "it is the wrong size", "it is not ours" -- and a call that was
     * refused came out as a statement about a file nobody had looked at. The
     * only reader of this text is somebody trying to find out why there is no
     * report, and "there is none" sends them to the installer while "it would
     * not open" sends them to the store. */
    if (reportfd < 0) {
        if (errno == ENOENT) {
            report("no report area on the user store; this start writes no report");
        } else {
            PW3_NOTE("open", report_path);
            report("the report area is there and will not open; this start writes no report");
        }
    } else if (fstat(reportfd, &report_stat)) {
        PW3_NOTE("fstat", report_path);
        report("the report area's own size cannot be read; this start writes no report");
        close(reportfd); reportfd = -1;
    } else if (!S_ISREG(report_stat.st_mode) || report_stat.st_nlink != 1 ||
               (uint64_t)report_stat.st_size != PW3_REPORT_BYTES) {
        report("the report area is not a file of its own size; this start writes no report");
        close(reportfd); reportfd = -1;
    } else {
        struct pw3_report_claim claim; unsigned char chunk[4096]; uint64_t at = 0;
        int unread = 0;
        pw3_report_claim_begin(&claim);
        while (at < PW3_REPORT_BYTES) {
            const ssize_t got = pread(reportfd, chunk, sizeof(chunk), (off_t)at);
            if (got < 0) { PW3_NOTE("pread", report_path); unread = 1; break; }
            if (got == 0) { unread = 1; break; }
            pw3_report_claim_feed(&claim, chunk, (size_t)got);
            at += (uint64_t)got;
        }
        if (unread) {
            report("the report area cannot be read through; this start writes no report");
            close(reportfd); reportfd = -1;
        } else if (!pw3_report_claim_ours(&claim)) {
            report("the file at the report area's name is not ours; this start writes no report");
            close(reportfd); reportfd = -1;
        }
    }
    memset(&maps, 0, sizeof(maps)); maps.magic = PW3_FILEMAP_MAGIC;
    char map_message[192];
    /* No part of either image may be mapped before the FAT's data area; the
     * boot sector was accepted by pw3_fat_bounds above, so this is its own. */
    const uint64_t data_start = pw3_fat_data_start(sector);
    if (!data_start) stop("the FAT boot sector does not give a data area");
    report("verifying the clusters and bounded extents of the system file");
    enum pw3_file_map_error map_error = pw3_file_mapping_read(systemfd, cfg.system_size, effective / 512,
                                                              data_start, &maps.map[PW3_FILE_SYSTEM]);
    if (map_error != PW3_FILEMAP_OK)
        stop(pw3_file_map_error_message(map_error, PW3_FILE_SYSTEM, map_message, sizeof(map_message)));
    report("verifying the clusters and bounded extents of the data file");
    map_error = pw3_file_mapping_read(datafd, data.size, effective / 512, data_start, &maps.map[PW3_FILE_DATA]);
    if (map_error != PW3_FILEMAP_OK)
        stop(pw3_file_map_error_message(map_error, PW3_FILE_DATA, map_message, sizeof(map_message)));
    if (reportfd >= 0) {
        report("verifying the clusters and bounded extents of the report area");
        map_error = pw3_file_mapping_read(reportfd, PW3_REPORT_BYTES, effective / 512, data_start,
                                          &maps.map[PW3_FILE_REPORT]);
        if (map_error != PW3_FILEMAP_OK) {
            /* Its clusters could not be measured, which for a diagnostic is a
             * reason to leave it alone rather than to stop a start. The mapping
             * goes back to absent so nothing downstream sees a half-measured one. */
            report(pw3_file_map_error_message(map_error, PW3_FILE_REPORT, map_message, sizeof(map_message)));
            memset(&maps.map[PW3_FILE_REPORT], 0, sizeof(maps.map[PW3_FILE_REPORT]));
            close(reportfd); reportfd = -1;
        }
    }
    if (!pw3_file_maps_bounds(&maps, p4_size / 512))
        stop("test file extent coverage, bounds or cross-file overlap rejected");
    /* The two files do not overlap each other; whether a third file on this
     * FAT claims their clusters takes both the table and the directory tree
     * to answer (FatCrossLink.h). Writing /data through a cross-linked map
     * would write into one of the user's books. */
    report("checking the FAT table and directories for clusters shared with another file");
    {
        /* cfg.offset / 512 is where the FAT volume begins in the extents'
         * frame, which is sectors of mmcblk0p4 (FatCrossLink.h). */
        int volume = open(fat, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        enum pw3_fat_cross cross = volume < 0 ? PW3_FAT_CROSS_READ
            : pw3_fat_cross_check(volume, sector, cfg.offset / 512, &maps);
        if (volume >= 0) close(volume);
        if (cross != PW3_FAT_CROSS_OK) stop(pw3_fat_cross_text(cross));
    }
    /* In the step file as well as the log: a boot that then compiles for
     * three minutes has pushed the kernel log's ring buffer past this long
     * before anything can read it. */
    char extent_summary[128];
    snprintf(extent_summary, sizeof(extent_summary), "verified file extents system=%u data=%u report=%u limit=%u",
             maps.map[PW3_FILE_SYSTEM].count, maps.map[PW3_FILE_DATA].count,
             maps.map[PW3_FILE_REPORT].count, PW3_FILEMAP_MAX_EXTENTS);
    report(extent_summary);
    if (posix_fadvise(systemfd, 0, cfg.system_size, POSIX_FADV_DONTNEED) ||
        posix_fadvise(datafd, 0, data.size, POSIX_FADV_DONTNEED))
        stop("test image scan-cache discard failed");
    if (reportfd >= 0) posix_fadvise(reportfd, 0, PW3_REPORT_BYTES, POSIX_FADV_DONTNEED);
    close(datafd);
    /* dm-1 is the system map and dm-2 the data map (fstab, InitCheck): the
     * read-only system map is made first; it is mounted only after its check. */
    control = open("/dev/device-mapper", O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (control < 0 || !pw3_file_map_create(control, PW3_FILE_SYSTEM, major(parent_st.st_rdev),
        minor(parent_st.st_rdev), p4_size / 512, &maps.map[PW3_FILE_SYSTEM], &dm_major, &dm_minor) ||
        !testfile_mapped_node("dm-1", dm_major, dm_minor, cfg.system_size, true, system))
        stop("read-only multi-extent test system map rejected");
    if (!testfile_mounted(FAT_MOUNT, 0x4d44, true, true) ||
        lstat(data_path, &after) || after.st_dev != before.st_dev || after.st_ino != before.st_ino ||
        after.st_size != before.st_size || !S_ISREG(after.st_mode) || after.st_nlink != 1 ||
        !pw3_file_map_create(control, PW3_FILE_DATA, major(parent_st.st_rdev), minor(parent_st.st_rdev), p4_size / 512,
            &maps.map[PW3_FILE_DATA], &dm_major, &dm_minor) ||
        !testfile_mapped_node("dm-2", dm_major, dm_minor, data.size, false, userdata))
        stop("writable multi-extent test data map rejected");
    /* dm-3 is the report area: written by sector from Android and by nothing
     * else, never mounted, and absent when there is nothing to map. A failure
     * here costs the diagnostic and not the start. */
    if (reportfd >= 0) {
        if (!pw3_file_map_create(control, PW3_FILE_REPORT, major(parent_st.st_rdev), minor(parent_st.st_rdev),
                p4_size / 512, &maps.map[PW3_FILE_REPORT], &dm_major, &dm_minor) ||
            !testfile_mapped_node("dm-3", dm_major, dm_minor, PW3_REPORT_BYTES, false, reportnode)) {
            report("the report area was not mapped; this start writes no report");
            memset(&maps.map[PW3_FILE_REPORT], 0, sizeof(maps.map[PW3_FILE_REPORT]));
        } else {
            /* From here on, a stop writes its reason where the next tap can
             * read it (report_stop in PreInit.c). The checks that follow are
             * the ones a tap can act on -- the system image's own hash first of
             * all -- which is why this is set at the mapping and not earlier. */
            snprintf(report_node, sizeof(report_node), "%s", reportnode);
        }
        close(reportfd); reportfd = -1;
    }
    close(control);
    if (!mkdir_ram("/data", 0771) || mount(userdata, "/data", "ext4", MS_NOSUID | MS_NODEV | MS_NOATIME,
        "errors=remount-ro,data=ordered,barrier=1") || !testfile_mounted("/data", 0xef53, false, false) ||
        chown("/data", 1000, 1000) || chmod("/data", 0771) || !testfile_physical(&cfg)) stop("test data mount or final protection check failed");
    testfile_drop_shadowed_apps();
    /* The record's directory, made here rather than when a record is first
     * written, so that init always has a path to label (init.pw3.rc). */
    if (mkdir(PW3_SYSTEM_STAMP_DIR, 0700) && errno != EEXIST)
        report("the record directory was not made; the image is read at every start");
    /* The system image: its record of a complete check, or the complete check,
     * of what dm-1 maps, which is what /system will be. */
    if (testfile_stamp_matches(&cfg, &system_stat, &maps.map[PW3_FILE_SYSTEM])) {
        report("system image unchanged since its last complete SHA256 check");
        /* Not fatal: the image is then read at the next start that has no
         * record, which is what the device does today without this. */
        if (!testfile_recheck_write(&cfg, system))
            report("the background re-check was not asked for; this image stays unread");
    } else {
        report("checking the complete read-only test system SHA256");
        int mapped = open(system, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        bool read_through = false;
        bool verified = mapped >= 0 && sha_fd(mapped, cfg.system_size, cfg.system_sha, &read_through);
        if (mapped >= 0) { posix_fadvise(mapped, 0, cfg.system_size, POSIX_FADV_DONTNEED); close(mapped); }
        if (!verified) {
            umount("/data");
            /* Two findings, not one, and only the first is one a tap can act
             * on. "Rejected" means the image was read through and is not the
             * image its record names, so installing it again is the answer and
             * the next tap does that (installer/install/install.c). A read that
             * did not get through says nothing about the content: the image may
             * be perfectly good and the store momentarily not. Saying
             * "rejected" there would have the next tap throw a good image away
             * and ask for 196 MB it may be offline for, where reading it again
             * is all that was ever needed. */
            stop(read_through ? "test system image SHA256 rejected"
                              : "test system image could not be read through");
        }
        if (!testfile_stamp_write(&cfg, &system_stat, &maps.map[PW3_FILE_SYSTEM]))
            report("system image record not written; the next start checks it again");
    }
    /* The superblock, after the hash and not before the report area. Whenever
     * the hash runs it finds a damaged image first and says so in the sentence
     * the installer acts on, so this is reached either by an image the hash
     * passed -- one this kernel cannot mount although it is the right image,
     * which is a build problem and now at least leaves a reason behind -- or by
     * one whose stamp let the hash be skipped, which is where bit rot under an
     * unchanged size and mtime lands. Both ask for the same answer, and
     * ReportArea.h holds this sentence and the installer's match together. */
    if (pread(systemfd, sb, sizeof(sb), 1024) != (ssize_t)sizeof(sb))
        /* The same two findings the hash above keeps apart, for the same
         * reason. A superblock that was not read says nothing about the image:
         * the store may be momentarily unwell and the image perfectly good, and
         * the sentence below would have the next tap throw it away and ask for
         * 196 MB the reader may be offline for. Reading it again is all that is
         * ever needed, so this one is not a sentence the installer acts on. */
        stop("test system image superblock could not be read");
    if (!pw3_ext4_clean(sb, cfg.system_size))
        stop("test system image format rejected");
    close(systemfd);
    /* O6: every FAT read is done (extent mapping, cross-check, system SHA256,
     * record) and systemfd -- the last fd under /pw3-user -- is closed, so the
     * read-only FAT mount can be released; hand the store to Android read-write
     * before the exec, while the SELinux policy is not loaded yet. */
    if (!testfile_handover_store(fat))
        report("the read-write hand-over store is not available this start");
    preinit_nav_settings();
    if (!mkdir_ram("/system", 0755) || mount(system, "/system", "ext4", MS_RDONLY | MS_NODEV, "noload,errors=remount-ro") ||
        !mounted_readonly("/system", 0xef53, false, false) || !hash_regular("/init", cfg.init_sha) ||
        (cfg.adb_key && !hash_regular("/adb_keys", cfg.adb_sha)) || !testfile_physical(&cfg))
        stop("test system, AOSP init, authentication or protected-area check failed");
    /* The one-way data image upgrade (TestFileFormat.h): the filesystem is
     * smaller than the image file the installer grew in place, so it is grown to
     * that file now. It has to happen here and not earlier: the tool that does
     * it is /system/bin/resize2fs, and /system has only just been admitted. It
     * has to happen before the exec: Android must mount a /data of the size the
     * configuration names.
     *
     * The filesystem is unmounted for it, because resize2fs 1.43.3 asks the
     * kernel for the new size with an ioctl this kernel does not have (ext4
     * online resize arrived in 3.7; this is 3.0.35), and because an offline
     * resize is the transaction that does not depend on either side's version.
     * No file of it is open by now -- the application sweep and the system
     * record are done -- but the hand-over above has bound two views of the
     * store into it (documents/ and the root view, the latter with its tmpfs
     * cover on android/), and VFS refuses to unmount a filesystem that has
     * mounts below it (EBUSY). They are this start's own, made a few lines
     * above, and nothing has seen them yet (Android has not started); they are
     * released here and made again below, on the resized filesystem. A build
     * that skipped this left the resized filesystem at its old size while the
     * log said it had grown (found on the emulator, 2026-11-10).
     *
     * Best effort with respect to the start: a shrink no, an image grown and a
     * filesystem still of its old size is a device that boots with the space it
     * had, and the next start tries again. Only losing /data altogether is a
     * stop, because there is nothing to hand Android then. */
    if (data_grow) {
        /* The order is the order they are stacked in: the cover inside the root
         * view, the root view itself, documents/. Their absence is not an error
         * -- each of the two views is best effort in the hand-over. */
        static const char *const views[] = {"/data/media/0/kindle/android",
            "/data/media/0/kindle", "/data/media/0/documents"};
        char grow_line[128];
        unsigned released = 0, held = 0;
        report("the data filesystem is smaller than its image; growing it in place");
        for (unsigned i = 0; i < 3; ++i) {
            if (!umount(views[i]) || errno == EINVAL || errno == ENOENT) ++released;
            else ++held;
        }
        snprintf(grow_line, sizeof(grow_line), "PW3_STORAGE grow_release views=%u held=%u errno=%d",
                 released, held, held ? errno : 0);
        report(grow_line);
        if (umount("/data")) {
            report("the data filesystem could not be unmounted; this start uses it at its present size");
        } else {
            if (!testfile_grow_data(userdata, data.size, data_fs_bytes, data.uuid))
                report("the data filesystem was not grown; this start uses it at its present size");
            if (mount(userdata, "/data", "ext4", MS_NOSUID | MS_NODEV | MS_NOATIME,
                      "errors=remount-ro,data=ordered,barrier=1") || !testfile_mounted("/data", 0xef53, false, false))
                stop("the data filesystem could not be mounted again after growing it");
            /* The two views the hand-over made are made again on the filesystem
             * that now has its size. Their own fail-closed behaviour is
             * unchanged: each withdraws itself if it cannot be made whole. The
             * store they are bound from (/kindle-us) was not touched. */
            testfile_documents_bind();
            testfile_kindle_root_view();
        }
    }
    /* PagePress keys: load the two modules the entry carries in its initramfs
     * and enable the keypad's event generation before the exec. */
    testfile_pagepress();
    int receipt = open(PW3_FILEMAP_PATH, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0400);
    if (receipt < 0 || fchmod(receipt, 0400) || fchown(receipt, 0, 0) ||
        write(receipt, &maps, sizeof(maps)) != (ssize_t)sizeof(maps) || close(receipt))
        stop("cannot record verified immutable test image extents");
    report("test data mounted on dm-2; executing the original AOSP init");
    char *argv[] = {"/init", NULL};
    char *envp[] = {"PATH=/sbin:/system/sbin:/system/bin:/system/xbin", "HOME=/", "TERM=linux", NULL};
    execve("/init", argv, envp); stop("execve of the original AOSP init failed");
}
