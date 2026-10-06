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

// Neither the rounding mode nor a flush mode changes the result: scaling by 2^23 is
// exact in binary64; d +- 0.5 is exact for 2^-7 <= |d| <= 2^23 (d has at most 24
// significant bits), and below 2^-7 any rounding of it stays inside (-1, 1); the
// conversion truncates (never lrint, which follows the rounding mode); and |i| <= 2^23
// converts back exactly. A subnormal x gives 0 whether or not DAZ zeroes it first.
float ConditionInput24(float x) noexcept {
  const uint32_t u = Bits(x);
  if ((u & kExponentMask) == kExponentMask && (u & 0x007FFFFFu) != 0u) return 0.0f;  // NaN
  double d = static_cast<double>(x) * 0x1p23;
  if (d > 0x1p23 - 1.0) d = 0x1p23 - 1.0;  // also saturates +inf
  if (d < -0x1p23) d = -0x1p23;
  const auto i = static_cast<int32_t>(d >= 0.0 ? d + 0.5 : d - 0.5);
  return static_cast<float>(i) * 0x1p-23f;
}

void ConditionInput24(const float* in, float* out, size_t numSamples) noexcept {
  for (size_t i = 0; i < numSamples; ++i) out[i] = ConditionInput24(in[i]);
}

}  // namespace brainscape
