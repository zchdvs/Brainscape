#pragma once
#include "detail/FpProfilePrivate.h"

#include <cassert>
#include <cfloat>
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

// ── exp family ───────────────────────────────────────────────────────────────────

// e^r for |r| <= 0.35 (Taylor to r^13; truncation < 5e-18 relative).
inline double ExpKernel(double r) noexcept {
  double p = 0x1.6124613a86d09p-33;  // 1/13!
  p = p * r;  p = p + 0x1.1eed8eff8d898p-29;  // 1/12!
  p = p * r;  p = p + 0x1.ae64567f544e4p-26;  // 1/11!
  p = p * r;  p = p + 0x1.27e4fb7789f5cp-22;  // 1/10!
  p = p * r;  p = p + 0x1.71de3a556c734p-19;  // 1/9!
  p = p * r;  p = p + 0x1.a01a01a01a01ap-16;  // 1/8!
  p = p * r;  p = p + 0x1.a01a01a01a01ap-13;  // 1/7!
  p = p * r;  p = p + 0x1.6c16c16c16c17p-10;  // 1/6!
  p = p * r;  p = p + 0x1.1111111111111p-7;   // 1/5!
  p = p * r;  p = p + 0x1.5555555555555p-5;   // 1/4!
  p = p * r;  p = p + 0x1.5555555555555p-3;   // 1/3!
  p = p * r;  p = p + 0x1.0000000000000p-1;   // 1/2!
  p = p * r;  p = p + 1.0;
  p = p * r;  p = p + 1.0;
  return p;
}

// Edge rule shared by the exp family, on t = log2 of the result: the result exponent
// n = round(t) is clamped to [-1022, 1023]; below, the result is 0, above, the largest
// finite binary64. The compares run before the integer conversion (whose out-of-range
// behavior differs by ISA) and send NaN to the low edge.
inline bool ExpUnderflows(double t) noexcept { return !(t > -1022.5); }
inline bool ExpOverflows(double t) noexcept { return t >= 1023.5; }

// 2^x. n = nearest integer; f = x - n is exact.
inline double Exp2D(double x) noexcept {
  if (ExpUnderflows(x)) return 0.0;
  if (ExpOverflows(x)) return DBL_MAX;
  const int32_t n = RoundHalfAwayI32(x);
  const double  f = x - static_cast<double>(n);
  const double  r = f * 0x1.62e42fefa39efp-1;  // ln 2
  return ScaleB(ExpKernel(r), n);
}

// e^x. Cody-Waite reduction with a 32-bit-trailing-zero ln2_hi, so n * ln2_hi is
// exact for |n| < 2^20.
inline double ExpD(double x) noexcept {
  const double t = x * 0x1.71547652b82fep+0;  // log2(e)
  if (ExpUnderflows(t)) return 0.0;
  if (ExpOverflows(t)) return DBL_MAX;
  const int32_t n  = RoundHalfAwayI32(t);
  const double  nd = static_cast<double>(n);
  const double  hi = nd * 0x1.62e42fee00000p-1;   // ln2_hi (exact product)
  const double  lo = nd * 0x1.a39ef35793c76p-33;  // ln2_lo
  double r = x - hi;
  r = r - lo;
  return ScaleB(ExpKernel(r), n);
}

// e^x - 1 without cancellation for small |x| (direct series below 0.25); -1 where
// ExpD underflows.
inline double Expm1D(double x) noexcept {
  const double ax = x < 0.0 ? -x : x;
  if (ax < 0.25) {
    // x + x^2/2! + ... + x^15/15!: truncation < 1e-19 relative at |x| = 0.25.
    double p = 0x1.ae7f3e733b81fp-41;  // 1/15!
    p = p * x;  p = p + 0x1.93974a8c07c9dp-37;  // 1/14!
    p = p * x;  p = p + 0x1.6124613a86d09p-33;  // 1/13!
    p = p * x;  p = p + 0x1.1eed8eff8d898p-29;  // 1/12!
    p = p * x;  p = p + 0x1.ae64567f544e4p-26;  // 1/11!
    p = p * x;  p = p + 0x1.27e4fb7789f5cp-22;  // 1/10!
    p = p * x;  p = p + 0x1.71de3a556c734p-19;  // 1/9!
    p = p * x;  p = p + 0x1.a01a01a01a01ap-16;  // 1/8!
    p = p * x;  p = p + 0x1.a01a01a01a01ap-13;  // 1/7!
    p = p * x;  p = p + 0x1.6c16c16c16c17p-10;  // 1/6!
    p = p * x;  p = p + 0x1.1111111111111p-7;   // 1/5!
    p = p * x;  p = p + 0x1.5555555555555p-5;   // 1/4!
    p = p * x;  p = p + 0x1.5555555555555p-3;   // 1/3!
    p = p * x;  p = p + 0x1.0000000000000p-1;   // 1/2!
    p = p * x;                                  // x/2 + x^2/6 + ...
    p = p * x;                                  // x^2/2 + ...
    return x + p;
  }
  const double e = ExpD(x);
  return e - 1.0;
}

