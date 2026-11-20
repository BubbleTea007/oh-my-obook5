#pragma once

// Minimal SHA-1/SHA-256 one-shot functions for the OBOOK5 port.
//
// The simulator's WolfsslCrypto host shim uses OpenSSL's SHA1()/SHA256() on
// non-Apple hosts.  OBOOK5 is a musl target without OpenSSL headers, so this
// header supplies the same entry points.  Only the two one-shot calls the
// shim needs are provided; there are no EVP/context APIs.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace obook5_sha_detail {

inline uint32_t rol(uint32_t v, unsigned n) {
  return (v << n) | (v >> (32 - n));
}

inline uint32_t ror(uint32_t v, unsigned n) {
  return (v >> n) | (v << (32 - n));
}

inline uint32_t load_be32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) |
         (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

inline void store_be32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v >> 24);
  p[1] = static_cast<uint8_t>(v >> 16);
  p[2] = static_cast<uint8_t>(v >> 8);
  p[3] = static_cast<uint8_t>(v);
}

inline void sha1_block(uint32_t h[5], const uint8_t block[64]) {
  uint32_t w[80];
  for (int i = 0; i < 16; ++i) w[i] = load_be32(block + i * 4);
  for (int i = 16; i < 80; ++i)
    w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

  uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
  for (int i = 0; i < 80; ++i) {
    uint32_t f, k;
    if (i < 20) {
      f = (b & c) | (~b & d);
      k = 0x5A827999u;
    } else if (i < 40) {
      f = b ^ c ^ d;
      k = 0x6ED9EBA1u;
    } else if (i < 60) {
      f = (b & c) | (b & d) | (c & d);
      k = 0x8F1BBCDCu;
    } else {
      f = b ^ c ^ d;
      k = 0xCA62C1D6u;
    }
    const uint32_t tmp = rol(a, 5) + f + e + k + w[i];
    e = d;
    d = c;
    c = rol(b, 30);
    b = a;
    a = tmp;
  }
  h[0] += a;
  h[1] += b;
  h[2] += c;
  h[3] += d;
  h[4] += e;
}

