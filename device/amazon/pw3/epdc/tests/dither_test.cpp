// SPDX-License-Identifier: Apache-2.0
// The luminance model (Tone.h) and the error diffusion of pictures (Dither.h).
#include "../Dither.h"
#include "../Tone.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <vector>

static int failures;
#define CHECK(x) do { if (!(x)) { std::printf("FAILED line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

namespace {
const unsigned Contrasts[] = {0, 1, 2, 12, 15, 1000};

std::vector<uint8_t> dither(unsigned contrast, int width, int height, const std::vector<uint8_t>& luma) {
    pw3::Dither d(width, contrast);
    std::vector<uint8_t> out(luma.size());
    for (int y = 0; y < height; ++y) d.row(luma.data() + size_t(y) * width, out.data() + size_t(y) * width);
    return out;
}
// Mean luminance (1/65535) of the levels shown in a block.
double meanLuminance(const uint16_t levels[16], const std::vector<uint8_t>& out, int width, int x0, int y0, int x1, int y1) {
    double sum = 0;
    for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) sum += levels[out[size_t(y) * width + x] / 17];
    return sum / double((x1 - x0) * (y1 - y0));
}
}  // namespace

int main() {
    for (unsigned contrast : Contrasts) {
        uint16_t levels[16], luma[256];
        pw3::panelLuminance(contrast, levels, luma);
        uint8_t shown[256];
        pw3::panelLevels(contrast, shown);
        // The model: levels rise, white is exact, luma only rises, black and white match.
        for (int k = 1; k < 16; ++k) CHECK(levels[k] > levels[k - 1]);
        for (int v = 1; v < 256; ++v) CHECK(luma[v] >= luma[v - 1]);
        CHECK(levels[15] == 65535 && luma[255] == 65535 && luma[0] == levels[0]);
        if (contrast >= 2) CHECK(std::abs(levels[0] - 65535.0 / contrast) <= 1);
        else for (int k = 0; k < 16; ++k) CHECK(luma[17 * k] == levels[k]);
        // panelLevels shows a luma as the level nearest in lightness: the
        // luminance it stands for lies between that level's neighbours.
        for (int v = 0; v < 256; ++v) {
            const int k = shown[v] / 17;
            CHECK(k == 0 || luma[v] > levels[k - 1]);
            CHECK(k == 15 || luma[v] < levels[k + 1]);
        }

        const int W = 97, H = 61;  // odd: rows in both directions end differently
        // Black and white stay exact, wherever they are.
        std::vector<uint8_t> picture(size_t(W) * H);
        for (size_t i = 0; i < picture.size(); ++i) picture[i] = (i * 7919 % 13) < 6 ? 0 : 255;
        CHECK(dither(contrast, W, H, picture) == picture);
        // Luma shown exactly as a level stays that level.
        if (contrast < 2) {
            for (size_t i = 0; i < picture.size(); ++i) picture[i] = uint8_t((i / 7 % 16) * 17);
            CHECK(dither(contrast, W, H, picture) == picture);
        }
        // A flat grey keeps its brightness and uses the two levels around it.
        for (int v : {1, 40, 100, 128, 129, 200, 250}) {
            std::vector<uint8_t> flat(size_t(W) * H, uint8_t(v));
            const std::vector<uint8_t> out = dither(contrast, W, H, flat);
            const double mean = meanLuminance(levels, out, W, 2, 2, W - 2, H - 2);
            CHECK(std::fabs(mean - luma[v]) <= 0.01 * 65535);
            int below = 0;
            while (below < 15 && levels[below + 1] <= luma[v]) ++below;
            size_t others = 0;
            for (uint8_t o : out) others += o / 17 != below && o / 17 != std::min(15, below + 1);
            CHECK(others * 100 <= out.size());
        }
        // A horizontal ramp: the brightness of each band of columns rises with luma.
        const int RW = 256, RH = 64;
        std::vector<uint8_t> ramp(size_t(RW) * RH);
        for (int y = 0; y < RH; ++y) for (int x = 0; x < RW; ++x) ramp[size_t(y) * RW + x] = uint8_t(x);
        const std::vector<uint8_t> out = dither(contrast, RW, RH, ramp);
        double previous = -1;
        for (int band = 0; band < RW; band += 32) {
            const double mean = meanLuminance(levels, out, RW, band, 0, band + 32, RH);
            CHECK(mean >= previous);
            double expected = 0;
            for (int x = band; x < band + 32; ++x) expected += luma[x];
            CHECK(std::fabs(mean - expected / 32) <= 0.02 * 65535);
            previous = mean;
        }
        // One pixel wide, and the same input gives the same output.
        std::vector<uint8_t> column(53);
        for (size_t i = 0; i < column.size(); ++i) column[i] = uint8_t(i * 5);
        CHECK(dither(contrast, 1, 53, column) == dither(contrast, 1, 53, column));
    }
    bool rejected = false;
    try { pw3::Dither d(0, 12); } catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
    std::printf(failures ? "DITHER_TEST_FAILED %d\n" : "DITHER_TEST_PASS%.0d\n", failures);
    return failures != 0;
}
