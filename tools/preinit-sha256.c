// SPDX-License-Identifier: Apache-2.0
/*
 * SHA-256 (FIPS 180-4) for the standalone storage-entry build.
 *
 * PreInit.c includes <openssl/sha.h> and calls SHA256_Init/Update/Final (and
 * once, in TestFileInit.h, the one-shot SHA256). Inside AOSP the preinit module
 * links BoringSSL's libcrypto for those; this file is what the standalone
 * cross-compile links instead, so the entry can be built without an AOSP tree
 * (readom-port/release-pipeline-report.md, gap G1) and without pulling a
 * differently licensed hash implementation into the binary.
 *
 * The interface is the OpenSSL shape the callers use and nothing more:
 * SHA256_CTX holds the state, and each function returns 1 on success, which is
 * also what the untouched PreInit.c checks. Only SHA256 of a byte stream is
 * provided -- no SHA224/384/512, no HMAC: nothing in the entry asks for them.
 *
 * Correctness is checked at build time, not by eye: tools/build-preinit.py
 * compiles this file for the host with PW3_PREINIT_SHA256_SELFTEST and runs the
 * NIST known-answer vectors below before it cross-compiles anything. A wrong
 * digest in PID 1 would reject an intact system image and be reported as "the
 * image is damaged", which is the one refusal the installer acts on, so the
 * vectors are part of the build rather than a comment.
 *
 * The struct layout lives in the openssl/sha.h shim that build-preinit.py
 * writes (SHA_HEADER there); this file and that shim are the two halves of one
 * interface, and both name the same fields.
 */
#include <openssl/sha.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* FIPS 180-4, 4.2.2: the first 32 bits of the fractional parts of the cube
 * roots of the first 64 primes. */
static const uint32_t sha256_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static uint32_t rotr(uint32_t x, unsigned bits)
{
    return (x >> bits) | (x << (32 - bits));
}

