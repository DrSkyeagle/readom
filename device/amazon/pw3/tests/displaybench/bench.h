// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <functional>
#include <vector>

namespace bench {
const int W = 1072, H = 1448, S = 1088;
inline double nowMs() {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}
inline uint32_t rnd() {
    static uint32_t seed = 20261012;
    seed = seed * 1664525u + 1013904223u;
    return seed >> 8;
}
inline void measure(const char* name, const std::function<void()>& prepare, const std::function<void()>& run) {
    std::vector<double> ms;
    for (int i = 0; i < 9; ++i) {
        prepare();
        const double a = nowMs();
        run();
        ms.push_back(nowMs() - a);
    }
    std::sort(ms.begin(), ms.end());
    printf("%-52s best %7.2f ms  median %7.2f ms\n", name, ms.front(), ms[ms.size() / 2]);
}
}  // namespace bench
