// DetMath accuracy and edge rules over the engine's real argument domains
// (docs/design/determinism-profile.md §3.9). Sampled here, on every pull request;
// the exhaustive sweeps belong to the nightly job. The reference is the host libm in
// binary64, rounded once to float: within 1 ULP of the correctly rounded result.
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

#include "DetMathDigest.h"
#include "catch.hpp"
#include "detail/DetMath.h"
#include "detail/GrainMath.h"

using namespace brainscape;

namespace {

int64_t Ordered(float x) {
  int32_t i;
  std::memcpy(&i, &x, sizeof i);
  return i < 0 ? int64_t{INT32_MIN} - i : i;
}
int64_t UlpDistance(float a, float b) {
  const int64_t d = Ordered(a) - Ordered(b);
  return d < 0 ? -d : d;
}
int64_t OrderedD(double x) {
  int64_t i;
  std::memcpy(&i, &x, sizeof i);
  return i < 0 ? INT64_MIN - i : i;
}
int64_t UlpDistanceD(double a, double b) {
  const int64_t d = OrderedD(a) - OrderedD(b);
  return d < 0 ? -d : d;
}
// sin(pi x), cos(pi x) from libm on an exactly reduced argument, so zero crossings stay
// exact zeros and the reference does not carry the rounding of pi times a large x.
void RefSinCosPi(double x, double* s, double* c) {
  const double n  = std::nearbyint(x + x);
  const double r  = 3.14159265358979323846 * (x - 0.5 * n);
  const double sr = std::sin(r), cr = std::cos(r);
  switch (static_cast<int64_t>(n) & 3) {
    case 0:  *s = sr;  *c = cr;  break;
    case 1:  *s = cr;  *c = -sr; break;
    case 2:  *s = -sr; *c = -cr; break;
    default: *s = -cr; *c = sr;  break;
  }
}

uint32_t Bits(float x) {
  uint32_t u;
  std::memcpy(&u, &x, sizeof u);
  return u;
}

// Worst float-ULP distance between a DetMath result and the libm reference.
struct UlpStat {
  int64_t worst = 0;
  double  worstArg = 0.0;
  void Add(float got, double ref, double arg) {
    const int64_t d = UlpDistance(got, static_cast<float>(ref));
    if (d > worst) {
      worst    = d;
      worstArg = arg;
    }
  }
};

}  // namespace

TEST_CASE("DetMath exact helpers match their libm counterparts") {
  using namespace detmath;
  REQUIRE(Bits(Abs(-0.0f)) == 0u);
  REQUIRE(Abs(-3.5f) == 3.5f);
  REQUIRE(Bits(Abs(-FLT_TRUE_MIN)) == 1u);

  // Halves, their neighbours and random values; counted, then checked once.
  size_t   mismatches = 0;
  uint32_t x          = 1u;
  auto check = [&](double v, double scale) {
    mismatches += RoundHalfAwayI32(v) != std::lround(v);
    mismatches += RoundHalfAwayI64(v * scale) != std::llround(v * scale);
    mismatches += CeilSmall(v) != std::ceil(v);
  };
  for (int32_t k = -200000; k <= 200000; ++k) {
    const double half = static_cast<double>(k) * 0.5;
    check(half, 4096.0);
    check(std::nextafter(half, -1e9), 4096.0);
    check(std::nextafter(half, 1e9), 4096.0);
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    check((static_cast<double>(x) - 2147483648.0) / 1024.0, 0x1p20);
  }
  REQUIRE(mismatches == 0);
  REQUIRE(SqrtF(2.0f) == std::sqrt(2.0f));
  REQUIRE(SqrtD(2.0) == std::sqrt(2.0));
}

TEST_CASE("DetMath exp2 is within 1 ULP over the pitch and trim domains") {
  UlpStat st;
  // SemitonesToRatio: st/12 for the composed pitch, |st| <= 24.
  for (int32_t k = -(1 << 17); k <= (1 << 17); ++k) {
    const float x = static_cast<float>(k) * 0x1p-16f;
    st.Add(detmath::Exp2F(x), std::exp2(static_cast<double>(x)), x);
  }
  // OutTrimDb: dB * log2(10)/20 for dB in [-24, 24].
  for (int32_t k = -24 * 256; k <= 24 * 256; ++k) {
    const float x = (static_cast<float>(k) * 0x1p-8f) * 0.16609640474436813f;
    st.Add(detmath::Exp2F(x), std::exp2(static_cast<double>(x)), x);
  }
  INFO("worst at x = " << st.worstArg);
  REQUIRE(st.worst <= 1);
  for (int32_t n = -126; n <= 127; ++n) {
    REQUIRE(detmath::Exp2F(static_cast<float>(n)) == std::ldexp(1.0f, n));
  }
  REQUIRE(grainmath::SemitonesToRatio(12.0f) == 2.0f);
  REQUIRE(grainmath::SemitonesToRatio(-24.0f) == 0.25f);
  REQUIRE(grainmath::SemitonesToRatio(0.0f) == 1.0f);
}

