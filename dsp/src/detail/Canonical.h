#pragma once
#include "detail/FpProfilePrivate.h"

#include <cstdint>
#include <cstring>

#include "brainscape/Params.h"

namespace brainscape::detail {

// The canonical plain value (determinism profile §3.7), decided on the bit pattern:
// under DAZ, which hosts set, comparisons treat subnormals as zero, and a
// comparison-based rule stored different bits and changed every grain's first sample.
// Once non-finite and subnormal values are gone, the clamp compares normal numbers.
// The one rule behind SetParam, Canonicalize and the taper functions (ParamDisplay.cpp);
// callers run it inside the FP environment guard.
inline float CanonicalValue(const ParamDescriptor& d, float v) noexcept {
  uint32_t u;
  std::memcpy(&u, &v, sizeof u);
  const uint32_t exponent = u & 0x7F800000u;
  if (exponent == 0x7F800000u) return d.min;  // NaN, ±inf
  if (exponent == 0u) v = 0.0f;               // ±0, subnormals
  if (v < d.min) v = d.min;
  if (v > d.max) v = d.max;
  return v;
}

}  // namespace brainscape::detail
