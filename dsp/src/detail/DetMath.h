#pragma once
#include "detail/FpProfilePrivate.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#if !defined(__GNUC__) && !defined(__clang__)
#include <emmintrin.h>
#endif

// Deterministic in-tree math (docs/design/determinism-profile.md §3.9): replaces every
// libm transcendental the engine reaches, because libm results differ between targets
// and even between compile-time folding and run time. Kernels use only IEEE-754 basic
// operations, comparisons, integer/bit operations and exact conversions, compute in
// binary64 and round once to the float result. They put ONE floating-point operation
// per statement, so the evaluation order is fixed by the source even under a wrong
// contraction flag, and every coefficient is a hex-float literal (no decimal
// conversion latitude). Accuracy over the engine's argument domains: correctly
// rounded or within 1 ULP of the float result (tests/test_detmath.cpp).
//
// Domains and edge rules (§3.9) are part of every build; Debug builds also assert
// the domains, since an out-of-domain result is identical on every target and no
// golden hash would catch it.
//
// Only the exact helpers are inline. The kernels are defined once, in DetMath.cpp:
// inlined at every call site they nearly doubled the engine's Cortex-M7 .text, which
// competes for the 16 KiB instruction cache (§7.1), and the tests call the same
// compiled code the engine does.
namespace brainscape::detmath {

// ── Exact helpers ────────────────────────────────────────────────────────────────

inline float Abs(float x) noexcept {  // bit-exact fabsf: clears the sign bit only
  uint32_t u;
  std::memcpy(&u, &x, sizeof u);
  u &= 0x7FFFFFFFu;
  std::memcpy(&x, &u, sizeof u);
  return x;
}

// lround/llround semantics (round half away from zero), exact: x - trunc(x) is always
// representable. The domain keeps t ± 1 and the truncating conversion in range;
// out-of-range float-to-int conversion differs by ISA (§3.10).
inline int32_t RoundHalfAwayI32(double x) noexcept {
  assert(x > -2147483648.5 && x < 2147483647.5);
  const auto   t    = static_cast<int32_t>(x);
  const double frac = x - static_cast<double>(t);
  if (frac >= 0.5) return t + 1;
  if (frac <= -0.5) return t - 1;
  return t;
}
inline int64_t RoundHalfAwayI64(double x) noexcept {
  assert(x > -0x1p62 && x < 0x1p62);
  const auto   t    = static_cast<int64_t>(x);
  const double frac = x - static_cast<double>(t);
  if (frac >= 0.5) return t + 1;
  if (frac <= -0.5) return t - 1;
  return t;
}

// ceil for |x| < 2^31, exact.
inline double CeilSmall(double x) noexcept {
  assert(x > -2147483648.0 && x < 2147483648.0);
  const auto t = static_cast<double>(static_cast<int32_t>(x));
  return t < x ? t + 1.0 : t;
}

// x * 2^n for n in [-1022, 1023]: an exact power-of-two factor built from its bits.
inline double ScaleB(double x, int32_t n) noexcept {
  const uint64_t bits = static_cast<uint64_t>(static_cast<int64_t>(n) + 1023) << 52;
  double s;
  std::memcpy(&s, &bits, sizeof s);
  return x * s;
}

// IEEE-754 requires sqrt to be correctly rounded, so it is the one non-basic operation
// the profile allows. GCC/Clang lower the builtins to the instruction under the
// profile's -fno-math-errno; MSVC's std::sqrt guards the instruction with a C-runtime
// call that the symbol audit rejects (§6.3), hence the intrinsics.
#if defined(__GNUC__) || defined(__clang__)
inline float  SqrtF(float x) noexcept { return __builtin_sqrtf(x); }
inline double SqrtD(double x) noexcept { return __builtin_sqrt(x); }
#elif defined(_MSC_VER) && defined(_M_X64)
inline float SqrtF(float x) noexcept { return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(x))); }
inline double SqrtD(double x) noexcept {
  const __m128d v = _mm_set_sd(x);
  return _mm_cvtsd_f64(_mm_sqrt_sd(v, v));
}
#else
#error "brainscape DetMath: no IEEE square root for this target"
#endif

// ── Kernels (DetMath.cpp) ────────────────────────────────────────────────────────

// 2^x, e^x and e^x - 1. Edge rule on t = log2 of the result: the result exponent
// n = round(t) is clamped to [-1022, 1023]; below, the result is 0 (-1 for Expm1D),
// above, the largest finite binary64. NaN takes the low edge.
double Exp2D(double x) noexcept;
double ExpD(double x) noexcept;
double Expm1D(double x) noexcept;

// ln(x) and log2(x), domain x > 0 and finite (subnormals included).
double LogD(double x) noexcept;
double Log2D(double x) noexcept;

// sin and cos of x radians, |x| <= 2^19 * pi/2.
void   SinCosD(double x, double* s, double* c) noexcept;
double SinD(double x) noexcept;

// sin(pi*x) and cos(pi*x), x in half-turns, |x| < 2^29 (so 2x rounds inside
// RoundHalfAwayI32's domain): the reduction x - n/2 is exact, so tables carry no
// rounding of pi in the argument.
void   SinCosPi(double x, double* s, double* c) noexcept;
double CosPi(double x) noexcept;

// Float front-ends: one rounding from a binary64 kernel. Results beyond the float
// range clamp to FLT_MAX: the engine never makes infinity.
float Exp2F(float x) noexcept;
float LogF(float x) noexcept;  // domain x > 0 and finite
// x^y = 2^(y * log2 x), domain x >= 0 and finite. Edge rules: y = 0 or x = 1 gives 1;
// x = 0 gives 0 for y > 0 (and FLT_MAX for y < 0, the clamped pole).
float PowF(float x, float y) noexcept;

}  // namespace brainscape::detmath