TEST_CASE("DetMath log is within 1 ULP over the jitter draw's domain") {
  UlpStat st;
  // Granular.cpp: log(1 - u * 0.999f), u = k / 2^24 from the counter RNG.
  for (uint32_t k = 0; k < (1u << 24); k += 61) {
    const float u = static_cast<float>(k) * (1.0f / 16777216.0f);
    const float m = u * 0.999f;
    const float x = 1.0f - m;
    st.Add(detmath::LogF(x), std::log(static_cast<double>(x)), x);
  }
  const float last = 1.0f - (static_cast<float>((1u << 24) - 1u) * (1.0f / 16777216.0f)) * 0.999f;
  st.Add(detmath::LogF(last), std::log(static_cast<double>(last)), last);
  INFO("worst at x = " << st.worstArg);
  REQUIRE(st.worst <= 1);
  REQUIRE(detmath::LogF(1.0f) == 0.0f);
  // Subnormal binary64 arguments are inside LogD's domain.
  REQUIRE(UlpDistanceD(detmath::LogD(0x1p-1060), std::log(0x1p-1060)) <= 2);
}

TEST_CASE("DetMath sin and cos are within 1 ULP for pan and morph weights") {
  // Granular.cpp pan law and PostChain.cpp morph weights: angle = p * float(pi/2).
  UlpStat ss, sc;
  for (uint32_t k = 0; k <= (1u << 18); ++k) {
    const float  p = static_cast<float>(k) * 0x1p-18f;
    const float  a = p * 1.5707963267948966f;
    double s, c;
    detmath::SinCosD(static_cast<double>(a), &s, &c);
    ss.Add(static_cast<float>(s), std::sin(static_cast<double>(a)), a);
    sc.Add(static_cast<float>(c), std::cos(static_cast<double>(a)), a);
  }
  REQUIRE(ss.worst <= 1);
  REQUIRE(sc.worst <= 1);
  // SVF frequency: 2 sin(pi_literal * f), f in [0, 0.25].
  UlpStat sv;
  for (uint32_t k = 0; k <= (1u << 18); ++k) {
    const double f = 0.25 * static_cast<double>(k) * 0x1p-18;
    const double a = 3.14159265358979 * f;
    sv.Add(static_cast<float>(2.0 * detmath::SinD(a)), 2.0 * std::sin(a), a);
  }
  REQUIRE(sv.worst <= 1);
}

TEST_CASE("DetMath half-turn tables are within 1 ULP and exact at quarter turns") {
  UlpStat win, hann, tw;
  double rs, rc;
  for (uint32_t i = 0; i < 4096; ++i) {  // Engine.cpp window LUT
    const double x = static_cast<double>(i) / 4095.0;
    RefSinCosPi(x, &rs, &rc);
    win.Add(static_cast<float>(0.5 * (1.0 - detmath::CosPi(x))), 0.5 * (1.0 - rc), x);
  }
  for (uint32_t i = 0; i < 512; ++i) {  // OnsetDetector.cpp Hann window and twiddles
    const double x = static_cast<double>(2u * i) / 512.0;
    RefSinCosPi(x, &rs, &rc);
    hann.Add(static_cast<float>(0.5 * (1.0 - detmath::CosPi(x))), 0.5 * (1.0 - rc), x);
    double s, c;
    detmath::SinCosPi(x, &s, &c);
    tw.Add(static_cast<float>(c), rc, x);
    tw.Add(static_cast<float>(s), rs, x);
  }
  for (double x : {0x1p29 - 0.25, -0x1p29 + 0.25}) {  // the ends of the domain |x| < 2^29
    RefSinCosPi(x, &rs, &rc);
    double s, c;
    detmath::SinCosPi(x, &s, &c);
    tw.Add(static_cast<float>(c), rc, x);
    tw.Add(static_cast<float>(s), rs, x);
  }
  REQUIRE(win.worst <= 1);
  REQUIRE(hann.worst <= 1);
  REQUIRE(tw.worst <= 1);
  auto quarterTurn = [](int32_t k) {
    double s, c;
    detmath::SinCosPi(static_cast<double>(k) * 0.5, &s, &c);
    const int32_t q = ((k % 4) + 4) % 4;
    REQUIRE(s == (q == 1 ? 1.0 : (q == 3 ? -1.0 : 0.0)));
    REQUIRE(c == (q == 0 ? 1.0 : (q == 2 ? -1.0 : 0.0)));
  };
  for (int32_t k = -8; k <= 8; ++k) quarterTurn(k);
  for (int32_t k = (1 << 30) - 4; k < (1 << 30); ++k) {
    quarterTurn(k);
    quarterTurn(-k);
  }
}

