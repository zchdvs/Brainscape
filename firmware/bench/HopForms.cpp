#include "bench/HopForms.h"

#include "detail/DetMath.h"

// Nothing here calls out of this object (the square root is the vsqrt instruction): the hop image
// places the whole object in ITCM, and cmake/ItcmCheck.cmake refuses a call out of ITCM.
#if defined(__GNUC__)
#define BS_HOP_INLINE inline __attribute__((always_inline))
#define BS_HOP_NOINLINE __attribute__((noinline))
#else
#define BS_HOP_INLINE inline
#define BS_HOP_NOINLINE
#endif
#define BS_RESTRICT __restrict

namespace brainscape::fw::bench {

namespace {

constexpr uint32_t kN    = 512;
constexpr uint32_t kBins = 257;

// ---- The textbook form (OnsetDetector::AnalyzeHop before step 1; dsp/tests/test_onset.cpp's
// ReferenceHop): a windowed copy, a bit-reversal permutation, nine j-inner butterfly stages.

// ---- P1's v2 (cpu-budget.md §4.1; the CPU plan's bit-exact prototypes, not built): the window,
// a compile-time bit reversal and stages 1-2 fused, per-stage templates with immediate offsets,
// group-inner loops for the early stages and j-inner loops for the late ones, four-way
// magnitudes, and the whitening as an independent per-bin pass followed by the flux summed in
// bin order (each d > 0 ? d : +0 added, and flux + +0 == flux exactly). Ported unchanged but for
// names and the inlining attributes that keep it in this object.
struct Rev7Table {
  uint8_t v[128];
};
constexpr Rev7Table MakeRev7() {
  Rev7Table t{};
  for (uint32_t g = 0; g < 128; ++g) {
    uint32_t r = 0, x = g;
    for (int b = 0; b < 7; ++b) {
      r = (r << 1) | (x & 1u);
      x >>= 1;
    }
    t.v[g] = static_cast<uint8_t>(r);
  }
  return t;
}
constexpr Rev7Table kRev7 = MakeRev7();

// One general radix-2 butterfly pair (j and j+Q) at element offset 0 of (re, im).
template <uint32_t H, uint32_t Q>
BS_HOP_INLINE void Pair(float* BS_RESTRICT re, float* BS_RESTRICT im, float wr0, float wi0, float wr1,
                        float wi1) noexcept {
  const float br0 = re[H], bi0 = im[H], br1 = re[H + Q], bi1 = im[H + Q];
  const float xr0 = br0 * wr0 - bi0 * wi0;
  const float xr1 = br1 * wr1 - bi1 * wi1;
  const float xi0 = br0 * wi0 + bi0 * wr0;
  const float xi1 = br1 * wi1 + bi1 * wr1;
  const float ar0 = re[0], ai0 = im[0], ar1 = re[Q], ai1 = im[Q];
  re[H]     = ar0 - xr0;
  im[H]     = ai0 - xi0;
  re[0]     = ar0 + xr0;
  im[0]     = ai0 + xi0;
  re[H + Q] = ar1 - xr1;
  im[H + Q] = ai1 - xi1;
  re[Q]     = ar1 + xr1;
  im[Q]     = ai1 + xi1;
}

template <uint32_t Len>
BS_HOP_INLINE void Stage(float* BS_RESTRICT re, float* BS_RESTRICT im, const float* BS_RESTRICT twc,
                         const float* BS_RESTRICT tws) noexcept {
  constexpr uint32_t H = Len / 2, Q = H / 2, S = kN / Len;
  // j = 0 (W = (1, -0)) and j = Q (W = (-0, -1)) without multiplies, all groups.
  for (uint32_t i0 = 0; i0 < kN; i0 += Len) {
    float*      r  = re + i0;
    float*      m  = im + i0;
    const float br = r[H], bi = m[H], ar = r[0], ai = m[0];
    const float cr = r[H + Q], ci = m[H + Q], dr = r[Q], di = m[Q];
    r[H]     = ar - br;
    m[H]     = ai - bi;
    r[0]     = ar + br;
    m[0]     = ai + bi;
    r[H + Q] = dr - ci;
    m[H + Q] = di + cr;
    r[Q]     = dr + ci;
    m[Q]     = di - cr;
  }
  if constexpr (Q > 1) {
    if constexpr (S >= 8) {
      // Many groups: twiddles hoisted, groups inner.
      for (uint32_t j = 1; j < Q; ++j) {
        const float wr0 = twc[j * S], wi0 = tws[j * S];
        const float wr1 = twc[(j + Q) * S], wi1 = tws[(j + Q) * S];
        for (uint32_t i0 = j; i0 < kN; i0 += Len) Pair<H, Q>(re + i0, im + i0, wr0, wi0, wr1, wi1);
      }
    } else {
      // Few groups: j inner, twiddles streamed.
      for (uint32_t i0 = 0; i0 < kN; i0 += Len) {
        const float* c0 = twc + S;
        const float* s0 = tws + S;
        float*       r  = re + i0 + 1;
        float*       m  = im + i0 + 1;
        for (uint32_t j = 1; j < Q; ++j) {
          Pair<H, Q>(r, m, c0[0], s0[0], c0[Q * S], s0[Q * S]);
          c0 += S;
          s0 += S;
          ++r;
          ++m;
        }
      }
    }
  }
}

}  // namespace

BS_HOP_NOINLINE float HopTextbook(const HopBuffers& b) noexcept {
  float* BS_RESTRICT re = b.re;
  float* BS_RESTRICT im = b.im;
  // Windowed copy of the last 512 samples (anaPos points one past the newest).
  for (uint32_t i = 0; i < kN; ++i) {
    const uint32_t src = (b.anaPos + i) & (kN - 1u);
    re[i]              = b.ana[src] * b.hann[i];
    im[i]              = 0.f;
  }
  // In-place iterative radix-2 FFT (decimation in time), bit-reversed input.
  for (uint32_t i = 1, j = 0; i < kN; ++i) {
    uint32_t bit = kN >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      const float tr = re[i];
      re[i]          = re[j];
      re[j]          = tr;
      const float ti = im[i];
      im[i]          = im[j];
      im[j]          = ti;
    }
  }
  for (uint32_t len = 2; len <= kN; len <<= 1) {
    const uint32_t half = len >> 1;
    const uint32_t step = kN / len;
    for (uint32_t i = 0; i < kN; i += len) {
      for (uint32_t j = 0; j < half; ++j) {
        const float    wr = b.twCos[j * step];
        const float    wi = b.twSin[j * step];
        const uint32_t a = i + j, c = i + j + half;
        const float    xr = re[c] * wr - im[c] * wi;
        const float    xi = re[c] * wi + im[c] * wr;
        re[c]             = re[a] - xr;
        im[c]             = im[a] - xi;
        re[a] += xr;
        im[a] += xi;
      }
    }
  }
  float frameMax = 1e-6f;
  for (uint32_t k = 1; k < kBins; ++k) {
    const float mag = detmath::SqrtF(re[k] * re[k] + im[k] * im[k]);
    re[k]           = mag;  // stash magnitudes (re is scratch after the FFT)
    if (mag > frameMax) frameMax = mag;
  }
  const float floorVal = frameMax * 0.01f;
  float       flux     = 0.f;
  for (uint32_t k = 1; k < kBins; ++k) {
    const float mag = re[k];
    float       pm  = b.peakMem[k] * b.whitenDecay;
    if (mag > pm) pm = mag;
    if (pm < floorVal) pm = floorVal;
    b.peakMem[k]    = pm;
    const float w   = mag / pm;
    const float avg = 0.5f * (w + b.prevW[k]);
    const float d   = avg - b.prevAvg[k];
    if (d > 0.f) flux += d;
    b.prevW[k]   = w;
    b.prevAvg[k] = avg;
  }
  return flux * (1.0f / static_cast<float>(kBins - 1u));
}

