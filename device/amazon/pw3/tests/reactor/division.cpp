// SPDX-License-Identifier: Apache-2.0
// Execute generated ARM shader arithmetic; never uses physical display hardware.
#include "Reactor/Reactor.hpp"
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
using namespace sw;
static unsigned checks;
static void check(bool ok) { ++checks; if(!ok) { std::fprintf(stderr,"PW3_REACTOR_DIV_FAIL check=%u\n",checks); std::exit(1); } }
static Routine *scalar(unsigned op) {
    if(op<2) {
        Function<Int(Int,Int)> f;Int a=f.Arg<0>(),b=f.Arg<1>();
        if(op==0)Return(a/b);else Return(a%b);
        return f(L"pw3_signed_divrem");
    }
    Function<Int(UInt,UInt)> f;UInt a=f.Arg<0>(),b=f.Arg<1>();
    if(op==2)Return(As<Int>(a/b));else Return(As<Int>(a%b));
    return f(L"pw3_unsigned_divrem");
}
static Routine *vector(unsigned op) {
    Function<Void(Pointer<Int4>,Pointer<Int4>,Pointer<Int4>)> f;
    Pointer<Int4> out=f.Arg<0>(),lhs=f.Arg<1>(),rhs=f.Arg<2>();
    Int4 a=lhs[0],b=rhs[0];
    Int4 c;
    if(op==0)c=a/b;
    else if(op==1)c=a%b;
    else if(op==2)c=As<Int4>(As<UInt4>(a)/As<UInt4>(b));
    else c=As<Int4>(As<UInt4>(a)%As<UInt4>(b));
    out[0]=c;
    Return();return f(L"pw3_vector_divrem");
}
static uint32_t expected(unsigned op,uint32_t a,uint32_t b) {
    if(op<2) {
        int64_t x=static_cast<int32_t>(a),y=static_cast<int32_t>(b);
        return static_cast<uint32_t>(op==0?x/y:x%y);
    }
    return op==2?a/b:a%b;
}
int main() {
    const uint32_t edge[]={0,1,2,3,7,16,31,65535,65536,0x7fffffff,0x80000000,0xfffffffd,0xfffffffe,0xffffffff};
    uint32_t seed=0x417c0921;
    for(unsigned op=0;op<4;++op) {
        Routine *r=scalar(op);check(r!=nullptr);
        auto fn=reinterpret_cast<uint32_t(*)(uint32_t,uint32_t)>(const_cast<void *>(r->getEntry()));
        check(fn!=nullptr);std::printf("PW3_REACTOR_DIV_START op=%u\n",op);std::fflush(stdout);
        for(uint32_t a:edge)for(uint32_t b:edge)if(b)check(fn(a,b)==expected(op,a,b));
        for(unsigned i=0;i<4096;++i) {
            seed=seed*1664525+1013904223;uint32_t a=seed;
            seed=seed*1664525+1013904223;uint32_t b=seed|1;
            check(fn(a,b)==expected(op,a,b));
        }
        delete r;
        r=vector(op);check(r!=nullptr);
        auto vf=reinterpret_cast<void(*)(uint32_t *,uint32_t *,uint32_t *)>(const_cast<void *>(r->getEntry()));
        check(vf!=nullptr);
        for(unsigned i=0;i<1024;++i) {
            alignas(16) uint32_t a[4],b[4],out[4]={};
            for(unsigned j=0;j<4;++j) {
                seed=seed*1664525+1013904223;a[j]=seed;
                seed=seed*1664525+1013904223;b[j]=seed|1;
            }
            vf(out,a,b);
            for(unsigned j=0;j<4;++j)check(out[j]==expected(op,a[j],b[j]));
        }
        delete r;
    }
    std::printf("PW3_REACTOR_DIV_PASS checks=%u\n",checks);
    return 0;
}
