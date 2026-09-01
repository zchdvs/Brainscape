#pragma once
#include <cmath>

namespace brainscape::detail {

// Per-sample one-pole with snap-on-stall (docs/design/grain-engine.md §3): the
// bare recurrence freezes short of its target once the increment rounds to a
// no-op, which would leave "1.0" never exactly 1.0 and break the bit-exact null
// contracts. Detecting `next == value` catches the stall at any magnitude.
struct Smoother {
  float value = 0.f, target = 0.f, coef = 1.f;

  void SetTau(float tauMs, double sr) noexcept {
    // expm1, not 1-exp: the subtraction cancels to ~18 mantissa bits (review finding).
    coef = -static_cast<float>(std::expm1(-1.0 / (tauMs * 0.001 * sr)));
  }
  void  Prime(float v) noexcept { value = target = v; }
  float Next() noexcept {
    const float next = value + coef * (target - value);
    value            = (next == value) ? target : next;
    return value;
  }
};

}  // namespace brainscape::detail
