// SPDX-License-Identifier: Apache-2.0
// Read-only contract checks. This program does not mount or repair storage,
// load modules, arm RTC alarms, enter suspend, or enable unauthenticated ADB.
#define LOG_TAG "pw3-init-check"
#include <android-base/properties.h>
#include "../epdc/EpdcContract.h"
#include "../preinit/TestFileFormat.h"
#include "../preinit/TestFileMapper.h"
#include "../report/ReportArea.h"
#include <sys/ioctl.h>
#include <log/log.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <sys/timerfd.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <time.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace {
constexpr unsigned long kFunctionFsMagic = 0x0a647361;
struct Mount { std::string path, flags, type, source, super_flags; };

/* The first system call of this run that failed, and why.
 *
 * The checks below are long `&&` chains, by design: each one states a contract
 * and the whole chain has to hold. What that costs is the reason -- a failed
 * stat and a value that does not match collapse into the same false, and the
 * message that comes out describes the comparison. On 2026-10-21 a V65 start
 * was refused with "test image mappings no longer match the admitted regular
 * file extents" when the truth was a denied stat of /dev/block/dm-3, and the
 * same shape was then found in four more places.
 *
 * Restructuring every chain to carry its own error would be a large change for
 * a decision that is already correct: when a check cannot be made, refusing is
 * right. What is wrong is only what gets said. So the information is kept where
 * it is lost -- once, at the first failure -- and every Fail() says it. */
struct Fault {
    const char* call = nullptr;
    std::string path;
    int why = 0;
};
Fault first_fault;
void Note(const char* call, const std::string& path) {
    if (!first_fault.call) first_fault = {call, path, errno};
}
std::string FaultText() {
    if (!first_fault.call) return "";
    // An ifstream that would not open does not always leave an errno behind, and
    // strerror(0) reads as "Success", which is the opposite of the truth.
    return std::string("; first failed system call this run: ") + first_fault.call + " " +
           first_fault.path + ": " +
           (first_fault.why ? strerror(first_fault.why) : "no reason reported");
}
void Report(const std::string& text) {
    ALOGE("%s", text.c_str());
    int fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
        dprintf(fd, "<3>pw3-init-check: %s\n", text.c_str());
        close(fd);
    }
}
bool Fail(const std::string& text) { Report(text + FaultText()); return false; }
/* The wrappers the chains use. Each one behaves exactly as the call it wraps --
 * same return value, same errno -- and notes the first failure on the way past,
 * so that adding one to a chain changes no logic. */
int Stat(const char* path, struct stat* out) {
    const int result = ::stat(path, out);
    if (result) Note("stat", path);
    return result;
}
int LStat(const std::string& path, struct stat* out) {
    const int result = ::lstat(path.c_str(), out);
    if (result) Note("lstat", path);
    return result;
}
int Access(const char* path, int mode) {
    const int result = ::access(path, mode);
    if (result) Note("access", path);
    return result;
}
int StatFs(const char* path, struct statfs* out) {
    const int result = ::statfs(path, out);
    if (result) Note("statfs", path);
    return result;
}
int Open(const std::string& path, int flags) {
    const int result = ::open(path.c_str(), flags);
    if (result < 0) Note("open", path);
    return result;
}
/* Returns the first line, and notes it when the file could not be read at all.
 * Every caller compares the result against a value, so an unreadable file used
 * to read as "not read-only" or "the size changed". */
