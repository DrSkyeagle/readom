// SPDX-License-Identifier: Apache-2.0
#include "FgState.h"

#include <cerrno>
#include <cstring>
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace pw3fg {
namespace {

Result Good(const char* reason) { return {true, reason, 0}; }
Result Bad(const char* reason, int error = 0) { return {false, reason, error}; }

class Fd {
public:
    explicit Fd(int value) : value_(value) {}
    ~Fd() { if (value_ >= 0) close(value_); }
    int get() const { return value_; }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
private:
    int value_;
};

uint32_t Get32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

// Reads up to capacity, then checks EOF. Extra bytes and short reads are errors;
// EINTR is retried. The output object is never changed on failure.
Result ReadExact(int fd, uint8_t* bytes, size_t size) {
    size_t used = 0;
    while (used < size) {
        const ssize_t count = read(fd, bytes + used, size - used);
        if (count < 0) {
            if (errno == EINTR) continue;
            return Bad("read_failed", errno);
        }
        if (count == 0) return Bad("short_read");
        used += size_t(count);
    }
    uint8_t extra = 0;
    for (;;) {
        const ssize_t count = read(fd, &extra, 1);
        if (count < 0) {
            if (errno == EINTR) continue;
            return Bad("read_failed", errno);
        }
        return count == 0 ? Good("read_complete") : Bad("oversized_read");
    }
}





}  // namespace

Result ParseSnapshot(const uint8_t* bytes, size_t size, Snapshot* out) {
    if (size != kSnapshotSize) return Bad("snapshot_size_invalid");
    if (std::memcmp(bytes, "FGS1", 4) != 0) return Bad("snapshot_magic_invalid");
    for (unsigned i = 4; i != 8; ++i)
        if (bytes[i] > 1) return Bad("snapshot_flag_invalid");
    const unsigned unused[] = {0, 8, 9};
    for (unsigned slot : unused)
        if (bytes[12 + slot * 2] != 0 || bytes[13 + slot * 2] != 0)
            return Bad("snapshot_reserved_invalid");
    Snapshot parsed {};
    std::memcpy(parsed.data(), bytes, parsed.size());
    *out = parsed;
    return Good("snapshot_valid");
}

Result ReadSnapshot(const std::string& path, Snapshot* out) {
    Fd fd(open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
    if (fd.get() < 0) return Bad("snapshot_open_failed", errno);
    struct stat st {};
    if (fstat(fd.get(), &st) != 0) return Bad("snapshot_stat_failed", errno);
    if (!S_ISREG(st.st_mode)) return Bad("snapshot_not_regular_file");
    Snapshot bytes {};
    Result read = ReadExact(fd.get(), bytes.data(), bytes.size());
    if (!read.ok) return read;
    return ParseSnapshot(bytes.data(), bytes.size(), out);
}

Result MaySave(const Snapshot& snapshot) {
    Snapshot parsed {};
    Result valid = ParseSnapshot(snapshot.data(), snapshot.size(), &parsed);
    if (!valid.ok) return valid;
    if (parsed[4] == 0) return Bad("gauge_not_initialized");
    if (parsed[5] != 0) return Bad("restore_pending");
    if (parsed[6] != 0) return Bad("hardware_por_set");
    if (parsed[7] == 0) return Bad("battery_invalid");
    if (Get32(parsed.data() + 8) != 0) return Bad("battery_error");
    return Good("save_eligible");
}





}  // namespace pw3fg
