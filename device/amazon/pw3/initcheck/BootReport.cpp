// SPDX-License-Identifier: Apache-2.0
#include "../epdc/EpdcAbi.h"
#include "../report/ReportArea.h"
#include <android-base/properties.h>
#include <log/log.h>
#include <log/log_read.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <sstream>

namespace {
volatile sig_atomic_t output = -1;
size_t remaining = 12000;

// The report area, when this start mapped one (device/amazon/pw3/report).
// Every line this program produces goes there, and nowhere else: the next tap
// on the Kindle entry copies the area into the folder a reader can send. There
// was a second sink until 2026-10-29 -- the kernel log, which a boot that asked
// for it with androidboot.pw3.diag read afterwards -- and that key went with
// the test entry, so `output` below is never opened and stays -1. One
// collection, because a second program sampling the same things on the same
// single core would cost what it measures, and because two programs would
// drift into saying different things about one boot.
const char* const kAreaPath = "/dev/block/dm-3";
char areaText[PW3_REPORT_BYTES];
size_t areaUsed = 0;
bool toArea = false;
// One line to the kernel log whatever kind of start this is. Until 2026-10-21
// everything this program had to say went through `output`, which was only
// open on a start that asked for a diagnostic -- so on an ordinary start the
// report area could fail to be written and the log said nothing at all. Every
// such line now comes through here, which opens /dev/kmsg for itself
// (pw3_boot_report.te, kmsg_device:chr_file { open write }). The diagnostic
// start itself went with the test entry on 2026-10-29.
//
// Under its own tag, and not this program's. The entry on the stock side keeps
// `pw3-` lines tagged `pw3-boot-report` out of the folder a reader hands on
// (boot/launcher/userlog.c, DENY), and that filter is a whitelist: a tag is let
// through only by being named. `pw3-boot-report` is not named, and after
// 2026-10-29 it reaches no kernel log at all -- the sink it used to write to
// went with androidboot.pw3.diag. Everything printed here is this program's own
// fixed text plus a path of ours and strerror(), so it belongs on the other
// side of that line. Tagging it `pw3-boot-report` meant the reasons added here
// on 2026-10-21 were collected and then dropped, which is the gap they were
// added to close.
void KernelLine(const std::string& text) {
    const std::string line = "<3>pw3-report-area: " + text + "\n";
    if (output >= 0) { (void)write(output, line.data(), line.size()); return; }
    const int fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (fd < 0) return;
    (void)write(fd, line.data(), line.size());
    close(fd);
}
// The text cannot fill the area: the first line declares its length, its
// checksum and which start wrote it, and that line is part of the 64 KiB.
constexpr size_t kAreaTextMax = PW3_REPORT_BYTES - PW3_REPORT_HEADER_MAX;

std::string Clean(const std::string& input, size_t limit) {
    std::string result;
    for (const unsigned char c : input) {
        if (result.size() == limit) break;
        result.push_back(c >= 32 && c < 127 ? char(c) : ' ');
    }
    return result;
}
void Report(const std::string& message) {
    const std::string text = Clean(message, 640);
    if (toArea && areaUsed + text.size() + 1 <= kAreaTextMax) {
        memcpy(areaText + areaUsed, text.data(), text.size());
        areaUsed += text.size();
        areaText[areaUsed++] = '\n';
    }
    const std::string line = "<3>pw3-boot-report: " + text + "\n";
    if (output < 0 || line.size() > remaining) return;
    remaining -= line.size();
    // /dev/kmsg accepts one complete record per write. Do not retry a partial
    // write as a new record or allow diagnostic logging to run without a bound.
    ssize_t count;
    do { count = write(output, line.data(), line.size()); } while (count < 0 && errno == EINTR);
}
// A round that blocks -- a sysfs read that does not return, logd that does not
// answer -- must not hold the start for ever, and that is what the alarm is
// for. It used to end the process, which was right while the kernel log was
// the only sink: a line had already gone out for every sample taken. With the
// area there is nothing on the owner's store until the end, and a boot that
// hung is the boot whose report matters most, so the alarm now returns to the
// loop instead: what was collected is written, and that it was cut short is
// part of what it says. siglongjmp is what a handler may call; the logger list
// Errors() may have been holding is abandoned, and the process exits next.
sigjmp_buf deadline;
volatile sig_atomic_t deadline_hit = 0;
void Deadline(int) {
    constexpr char line[] = "<3>pw3-boot-report: snapshot deadline reached; diagnostic stopped\n";
    if (output >= 0) (void)write(output, line, sizeof(line)-1);
    deadline_hit = 1;
    siglongjmp(deadline, 1);
}
std::string Read(const std::string& path) {
    char buffer[4096];
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return {};
    ssize_t n;
    do { n = read(fd, buffer, sizeof(buffer)-1); } while (n < 0 && errno == EINTR);
    close(fd);
    return n > 0 ? std::string(buffer, size_t(n)) : std::string();
}
void Properties(unsigned round) {
    constexpr const char* names[] = {
        // Which release wrote this. A report reaches whoever reads it without
        // the boot it came from, and every other line means something else
        // depending on the answer.
        "ro.build.version.incremental",
        "sys.pw3.storage_ready", "sys.pw3.hardware_ready", "sys.pw3.init_error",
        "sys.pw3.usb.phase", "sys.pw3.ffs_ready", "sys.usb.ffs.ready", "sys.usb.state",
        "init.svc.adbd", "init.svc.zygote", "init.svc.surfaceflinger", "init.svc.bootanim",
        "init.svc.pw3-fg-observe", "sys.boot_completed", "dev.bootcomplete", "service.bootanim.exit"
    };
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    Report("snapshot=" + std::to_string(round) + " uptime_s=" + std::to_string(now.tv_sec));
    for (const auto name : names) {
        Report(std::string(name) + "=" + Clean(android::base::GetProperty(name, "unset"), 40));
    }
}
void Memory() {
    std::istringstream input(Read("/proc/meminfo"));
    std::string line, summary = "memory_kib";
    while (std::getline(input, line)) {
        char key[40]; unsigned long value;
        if (sscanf(line.c_str(), "%39s %lu", key, &value) != 2) continue;
        if (!strcmp(key,"MemFree:") || !strcmp(key,"Cached:") || !strcmp(key,"Buffers:") ||
            !strcmp(key,"AnonPages:") || !strcmp(key,"Slab:") || !strcmp(key,"SwapFree:"))
            summary += " " + std::string(key) + std::to_string(value);
    }
    Report(summary);
    const auto buddy = Read("/proc/buddyinfo");
    if (!buddy.empty()) Report("buddy " + Clean(buddy, 360));
}
void Inputs() {
    DIR* directory = opendir("/sys/class/input");
    bool power = false, touch = false; unsigned checked = 0;
    if (directory) {
        while (auto* entry = readdir(directory)) {
            const std::string name(entry->d_name);
            if (name.compare(0,5,"event") || name.size() < 6 ||
                name.find_first_not_of("0123456789",5) != std::string::npos) continue;
            if (++checked > 32) break;
            const auto label = Read("/sys/class/input/" + name + "/device/name");
            power |= label == "max77696-onkey\n";
            touch |= label == "cyttsp4_mt\n";
        }
        closedir(directory);
    }
    Report("input_nodes power=" + std::to_string(power) + " touch=" + std::to_string(touch));
}
void Framebuffer() {
    const int fd = open("/dev/graphics/fb0", O_RDONLY | O_CLOEXEC);
    if (fd < 0) { Report("fb_open_errno=" + std::to_string(errno)); return; }
    fb_var_screeninfo screen{}; fb_fix_screeninfo fixed{}; pw3::EpdcAndroidInfo info{};
    if (ioctl(fd, FBIOGET_VSCREENINFO, &screen) || ioctl(fd, FBIOGET_FSCREENINFO, &fixed)) {
        const int error = errno; close(fd); Report("fb_geometry_errno=" + std::to_string(error)); return;
    }
    std::ostringstream geometry;
    geometry << "fb " << screen.xres << 'x' << screen.yres << " virtual="
             << screen.xres_virtual << 'x' << screen.yres_virtual << " bpp=" << screen.bits_per_pixel
             << " gray=" << screen.grayscale << " rotate=" << screen.rotate << " stride=" << fixed.line_length
             << " offset=" << screen.xoffset << ',' << screen.yoffset << " bytes=" << fixed.smem_len;
    Report(geometry.str());
    if (ioctl(fd, pw3::GetAndroidInfo, &info)) Report("epdc_query_errno=" + std::to_string(errno));
    else Report("epdc flags=" + std::to_string(info.flags) + " mode_version=" + std::to_string(info.modeVersion) +
                " gc16=" + std::to_string(info.gc16Mode) + " modes=" + std::to_string(info.modeCount));
    // Sample 16 visible rows without modifying framebuffer memory or issuing
    // display updates. Concurrent compositor writes can make this sample torn.
    if (screen.bits_per_pixel == 8 && screen.xres > 0 && screen.xres <= 4096 &&
        screen.yres > 0 && screen.yres <= 4096 &&
        uint64_t(screen.xoffset) + screen.xres <= fixed.line_length &&
        (uint64_t(screen.yoffset) + screen.yres) * fixed.line_length <= fixed.smem_len) {
        uint8_t pixels[4096]; bool values[256]{}; unsigned minimum=255,maximum=0,rows=0,distinct=0;
        uint32_t hash=2166136261U;
        for (unsigned i=0;i<16;++i) {
            const uint64_t row=uint64_t(i)*(screen.yres-1)/15;
            const uint64_t offset=(row+screen.yoffset)*fixed.line_length+screen.xoffset;
            if (offset > INT32_MAX) break;
            const ssize_t n=pread(fd,pixels,screen.xres,off_t(offset));
            if (n != ssize_t(screen.xres)) { Report("fb_sample_errno="+std::to_string(n<0?errno:EIO));break; }
            ++rows;
            for (ssize_t j=0;j<n;++j) {
                const unsigned value=pixels[j];values[value]=true;
                if(value<minimum)minimum=value;if(value>maximum)maximum=value;
                hash=(hash^value)*16777619U;
            }
        }
        for (const bool present:values) distinct+=present;
        Report("fb_sample rows="+std::to_string(rows)+" min="+std::to_string(minimum)+
               " max="+std::to_string(maximum)+" levels="+std::to_string(distinct)+" hash="+std::to_string(hash));
    }
    close(fd);
}
bool Relevant(const std::string& tag) {
    constexpr const char* tags[] = {"SurfaceFlinger","FramebufferSurface","HWComposer","libEGL","gralloc","PW3Gralloc",
        "OpenGLRenderer","AndroidRuntime","Zygote","zygote","art","ActivityManager","pw3-fg-state"};
    for (const auto item:tags) if (tag==item) return true;
    return false;
}
// How many warnings and errors each component logged, and nothing of what they
// said. What an application writes to the log is the application's and often
// the owner's -- a package name, a path, a URL, a file they opened -- and this
// report is written to the user store for the owner to attach to a problem
// report, where it becomes public. Counting says the same thing a reader needs
// ("SurfaceFlinger logged three errors") without carrying any of it. Until
// 2026-10-29 this copied up to four whole lines.
void Errors() {
    auto* list=android_logger_list_alloc(ANDROID_LOG_RDONLY|ANDROID_LOG_NONBLOCK,200,0);
    if (!list) {Report("log_reader_alloc_failed");return;}
    android_logger_open(list,LOG_ID_MAIN);android_logger_open(list,LOG_ID_SYSTEM);android_logger_open(list,LOG_ID_CRASH);
    std::map<std::string,unsigned> errors, status;
    for (unsigned count=0;count<600;++count) {
        log_msg message{};const int rc=android_logger_list_read(list,&message);
        if (rc<=0) {if(rc!=-EAGAIN && rc!=0)Report("log_read_result="+std::to_string(rc));break;}
        const size_t header=message.entry.hdr_size?message.entry.hdr_size:sizeof(logger_entry);
        const size_t length=message.entry.len;
        if (header>sizeof(message.buf)||length<3||length>sizeof(message.buf)-header||header+length>size_t(rc))continue;
        const auto* payload=reinterpret_cast<const char*>(message.buf+header);
        const unsigned priority=static_cast<unsigned char>(payload[0]);
        const auto* tagEnd=static_cast<const char*>(memchr(payload+1,0,length-1));
        if (!tagEnd)continue;
        // The tag is matched against a fixed list, so the name in the report is
        // one of those literals and never a string the device supplied.
        const std::string tag(payload+1,size_t(tagEnd-payload-1));if(!Relevant(tag))continue;
        if (priority>=ANDROID_LOG_WARN) ++errors[tag];
        else if (tag=="PW3Gralloc" && priority==ANDROID_LOG_INFO) ++status[tag];
    }
    android_logger_list_free(list);
    for (const auto* pair : {&errors, &status}) {
        if (pair->empty()) continue;
        std::string line;
        for (const auto& item : *pair)
            line += (line.empty() ? "" : " ") + item.first + "=" + std::to_string(item.second);
        Report((pair==&errors ? "android_error_count " : "android_status_count ") + line);
    }
}
// Which Android start this is, as the area itself records it: the number in
// the report that is already there, carried on by pw3_report_boot_after. It is
// read with the same function the launcher reads the area with, so a report
// this cannot parse -- an area of zeros, a half-written one -- is start 1
// rather than a guess.
uint32_t PreviousBoot() {
    static unsigned char area[PW3_REPORT_BYTES];
    const int fd = open(kAreaPath, O_RDONLY | O_CLOEXEC);
    // An area that could not be read is said so in the kernel log and then
    // counted as none, which makes this start 1.
    //
    // It used to return UINT32_MAX here and the caller wrote that number into
    // the report, as "unknown". That put a sentinel in a counter: from then on
    // every report carried UINT32_MAX, and the stock side -- which acts on "the
    // count is not the one I accounted for" -- would never act on this device
    // again (installer/install/install.c; pw3_report_boot_after in
    // ReportArea.h). One open() that failed was enough to do that for good.
    // Being wrong about the number costs one report read as already-seen; not
    // changing the number costs all of them.
    if (fd < 0) {
        KernelLine(std::string("cannot read the previous report count: ") + strerror(errno));
        return 0;
    }
    size_t got = 0;
    while (got < sizeof(area)) {
        const ssize_t n = read(fd, area + got, sizeof(area) - got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        got += size_t(n);
    }
    close(fd);
    pw3_report previous{};
    return pw3_report_parse(area, got, &previous) ? previous.boot : 0;
}

// One write of the whole area, built by the format's own function
// (pw3_report_build). It is a block device of exactly 64 KiB mapped from the
// clusters of a file on the owner's store, so what is written is all of it.
void SaveArea() {
    static unsigned char area[PW3_REPORT_BYTES];
    const uint32_t boot = pw3_report_boot_after(PreviousBoot());
    // Said in the kernel log, not in the area: an area that was not written
    // cannot carry the news that it was not written. Every way this can fail
    // says so, and says which one it was -- a silent return here is what let
    // the whole thing be broken on every start without anybody seeing it.
    if (!pw3_report_build(area, sizeof(area), areaText, areaUsed, boot)) {
        KernelLine("report area not built boot=" + std::to_string(boot) +
                   " bytes=" + std::to_string(areaUsed));
        return;
    }
    const int fd = open(kAreaPath, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        KernelLine(std::string("report area cannot be opened for writing: ") + strerror(errno));
        return;
    }
    size_t written = 0;
    int why = 0;
    while (written < sizeof(area)) {
        const ssize_t n = write(fd, area + written, sizeof(area) - written);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { why = errno; break; }
        if (n == 0) break;
        written += size_t(n);
    }
    const bool synced = fsync(fd) == 0;
    if (!synced && !why) why = errno;
    const bool complete = written == sizeof(area) && synced;
    close(fd);
    KernelLine(std::string("report area ") + (complete ? "written" : "incomplete") +
               " boot=" + std::to_string(boot) + " bytes=" + std::to_string(areaUsed) +
               (complete ? "" : " at=" + std::to_string(written) +
                               (why ? std::string(" errno=") + strerror(why) : std::string())));
}
} // namespace

int main(int argc,char** argv) {
    const bool once=argc==2 && !strcmp(argv[1],"--once");
    if (argc!=1 && !once) return 2;
    // One sink, open when there is something to open: the area, which every
    // boot has when the installation does -- an installation from before it
    // existed has no dm-3, and neither has one whose store had no room to spare
    // for it. Nothing to write to is not a failure: it is a release of a device
    // whose owner has no diagnostic to send, which is the state this ran in
    // before the area existed. (The kernel-log sink went on 2026-10-29 with the
    // key that selected it; the write path behind `output` is kept because
    // KernelLine() still uses the same bound and record rules.)
    struct stat area{};
    const bool found = stat(kAreaPath,&area)==0;
    const int why = found ? 0 : errno;
    toArea = found && S_ISBLK(area.st_mode);
    // Absent is normal and silent: an installation from before the area existed,
    // or one whose store had no room for it, has no dm-3. Anything else is not.
    // On 2026-10-21 the difference was the whole bug: this domain had no rule for
    // the directory the node sits in, so the stat failed with EACCES, which read
    // exactly like "this device has no area", and the service exited 0 on every
    // start without writing a byte. One line in the kernel log -- the only
    // channel open to a start that is not a diagnostic one -- so that a denial
    // cannot go on looking like an absence.
    if (!toArea && why != ENOENT)
        KernelLine(std::string(kAreaPath) + " unusable: " +
                   (found ? "not a block device" : strerror(why)));
    // The report area is the one channel there is. The second one, a start
    // that asked for androidboot.pw3.diag and got the whole report in the
    // kernel log, went with the test entry on 2026-10-29: nothing sets that
    // key any more.
    if (!toArea) return 0;
    struct sigaction action{};action.sa_handler=Deadline;sigemptyset(&action.sa_mask);
    if(sigaction(SIGALRM,&action,nullptr))return 1;
    constexpr unsigned delays[]={0,30,30,60};
    for(unsigned round=0;round<(once?1U:4U) && !deadline_hit;++round) {
        sleep(delays[round]);
        if (sigsetjmp(deadline,1)) break;
        alarm(5);
        Properties(round);Memory();Inputs();Framebuffer();Errors();
        alarm(0);
    }
    alarm(0);
    Report(deadline_hit ? "stopped at the snapshot deadline" : "complete");
    if (toArea) SaveArea();
    if (output>=0) close(output);
    return deadline_hit ? 124 : 0;
}
