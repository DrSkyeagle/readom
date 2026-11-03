// SPDX-License-Identifier: Apache-2.0
#include "Picture.h"
#include <cctype>
#include <sys/stat.h>

namespace pw3 {
namespace {
// The next header number: whitespace and comments (# to the end of the line) before it.
bool number(FILE* f, long* out) {
    int c = std::fgetc(f);
    for (;;) {
        if (c == '#') { do c = std::fgetc(f); while (c != '\n' && c != EOF); }
        else if (std::isspace(c)) c = std::fgetc(f);
        else break;
    }
    if (!std::isdigit(c)) return false;
    long value = 0;
    for (; std::isdigit(c); c = std::fgetc(f)) {
        value = value * 10 + (c - '0');
        if (value > 65535) return false;
    }
    // Exactly one whitespace character ends a number; after the last one the rows begin.
    if (!std::isspace(c)) return false;
    *out = value;
    return true;
}
}  // namespace

FILE* openPicture(const char* path, int* width, int* height) {
    FILE* f = std::fopen(path, "rbe");
    if (!f) return nullptr;
    long w = 0, h = 0, max = 0;
    struct stat st{};
    const bool valid = std::fgetc(f) == 'P' && std::fgetc(f) == '5' && number(f, &w) && number(f, &h) &&
                       number(f, &max) && w > 0 && h > 0 && max == 255 && !fstat(fileno(f), &st) &&
                       st.st_size == std::ftell(f) + w * h;
    if (!valid) { std::fclose(f); return nullptr; }
    *width = int(w);
    *height = int(h);
    return f;
}
FILE* openPicture(const char* path, int width, int height) {
    int w = 0, h = 0;
    FILE* f = openPicture(path, &w, &h);
    if (f && (w != width || h != height)) { std::fclose(f); return nullptr; }
    return f;
}
}  // namespace pw3
