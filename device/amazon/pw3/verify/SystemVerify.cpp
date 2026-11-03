// SPDX-License-Identifier: Apache-2.0
//
// Reads the system image once Android is up, on the starts that did not read
// it. preinit/SystemRecheck.h says why there are such starts and what has to
// happen about a mismatch; this is the reading.
//
// It runs at the lowest scheduling and I/O priority (pw3-system-verify.rc) and
// drops every block it reads from the page cache: 511 MB of RAM has no room
// for 512 MiB of image, and the reader is the one using it. It also waits out
// the first minute and a half, which on this device is the launcher being
// drawn and the first application starting.
//
// Nothing here writes anything, except that a mismatch deletes the record of
// the last complete check. The next start then reads the image itself, finds
// the same mismatch and refuses to boot it, which is the path the installer's
// "damaged system image" screen describes.

#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <memory>
#include <string>

#include <openssl/sha.h>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <android-base/unique_fd.h>

#include "../preinit/SystemRecheck.h"

namespace {

// Read and hashed at a time: small enough that dropping it from the page cache
// leaves the reader's own pages alone, large enough for the read to be
// sequential.
constexpr size_t kChunk = 256 * 1024;
// The default wait before the first read. The launcher is drawn and the first
// application started well inside it (Test V55: the launcher at 97 s of a
// 181 s start, counted from the kernel).
constexpr unsigned kQuietSeconds = 90;
// Where the result is published, for anything that wants to ask.
constexpr char kProperty[] = "sys.pw3.system_verified";

struct Options {
    const char* recheck = PW3_SYSTEM_RECHECK_PATH;
    const char* stamp = PW3_SYSTEM_STAMP_PATH;
    const char* mounts = "/proc/mounts";
    unsigned quiet_seconds = kQuietSeconds;
};

// Whether /system is mounted read-only from the device the record names. A
// record that named something else -- a path that has come to belong to
// another device -- would otherwise send this reading somewhere it must
// mismatch, and a mismatch deletes a record that was good.
bool SystemIsMountedFrom(const char* mounts_path, const char* device) {
    std::string mounts;
    if (!android::base::ReadFileToString(mounts_path, &mounts)) {
        PLOG(ERROR) << "cannot read " << mounts_path;
        return false;
    }
    for (size_t at = 0; at < mounts.size();) {
        size_t end = mounts.find('\n', at);
        if (end == std::string::npos) end = mounts.size();
        const std::string line = mounts.substr(at, end - at);
        at = end + 1;
        char source[256], point[256], type[64], options[512];
        if (sscanf(line.c_str(), "%255s %255s %63s %511s", source, point, type, options) != 4) {
            continue;
        }
        if (strcmp(point, "/system")) continue;
        if (strcmp(source, device)) {
            LOG(ERROR) << "/system is mounted from " << source << ", not from " << device;
            return false;
        }
        // "ro" as a whole option, never as the beginning of another one.
        for (char* option = strtok(options, ","); option; option = strtok(nullptr, ",")) {
            if (!strcmp(option, "ro")) return true;
        }
        LOG(ERROR) << "/system is not mounted read-only";
        return false;
    }
    LOG(ERROR) << "nothing is mounted on /system";
    return false;
}

// Every byte of the device, in order, each block dropped from the page cache
// once it has been hashed.
bool HashDevice(const char* path, uint64_t size, unsigned char out[SHA256_DIGEST_LENGTH]) {
    android::base::unique_fd fd(open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (fd.get() < 0) {
        PLOG(ERROR) << "cannot open " << path;
        return false;
    }
    struct stat status;
    uint64_t bytes = 0;
    int readonly = 0;
    if (fstat(fd.get(), &status) || !S_ISBLK(status.st_mode) ||
        ioctl(fd.get(), BLKGETSIZE64, &bytes) || ioctl(fd.get(), BLKROGET, &readonly) ||
        readonly != 1) {
        PLOG(ERROR) << path << " is not a read-only block device";
        return false;
    }
    if (bytes != size) {
        LOG(ERROR) << path << " holds " << bytes << " bytes; the record says " << size;
        return false;
    }
    std::unique_ptr<unsigned char[]> buffer(new unsigned char[kChunk]);
    SHA256_CTX context;
    SHA256_Init(&context);
    for (uint64_t at = 0; at < size;) {
        const uint64_t left = size - at;
        const size_t want = left < kChunk ? static_cast<size_t>(left) : kChunk;
        const ssize_t got =
            TEMP_FAILURE_RETRY(pread(fd.get(), buffer.get(), want, static_cast<off_t>(at)));
        if (got <= 0) {
            PLOG(ERROR) << "reading " << path << " stopped at " << at;
            return false;
        }
        SHA256_Update(&context, buffer.get(), static_cast<size_t>(got));
        posix_fadvise(fd.get(), static_cast<off_t>(at), got, POSIX_FADV_DONTNEED);
        at += static_cast<uint64_t>(got);
    }
    SHA256_Final(out, &context);
    return true;
}

int Verify(const Options& options) {
    // Absent means the start read the image itself: nothing to do, and nothing
    // to say. Any other failure means this cannot tell whether there is a check
    // to make, which is not the same thing -- and until 2026-10-21 both
    // returned 0, so a denial read as "the start already did it" and the image
    // went unverified with the service reporting success.
    if (access(options.recheck, F_OK)) {
        if (errno == ENOENT) return 0;
        PLOG(ERROR) << "cannot tell whether " << options.recheck
                    << " is there, so cannot tell whether the image still needs reading";
        return 1;
    }
    std::string line;
    if (!android::base::ReadFileToString(options.recheck, &line)) {
        PLOG(ERROR) << "cannot read " << options.recheck;
        return 1;
    }
    struct pw3_system_recheck record;
    if (!pw3_system_recheck_parse(line.c_str(), &record)) {
        LOG(ERROR) << options.recheck << " is not a record this understands";
        return 1;
    }
    if (options.quiet_seconds) sleep(options.quiet_seconds);
    if (!SystemIsMountedFrom(options.mounts, record.device)) return 1;

    struct timespec started, finished;
    clock_gettime(CLOCK_MONOTONIC, &started);
    unsigned char digest[SHA256_DIGEST_LENGTH];
    if (!HashDevice(record.device, record.size, digest)) return 1;
    clock_gettime(CLOCK_MONOTONIC, &finished);
    const double seconds = static_cast<double>(finished.tv_sec - started.tv_sec) +
                           static_cast<double>(finished.tv_nsec - started.tv_nsec) / 1e9;

    if (!memcmp(digest, record.sha, sizeof(digest))) {
        LOG(INFO) << android::base::StringPrintf(
            "the system image is what it was verified to be (%llu bytes read in %.1f s)",
            static_cast<unsigned long long>(record.size), seconds);
        android::base::SetProperty(kProperty, "1");
        return 0;
    }
    // Said before anything is touched: this is the one line that explains a
    // device which refuses to start Android at the next attempt.
    LOG(ERROR) << "the system image is not what it was verified to be; deleting "
               << options.stamp << " so that the next start reads it and refuses it";
    android::base::SetProperty(kProperty, "0");
    if (unlink(options.stamp) && errno != ENOENT) {
        PLOG(ERROR) << "cannot delete " << options.stamp
                    << "; the next start would accept the image again";
        return 1;
    }
    // The deletion is the whole of what this does about a bad image, and a
    // power loss before ext4 commits the directory would undo it and leave
    // the next start trusting the record again. The start syncs the
    // directory when it writes the record; this syncs it when it takes it
    // away. (The directory is /data/pw3, the path up to the last separator.)
    const std::string stamp(options.stamp);
    const size_t separator = stamp.rfind('/');
    if (separator != std::string::npos && separator > 0) {
        const std::string directory = stamp.substr(0, separator);
        android::base::unique_fd fd(open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        if (fd.get() < 0 || TEMP_FAILURE_RETRY(fsync(fd.get()))) {
            PLOG(ERROR) << "cannot sync " << directory
                        << "; the deletion may not survive a power loss";
        }
    }
    return 1;
}

}  // namespace

// init starts this with no arguments. The paths and the wait are arguments so
// that the device test can run every branch of it against files of its own
// (tools/verify-device.sh).
int main(int argc, char** argv) {
    android::base::InitLogging(argv);
    Options options;
    for (int i = 1; i < argc; ++i) {
        const bool has_value = i + 1 < argc;
        if (has_value && !strcmp(argv[i], "--recheck")) {
            options.recheck = argv[++i];
        } else if (has_value && !strcmp(argv[i], "--stamp")) {
            options.stamp = argv[++i];
        } else if (has_value && !strcmp(argv[i], "--mounts")) {
            options.mounts = argv[++i];
        } else if (has_value && !strcmp(argv[i], "--quiet-seconds")) {
            options.quiet_seconds = static_cast<unsigned>(strtoul(argv[++i], nullptr, 10));
        } else {
            LOG(ERROR) << "unknown argument: " << argv[i];
            return 2;
        }
    }
    return Verify(options);
}
