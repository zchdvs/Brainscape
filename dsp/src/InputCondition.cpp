#include "detail/FpProfilePrivate.h"

#include "brainscape/InputCondition.h"

#include <cstdint>
#include <cstring>

namespace brainscape {

namespace {

constexpr uint32_t kExponentMask = 0x7F800000u;

inline uint32_t Bits(float x) noexcept {
  uint32_t u;
  std::memcpy(&u, &x, sizeof u);
  return u;
}

}  // namespace

// Integer operations only: the value is never touched by FP arithmetic, so DAZ cannot
// zero a subnormal on its way through.
float SanitizeInput(float x) noexcept {
  uint32_t u = Bits(x);
  if ((u & kExponentMask) == kExponentMask) u = 0u;
  std::memcpy(&x, &u, sizeof u);
  return x;
}

void SanitizeInput(const float* in, float* out, size_t numSamples) noexcept {
  for (size_t i = 0; i < numSamples; ++i) {
    uint32_t u;
    std::memcpy(&u, in + i, sizeof u);
    if ((u & kExponentMask) == kExponentMask) u = 0u;
    std::memcpy(out + i, &u, sizeof u);
  }
}

// The rounding is done in integers on the bit pattern: an FP version (scale to binary64,
// add a half, truncate) gave the same results but trapped on a subnormal operand or an
// inexact conversion when a host had unmasked those exceptions (review finding). The
// only FP operations left, converting |i| <= 2^23 and scaling by 2^-23, are exact.
float ConditionInput24(float x) noexcept {
  const uint32_t u        = Bits(x);
  const uint32_t biased   = (u & kExponentMask) >> 23;
  const uint32_t fraction = u & 0x007FFFFFu;
  if (biased == 0xFFu && fraction != 0u) return 0.0f;  // NaN
  // |x| * 2^23 = m * 2^(biased - 127) with the 24-bit significand m. At biased >= 127
  // (|x| >= 1, and ±inf) it reaches the clamp; below, round half away from zero by
  // adding half of the shifted-out weight. Past a shift of 25, m / 2^shift < 1/2, and
  // subnormals (biased 0) land there too.
  uint32_t q = 1u << 23;
  if (biased < 127u) {
    const uint32_t shift = 127u - biased;
    q = shift > 25u ? 0u : ((fraction | 0x00800000u) + (1u << (shift - 1u))) >> shift;
  }
  const bool negative = (u >> 31) != 0u;
  if (!negative && q > (1u << 23) - 1u) q = (1u << 23) - 1u;
  const auto i = negative ? -static_cast<int32_t>(q) : static_cast<int32_t>(q);
  return static_cast<float>(i) * 0x1p-23f;
}

void ConditionInput24(const float* in, float* out, size_t numSamples) noexcept {
  for (size_t i = 0; i < numSamples; ++i) out[i] = ConditionInput24(in[i]);
}

}  // namespace brainscape
