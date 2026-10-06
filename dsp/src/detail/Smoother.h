#pragma once
#include "detail/FpProfilePrivate.h"

#include "detail/DetMath.h"
#include "detail/FlushTiny.h"

namespace brainscape::detail {

// Per-sample one-pole with snap-on-stall (docs/design/grain-engine.md §3): the
// bare recurrence freezes short of its target once the increment rounds to a
// no-op, which would leave "1.0" never exactly 1.0 and break the bit-exact null
// contracts. Detecting `next == value` catches the stall at any magnitude. It also
// snaps once within 1e-20 of the target (determinism profile §4.3): a ramp to 0
// would otherwise crawl through ~5,000 subnormal samples before it stalls.
struct Smoother {
  float value = 0.f, target = 0.f, coef = 1.f;

  void SetTau(float tauMs, double sr) noexcept {
    // expm1, not 1-exp: the subtraction cancels to ~18 mantissa bits (review finding).
    coef = -static_cast<float>(detmath::Expm1D(-1.0 / (tauMs * 0.001 * sr)));
  }
  void  Prime(float v) noexcept { value = target = v; }
  float Next() noexcept {
    const float next = value + coef * (target - value);
    value            = (next == value || IsTiny(next - target)) ? target : next;
    return value;
  }
};

}  // namespace brainscape::detail
