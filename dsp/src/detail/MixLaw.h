#pragma once
#include "detail/FpProfilePrivate.h"

// The Mix law (docs/design/mode-compiler.md §7.1, R3b; sound revision 3, owner question Q13):
// the dry signal stays at unity up to the knob's middle and the wet signal is at unity from it,
//
//   dry = min(1, 2(1 − m)),  wet = min(1, 2m),
//
// for the smoothed, canonical Mix m in [0, 1]. Under revision 2's linear crossfade, (1 − m) and
// m, all 84 first-set renders at their stored Mix of 0.35–0.55 played 1.2–6.9 LU quieter engaged
// than bypassed (K-weighted; the re-measurement in mode-compiler-record.md §2.2); under this law
// engaging takes no dry level away below the middle, and a decorrelated wet at the dry's level
// adds to it.
//
// Exact by construction, so it needs no DetMath: 2m is exact (a power-of-two scale, and 2m
// at most 2); for m >= 0.5, 1 − m is exact (Sterbenz: m/2 <= 1 <= 2m) and so is 2(1 − m); below
// the middle the rounded 1 − m is at least 0.5 and the min returns exactly 1. Hence m = 0 gives
// (1, 0), m = 0.5 gives (1, 1) and m = 1 gives (0, 1), each gain is monotonic in m, and at the
// endpoints the engine's two-multiply mix, dry·gd + wet·gw, does revision 2's arithmetic bit for
// bit (dry·1 + wet·0, dry·0 + wet·1), which contract #2's null and "Mix 0 is the dry input" need.
//
// Bit for bit up to the sign of a zero: x·0 is a zero with x's sign, and −0 + +0 is +0. So at
// Mix 0 a −0 input sample comes out +0 where the wet sample's sign is clear (−0 where it is set),
// and at Mix 1 a −0 wet sample comes out +0 where the dry's sign is clear. Every other sample at
// Mix 0, subnormals and values near ±FLT_MAX included, is the input's bits, and every other
// finite wet sample at Mix 1 is the wet's; revisions 1 and 2 did the same. test_modes.cpp pins
// the Mix 0 case on hostile input.
namespace brainscape::detail {

struct MixGains {
  float dry;
  float wet;
};

inline MixGains MixLaw(float m) noexcept {
  const float d = 2.0f * (1.0f - m);
  const float w = 2.0f * m;
  return MixGains{d < 1.0f ? d : 1.0f, w < 1.0f ? w : 1.0f};
}

}  // namespace brainscape::detail
