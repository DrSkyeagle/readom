// SPDX-License-Identifier: Apache-2.0
#include "Png.h"
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

// stb_image (v2.30, https://github.com/nothings/stb) is public domain -- the
// licence is in sleep/third_party/stb_image.h, and this is the only translation
// unit that compiles it. PNG only: the folder takes pictures a user chose, not
// any format the library happens to know. No stdio: the file is read here so
// that its size can be checked before the decoder is given it.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
// The library is not this project's code and is not written to compile under
// -Wall -Wextra -Werror, which every test of this project does. What it may
// warn about is silenced here, around the include, rather than by building the
// picture path without the flags the rest of the project is held to.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#include "stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace pw3 {
bool decodePng(const uint8_t* data, size_t size, Png* out) {
    if (!data || !size || !out) return false;
    out->width = out->height = 0;
    out->rgba.clear();
    int width = 0, height = 0, channels = 0;
    // The header first: a file that claims to be larger than this device can
    // hold is refused before stb_image is asked to allocate a picture for it.
    if (!stbi_info_from_memory(data, int(size), &width, &height, &channels)) return false;
    if (width <= 0 || height <= 0 || width > PngMaxSide || height > PngMaxSide) return false;
    if (static_cast<long long>(width) * height > PngMaxPixels) return false;
    bool failed = false;
    stbi_uc* pixels = stbi_load_from_memory(data, int(size), &width, &height, &channels, 4);
    if (!pixels) return false;
    try {
        out->rgba.assign(pixels, pixels + size_t(width) * size_t(height) * 4);
    } catch (...) {
        failed = true;
    }
    stbi_image_free(pixels);
    if (failed) return false;
    out->width = width;
    out->height = height;
    return true;
}
bool loadPng(const char* path, Png* out) {
    if (!out) return false;
    out->width = out->height = 0;
    out->rgba.clear();
    if (!path) return false;
    FILE* file = std::fopen(path, "rbe");
    if (!file) return false;
    struct stat st{};
    if (fstat(fileno(file), &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > PngMaxBytes) {
        std::fclose(file);
        return false;
    }
    std::vector<uint8_t> bytes;
    bool ok = false;
    try {
        bytes.resize(size_t(st.st_size));
        ok = std::fread(bytes.data(), 1, bytes.size(), file) == bytes.size();
    } catch (...) {
        ok = false;
    }
    std::fclose(file);
    return ok && decodePng(bytes.data(), bytes.size(), out);
}
}  // namespace pw3
