// SPDX-License-Identifier: Apache-2.0
// Picture files (Picture.h): only an 8-bit binary PGM of the screen's size opens.
#include "../Picture.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

static int failures;
#define CHECK(x) do { if (!(x)) { std::printf("FAILED line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

namespace {
std::string directory;
// Writes `header` and `rows` bytes 0, 1, 2, ... to a file; opens it as a 7 x 3 picture.
FILE* open(const std::string& header, long rows) {
    const std::string path = directory + "/p.pgm";
    FILE* f = std::fopen(path.c_str(), "wb");
    std::fputs(header.c_str(), f);
    for (long i = 0; i < rows; ++i) std::fputc(int(i & 255), f);
    std::fclose(f);
    return pw3::openPicture(path.c_str(), 7, 3);
}
bool opens(const std::string& header, long rows = 21) {
    FILE* f = open(header, rows);
    if (f) std::fclose(f);
    return f != nullptr;
}
}  // namespace

int main() {
    // TMPDIR on the device (it has no /tmp), e.g. /data/local/tmp.
    const char* base = std::getenv("TMPDIR");
    std::string name = std::string(base ? base : "/tmp") + "/pw3-picture-XXXXXX";
    directory = mkdtemp(&name[0]);
    // Positioned at the first row.
    FILE* f = open("P5\n7 3\n255\n", 21);
    CHECK(f);
    if (f) {
        unsigned char row[7];
        CHECK(std::fread(row, 1, 7, f) == 7 && row[0] == 0 && row[6] == 6);
        std::fclose(f);
    }
    // Any whitespace and comments between the numbers; one whitespace byte after the last.
    CHECK(opens("P5 7 3 255 "));
    CHECK(opens("P5\n# made for the sleep screen\n7\t3\r\n# grey\n255\n"));
    CHECK(opens("P5\n7 3\n255\n" + std::string(1, '\n'), 20));  // a row byte that looks like whitespace
    // Not this picture.
    CHECK(!opens("P2\n7 3\n255\n"));            // text PGM
    CHECK(!opens("P6\n7 3\n255\n", 63));        // colour
    CHECK(!opens("P5\n8 3\n255\n", 24));        // another size
    CHECK(!opens("P5\n7 3\n65535\n", 42));      // 16-bit
    CHECK(!opens("P5\n7 3\n15\n"));             // other grey scale
    CHECK(!opens("P5\n7 3\n255\n", 20));        // short
    CHECK(!opens("P5\n7 3\n255\n", 22));        // longer
    CHECK(!opens("P5\n7 3\n255"));              // no byte after the header
    CHECK(!opens("P5\n7 3 255x", 21));
    CHECK(!opens("P5\n-7 3\n255\n"));
    CHECK(!opens("P5\n99999999999 3\n255\n"));
    CHECK(!opens(""));
    CHECK(!pw3::openPicture((directory + "/missing.pgm").c_str(), 7, 3));
    unlink((directory + "/p.pgm").c_str());
    rmdir(directory.c_str());
    std::printf(failures ? "PICTURE_TEST_FAILED %d\n" : "PICTURE_TEST_PASS%.0d\n", failures);
    return failures != 0;
}
