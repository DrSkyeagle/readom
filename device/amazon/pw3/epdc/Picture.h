// SPDX-License-Identifier: Apache-2.0
// Picture files for the panel: binary PGM (P5), 8-bit grey, the size of the
// screen. PGM is the plainest grey picture format there is - a short text
// header, then the rows - so a picture is read row by row, with no decoder
// and without memory for all of it.
#pragma once
#include <cstdio>

namespace pw3 {
// Opens `path` and reads its header. Returns the file positioned at the first
// row, or null unless it is an 8-bit binary PGM of exactly width x height
// pixels: the header, then width * height bytes and nothing else.
FILE* openPicture(const char* path, int width, int height);
// The same for a picture of any size, which is returned in *width, *height.
FILE* openPicture(const char* path, int* width, int* height);
}  // namespace pw3
