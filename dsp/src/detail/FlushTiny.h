#pragma once
#include "detail/FpProfilePrivate.h"

#include <cstdint>
#include <cstring>

namespace brainscape::detail {

// Deterministic flush of recursive state (docs/design/determinism-profile.md §4.3).
// The engine runs with gradual underflow on every target, because x86 FTZ and Arm FZ
// flush different results (§4.2). Without help a decaying one-pole would then lock
// onto a permanent subnormal, whose trajectory depends on the flush mode and which x86
// pays for on every sample. So each state update snaps values below 1e-20 (about
// -400 dBFS) to zero. Applied per sample, never per block, so state stays independent
// of block splits.
inline constexpr uint32_t kTinyBits = 0x1E3CE508u;  // 1e-20f

// |x| < 1e-20f, decided on the bit pattern: for every input, NaN included, the same
// answer as the profile's x < 1e-20f && x > -1e-20f, without its two FP compares. With
// every post stage engaged, the compare form cost 28 % on x86 (MSVC) and this one 5 %;
// the cheapest form on the M7 is open question §8.3 Q5.
inline bool IsTiny(float x) noexcept {
  uint32_t u;
  std::memcpy(&u, &x, sizeof u);
  return (u & 0x7FFFFFFFu) < kTinyBits;
}

inline void FlushTiny(float& x) noexcept {
  if (IsTiny(x)) x = 0.f;
}

}  // namespace brainscape::detail