BS_HOP_NOINLINE float HopP1v2(const HopBuffers& b) noexcept {
  float* BS_RESTRICT       re      = b.re;
  float* BS_RESTRICT       im      = b.im;
  const float* BS_RESTRICT ana     = b.ana;
  const float* BS_RESTRICT hann    = b.hann;
  const float* BS_RESTRICT twc     = b.twCos;
  const float* BS_RESTRICT tws     = b.twSin;
  float* BS_RESTRICT       prevW   = b.prevW;
  float* BS_RESTRICT       prevAvg = b.prevAvg;
  float* BS_RESTRICT       peakMem = b.peakMem;
  constexpr uint32_t       M       = kN - 1u;
  for (uint32_t g = 0; g < kN / 4u; ++g) {
    const uint32_t m  = kRev7.v[g];
    const uint32_t p  = b.anaPos + m;
    const float    p0 = ana[p & M] * hann[m];
    const float    p1 = ana[(p + 256u) & M] * hann[m + 256u];
    const float    p2 = ana[(p + 128u) & M] * hann[m + 128u];
    const float    p3 = ana[(p + 384u) & M] * hann[m + 384u];
    const float    a0 = p0 + p1, a1 = p0 - p1, a2 = p2 + p3, a3 = p2 - p3;
    float*         r  = re + 4u * g;
    float*         i  = im + 4u * g;
    r[0]              = a0 + a2;
    i[0]              = 0.f;
    r[1]              = a1;
    i[1]              = -a3;
    r[2]              = a0 - a2;
    i[2]              = 0.f;
    r[3]              = a1;
    i[3]              = a3;
  }
  Stage<8>(re, im, twc, tws);
  Stage<16>(re, im, twc, tws);
  Stage<32>(re, im, twc, tws);
  Stage<64>(re, im, twc, tws);
  Stage<128>(re, im, twc, tws);
  Stage<256>(re, im, twc, tws);
  Stage<512>(re, im, twc, tws);

  // Magnitudes of bins 1..256, four at a time, two partial maxima.
  float mx0 = 1e-6f, mx1 = 1e-6f;
  for (uint32_t k = 1; k < kBins; k += 4u) {
    const float r0 = re[k], i0 = im[k], r1 = re[k + 1], i1 = im[k + 1];
    const float r2 = re[k + 2], i2 = im[k + 2], r3 = re[k + 3], i3 = im[k + 3];
    const float q0 = r0 * r0, q1 = r1 * r1, q2 = r2 * r2, q3 = r3 * r3;
    const float s0 = i0 * i0, s1 = i1 * i1, s2 = i2 * i2, s3 = i3 * i3;
    const float m0 = detmath::SqrtF(q0 + s0);
    const float m1 = detmath::SqrtF(q1 + s1);
    const float m2 = detmath::SqrtF(q2 + s2);
    const float m3 = detmath::SqrtF(q3 + s3);
    re[k]          = m0;
    re[k + 1]      = m1;
    re[k + 2]      = m2;
    re[k + 3]      = m3;
    if (m0 > mx0) mx0 = m0;
    if (m1 > mx1) mx1 = m1;
    if (m2 > mx0) mx0 = m2;
    if (m3 > mx1) mx1 = m3;
  }
  const float frameMax = mx1 > mx0 ? mx1 : mx0;
  const float floorVal = frameMax * 0.01f;
  // Per-bin pass (independent bins, two at a time); the positive part of d into im[k].
  for (uint32_t k = 1; k < kBins; k += 2u) {
    const float mag0 = re[k], mag1 = re[k + 1];
    float       pm0 = peakMem[k] * b.whitenDecay, pm1 = peakMem[k + 1] * b.whitenDecay;
    if (mag0 > pm0) pm0 = mag0;
    if (mag1 > pm1) pm1 = mag1;
    if (pm0 < floorVal) pm0 = floorVal;
    if (pm1 < floorVal) pm1 = floorVal;
    peakMem[k]        = pm0;
    peakMem[k + 1]    = pm1;
    const float w0    = mag0 / pm0, w1 = mag1 / pm1;
    const float avg0  = 0.5f * (w0 + prevW[k]), avg1 = 0.5f * (w1 + prevW[k + 1]);
    const float d0    = avg0 - prevAvg[k], d1 = avg1 - prevAvg[k + 1];
    im[k]             = d0 > 0.f ? d0 : 0.f;
    im[k + 1]         = d1 > 0.f ? d1 : 0.f;
    prevW[k]          = w0;
    prevW[k + 1]      = w1;
    prevAvg[k]        = avg0;
    prevAvg[k + 1]    = avg1;
  }
  float flux = 0.f;
  for (uint32_t k = 1; k < kBins; ++k) flux += im[k];
  return flux * (1.0f / static_cast<float>(kBins - 1u));
}

}  // namespace brainscape::fw::bench
