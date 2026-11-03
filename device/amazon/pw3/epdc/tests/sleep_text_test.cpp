// SPDX-License-Identifier: Apache-2.0
// The line on the sleep picture (SleepText.h): its pieces, and how it is laid
// out and written, with a made-up strip.
#include "../SleepText.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>

static int failures;
#define CHECK(x) do { if (!(x)) { std::printf("FAILED line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

static std::string join(const std::vector<std::string>& v) { std::string s; for (const auto& p : v) s += p; return s; }

int main() {
    std::tm t{};
    t.tm_year = 2026 - 1900; t.tm_mon = 9; t.tm_mday = 15; t.tm_hour = 9; t.tm_min = 7;
    CHECK(join(pw3::sleepLine(true, &t, 63)) == "休眠于 09:07 · 10月15日 · 电量 63%");
    CHECK(join(pw3::sleepLine(false, &t, 63)) == "Asleep at 09:07 · Oct 15 · Battery 63%");
    CHECK(join(pw3::sleepLine(true, &t, -1)) == "休眠于 09:07 · 10月15日");
    std::tm unset{};  // 1970: the clock was never set
    CHECK(join(pw3::sleepLine(true, &unset, 100)) == "电量 100%");
    CHECK(pw3::sleepLine(false, nullptr, -1).empty());
    CHECK(pw3::sleepLine(true, &t, 63).size() == 20);   // one piece per character, the words and " · " whole
    // A strip 4 high: "1" is all ink (x 0..1), "%" half ink (x 2..4), " · " none (x 5..7).
    char dir[] = "/tmp/sleeptext-XXXXXX";
    CHECK(mkdtemp(dir) != nullptr);
    const std::string strip = std::string(dir) + "/s.pgm", index = std::string(dir) + "/s.txt";
    FILE* f = std::fopen(strip.c_str(), "wb");
    std::fputs("P5\n8 4\n255\n", f);
    for (int y = 0; y < 4; ++y) { const unsigned char row[8] = {255, 255, 128, 128, 128, 0, 0, 0}; std::fwrite(row, 1, 8, f); }
    std::fclose(f);
    f = std::fopen(index.c_str(), "w");
    std::fputs("pw3-sleep-glyphs 1\n0 2 1\n2 3 %\n5 3  · \n", f);
    std::fclose(f);
    pw3::SleepText text;
    CHECK(text.load(strip.c_str(), index.c_str()) && text.height() == 4);
    CHECK(text.place({"1", " · ", "%"}, 16, 3, 24, 10));
    CHECK(!text.place({"1", "2"}, 16, 3, 24, 10));           // "2" is not on the strip
    CHECK(!text.place({"1", "%"}, 16, 8, 24, 10));           // would end below the picture
    CHECK(!text.place({"1", " · ", "%"}, 22, 3, 24, 10));    // would end past the right edge
    CHECK(text.place({"1", " · ", "%"}, 16, 3, 24, 10));
    // The line is 8 wide, centred on x 16: "1" at 12..13, " · " at 14..16, "%" at 17..19.
    uint8_t row[24];
    std::memset(row, 200, sizeof(row));
    text.row(2, row, 24, 60);                                 // above the line: untouched
    for (int x = 0; x < 24; ++x) CHECK(row[x] == 200);
    text.row(4, row, 24, 60);
    for (int x = 0; x < 24; ++x) {
        const int want = x == 12 || x == 13 ? 60 : x >= 17 && x <= 19 ? (200 * 127 + 60 * 128 + 127) / 255 : 200;
        CHECK(row[x] == want);
    }
    // A broken index is refused.
    f = std::fopen(index.c_str(), "w"); std::fputs("pw3-sleep-glyphs 1\n6 3 x\n", f); std::fclose(f);   // past the strip's end
    CHECK(!text.load(strip.c_str(), index.c_str()));
    f = std::fopen(index.c_str(), "w"); std::fputs("other 1\n0 2 1\n", f); std::fclose(f);
    CHECK(!text.load(strip.c_str(), index.c_str()));
    unlink(strip.c_str()); unlink(index.c_str()); rmdir(dir);
    if (failures) return 1;
    std::puts("SLEEP_TEXT_TEST_PASS");
    return 0;
}
