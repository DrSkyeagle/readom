// SPDX-License-Identifier: Apache-2.0
#include "PosixChecks.h"
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <limits.h>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace pw3 {
bool ReadSmallFile(const std::string& path, size_t maximum, std::string* out) {
    int fd;
    do { fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW); } while (fd < 0 && errno == EINTR);
    if (fd < 0) return false;
    std::string result;
    char buf[256];
    bool ok = true;
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { ok = false; break; }
        if (!n) break;
        if (static_cast<size_t>(n) > maximum - result.size()) { ok = false; break; }
        result.append(buf, static_cast<size_t>(n));
    }
    close(fd);
    if (ok) *out = result;
    return ok;
}
Presence DirectoryPresence(const std::string& path) {
    struct stat st;
    if (lstat(path.c_str(), &st) == 0)
        return S_ISDIR(st.st_mode) ? Presence::Present : Presence::Error;
    return errno == ENOENT ? Presence::Absent : Presence::Error;
}
bool ExactBinding(const std::string& link, const std::string& expected) {
    struct stat st;
    if (lstat(link.c_str(), &st) != 0 || !S_ISLNK(st.st_mode)) return false;
    char actual[PATH_MAX], target[PATH_MAX];
    return realpath(link.c_str(), actual) && realpath(expected.c_str(), target) &&
           std::string(actual) == target;
}
bool ContainsFunctionFs(const std::string& text) {
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream words(line);
        std::string first, second, extra;
        if (words >> first >> second && !(words >> extra) &&
            first == "nodev" && second == "functionfs") return true;
    }
    return false;
}
bool ReadRandomSerial(const std::string& path, std::string* serial) {
    int fd;
    do { fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW); } while (fd < 0 && errno == EINTR);
    if (fd < 0) return false;
    unsigned char bytes[16];
    size_t used = 0;
    while (used < sizeof(bytes)) {
        ssize_t n = read(fd, bytes + used, sizeof(bytes) - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(fd); return false; }
        used += static_cast<size_t>(n);
    }
    close(fd);
    static const char hex[] = "0123456789abcdef";
    std::string value;
    value.reserve(32);
    for (unsigned char b : bytes) { value += hex[b >> 4]; value += hex[b & 15]; }
    if (!ValidSerial(value)) return false;
    *serial = value;
    return true;
}
}
