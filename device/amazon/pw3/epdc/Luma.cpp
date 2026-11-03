// SPDX-License-Identifier: Apache-2.0
#include "Luma.h"
#include <algorithm>
#if (defined(__ARM_NEON) || defined(__ARM_NEON__)) && !defined(PW3_LUMA_SCALAR)
#include <arm_neon.h>
#define PW3_LUMA_NEON 1
#endif

namespace pw3 {
namespace {
// round(value / 255) for every product of two bytes, without a division: the
// Cortex-A9 has no divide instruction.
inline unsigned divide255(unsigned value) {
    const unsigned t = value + 128u;
    return (t + (t >> 8)) >> 8;
}
// The Cortex-A9 does not read ahead on its own for these access patterns;
// without the hint the loops run at a third of the speed of memcpy.
inline void ahead(const uint8_t* p) {
#if defined(__GNUC__)
    __builtin_prefetch(p + 256);
    __builtin_prefetch(p + 288);
#else
    (void)p;
#endif
}
#ifdef PW3_LUMA_NEON
inline uint8x16_t luma16(const uint8x16x4_t& p, bool bgra) {
    const uint8x8_t first = vdup_n_u8(bgra ? 29 : 77), green = vdup_n_u8(150), third = vdup_n_u8(bgra ? 77 : 29);
    uint16x8_t low = vmull_u8(vget_low_u8(p.val[0]), first);
    low = vmlal_u8(low, vget_low_u8(p.val[1]), green);
    low = vmlal_u8(low, vget_low_u8(p.val[2]), third);
    uint16x8_t high = vmull_u8(vget_high_u8(p.val[0]), first);
    high = vmlal_u8(high, vget_high_u8(p.val[1]), green);
    high = vmlal_u8(high, vget_high_u8(p.val[2]), third);
    return vcombine_u8(vrshrn_n_u16(low, 8), vrshrn_n_u16(high, 8));
}
// Same rounding as divide255(), sixteen products at a time.
inline uint8x8_t divide255(uint16x8_t v) { return vrshrn_n_u16(vrsraq_n_u16(v, v, 8), 8); }
inline uint8x16_t scale(uint8x16_t a, uint8x16_t b) {
    return vcombine_u8(divide255(vmull_u8(vget_low_u8(a), vget_low_u8(b))),
                       divide255(vmull_u8(vget_high_u8(a), vget_high_u8(b))));
}
// Whether any lane holds a non-zero byte.
inline bool any(uint8x16_t v) {
    const uint8x8_t folded = vorr_u8(vget_low_u8(v), vget_high_u8(v));
    return vget_lane_u64(vreinterpret_u64_u8(folded), 0) != 0;
}
#endif
}  // namespace

uint8_t lumaOf(const uint8_t* p, bool bgra) {
    return uint8_t((77u * p[bgra ? 2 : 0] + 150u * p[1] + 29u * p[bgra ? 0 : 2] + 128u) >> 8);
}
uint8_t lumaBlendOf(const uint8_t* p, uint8_t below, const LumaMode& m) {
    const unsigned luma = lumaOf(p, m.bgra);
    const unsigned alpha = m.opaque ? m.plane : divide255(unsigned(p[3]) * m.plane);
    const unsigned value = divide255(luma * ((m.opaque || m.coverage) ? alpha : m.plane));
    return uint8_t(std::min(255u, value + divide255(unsigned(below) * (255u - alpha))));
}
bool lumaRow(const uint8_t* src, uint8_t* out, int count, bool bgra) {
    int x = 0;
    bool changed = false;
#ifdef PW3_LUMA_NEON
    uint8x16_t differs = vdupq_n_u8(0);
    for (; x + 16 <= count; x += 16, src += 64) {
        ahead(src);
        const uint8x16_t value = luma16(vld4q_u8(src), bgra);
        differs = vorrq_u8(differs, veorq_u8(value, vld1q_u8(out + x)));
        vst1q_u8(out + x, value);
    }
    changed = any(differs);
#endif
    for (; x < count; ++x, src += 4) {
        const uint8_t value = lumaOf(src, bgra);
        changed |= value != out[x];
        out[x] = value;
    }
    return changed;
}
bool lumaBlendRow(const uint8_t* src, uint8_t* out, int count, const LumaMode& m) {
    int x = 0;
    bool changed = false;
#ifdef PW3_LUMA_NEON
    const uint8x16_t plane = vdupq_n_u8(m.plane);
    uint8x16_t differs = vdupq_n_u8(0);
    for (; x + 16 <= count; x += 16, src += 64) {
        ahead(src);
        const uint8x16x4_t p = vld4q_u8(src);
        const uint8x16_t luma = luma16(p, m.bgra);
        const uint8x16_t alpha = m.opaque ? plane : m.plane == 255 ? p.val[3] : scale(p.val[3], plane);
        const uint8x16_t value = (m.opaque || m.coverage) ? scale(luma, alpha)
                               : m.plane == 255 ? luma : scale(luma, plane);
        const uint8x16_t below = vld1q_u8(out + x);
        const uint8x16_t result = vqaddq_u8(value, scale(below, vmvnq_u8(alpha)));
        differs = vorrq_u8(differs, veorq_u8(result, below));
        vst1q_u8(out + x, result);
    }
    changed = any(differs);
#endif
    for (; x < count; ++x, src += 4) {
        const uint8_t value = lumaBlendOf(src, out[x], m);
        changed |= value != out[x];
        out[x] = value;
    }
    return changed;
}
bool lumaDimRow(uint8_t* out, int count, uint8_t alpha) {
    const unsigned keep = 255u - alpha;
    int x = 0;
    bool changed = false;
#ifdef PW3_LUMA_NEON
    const uint8x16_t factor = vdupq_n_u8(uint8_t(keep));
    uint8x16_t differs = vdupq_n_u8(0);
    for (; x + 16 <= count; x += 16) {
        const uint8x16_t below = vld1q_u8(out + x), value = scale(below, factor);
        differs = vorrq_u8(differs, veorq_u8(value, below));
        vst1q_u8(out + x, value);
    }
    changed = any(differs);
#endif
    for (; x < count; ++x) {
        const uint8_t value = uint8_t(divide255(unsigned(out[x]) * keep));
        changed |= value != out[x];
        out[x] = value;
    }
    return changed;
}
bool lumaFillRow(uint8_t* out, uint8_t value, int count) {
    int x = 0;
    bool changed = false;
#ifdef PW3_LUMA_NEON
    const uint8x16_t fill = vdupq_n_u8(value);
    uint8x16_t differs = vdupq_n_u8(0);
    for (; x + 16 <= count; x += 16) {
        differs = vorrq_u8(differs, veorq_u8(fill, vld1q_u8(out + x)));
        vst1q_u8(out + x, fill);
    }
    changed = any(differs);
#endif
    for (; x < count; ++x) {
        changed |= out[x] != value;
        out[x] = value;
    }
    return changed;
}
}  // namespace pw3
