#pragma once

// Minimal MD5 context/API for the OBOOK5 port.  The simulator's
// MD5Builder_linux.h expects the classic OpenSSL MD5_Init/Update/Final
// surface; musl has no OpenSSL, so provide the small subset used there.

#include <cstddef>
#include <cstdint>
#include <cstring>

typedef struct MD5_CTX {
  uint32_t state[4];
  uint64_t count;  // bytes processed
  uint8_t buffer[64];
} MD5_CTX;

namespace obook5_md5_detail {

inline uint32_t rol(uint32_t v, unsigned n) {
  return (v << n) | (v >> (32 - n));
}

inline uint32_t load_le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

inline void store_le32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
  p[2] = static_cast<uint8_t>(v >> 16);
  p[3] = static_cast<uint8_t>(v >> 24);
}

inline void transform(uint32_t state[4], const uint8_t block[64]) {
  static constexpr uint32_t k[64] = {
      0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu,
      0x4787c62au, 0xa8304613u, 0xfd469501u, 0x698098d8u, 0x8b44f7afu,
      0xffff5bb1u, 0x895cd7beu, 0x6b901122u, 0xfd987193u, 0xa679438eu,
      0x49b40821u, 0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau,
      0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u, 0x21e1cde6u,
      0xc33707d6u, 0xf4d50d87u, 0x455a14edu, 0xa9e3e905u, 0xfcefa3f8u,
      0x676f02d9u, 0x8d2a4c8au, 0xfffa3942u, 0x8771f681u, 0x6d9d6122u,
      0xfde5380cu, 0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
      0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u, 0xd9d4d039u,
      0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u, 0xf4292244u, 0x432aff97u,
      0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u, 0xffeff47du,
      0x85845dd1u, 0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
      0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u};
  static constexpr uint8_t s[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17,
                                    22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14,
                                    20, 5, 9, 14, 20, 5, 9, 14, 20, 4, 11, 16,
                                    23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16,
                                    23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15,
                                    21, 6, 10, 15, 21};

  uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
  uint32_t m[16];
  for (int i = 0; i < 16; ++i) m[i] = load_le32(block + i * 4);

  for (int i = 0; i < 64; ++i) {
    uint32_t f, g;
    if (i < 16) {
      f = (b & c) | (~b & d);
      g = i;
    } else if (i < 32) {
      f = (d & b) | (~d & c);
      g = (5 * i + 1) & 15;
    } else if (i < 48) {
      f = b ^ c ^ d;
      g = (3 * i + 5) & 15;
    } else {
      f = c ^ (b | ~d);
      g = (7 * i) & 15;
    }
    const uint32_t tmp = d;
    d = c;
    c = b;
    b = b + rol(a + f + k[i] + m[g], s[i]);
    a = tmp;
  }
  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
}

}  // namespace obook5_md5_detail

inline void MD5_Init(MD5_CTX* ctx) {
  ctx->state[0] = 0x67452301u;
  ctx->state[1] = 0xefcdab89u;
  ctx->state[2] = 0x98badcfeu;
  ctx->state[3] = 0x10325476u;
  ctx->count = 0;
  std::memset(ctx->buffer, 0, sizeof(ctx->buffer));
}

inline void MD5_Update(MD5_CTX* ctx, const void* data, size_t len) {
  using namespace obook5_md5_detail;
  const uint8_t* input = static_cast<const uint8_t*>(data);
  size_t index = static_cast<size_t>(ctx->count & 63);
  ctx->count += len;

  size_t offset = 0;
  if (index) {
    const size_t fill = 64 - index;
    if (len < fill) {
      std::memcpy(ctx->buffer + index, input, len);
      return;
    }
    std::memcpy(ctx->buffer + index, input, fill);
    transform(ctx->state, ctx->buffer);
    offset = fill;
  }
  while (len - offset >= 64) {
    transform(ctx->state, input + offset);
    offset += 64;
  }
  if (offset < len) std::memcpy(ctx->buffer, input + offset, len - offset);
}

inline void MD5_Final(unsigned char digest[16], MD5_CTX* ctx) {
  using namespace obook5_md5_detail;
  const uint64_t bit_len = ctx->count * 8;
  uint8_t pad[72] = {0x80};
  size_t index = static_cast<size_t>(ctx->count & 63);
  const size_t pad_len = index < 56 ? 56 - index : 120 - index;
  for (int i = 0; i < 8; ++i)
    pad[pad_len + i] = static_cast<uint8_t>(bit_len >> (i * 8));
  MD5_Update(ctx, pad, pad_len + 8);
  for (int i = 0; i < 4; ++i) store_le32(digest + i * 4, ctx->state[i]);
}
