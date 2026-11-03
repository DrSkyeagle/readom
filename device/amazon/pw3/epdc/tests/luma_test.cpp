// SPDX-License-Identifier: Apache-2.0
// The row functions against their one-pixel definitions, for every mode.
#include "../Luma.h"
#include <cassert>
#include <cstdio>
#include <vector>

static uint32_t seed = 20261013;
static uint8_t byte() { seed = seed * 1664525u + 1013904223u; return uint8_t(seed >> 24); }

int main() {
    // Grey in, the same grey out; the weights add up to 256.
    for (int v = 0; v < 256; ++v) {
        const uint8_t rgba[4] = {uint8_t(v), uint8_t(v), uint8_t(v), 255};
        assert(pw3::lumaOf(rgba, false) == v && pw3::lumaOf(rgba, true) == v);
    }
    const uint8_t red[4] = {255, 0, 0, 255}, blue[4] = {0, 0, 255, 255};
    assert(pw3::lumaOf(red, false) == 77 && pw3::lumaOf(blue, false) == 29);
    assert(pw3::lumaOf(red, true) == 29 && pw3::lumaOf(blue, true) == 77);

    long long checked = 0;
    for (int width : {1, 15, 16, 17, 31, 32, 33, 64, 269}) {
        const size_t n = size_t(width);
        std::vector<uint8_t> src(n * 4), below(n), out(n + 2);
        for (int round = 0; round < 40; ++round) {
            for (auto& v : src) v = byte();
            // Runs of transparent, opaque and premultiplied pixels as real windows have them.
            for (int x = 0; x < width; ++x) {
                uint8_t* p = &src[size_t(x) * 4];
                switch ((x / 5 + round) % 4) {
                    case 0: p[0] = p[1] = p[2] = p[3] = 0; break;
                    case 1: p[3] = 255; break;
                    case 2: for (int c = 0; c < 3; ++c) p[c] = uint8_t(p[c] * p[3] / 255); break;
                    default: break;
                }
            }
            for (auto& v : below) v = byte();
            for (bool bgra : {false, true}) {
                out.assign(out.size(), 0xa5);
                std::vector<uint8_t> was(out);
                bool changed = pw3::lumaRow(src.data(), out.data() + 1, width, bgra);
                assert(out.front() == 0xa5 && out.back() == 0xa5);
                assert(changed == (out != was));
                // Run again over its own result: nothing may change the second time.
                assert(!pw3::lumaRow(src.data(), out.data() + 1, width, bgra));
                for (int x = 0; x < width; ++x, ++checked) assert(out[size_t(x) + 1] == pw3::lumaOf(&src[size_t(x) * 4], bgra));
                for (bool opaque : {false, true}) for (bool coverage : {false, true})
                    for (uint8_t plane : {uint8_t(255), uint8_t(254), uint8_t(128), uint8_t(77), uint8_t(1), uint8_t(0)}) {
                        const pw3::LumaMode mode{opaque, coverage, bgra, plane};
                        out.assign(out.size(), 0xa5);
                        for (int x = 0; x < width; ++x) out[size_t(x) + 1] = below[size_t(x)];
                        const std::vector<uint8_t> before(out);
                        const bool blended = pw3::lumaBlendRow(src.data(), out.data() + 1, width, mode);
                        assert(out.front() == 0xa5 && out.back() == 0xa5);
                        assert(blended == (out != before));
                        for (int x = 0; x < width; ++x, ++checked)
                            assert(out[size_t(x) + 1] == pw3::lumaBlendOf(&src[size_t(x) * 4], below[size_t(x)], mode));
                    }
            }
            for (uint8_t alpha : {uint8_t(0), uint8_t(1), uint8_t(102), uint8_t(153), uint8_t(255)}) {
                out.assign(out.size(), 0xa5);
                for (int x = 0; x < width; ++x) out[size_t(x) + 1] = below[size_t(x)];
                const std::vector<uint8_t> before(out);
                const bool dimmed = pw3::lumaDimRow(out.data() + 1, width, alpha);
                assert(out.front() == 0xa5 && out.back() == 0xa5);
                assert(dimmed == (out != before));
                for (int x = 0; x < width; ++x, ++checked) {
                    const unsigned t = unsigned(below[size_t(x)]) * (255u - alpha) + 128u;
                    assert(out[size_t(x) + 1] == uint8_t((t + (t >> 8)) >> 8));
                }
            }
        }
    }
    // The paper fill reports a change exactly when it writes something new.
    {
        std::vector<uint8_t> row(70, 255);
        assert(!pw3::lumaFillRow(row.data(), 255, int(row.size())));
        row[33] = 254;
        assert(pw3::lumaFillRow(row.data(), 255, int(row.size())));
        assert(!pw3::lumaFillRow(row.data(), 255, int(row.size())));
        for (uint8_t v : row) assert(v == 255);
    }
    // An opaque replacing layer and a blend with full coverage give the same result.
    {
        std::vector<uint8_t> src(64 * 4), a(64), b(64, 9);
        for (auto& v : src) v = byte();
        for (size_t x = 0; x < 64; ++x) src[x * 4 + 3] = 255;
        pw3::lumaRow(src.data(), a.data(), 64, false);
        pw3::lumaBlendRow(src.data(), b.data(), 64, pw3::LumaMode{false, false, false, 255});
        assert(a == b);
    }
    printf("luma: %lld pixels checked\n", checked);
    return 0;
}