std::string Read(const std::string& path) {
    std::ifstream in(path);
    if (!in) Note("open", path);
    std::string value;
    std::getline(in, value);
    return value;
}
bool HasFlag(const std::string& flags, const std::string& wanted) {
    std::istringstream in(flags);
    std::string item;
    while (std::getline(in, item, ',')) if (item == wanted) return true;
    return false;
}
bool ReadMount(const std::string& path, Mount* result) {
    std::ifstream in("/proc/self/mountinfo");
    // Not readable and "there is no such mount" were the same false, and the
    // callers say the latter.
    if (!in) Note("open", "/proc/self/mountinfo");
    std::string line;
    bool found = false;
    while (std::getline(in, line)) {
        std::istringstream fields(line);
        std::string id, parent, device, root, token;
        Mount m;
        if (!(fields >> id >> parent >> device >> root >> m.path >> m.flags)) continue;
        while (fields >> token && token != "-") {}
        if (token != "-" || !(fields >> m.type >> m.source >> m.super_flags)) continue;
        if (m.path == path) { *result = m; found = true; }
    }
    return found;
}
bool CheckTmpfs(const char* path, uint64_t limit) {
    Mount m;
    struct statvfs st {};
    if (!ReadMount(path, &m) || m.type != "tmpfs" || HasFlag(m.flags, "ro"))
        return Fail(std::string(path) + " must be its own writable tmpfs mount");
    if (statvfs(path, &st) || !st.f_frsize || st.f_blocks > limit / st.f_frsize)
        return Fail(std::string(path) + " has no verified bounded RAM capacity");
    return true;
}
bool CheckAuthentication() {
    return android::base::GetProperty("ro.adb.secure", "") == "1" &&
           android::base::GetProperty("ro.secure", "") == "1"
        ? true : Fail("authenticated, privilege-dropping adbd defaults are required");
}
bool CheckTestFileData() {
    std::ifstream input("/pw3-testdata.conf");
    std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    pw3_test_data config {};
    if (!input || !pw3_test_data_parse(text.c_str(), &config)) return Fail("test data configuration rejected");
    Mount data, fat, system;
    struct stat node {}, file {}, mounted {}, p4 {}, mapped {}, receipt {}, system_file {}, system_node {}, fat_root {};
    struct statvfs fs {};
    const std::string path = std::string("/pw3-user/") + config.path;
    const auto slash = path.rfind('/');
    const std::string system_path = path.substr(0, slash + 1) + "system.img";
    pw3_file_maps maps {};
    int receipt_fd = Open(PW3_FILEMAP_PATH, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    bool receipt_ok = receipt_fd >= 0 && fstat(receipt_fd, &receipt) == 0 &&
        S_ISREG(receipt.st_mode) && (receipt.st_mode & 07777) == 0400 &&
        receipt.st_uid == 0 && receipt.st_gid == 0 && receipt.st_nlink == 1 &&
        receipt.st_size == sizeof(maps) && read(receipt_fd, &maps, sizeof(maps)) == sizeof(maps);
    if (receipt_fd >= 0) close(receipt_fd);
    if (!receipt_ok || !ReadMount("/data", &data) || data.type != "ext4" || data.source != "/dev/block/dm-2" ||
        HasFlag(data.flags, "ro") || !HasFlag(data.flags, "nosuid") || !HasFlag(data.flags, "nodev") ||
        !ReadMount("/pw3-user", &fat) || fat.type != "vfat" || fat.source != "/dev/block/dm-0" ||
        !HasFlag(fat.flags, "ro") || !HasFlag(fat.flags, "nosuid") || !HasFlag(fat.flags, "nodev") || !HasFlag(fat.flags, "noexec") ||
        !ReadMount("/system", &system) || system.type != "ext4" || system.source != "/dev/block/dm-1" ||
        !HasFlag(system.flags, "ro") || !HasFlag(system.flags, "nodev") ||
        LStat(path, &file) || !S_ISREG(file.st_mode) || file.st_nlink != 1 || (uint64_t)file.st_size != config.size ||
        maps.map[PW3_FILE_DATA].sectors != config.size / 512 ||
        LStat(system_path, &system_file) || !S_ISREG(system_file.st_mode) || system_file.st_nlink != 1 ||
        (uint64_t)system_file.st_size != maps.map[PW3_FILE_SYSTEM].sectors * 512 ||
        Stat("/dev/block/dm-2", &node) || !S_ISBLK(node.st_mode) || Stat("/data", &mounted) || mounted.st_dev != node.st_rdev ||
        Stat("/dev/block/dm-1", &system_node) || !S_ISBLK(system_node.st_mode) ||
        Stat("/system", &mounted) || mounted.st_dev != system_node.st_rdev ||
        Stat("/dev/block/dm-0", &mapped) || !S_ISBLK(mapped.st_mode) || Stat("/pw3-user", &fat_root) || fat_root.st_dev != mapped.st_rdev ||
        Stat("/dev/block/mmcblk0p4", &p4) || !S_ISBLK(p4.st_mode) ||
        maps.map[PW3_FILE_DATA].device != (uint64_t)file.st_dev ||
        maps.map[PW3_FILE_DATA].inode != (uint64_t)file.st_ino ||
        maps.map[PW3_FILE_SYSTEM].device != (uint64_t)system_file.st_dev ||
        maps.map[PW3_FILE_SYSTEM].inode != (uint64_t)system_file.st_ino ||
        maps.map[PW3_FILE_DATA].device != (uint64_t)mapped.st_rdev ||
        statvfs("/data", &fs) || (fs.f_flag & ST_RDONLY) || !fs.f_frsize ||
        fs.f_blocks < (384ULL << 20) / fs.f_frsize || fs.f_blocks > config.size / fs.f_frsize)
        return Fail("test data did not retain its verified file extent and read-only FAT mount");
    const auto count_text = Read("/sys/class/block/mmcblk0p4/size");
    std::istringstream count_stream(count_text); uint64_t sectors = 0; char extra;
    bool ok = (count_stream >> sectors) && !(count_stream >> extra) && sectors > 16 && pw3_file_maps_bounds(&maps, sectors);
    int fd = Open("/dev/device-mapper", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    ok = ok && fd >= 0 && pw3_testfat_verify(fd, major(mapped.st_rdev), minor(mapped.st_rdev),
        major(p4.st_rdev), minor(p4.st_rdev), 16, sectors - 16) &&
        pw3_file_map_verify(fd, PW3_FILE_SYSTEM, major(system_node.st_rdev), minor(system_node.st_rdev),
            major(p4.st_rdev), minor(p4.st_rdev), sectors, &maps.map[PW3_FILE_SYSTEM]) &&
        pw3_file_map_verify(fd, PW3_FILE_DATA, major(node.st_rdev), minor(node.st_rdev),
            major(p4.st_rdev), minor(p4.st_rdev), sectors, &maps.map[PW3_FILE_DATA]);
    /* The report area, when this start mapped one: the same table check, the
     * same file identity, and writable like /data. Absent is not a fault --
     * it is a diagnostic the installer may not have been able to make
     * (TestFileMapper.h) -- but a mapping that is there and does not match
     * what dm reports is the same fault as for the images. */
    bool report_ok = true;
    if (pw3_file_maps_present(&maps, PW3_FILE_REPORT)) {
        const std::string report_path = path.substr(0, slash + 1) + PW3_REPORT_NAME;
        struct stat report_file {}, report_node {};
        report_ok = LStat(report_path, &report_file) == 0 && S_ISREG(report_file.st_mode) &&
            report_file.st_nlink == 1 && (uint64_t)report_file.st_size == PW3_REPORT_BYTES &&
            maps.map[PW3_FILE_REPORT].sectors == PW3_REPORT_BYTES / 512 &&
            maps.map[PW3_FILE_REPORT].device == (uint64_t)report_file.st_dev &&
            maps.map[PW3_FILE_REPORT].inode == (uint64_t)report_file.st_ino &&
            Stat("/dev/block/dm-3", &report_node) == 0 && S_ISBLK(report_node.st_mode) &&
            pw3_file_map_verify(fd, PW3_FILE_REPORT, major(report_node.st_rdev), minor(report_node.st_rdev),
                major(p4.st_rdev), minor(p4.st_rdev), sectors, &maps.map[PW3_FILE_REPORT]) &&
            Read("/sys/class/block/dm-3/ro") == "0" &&
            Read("/sys/class/block/dm-3/size") == std::to_string(maps.map[PW3_FILE_REPORT].sectors);
        ok = ok && report_ok;
    }
    if (fd >= 0) close(fd);
    /* Three messages rather than one. On 2026-10-21 a V65 start failed here and
     * said "mappings no longer match the admitted extents", which was true of
     * nothing: pw3_init_check had no SELinux rule for the report area's device
     * label, so stat("/dev/block/dm-3") failed and a long && chain went false.
     * Whoever reads the next failure should be told which group gave way. */
    if (!report_ok)
        return Fail("the report area is mapped, but its file, its extents or its dm table do not "
                    "agree -- a denied stat of /dev/block/dm-3 reads the same way, so check the "
                    "kernel log for an avc line before the extents");
    if (!ok)
        return Fail("test image mappings no longer match the admitted regular file extents");
    if (Read("/sys/class/block/dm-0/ro") != "0" || Read("/sys/class/block/dm-1/ro") != "1" ||
        Read("/sys/class/block/dm-2/ro") != "0" ||
        Read("/sys/class/block/dm-1/size") != std::to_string(maps.map[PW3_FILE_SYSTEM].sectors) ||
        Read("/sys/class/block/dm-2/size") != std::to_string(maps.map[PW3_FILE_DATA].sectors))
        return Fail("a mapped device's read-only flag or size does not match the extent receipt");
    static const char* names[] = {"mmcblk0", "mmcblk0p1", "mmcblk0p2", "mmcblk0p3", "mmcblk0p4", "mmcblk0boot0", "mmcblk0boot1"};
    DIR* dir = opendir("/sys/class/block");
    if (!dir) return Fail("cannot recheck test storage protection");
    unsigned found = 0;
    while (auto* entry = readdir(dir)) {
        const std::string name(entry->d_name);
        if (name.compare(0, 7, "mmcblk0")) continue;
        ++found; bool known = false;
        for (const char* expected : names) if (name == expected) known = true;
        const bool writable = name == "mmcblk0" || name == "mmcblk0p4";
        if (!known || Read("/sys/class/block/" + name + "/ro") != (writable ? "0" : "1")) ok = false;
    }
    closedir(dir);
    return ok && found == 7 ? true : Fail("test storage protected-area state changed");
}
bool CheckStorage() {
    // One storage arrangement: /system and /data are extents of image files on
     // the FAT user store, mapped as dm-1 and dm-2. The two cmdline keys that
     // used to choose between arrangements went with the arrangements they
     // chose, on 2026-10-29; the whole cmdline is still pinned by its sha256
     // (preinit/TestFileInit.h).
    if (android::base::GetProperty("ro.boot.hardware", "") != "pw3")
        return Fail("missing explicit PW3 storage profile");
    Mount m;
    struct statvfs fs {};
    if (!ReadMount("/proc", &m) || m.type != "proc" ||
        !HasFlag(m.super_flags, "hidepid=2") || !HasFlag(m.super_flags, "gid=3009"))
        return Fail("procfs did not retain the required hidepid=2,gid=3009 options");
    char ns_target[80] {};
    ssize_t ns_len = readlink("/proc/self/ns/mnt", ns_target, sizeof(ns_target)-1);
    if (ns_len <= 5 || std::string(ns_target).compare(0, 5, "mnt:[") != 0)
        return Fail("real mount namespace procfd symlink is unavailable");
    if (!ReadMount("/system", &m) || m.type != "ext4" || !HasFlag(m.flags, "ro") ||
        statvfs("/system", &fs) || !(fs.f_flag & ST_RDONLY))
        return Fail("/system must already be a read-only ext4 image mount");
    // The one source there is: the system image's extent, mapped as dm-1.
    // A loop device was how the RAM-boot mode and the first two entry
    // contracts mounted it; both went on 2026-10-29.
    if (m.source != "/dev/block/dm-1" || Read("/sys/class/block/dm-1/ro") != "1")
        return Fail("/system requires a verified read-only block source");
    if (!CheckTmpfs("/cache", 16ULL << 20) || !CheckAuthentication()) return false;
    if (!CheckTestFileData()) return false;
    for (const char* path : {"/system/build.prop", "/system/bin/app_process32"}) {
        struct stat st {};
        if (Stat(path, &st) || !S_ISREG(st.st_mode) || st.st_size == 0)
            return Fail(std::string("missing real system artifact: ") + path);
    }
    return true;
}
// These are kernel/hardware prerequisites, independent of the data backend.
bool CheckKernelPrerequisites() {
    for (const char* path : {"/dev/binder", "/dev/hwbinder", "/dev/vndbinder", "/dev/ashmem"}) {
        struct stat st {};
        if (Stat(path, &st) || !S_ISCHR(st.st_mode)) return Fail(std::string("missing kernel device: ") + path);
    }
    for (const char* path : {"/sys/power/state", "/sys/power/wakeup_count",
                             "/sys/power/wake_lock", "/sys/power/wake_unlock",
                             "/sys/module/lowmemorykiller/parameters/minfree",
                             "/sys/module/lowmemorykiller/parameters/adj"}) {
        if (Access(path, F_OK)) return Fail(std::string("missing kernel control: ") + path);
    }
    for (clockid_t clock : {CLOCK_REALTIME_ALARM, CLOCK_BOOTTIME_ALARM, CLOCK_BOOTTIME}) {
        struct timespec now {};
        if (clock_gettime(clock, &now)) return Fail("required alarm clock/RTC is unavailable");
        int fd = timerfd_create(clock, TFD_CLOEXEC);
        if (fd < 0) return Fail("required alarm timerfd is unavailable");
        close(fd); // Never armed; no RTC programming or suspend is performed.
    }
    return true;
}
enum class EpdcState { Ready, Retry, Failed };

EpdcState EpdcFailure(const char* operation, int error) {
    Report(std::string("EPDC ") + operation + " failed: errno=" + std::to_string(error));
    return EpdcState::Failed;
}

EpdcState CheckEpdcContract() {
    int fd = Open("/dev/graphics/fb0", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        const int error = errno;
        if (error == ENOENT || error == ENODEV || error == ENXIO || error == EINTR)
            return EpdcState::Retry;
        return EpdcFailure("read-only open", error);
    }
    fb_var_screeninfo screen {};
    pw3::EpdcAndroidInfo info {};
    int screenRc, infoRc;
    do { screenRc = ioctl(fd, FBIOGET_VSCREENINFO, &screen); } while (screenRc < 0 && errno == EINTR);
    if (screenRc < 0) {
        const int error = errno;
        close(fd);
        return EpdcFailure("geometry query", error);
    }
    do { infoRc = ioctl(fd, pw3::GetAndroidInfo, &info); } while (infoRc < 0 && errno == EINTR);
    const int infoError = errno;
    close(fd);
    if (infoRc < 0) {
        if (infoError == EAGAIN) return EpdcState::Retry;
        return EpdcFailure("startup query", infoError);
    }
    if (!pw3::validEpdcContract(info, screen)) {
        std::ostringstream message;
        message << "EPDC contract mismatch: screen=" << screen.xres << 'x' << screen.yres
                << " rotate=" << screen.rotate << " abi=" << info.version
                << " size=" << info.size << " flags=" << info.flags
                << " epdc=" << info.xres << 'x' << info.yres << " rotate=" << info.rotate
                << " mode_version=" << info.modeVersion << " gc16=" << info.gc16Mode
                << " modes=" << info.modeCount << " temperature=" << info.temperatureAuto;
        Report(message.str());
        return EpdcState::Failed;
    }
    return EpdcState::Ready;
}
bool CheckInterfaces() {
    if (!CheckKernelPrerequisites()) return false;
    // Loading modules is owned by the separately verified hardware bundle.
    // Wait only for real nodes and known input names; no synthetic input events.
    bool display=false, power=false, touch=false;
    for (int attempt=0; attempt<100; ++attempt) {
        // This loop is waiting for nodes that are not there yet, so a failure
        // inside it is expected and says nothing. Clearing the record at the
        // top of each attempt leaves the one from the last attempt, which is
        // the one the message below is about; without this, the first attempt's
        // ENOENT would be carried into every later report.
        first_fault = {};
        struct stat fb {};
        display = Stat("/dev/graphics/fb0", &fb)==0 && S_ISCHR(fb.st_mode) &&
            Read("/sys/class/graphics/fb0/name")=="mxc_epdc_fb" &&
            Access("/sys/class/backlight/max77696-bl/brightness",F_OK)==0 &&
            Access("/sys/class/backlight/max77696-bl/bl_power",F_OK)==0;
        if (display) {
            const auto state = CheckEpdcContract();
            if (state == EpdcState::Failed) return false;
            display = state == EpdcState::Ready;
        }
        power=false; touch=false;
        DIR* input=opendir("/sys/class/input");
        if (input) {
            while (auto* entry=readdir(input)) {
                const std::string event(entry->d_name);
                if (event.compare(0,5,"event")!=0) continue;
                const auto name=Read("/sys/class/input/"+event+"/device/name");
                power |= name=="max77696-onkey";
                touch |= name=="cyttsp4_mt";
            }
            closedir(input);
        }
        if (display && power && touch) return true;
        usleep(50000);
    }
    return Fail("real interfaces not ready: display=" + std::to_string(display) +
                " power_key=" + std::to_string(power) + " touch=" + std::to_string(touch) +
                "; framework remains stopped");
}
bool CheckUsb() {
    struct statfs fs {};
    if (!CheckAuthentication())
        return Fail("USB ADB without authentication is not allowed");
    if (StatFs("/dev/usb-ffs/adb", &fs) ||
        static_cast<unsigned long>(fs.f_type) != kFunctionFsMagic ||
        Access("/dev/usb-ffs/adb/ep0", F_OK))
        return Fail("real FunctionFS mount/control endpoint is absent; adbd remains stopped");
    return true;
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::string mode(argv[1]);
    if (mode!="storage" && mode!="usb" && mode!="interfaces") return 2;
    const bool ok = mode=="storage" ? CheckStorage() : mode=="usb" ? CheckUsb() : CheckInterfaces();
    if (!ok) { android::base::SetProperty("sys.pw3.init_error",mode); return 1; }
    const char* ready = mode=="storage" ? "sys.pw3.storage_ready" :
        mode=="usb" ? "sys.pw3.ffs_ready" : "sys.pw3.hardware_ready";
    if (!android::base::SetProperty(ready, "1")) { Report("cannot publish verified initialization state"); return 1; }
    Report("interface-presence contract verified for "+mode+"; hardware behavior remains untested");
    return 0;
}