// ── log family ───────────────────────────────────────────────────────────────────

// ln(x), domain x > 0 and finite. x = m * 2^e with m in [sqrt(1/2), sqrt(2));
// ln(m) = 2 atanh(s), s = (m-1)/(m+1), |s| <= 0.1716 (series to s^25).
inline double LogD(double x) noexcept {
  assert(x > 0.0 && x <= DBL_MAX);
  uint64_t bits;
  std::memcpy(&bits, &x, sizeof bits);
  int32_t e = 0;
  if ((bits >> 52) == 0) {  // subnormal: normalize exactly
    const double xs = x * 0x1p54;
    std::memcpy(&bits, &xs, sizeof bits);
    e = -54;
  }
  e = e + static_cast<int32_t>((bits >> 52) & 0x7FFu) - 1023;
  bits = (bits & 0x000FFFFFFFFFFFFFull) | 0x3FF0000000000000ull;  // m in [1, 2)
  double m;
  std::memcpy(&m, &bits, sizeof m);
  if (m > 0x1.6a09e667f3bcdp+0) {  // sqrt(2)
    m = m * 0.5;                    // exact
    e = e + 1;
  }
  const double num = m - 1.0;  // exact (Sterbenz)
  const double den = m + 1.0;
  const double s   = num / den;
  const double z   = s * s;
  // 2/(2k+1) for k = 12..1, Horner in z.
  double p = 0x1.47ae147ae147bp-4;          // 2/25
  p = p * z;  p = p + 0x1.642c8590b2164p-4;  // 2/23
  p = p * z;  p = p + 0x1.8618618618618p-4;  // 2/21
  p = p * z;  p = p + 0x1.af286bca1af28p-4;  // 2/19
  p = p * z;  p = p + 0x1.e1e1e1e1e1e1ep-4;  // 2/17
  p = p * z;  p = p + 0x1.1111111111111p-3;  // 2/15
  p = p * z;  p = p + 0x1.3b13b13b13b14p-3;  // 2/13
  p = p * z;  p = p + 0x1.745d1745d1746p-3;  // 2/11
  p = p * z;  p = p + 0x1.c71c71c71c71cp-3;  // 2/9
  p = p * z;  p = p + 0x1.2492492492492p-2;  // 2/7
  p = p * z;  p = p + 0x1.999999999999ap-2;  // 2/5
  p = p * z;  p = p + 0x1.5555555555555p-1;  // 2/3
  p = p * z;                                 // (2/3)z + ...
  p = p * s;                                 // tail of 2 atanh(s) beyond 2s
  const double s2  = s + s;                  // exact
  const double lnm = s2 + p;
  const double ed  = static_cast<double>(e);
  const double hi  = ed * 0x1.62e42fee00000p-1;   // exact
  const double lo  = ed * 0x1.a39ef35793c76p-33;
  double r = lo + lnm;
  r = r + hi;
  return r;
}

inline double Log2D(double x) noexcept {
  const double l = LogD(x);
  return l * 0x1.71547652b82fep+0;  // 1/ln2
}

// ── sin / cos ────────────────────────────────────────────────────────────────────

