#pragma once
#include <cstdint>
#include <cstring>

#include "detail/DetMath.h"

// Function-level identity digest for DetMath (docs/design/determinism-profile.md §3.9):
// FNV-1a over the result bits of every kernel on a fixed argument set. Arguments are
// integers scaled by powers of two, so they are exact on every build and the digest
// depends only on the kernels. Every conforming build must produce the same value;
// test_detmath.cpp pins it, and the parity tooling can run this header on targets
// without the test framework (the emulated Cortex-M7).
namespace brainscape::testing {

struct Fnv1a {
  uint64_t h = 0xCBF29CE484222325ull;
  void Bytes(const void* p, size_t n) {
    const auto* b = static_cast<const unsigned char*>(p);
    for (size_t i = 0; i < n; ++i) {
      h ^= b[i];
      h *= 0x100000001B3ull;
    }
  }
  void F(float x) { Bytes(&x, sizeof x); }
  void D(double x) { Bytes(&x, sizeof x); }
  void I(int64_t x) { Bytes(&x, sizeof x); }
};

inline uint64_t DetMathDigest() {
  namespace dm = brainscape::detmath;
  Fnv1a f;
  for (int32_t k = -(1 << 16); k <= (1 << 16); ++k) {  // |x| <= 4: pitch, trim
    const double x = static_cast<double>(k) * 0x1p-14;
    f.F(dm::Exp2F(static_cast<float>(x)));
    f.F(dm::Exp2F(static_cast<float>(x * 64.0)));  // float overflow and underflow
    f.D(dm::Exp2D(x * 256.0));                     // the exponent clamp
    f.D(dm::ExpD(x));
    f.D(dm::Expm1D(x * 0.375));
    double s, c;
    dm::SinCosD(x, &s, &c);
    f.D(s);
    f.D(c);
    dm::SinCosPi(x, &s, &c);
    f.D(s);
    f.D(c);
    f.I(dm::RoundHalfAwayI32(x * 4096.0));
    f.I(dm::RoundHalfAwayI64(x * 4096.0 + 0x1p40));
    f.D(dm::CeilSmall(x * 1000.0));
    f.F(dm::Abs(static_cast<float>(x)));
  }
  for (uint32_t k = 1; k <= (1u << 17); ++k) {  // (0, 1]: jitter log, curve bases
    const double x = static_cast<double>(k) * 0x1p-17;
    f.F(dm::LogF(static_cast<float>(x)));
    f.D(dm::LogD(x * 0x1p60));
    f.D(dm::LogD(x * 0x1p-1040));  // subnormal
    f.F(dm::SqrtF(static_cast<float>(x)));
    f.D(dm::SqrtD(x));
    f.F(dm::PowF(static_cast<float>(x), 1.5f));
    f.F(dm::PowF(static_cast<float>(x) * 64.0f, -0.75f));
  }
  return f.h;
}

}  // namespace brainscape::testing
