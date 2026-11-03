// SPDX-License-Identifier: Apache-2.0
// Throughput of Reactor-generated ARM code for the operations SwiftShader's
// pixel routines are made of (N38): one loop per operation over a buffer,
// timed against the same loop in C++. Never uses display hardware.
#include "Reactor/Reactor.hpp"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>
using namespace sw;
namespace {
const int N = 1 << 18;   // 4-lane elements per run
double timed(void (*fn)(void*, int), void* data) {
    double best = 1e9;
    for (int i = 0; i < 3; ++i) {
        auto start = std::chrono::steady_clock::now();
        fn(data, N);
        auto end = std::chrono::steady_clock::now();
        best = std::min(best, std::chrono::duration<double, std::nano>(end - start).count() / N);
    }
    return best;
}
template <class Body>
Routine* loop(const wchar_t* name, Body body) {
    Function<Void(Pointer<Byte>, Int)> f;
    Pointer<Byte> data = f.Arg<0>();
    Int n = f.Arg<1>();
    Int i = 0;
    For(i = 0, i < n, i++) {
        body(data + i * 16);
    }
    Return();
    return f(name);
}
void report(const char* what, Routine* r, void* data) {
    auto fn = reinterpret_cast<void (*)(void*, int)>(const_cast<void*>(r->getEntry()));
    std::printf("%-34s %8.1f ns per 4 lanes\n", what, timed(fn, data));
}
}  // namespace

int main() {
    std::vector<uint8_t> buffer(size_t(N) * 16 + 64, 1);
    void* data = buffer.data();
    report("empty loop", loop(L"empty", [](Pointer<Byte>) {}), data);
    report("Int4 add", loop(L"int4", [](Pointer<Byte> p) {
        Int4 v = *Pointer<Int4>(p); *Pointer<Int4>(p) = v + v; }), data);
    report("Float4 mul-add", loop(L"float4", [](Pointer<Byte> p) {
        Float4 v = *Pointer<Float4>(p); *Pointer<Float4>(p) = v * Float4(0.5f) + Float4(1.0f); }), data);
    report("Short4 unpack + add", loop(L"short4", [](Pointer<Byte> p) {
        Short4 v = *Pointer<Short4>(p); *Pointer<Short4>(p) = v + v; }), data);
    report("Int4 -> Float4", loop(L"convert", [](Pointer<Byte> p) {
        Int4 v = *Pointer<Int4>(p); *Pointer<Float4>(p) = Float4(v); }), data);
    report("Float4 -> Int4 (round)", loop(L"round", [](Pointer<Byte> p) {
        Float4 v = *Pointer<Float4>(p); *Pointer<Int4>(p) = RoundInt(v); }), data);
    report("Byte8 -> Short4 unpack", loop(L"unpack", [](Pointer<Byte> p) {
        Byte8 v = *Pointer<Byte8>(p); *Pointer<Short4>(p) = As<Short4>(UnpackLow(v, Byte8(0, 0, 0, 0, 0, 0, 0, 0))); }), data);
    report("Float4 min/max clamp", loop(L"clamp", [](Pointer<Byte> p) {
        Float4 v = *Pointer<Float4>(p); *Pointer<Float4>(p) = Min(Max(v, Float4(0.0f)), Float4(1.0f)); }), data);
    // The same as plain C++, for scale.
    auto native = [](void* d, int n) {
        float* f = static_cast<float*>(d);
        for (int i = 0; i < n * 4; ++i) f[i] = f[i] * 0.5f + 1.0f;
    };
    std::printf("%-34s %8.1f ns per 4 lanes\n", "C++ float mul-add", timed(native, data));
    return 0;
}