inline void sha256_block(uint32_t h[8], const uint8_t block[64]) {
  static constexpr uint32_t k[64] = {
      0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
      0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
      0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
      0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
      0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
      0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
      0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
      0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
      0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
      0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
      0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
      0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
      0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

  uint32_t w[64];
  for (int i = 0; i < 16; ++i) w[i] = load_be32(block + i * 4);
  for (int i = 16; i < 64; ++i) {
    const uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
  uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
  for (int i = 0; i < 64; ++i) {
    const uint32_t s1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
    const uint32_t ch = (e & f) ^ (~e & g);
    const uint32_t temp1 = hh + s1 + ch + k[i] + w[i];
    const uint32_t s0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
    const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t temp2 = s0 + maj;
    hh = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }
  h[0] += a;
  h[1] += b;
  h[2] += c;
  h[3] += d;
  h[4] += e;
  h[5] += f;
  h[6] += g;
  h[7] += hh;
}

}  // namespace obook5_sha_detail

// OpenSSL-style incremental contexts.  The simulator's mbedtls/sha256.h shim
// uses SHA256_CTX + SHA256_Init/Update/Final; upstream's WeRead client reaches
// it through that shim.
typedef struct SHA256_CTX {
  uint32_t state[8];
  uint64_t count;
  uint8_t buffer[64];
} SHA256_CTX;

typedef struct SHA1_CTX {
  uint32_t state[5];
  uint64_t count;
  uint8_t buffer[64];
} SHA1_CTX;

inline int SHA256_Init(SHA256_CTX* ctx) {
  if (!ctx) return 0;
  ctx->state[0] = 0x6a09e667u;
  ctx->state[1] = 0xbb67ae85u;
  ctx->state[2] = 0x3c6ef372u;
  ctx->state[3] = 0xa54ff53au;
  ctx->state[4] = 0x510e527fu;
  ctx->state[5] = 0x9b05688cu;
  ctx->state[6] = 0x1f83d9abu;
  ctx->state[7] = 0x5be0cd19u;
  ctx->count = 0;
  std::memset(ctx->buffer, 0, sizeof(ctx->buffer));
  return 1;
}

inline int SHA256_Update(SHA256_CTX* ctx, const void* data, size_t len) {
  if (!ctx || (!data && len)) return 0;
  using namespace obook5_sha_detail;
  const uint8_t* input = static_cast<const uint8_t*>(data);
  size_t index = static_cast<size_t>(ctx->count & 63);
  ctx->count += len;
  size_t offset = 0;
  if (index) {
    const size_t fill = 64 - index;
    if (len < fill) {
      std::memcpy(ctx->buffer + index, input, len);
      return 1;
    }
    std::memcpy(ctx->buffer + index, input, fill);
    sha256_block(ctx->state, ctx->buffer);
    offset = fill;
  }
  while (len - offset >= 64) {
    sha256_block(ctx->state, input + offset);
    offset += 64;
  }
  if (offset < len) std::memcpy(ctx->buffer, input + offset, len - offset);
  return 1;
}

inline int SHA256_Final(unsigned char out[32], SHA256_CTX* ctx) {
  if (!ctx || !out) return 0;
  using namespace obook5_sha_detail;
  const uint64_t bit_len = ctx->count * 8;
  uint8_t pad[72] = {0x80};
  const size_t index = static_cast<size_t>(ctx->count & 63);
  const size_t pad_len = index < 56 ? 56 - index : 120 - index;
  for (int i = 0; i < 8; ++i)
    pad[pad_len + 7 - i] = static_cast<uint8_t>(bit_len >> (i * 8));
  SHA256_Update(ctx, pad, pad_len + 8);
  for (int i = 0; i < 8; ++i) store_be32(out + i * 4, ctx->state[i]);
  return 1;
}

inline int SHA1_Init(SHA1_CTX* ctx) {
  if (!ctx) return 0;
  ctx->state[0] = 0x67452301u;
  ctx->state[1] = 0xEFCDAB89u;
  ctx->state[2] = 0x98BADCFEu;
  ctx->state[3] = 0x10325476u;
  ctx->state[4] = 0xC3D2E1F0u;
  ctx->count = 0;
  std::memset(ctx->buffer, 0, sizeof(ctx->buffer));
  return 1;
}

inline int SHA1_Update(SHA1_CTX* ctx, const void* data, size_t len) {
  if (!ctx || (!data && len)) return 0;
  using namespace obook5_sha_detail;
  const uint8_t* input = static_cast<const uint8_t*>(data);
  size_t index = static_cast<size_t>(ctx->count & 63);
  ctx->count += len;
  size_t offset = 0;
  if (index) {
    const size_t fill = 64 - index;
    if (len < fill) {
      std::memcpy(ctx->buffer + index, input, len);
      return 1;
    }
    std::memcpy(ctx->buffer + index, input, fill);
    sha1_block(ctx->state, ctx->buffer);
    offset = fill;
  }
  while (len - offset >= 64) {
    sha1_block(ctx->state, input + offset);
    offset += 64;
  }
  if (offset < len) std::memcpy(ctx->buffer, input + offset, len - offset);
  return 1;
}

inline int SHA1_Final(unsigned char out[20], SHA1_CTX* ctx) {
  if (!ctx || !out) return 0;
  using namespace obook5_sha_detail;
  const uint64_t bit_len = ctx->count * 8;
  uint8_t pad[72] = {0x80};
  const size_t index = static_cast<size_t>(ctx->count & 63);
  const size_t pad_len = index < 56 ? 56 - index : 120 - index;
  for (int i = 0; i < 8; ++i)
    pad[pad_len + 7 - i] = static_cast<uint8_t>(bit_len >> (i * 8));
  SHA1_Update(ctx, pad, pad_len + 8);
  for (int i = 0; i < 5; ++i) store_be32(out + i * 4, ctx->state[i]);
  return 1;
}

inline unsigned char* SHA1(const unsigned char* data, size_t len,
                           unsigned char* out) {
  using namespace obook5_sha_detail;
  uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u,
                   0xC3D2E1F0u};
  size_t offset = 0;
  while (len - offset >= 64) {
    sha1_block(h, data + offset);
    offset += 64;
  }
  uint8_t tail[128] = {};
  const size_t remaining = len - offset;
  if (remaining) std::memcpy(tail, data + offset, remaining);
  const uint64_t bit_len = static_cast<uint64_t>(len) * 8;
  if (remaining < 56) {
    tail[remaining] = 0x80;
    for (int i = 0; i < 8; ++i)
      tail[63 - i] = static_cast<uint8_t>(bit_len >> (i * 8));
    sha1_block(h, tail);
  } else {
    tail[remaining] = 0x80;
    for (int i = 0; i < 8; ++i)
      tail[127 - i] = static_cast<uint8_t>(bit_len >> (i * 8));
    sha1_block(h, tail);
    sha1_block(h, tail + 64);
  }
  for (int i = 0; i < 5; ++i) store_be32(out + i * 4, h[i]);
  return out;
}

inline unsigned char* SHA256(const unsigned char* data, size_t len,
                             unsigned char* out) {
  using namespace obook5_sha_detail;
  uint32_t h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                   0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
  size_t offset = 0;
  while (len - offset >= 64) {
    sha256_block(h, data + offset);
    offset += 64;
  }
  uint8_t tail[128] = {};
  const size_t remaining = len - offset;
  if (remaining) std::memcpy(tail, data + offset, remaining);
  const uint64_t bit_len = static_cast<uint64_t>(len) * 8;
  if (remaining < 56) {
    tail[remaining] = 0x80;
    for (int i = 0; i < 8; ++i)
      tail[63 - i] = static_cast<uint8_t>(bit_len >> (i * 8));
    sha256_block(h, tail);
  } else {
    tail[remaining] = 0x80;
    for (int i = 0; i < 8; ++i)
      tail[127 - i] = static_cast<uint8_t>(bit_len >> (i * 8));
    sha256_block(h, tail);
    sha256_block(h, tail + 64);
  }
  for (int i = 0; i < 8; ++i) store_be32(out + i * 4, h[i]);
  return out;
}
