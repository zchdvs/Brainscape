// PROBE: cost on the Cortex-M7 of one macro target evaluated with DetMath at the event,
// against a 257-point table + lerp precomputed into the blob. Compiled against the real
// dsp/src/DetMath.cpp (read-only include from the main checkout).
//   arm-none-eabi-g++ firmware flags -O2 -c  -> static instruction counts per function
//   clang --target=thumbv7em ... -DFLATTEN -S -> one straight-line body for llvm-mca
#include <cstdint>
#include <cstring>

#if defined(FLATTEN)
#include "DetMath.cpp"  // one TU, so PowF's callees can be inlined into the evaluator
#define EVAL_ATTR __attribute__((flatten, noinline))
#else
#include "detail/DetMath.h"
#if defined(_MSC_VER)
#define EVAL_ATTR __declspec(noinline)
#else
#define EVAL_ATTR __attribute__((noinline))
#endif
#endif

namespace probe {
struct Target { uint32_t param; float lo, hi, inLo, inHi, curve; };

// position p in [0,1] (exact ADC code / codeMax, or a plugin knob's PlainFromNormalized input)
// -> plain value for one target: in_range window, skew curve, then lo..hi, one FP op per
// statement, binary64 lerp rounded once (as the taper functions do).
EVAL_ATTR float EvalTarget(const Target& t, float p) {
  float n;
  if (!(p > t.inLo)) {
    n = 0.0f;
  } else if (!(p < t.inHi)) {
    n = 1.0f;
  } else {
    const float a = p - t.inLo;
    const float w = t.inHi - t.inLo;
    n = a / w;
  }
  const float  c  = brainscape::detmath::PowF(n, t.curve);
  const double lo = static_cast<double>(t.lo);
  const double d  = static_cast<double>(t.hi) - lo;
  const double m  = d * static_cast<double>(c);
  const double v  = lo + m;
  return static_cast<float>(v);
}

// the alternative: a 257-entry table per target in the blob, linear interpolation
EVAL_ATTR float EvalTable(const float* tab, float p) {
  const float    x = p * 256.0f;
  const uint32_t i = x >= 256.0f ? 255u : static_cast<uint32_t>(x);
  const float    f = x - static_cast<float>(i);
  const float    a = tab[i];
  const float    b = tab[i + 1];
  const float    d = b - a;
  const float    m = d * f;
  return a + m;
}

// one macro move: fan out to n targets in target-list order (profile §5.11)
EVAL_ATTR void FanOut(const Target* ts, uint32_t n, float p, float* out) {
  for (uint32_t i = 0; i < n; ++i) out[i] = EvalTarget(ts[i], p);
}
}  // namespace probe

#if defined(HOST_MAIN)
#include <chrono>
#include <cstdio>
int main() {
  probe::Target t{1, 600.f, 2000.f, 0.f, 1.f, 1.5f};
  volatile float sink = 0;
  auto t0 = std::chrono::steady_clock::now();
  const int N = 4096 * 1000;
  for (int i = 0; i < N; ++i) sink = sink + probe::EvalTarget(t, float(i & 4095) / 4095.0f);
  const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("host: EvalTarget %.1f ns/target\n", s / N * 1e9);
  // monotonic and exact endpoints over every 12-bit pot code, for a grid of curve exponents
  // and every float position in [0,1] (2^30 + 1 values) for the example's 1.5 and 3.0
  const float curves[] = {0.0625f, 0.25f, 0.5f, 1.0f, 1.5f, 2.0f, 3.0f, 8.0f, 16.0f};
  int badTotal = 0, endBad = 0;
  for (float cv : curves) {
    probe::Target u{1, 600.f, 2000.f, 0.f, 1.f, cv};
    float prev = -1;
    for (int c = 0; c <= 4095; ++c) {
      const float v = probe::EvalTarget(u, float(c) / 4095.0f);
      if (v < prev) ++badTotal;
      prev = v;
    }
    if (probe::EvalTarget(u, 0.f) != 600.f || probe::EvalTarget(u, 1.f) != 2000.f) ++endBad;
  }
  std::printf("12-bit codes x 9 curves: non-monotonic steps %d, endpoint misses %d\n", badTotal, endBad);
  for (float cv : {1.5f, 3.0f}) {
    probe::Target u{1, 0.f, 1.f, 0.f, 1.f, cv};
    float prev = -1;
    long long nm = 0;
    for (uint32_t b = 0; b <= 0x3F800000u; ++b) {
      float p;
      std::memcpy(&p, &b, 4);
      const float v = probe::EvalTarget(u, p);
      if (v < prev) ++nm;
      prev = v;
    }
    std::printf("every float position in [0,1], curve %.1f: non-monotonic steps %lld\n", cv, nm);
  }
  return 0;
}
#endif