TEST_CASE("DetMath expm1 coefficients are within 1 ULP") {
  // One-pole coefficients -expm1(-2 pi fc / sr) (PostChain.cpp) and smoother taus
  // -expm1(-1 / (tau * sr)) (Smoother.h), as the float the engine stores.
  UlpStat co;
  for (double sr : {44100.0, 48000.0, 96000.0}) {
    for (uint32_t fc = 1; fc <= 12000; ++fc) {
      const double x = -6.283185307179586 * static_cast<double>(fc) / sr;
      co.Add(-static_cast<float>(detmath::Expm1D(x)), -std::expm1(x), x);
    }
    for (float tau : {10.0f, 100.0f}) {
      const double x = -1.0 / (tau * 0.001 * sr);
      co.Add(-static_cast<float>(detmath::Expm1D(x)), -std::expm1(x), x);
    }
  }
  int64_t worstD = 0;
  for (int32_t k = -(1 << 16); k <= (1 << 16); ++k) {
    const double x = static_cast<double>(k) * 0x1p-15;  // [-2, 2]
    co.Add(static_cast<float>(detmath::Expm1D(x)), std::expm1(x), x);
    const int64_t d = UlpDistanceD(detmath::Expm1D(x), std::expm1(x));
    if (d > worstD) worstD = d;
  }
  INFO("worst at x = " << co.worstArg);
  REQUIRE(co.worst <= 1);
  REQUIRE(worstD <= 8);  // binary64 kernel sanity; the float result is what is used
}

TEST_CASE("DetMath pow is within 1 ULP over normalization and curve grids") {
  UlpStat st;
  // Engine.cpp normalization: target^-p, target = clamp(64 o^3, 1, 64), p in [0.5, 1].
  for (uint32_t i = 0; i <= 1024; ++i) {
    const float o = static_cast<float>(i) * (1.0f / 1024.0f);
    float t = 64.0f * o;
    t = t * o;
    t = t * o;
    if (t < 1.0f) t = 1.0f;
    for (uint32_t j = 0; j <= 128; ++j) {
      const float p = 1.0f - 0.5f * (static_cast<float>(j) * (1.0f / 128.0f));
      st.Add(detmath::PowF(t, -p), std::pow(static_cast<double>(t), -static_cast<double>(p)), t);
    }
  }
  // Macro and expression curves (grain-engine.md §5): base in [0, 1], tiny bases.
  for (float y : {0.25f, 0.5f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f}) {
    for (uint32_t k = 1; k <= 4096; ++k) {
      const float x = static_cast<float>(k) * 0x1p-12f;
      st.Add(detmath::PowF(x, y), std::pow(static_cast<double>(x), static_cast<double>(y)), x);
    }
    for (float x : {FLT_TRUE_MIN, FLT_MIN, 1e-30f, 1e-20f, 1e-10f}) {
      st.Add(detmath::PowF(x, y), std::pow(static_cast<double>(x), static_cast<double>(y)), x);
    }
  }
  INFO("worst at x = " << st.worstArg);
  REQUIRE(st.worst <= 1);
}

TEST_CASE("DetMath edge rules keep every result finite and defined") {
  using namespace detmath;
  // PowF at 0 and 1 (the record's prototype gave -inf for 1 < y < 2 at x = 0).
  for (float y : {0.25f, 1.0f, 1.0005f, 1.5f, 2.0f, 3.0f, 4.0f}) {
    REQUIRE(Bits(PowF(0.0f, y)) == 0u);
    REQUIRE(PowF(1.0f, y) == 1.0f);
    REQUIRE(PowF(1.0f, -y) == 1.0f);
  }
  REQUIRE(PowF(0.0f, 0.0f) == 1.0f);
  REQUIRE(PowF(0.37f, 0.0f) == 1.0f);
  REQUIRE(PowF(0.0f, -1.0f) == FLT_MAX);
  REQUIRE(PowF(1e30f, 4.0f) == FLT_MAX);
  REQUIRE(Exp2F(200.0f) == FLT_MAX);
  REQUIRE(Exp2F(-200.0f) == 0.0f);
  // The exp family's exponent clamp.
  REQUIRE(Exp2D(-1100.0) == 0.0);
  REQUIRE(Exp2D(-1022.5) == 0.0);
  REQUIRE(Exp2D(-1022.4) > 0.0);
  REQUIRE(Exp2D(1023.4) < DBL_MAX);
  REQUIRE(Exp2D(1023.4) > 0x1p1022);
  REQUIRE(Exp2D(1023.5) == DBL_MAX);
  REQUIRE(Exp2D(1030.0) == DBL_MAX);
  REQUIRE(Exp2D(std::numeric_limits<double>::quiet_NaN()) == 0.0);
  REQUIRE(ExpD(-800.0) == 0.0);
  REQUIRE(ExpD(800.0) == DBL_MAX);
  REQUIRE(Expm1D(-800.0) == -1.0);
  REQUIRE(Expm1D(0.0) == 0.0);
  REQUIRE(ExpD(0.0) == 1.0);
  REQUIRE(LogD(1.0) == 0.0);
  REQUIRE(UlpDistanceD(LogD(DBL_MAX), std::log(DBL_MAX)) <= 1);
}

TEST_CASE("DetMath function-level digest is identical on every conforming build") {
  // One value for MSVC x64 (SSE2 and AVX2), GCC and Clang x86-64 and the emulated
  // Cortex-M7. A change here is a sound change (determinism profile §5.12).
  REQUIRE(testing::DetMathDigest() == 0xC7BF15265160AA18ull);
}
