#include "detail/FpProfilePrivate.h"

#include "brainscape/Sha256.h"

// The digest core moved here from dsp/tests/golden/Sha256.h (mode-compiler.md §6.3, lane B):
// integer-only, no allocation, no library call. The firmware links it with the package
// decoder; the golden harness and the plugin tests hash renders with it.
namespace brainscape {

namespace {

constexpr uint32_t kRound[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

inline uint32_t Rotr(uint32_t x, int r) noexcept { return (x >> r) | (x << (32 - r)); }

}  // namespace

void Sha256Hasher::Reset() noexcept {
  h_[0]  = 0x6a09e667u;
  h_[1]  = 0xbb67ae85u;
  h_[2]  = 0x3c6ef372u;
  h_[3]  = 0xa54ff53au;
  h_[4]  = 0x510e527fu;
  h_[5]  = 0x9b05688cu;
  h_[6]  = 0x1f83d9abu;
  h_[7]  = 0x5be0cd19u;
  bytes_ = 0;
  used_  = 0;
}

void Sha256Hasher::Update(const void* data, size_t length) noexcept {
  const auto* p = static_cast<const uint8_t*>(data);
  bytes_ += length;
  if (used_ > 0) {
    while (length > 0 && used_ < 64) {
      buffer_[used_++] = *p++;
      --length;
    }
    if (used_ < 64) return;
    Block(buffer_);
    used_ = 0;
  }
  for (; length >= 64; p += 64, length -= 64) Block(p);
  while (length > 0) {
    buffer_[used_++] = *p++;
    --length;
  }
}

void Sha256Hasher::Final(uint8_t digest[kDigestBytes]) noexcept {
  const uint64_t bits = bytes_ * 8u;
  const uint8_t  pad = 0x80, zero = 0;
  Update(&pad, 1);
  while (used_ != 56) Update(&zero, 1);
  uint8_t length[8];
  for (int i = 0; i < 8; ++i) length[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
  Update(length, 8);
  for (int i = 0; i < 8; ++i) {
    for (int j = 0; j < 4; ++j) {
      digest[4 * i + j] = static_cast<uint8_t>(h_[i] >> (24 - 8 * j));
    }
  }
  Reset();
}

void Sha256Hasher::Digest(const void* data, size_t length,
                          uint8_t digest[kDigestBytes]) noexcept {
  Sha256Hasher sha;
  sha.Update(data, length);
  sha.Final(digest);
}

void Sha256Hasher::Block(const uint8_t* p) noexcept {
  uint32_t w[64];
  for (int i = 0; i < 16; ++i) {
    w[i] = (uint32_t{p[4 * i]} << 24) | (uint32_t{p[4 * i + 1]} << 16) |
           (uint32_t{p[4 * i + 2]} << 8) | uint32_t{p[4 * i + 3]};
  }
  for (int i = 16; i < 64; ++i) {
    const uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i]              = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6],
           hh = h_[7];
  for (int i = 0; i < 64; ++i) {
    const uint32_t t1 = hh + (Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25)) + ((e & f) ^ (~e & g)) +
                        kRound[i] + w[i];
    const uint32_t t2 = (Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    hh                = g;
    g                 = f;
    f                 = e;
    e                 = d + t1;
    d                 = c;
    c                 = b;
    b                 = a;
    a                 = t1 + t2;
  }
  h_[0] += a;
  h_[1] += b;
  h_[2] += c;
  h_[3] += d;
  h_[4] += e;
  h_[5] += f;
  h_[6] += g;
  h_[7] += hh;
}

}  // namespace brainscape
