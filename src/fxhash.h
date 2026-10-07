#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <sys/cdefs.h>

static __always_inline uint64_t fx_rd64(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

static __always_inline uint64_t fx_rd32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return (uint64_t)v;
}

static __always_inline uint64_t fx_mulmix(uint64_t a, uint64_t b) {
    uint64_t a0 = (uint32_t)a, a1 = a >> 32;
    uint64_t b0 = (uint32_t)b, b1 = b >> 32;
    uint64_t p00 = a0 * b0;
    uint64_t p01 = a0 * b1;
    uint64_t p10 = a1 * b0;
    uint64_t p11 = a1 * b1;
    uint64_t mid = (p00 >> 32) + (uint32_t)p01 + (uint32_t)p10;
    uint64_t lo = (p00 & 0xffffffffULL) | (mid << 32);
    uint64_t hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
    return lo ^ hi;
}

static __always_inline uint64_t fx_hash_bytes(const uint8_t *b, size_t len) {
    const uint64_t SEED1 = 0x243f6a8885a308d3ULL;
    const uint64_t SEED2 = 0x13198a2e03707344ULL;
    const uint64_t ANTIZ = 0xa4093822299f31d0ULL;
    uint64_t s0 = SEED1, s1 = SEED2;
    if (len <= 16) {
        if (len >= 8) {
            s0 ^= fx_rd64(b);
            s1 ^= fx_rd64(b + len - 8);
        } else if (len >= 4) {
            s0 ^= fx_rd32(b);
            s1 ^= fx_rd32(b + len - 4);
        } else if (len > 0) {
            uint8_t lo = b[0], mid = b[len / 2], hi = b[len - 1];
            s0 ^= lo;
            s1 ^= ((uint64_t)hi << 8) | mid;
        }
    } else {
        size_t i = 0;
        for (; i + 16 <= len; i += 16) {
            uint64_t x = fx_rd64(b + i), y = fx_rd64(b + i + 8);
            uint64_t t = fx_mulmix(s0 ^ x, ANTIZ ^ y);
            s0 = s1;
            s1 = t;
        }
        const uint8_t *tail = b + len - 16;
        s0 ^= fx_rd64(tail);
        s1 ^= fx_rd64(tail + 8);
    }
    return fx_mulmix(s0, s1) ^ (uint64_t)len;
}

static __always_inline uint64_t fxhash64(const uint8_t *data, size_t len) {
    const uint64_t K = 0xf1357aea2e62a9c5ULL;
    uint64_t state = fx_hash_bytes(data, len) * K;
    return (state << 26) | (state >> (64 - 26));
}
