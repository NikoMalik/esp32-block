// blocklist container format, shared by host builder and firmware
// 16-byte header, then payload; CRC32 is over the whole payload
//   0..3  magic 'C3BL'
//   4     version (2 = bucketed)
//   5     suffix bytes (3)
//   6..7  reserved (0)
//   8..11 entry count, little-endian
//   12..15 CRC32 of payload, little-endian
//
// v2 payload (bucketed, for mmap): split each 40-bit hash into a 16-bit prefix
// (bucket) + 24-bit suffix.
//   (BL_BUCKETS+1) * 4 bytes : bucket start offsets (little-endian), last = count
//   count * 3 bytes          : suffixes, grouped by bucket, sorted within bucket
#pragma once
#include <stdint.h>
#include <stddef.h>

#define BL_MAGIC0 'C'
#define BL_MAGIC1 '3'
#define BL_MAGIC2 'B'
#define BL_MAGIC3 'L'
#define BL_VERSION 2
#define BL_HEADER_SIZE 16
#define BL_PREFIX_BITS 16
#define BL_BUCKETS (1 << BL_PREFIX_BITS) // 65536
#define BL_SUFFIX_BYTES 3
#define BL_SUFFIX_MASK 0xFFFFFFu
#define BL_CRC_INIT 0xFFFFFFFFu

static inline uint32_t bl_crc32_update(uint32_t crc, const uint8_t *d, size_t n) {
    for (size_t i = 0; i < n; i++) {
        crc ^= d[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u * (crc & 1));
    }
    return crc;
}

static inline uint32_t bl_crc32(const uint8_t *d, size_t n) {
    return ~bl_crc32_update(BL_CRC_INIT, d, n);
}