/* FIPS 180-4, 6.2.2: one 512-bit block through the compression function. */
static void sha256_compress(SHA256_CTX *ctx, const unsigned char block[64])
{
    uint32_t w[64], a, b, c, d, e, f, g, h;
    unsigned i;

    for (i = 0; i < 16; ++i) {
        w[i] = ((uint32_t)block[4 * i] << 24) | ((uint32_t)block[4 * i + 1] << 16) |
               ((uint32_t)block[4 * i + 2] << 8) | (uint32_t)block[4 * i + 3];
    }
    for (i = 16; i < 64; ++i) {
        const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    a = ctx->h[0];
    b = ctx->h[1];
    c = ctx->h[2];
    d = ctx->h[3];
    e = ctx->h[4];
    f = ctx->h[5];
    g = ctx->h[6];
    h = ctx->h[7];

    for (i = 0; i < 64; ++i) {
        const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const uint32_t ch = (e & f) ^ (~e & g);
        const uint32_t t1 = h + s1 + ch + sha256_k[i] + w[i];
        const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    ctx->h[0] += a;
    ctx->h[1] += b;
    ctx->h[2] += c;
    ctx->h[3] += d;
    ctx->h[4] += e;
    ctx->h[5] += f;
    ctx->h[6] += g;
    ctx->h[7] += h;
}

int SHA256_Init(SHA256_CTX *ctx)
{
    if (!ctx) {
        return 0;
    }
    /* FIPS 180-4, 5.3.3: the first 32 bits of the fractional parts of the
     * square roots of the first eight primes. */
    ctx->h[0] = 0x6a09e667u;
    ctx->h[1] = 0xbb67ae85u;
    ctx->h[2] = 0x3c6ef372u;
    ctx->h[3] = 0xa54ff53au;
    ctx->h[4] = 0x510e527fu;
    ctx->h[5] = 0x9b05688cu;
    ctx->h[6] = 0x1f83d9abu;
    ctx->h[7] = 0x5be0cd19u;
    ctx->bits = 0;
    ctx->used = 0;
    return 1;
}

int SHA256_Update(SHA256_CTX *ctx, const void *data, size_t bytes)
{
    const unsigned char *p = data;
    size_t left;

    if (!ctx || (!p && bytes)) {
        return 0;
    }
    /* A null-length update is not an error; it is also not a reason to touch
     * `data`, which OpenSSL's callers are allowed to pass as NULL there. */
    ctx->bits += (uint64_t)bytes << 3;

    left = (size_t)(sizeof(ctx->block) - ctx->used);
    if (bytes < left) {
        if (bytes) {
            memcpy(ctx->block + ctx->used, p, bytes);
            ctx->used += bytes;
        }
        return 1;
    }
    memcpy(ctx->block + ctx->used, p, left);
    sha256_compress(ctx, ctx->block);
    p += left;
    bytes -= left;
    while (bytes >= sizeof(ctx->block)) {
        sha256_compress(ctx, p);
        p += sizeof(ctx->block);
        bytes -= sizeof(ctx->block);
    }
    if (bytes) {
        memcpy(ctx->block, p, bytes);
    }
    ctx->used = bytes;
    return 1;
}

int SHA256_Final(unsigned char out[32], SHA256_CTX *ctx)
{
    unsigned char tail[128];
    size_t pad, used;
    uint64_t bits;
    unsigned i;

    if (!ctx || !out) {
        return 0;
    }
    used = (size_t)ctx->used;
    memcpy(tail, ctx->block, used);
    /* FIPS 180-4, 5.1.1: 0x80, then zeros, then the length in bits as a 64-bit
     * big-endian number, filling out to a whole number of blocks. */
    bits = ctx->bits;
    tail[used++] = 0x80;
    pad = (used <= 56) ? (56 - used) : (120 - used);
    memset(tail + used, 0, pad);
    used += pad;
    for (i = 0; i < 8; ++i) {
        tail[used + i] = (unsigned char)(bits >> (56 - 8 * i));
    }
    used += 8;
    for (i = 0; i < used / sizeof(ctx->block); ++i) {
        sha256_compress(ctx, tail + i * sizeof(ctx->block));
    }
    for (i = 0; i < 32; ++i) {
        out[i] = (unsigned char)(ctx->h[i / 4] >> (24 - 8 * (i % 4)));
    }
    /* The state is left unusable for a further update, as OpenSSL's is: every
     * caller here takes the digest and returns. */
    memset(ctx, 0, sizeof(*ctx));
    return 1;
}

unsigned char *SHA256(const unsigned char *data, size_t bytes, unsigned char *out)
{
    SHA256_CTX ctx;

    if (!out || !SHA256_Init(&ctx) || !SHA256_Update(&ctx, data, bytes) || !SHA256_Final(out, &ctx)) {
        return NULL;
    }
    return out;
}

#ifdef PW3_PREINIT_SHA256_SELFTEST
/*
 * NIST CAVP / FIPS 180-4 known-answer vectors, run by tools/build-preinit.py on
 * the host. Compiled only for that self-test: this file is never linked with a
 * main() on the target.
 */
#include <stdio.h>

static const struct {
    const char *name;
    const char *input;
    size_t bytes;
    const char *digest;
} vectors[] = {
    {"empty", "", 0,
     "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
    {"abc", "abc", 3,
     "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
    {"448-bit message",
     "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56,
     "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
};

static int hex_equals(const unsigned char digest[32], const char *hex)
{
    unsigned i;
    for (i = 0; i < 32; ++i) {
        unsigned value = 0, j;
        for (j = 0; j < 2; ++j) {
            const char c = hex[2 * i + j];
            value = (value << 4) | (unsigned)(c >= 'a' ? c - 'a' + 10 : c - '0');
        }
        if (digest[i] != (unsigned char)value) {
            return 0;
        }
    }
    return 1;
}

int main(void)
{
    unsigned char digest[32];
    unsigned long i;
    int failed = 0;

    for (i = 0; i < sizeof(vectors) / sizeof(vectors[0]); ++i) {
        if (!SHA256((const unsigned char *)vectors[i].input, vectors[i].bytes, digest) ||
            !hex_equals(digest, vectors[i].digest)) {
            printf("sha256 self-test: FAIL %s\n", vectors[i].name);
            failed = 1;
        } else {
            printf("sha256 self-test: ok %s\n", vectors[i].name);
        }
    }
    /* One million 'a': exercises multi-block updates and the length field at a
     * size the kernel log and system image reads actually reach. */
    {
        static char million[1000000];
        memset(million, 'a', sizeof(million));
        if (!SHA256((const unsigned char *)million, sizeof(million), digest) ||
            !hex_equals(digest, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0")) {
            printf("sha256 self-test: FAIL one million 'a'\n");
            failed = 1;
        } else {
            printf("sha256 self-test: ok one million 'a'\n");
        }
    }
    return failed;
}
#endif