// sin(r), cos(r) for |r| <= pi/4 (+tiny): Taylor to r^17 / r^18, truncation < 1e-19
// relative.
inline double SinKernel(double r) noexcept {
  const double z = r * r;
  double p = 0x1.952c77030ad4ap-49;          // 1/17!
  p = p * z;  p = p - 0x1.ae7f3e733b81fp-41;  // 1/15!
  p = p * z;  p = p + 0x1.6124613a86d09p-33;  // 1/13!
  p = p * z;  p = p - 0x1.ae64567f544e4p-26;  // 1/11!
  p = p * z;  p = p + 0x1.71de3a556c734p-19;  // 1/9!
  p = p * z;  p = p - 0x1.a01a01a01a01ap-13;  // 1/7!
  p = p * z;  p = p + 0x1.1111111111111p-7;   // 1/5!
  p = p * z;  p = p - 0x1.5555555555555p-3;   // 1/3!
  p = p * z;
  p = p * r;
  return r + p;
}
inline double CosKernel(double r) noexcept {
  const double z = r * r;
  double p = 0x1.6827863b97d97p-53;           // 1/18!
  p = p * z;  p = p - 0x1.ae7f3e733b81fp-45;  // 1/16!
  p = p * z;  p = p + 0x1.93974a8c07c9dp-37;  // 1/14!
  p = p * z;  p = p - 0x1.1eed8eff8d898p-29;  // 1/12!
  p = p * z;  p = p + 0x1.27e4fb7789f5cp-22;  // 1/10!
  p = p * z;  p = p - 0x1.a01a01a01a01ap-16;  // 1/8!
  p = p * z;  p = p + 0x1.6c16c16c16c17p-10;  // 1/6!
  p = p * z;  p = p - 0x1.5555555555555p-5;   // 1/4!
  p = p * z;  p = p + 0x1.0000000000000p-1;   // 1/2!
  p = p * z;                                  // z/2 - z^2/24 + ...
  return 1.0 - p;
}

// Quadrant selection shared by SinCosD and SinCosPi.
inline void SinCosQuadrant(int32_t n, double sk, double ck, double* s, double* c) noexcept {
  switch (static_cast<uint32_t>(n) & 3u) {
    case 0:  *s = sk;  *c = ck;  break;
    case 1:  *s = ck;  *c = -sk; break;
    case 2:  *s = -sk; *c = -ck; break;
    default: *s = -ck; *c = sk;  break;
  }
}

// sin and cos of x radians, |x| <= 2^19 * pi/2. Cody-Waite three-part pi/2 (fdlibm's
// split: n*P1 and n*P2 are exact products for |n| < 2^20).
inline void SinCosD(double x, double* s, double* c) noexcept {
  assert(x > -823550.0 && x < 823550.0);
  const double  t  = x * 0x1.45f306dc9c883p-1;  // 2/pi
  const int32_t n  = RoundHalfAwayI32(t);
  const double  nd = static_cast<double>(n);
  const double  p1 = nd * 0x1.921fb54400000p+0;
  const double  p2 = nd * 0x1.0b4611a600000p-34;
  const double  p3 = nd * 0x1.3198a2e037073p-69;
  double r = x - p1;
  r = r - p2;
  r = r - p3;
  SinCosQuadrant(n, SinKernel(r), CosKernel(r), s, c);
}
inline double SinD(double x) noexcept {
  double s, c;
  SinCosD(x, &s, &c);
  return s;
}

// sin(pi*x) and cos(pi*x), x in half-turns, |x| < 2^30: the reduction x - n/2 is exact,
// so tables carry no rounding of pi in the argument.
inline void SinCosPi(double x, double* s, double* c) noexcept {
  assert(x > -0x1p30 && x < 0x1p30);
  const double  t = x + x;                      // exact
  const int32_t n = RoundHalfAwayI32(t);
  const double  h = static_cast<double>(n) * 0.5;  // exact
  const double  f = x - h;                      // exact, |f| <= 1/4
  const double  r = f * 0x1.921fb54442d18p+1;   // pi
  SinCosQuadrant(n, SinKernel(r), CosKernel(r), s, c);
}
inline double CosPi(double x) noexcept {
  double s, c;
  SinCosPi(x, &s, &c);
  return c;
}

// ── float front-ends: one rounding from a binary64 kernel ────────────────────────

// Results beyond the float range clamp to FLT_MAX: the engine never makes infinity.
inline float RoundToFloat(double r) noexcept {
  return static_cast<float>(r < static_cast<double>(FLT_MAX) ? r : static_cast<double>(FLT_MAX));
}

inline float Exp2F(float x) noexcept { return RoundToFloat(Exp2D(static_cast<double>(x))); }

// Domain x > 0 and finite.
inline float LogF(float x) noexcept { return static_cast<float>(LogD(static_cast<double>(x))); }

// x^y = 2^(y * log2 x), domain x >= 0 and finite. Edge rules: y = 0 or x = 1 gives 1;
// x = 0 gives 0 for y > 0 (and FLT_MAX for y < 0, the clamped pole).
inline float PowF(float x, float y) noexcept {
  assert(x >= 0.0f && x <= FLT_MAX);
  if (y == 0.0f || x == 1.0f) return 1.0f;
  if (x == 0.0f) return y > 0.0f ? 0.0f : FLT_MAX;
  const double l = Log2D(static_cast<double>(x));
  const double t = static_cast<double>(y) * l;
  return RoundToFloat(Exp2D(t));
}

}  // namespace brainscape::detmath
