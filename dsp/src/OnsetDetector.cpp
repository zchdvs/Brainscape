#include "detail/FpProfilePrivate.h"

#include "detail/OnsetDetector.h"

#include <cassert>

#include "detail/DetMath.h"

namespace brainscape::detail {

namespace {

// AnalyzeHop's first pass is specialised to the 512-point transform: its 128 groups of four
// are addressed by a 7-bit bit reversal.
static_assert(kOnsetFftSize == 512u && kOnsetBins == 257u, "AnalyzeHop is a 512-point FFT");

struct Rev7Table {
  uint8_t v[128];
};

constexpr Rev7Table MakeRev7() noexcept {
  Rev7Table t{};
  for (uint32_t g = 0; g < 128u; ++g) {
    uint32_t r = 0;
    for (uint32_t b = 0; b < 7u; ++b) r |= ((g >> b) & 1u) << (6u - b);
    t.v[g] = static_cast<uint8_t>(r);
  }
  return t;
}

constexpr Rev7Table kRev7 = MakeRev7();

}  // namespace

void OnsetDetector::Init(float* warm, double sampleRate) noexcept {
  float* p = warm;
  hann_    = p;
  p += kOnsetFftSize;
  ana_ = p;
  p += kOnsetFftSize;
  re_ = p;
  p += kOnsetFftSize;
  im_ = p;
  p += kOnsetFftSize;
  twCos_ = p;
  p += kOnsetFftSize / 2;
  twSin_ = p;
  p += kOnsetFftSize / 2;
  prevW_ = p;
  p += kOnsetBins;
  prevAvg_ = p;
  p += kOnsetBins;
  peakMem_ = p;

  sr_ = sampleRate;
  // aubio's 20 ms min-IOI, rounded UP to whole hops so the constant matches the
  // realized behavior (the gate only runs at hop boundaries).
  const double ioi   = detmath::CeilSmall(0.020 * sampleRate / kOnsetHop);
  assert(ioi >= 1.0 && ioi <= 64.0);  // determinism profile §3.10: 8-384 kHz
  const auto ioiHops = static_cast<int64_t>(ioi);
  minIoi_            = ioiHops * kOnsetHop;
  // Whitening memory ~0.4 s in TIME regardless of rate (review: the fixed 0.997
  // constant was a 1.78 s memory that suppressed quiet notes after loud ones).
  whitenDecay_ =
      static_cast<float>(detmath::ExpD(-(static_cast<double>(kOnsetHop) / sampleRate) / 0.4));

  // Tables built in-tree at Init: libm tables differ by a ULP across builds, and
  // compilers fold constant libm calls differently (determinism profile §3.9). The
  // half-turn argument 2i/N is dyadic, so the reduction is exact.
  for (uint32_t i = 0; i < kOnsetFftSize; ++i) {
    const double c  = detmath::CosPi(static_cast<double>(2u * i) / kOnsetFftSize);
    const double om = 1.0 - c;
    hann_[i]        = static_cast<float>(0.5 * om);
  }
  for (uint32_t k = 0; k < kOnsetFftSize / 2; ++k) {
    double s, c;
    detmath::SinCosPi(static_cast<double>(2u * k) / kOnsetFftSize, &s, &c);
    twCos_[k] = static_cast<float>(c);
    twSin_[k] = static_cast<float>(-s);
  }
  // AnalyzeHop skips the multiplies by W^0 = (1, -0) and W^128 = (0, -1); DetMath is exact
  // at quarter turns (dsp/tests/test_detmath.cpp), so these are those values exactly.
  assert(twCos_[0] == 1.0f && twSin_[0] == 0.0f && twCos_[kOnsetFftSize / 4u] == 0.0f &&
         twSin_[kOnsetFftSize / 4u] == -1.0f);
  Reset();
}

void OnsetDetector::Reset() noexcept {
  for (uint32_t i = 0; i < kOnsetFftSize; ++i) ana_[i] = 0.f;
  for (uint32_t k = 0; k < kOnsetBins; ++k) {
    prevW_[k]   = 0.f;
    prevAvg_[k] = 0.f;
    peakMem_[k] = 1e-6f;
  }
  for (auto& f : fluxRing_) f = 0.f;
  fluxRingPos_  = 0;
  fluxRingLen_  = 0;
  hopEnergy_    = 0.f;
  flux_         = 0.f;
  prevFlux_     = 0.f;
  armed_        = true;
  anaPos_       = 0;
  lastOnsetAbs_ = -1000000;
  warmupHops_   = 4;  // relative warmup — survives mid-stream Resets (review finding)
}

void OnsetDetector::SetSensitivity(float s01) noexcept {
  if (s01 < 0.f) s01 = 0.f;
  if (s01 > 1.f) s01 = 1.f;
  // Sensitivity maps to Dixon's margin over the moving-mean baseline. With the
  // baseline in place the knob is monotone and usable across its whole travel;
  // neither endpoint is "deaf" or a guaranteed hair trigger — endpoints are
  // margin extremes (review finding on the old absolute-threshold mapping).
  // Halved vs the single-hop mapping: 2-hop averaging halves a real onset's
  // peak flux.
  delta_ = 0.01f + (1.0f - s01) * 0.13f;
}

// The windowed last 512 samples through a radix-2 decimation-in-time FFT, then the whitened
// positive flux. Bit for bit the textbook loop it replaced (a windowed copy, a bit-reversal
// permutation, then nine stages of j-inner butterflies, which dsp/tests/test_onset.cpp keeps
// as its reference and compares hop by hop): every value that reaches a magnitude is made by
// the same IEEE operations on the same operands. What changed is where the work goes:
//
//   - The window, the permutation and stages 1-2 are one pass. Positions 4g..4g+3 take the
//     windowed samples m, m+256, m+128 and m+384 with m = rev7(g) (rev9(4g+t) is
//     rev2(t)·128 + rev7(g)); the input is real and those stages' twiddles are W^0 = (1, -0)
//     and W^128 = (0, -1), so their butterflies are sums and differences.
//   - In stages 3-8 the W^0 and W^128 butterflies skip their multiplies, and the others run
//     with the twiddle hoisted out of the group loop. Butterflies of one stage touch disjoint
//     pairs, so their order is free.
//   - Stage 9 computes only bins 1-256, the ones the flux reads, each straight into its
//     magnitude.
//
// A skipped multiply changes nothing but the sign of a zero. With W^0, br·1 - bi·(-0) is br
// and br·(-0) + bi·1 is bi; with W^128, br·0 - bi·(-1) is bi and br·(-1) + bi·0 is -br; each
// up to the sign of a zero result. A zero's sign changes no sum with a non-zero operand and no
// product's magnitude, and re² + im² erases it. That holds for finite operands, which the
// engine's ±2^16 clamp on the detector input guarantees (Engine.cpp, kDetectorBound: |X[k]|
// stays below 2^25). The hop's whole cost lands in one block in every 256 samples; this form
// is modelled at about half of it (docs/design/cpu-budget.md §4, step 1).
void OnsetDetector::AnalyzeHop() noexcept {
  float* __restrict re         = re_;
  float* __restrict im         = im_;
  const float* __restrict ana  = ana_;
  const float* __restrict hann = hann_;
  const float* __restrict twc  = twCos_;
  const float* __restrict tws  = twSin_;
  const uint32_t a0            = anaPos_;  // one past the newest sample: the oldest

  // Window, bit reversal and stages 1-2.
  for (uint32_t g = 0; g < 128u; ++g) {
    const uint32_t m   = kRev7.v[g];
    const float    v0  = ana[(a0 + m) & 511u] * hann[m];
    const float    v1  = ana[(a0 + m + 256u) & 511u] * hann[m + 256u];
    const float    v2  = ana[(a0 + m + 128u) & 511u] * hann[m + 128u];
    const float    v3  = ana[(a0 + m + 384u) & 511u] * hann[m + 384u];
    const float    s01 = v0 + v1, d01 = v0 - v1;
    const float    s23 = v2 + v3, d23 = v2 - v3;
    float* __restrict r = re + 4u * g;
    float* __restrict i = im + 4u * g;
    r[0] = s01 + s23;
    i[0] = 0.f;
    r[1] = d01;
    i[1] = -d23;
    r[2] = s01 - s23;
    i[2] = 0.f;
    r[3] = d01;
    i[3] = d23;
  }

  // Stages 3-8.
  for (uint32_t len = 8; len <= 256u; len <<= 1) {
    const uint32_t half = len >> 1;
    const uint32_t step = 512u / len;
    const uint32_t q    = half >> 1;  // j = q is W^128 = (0, -1)
    for (uint32_t i = 0; i < 512u; i += len) {  // j = 0: W^0 = (1, -0)
      const uint32_t a = i, b = i + half;
      const float br = re[b], bi = im[b], ar = re[a], ai = im[a];
      re[b] = ar - br;
      im[b] = ai - bi;
      re[a] = ar + br;
      im[a] = ai + bi;
    }
    for (uint32_t i = 0; i < 512u; i += len) {  // j = q
      const uint32_t a = i + q, b = a + half;
      const float br = re[b], bi = im[b], ar = re[a], ai = im[a];
      re[b] = ar - bi;
      im[b] = ai + br;
      re[a] = ar + bi;
      im[a] = ai - br;
    }
    for (uint32_t j = 1; j < half; ++j) {
      if (j == q) continue;
      const float wr = twc[j * step];
      const float wi = tws[j * step];
      for (uint32_t i = 0; i < 512u; i += len) {
        const uint32_t a = i + j, b = a + half;
        const float br = re[b], bi = im[b];
        const float xr = br * wr - bi * wi;
        const float xi = br * wi + bi * wr;
        const float ar = re[a], ai = im[a];
        re[b] = ar - xr;
        im[b] = ai - xi;
        re[a] = ar + xr;
        im[a] = ai + xi;
      }
    }
  }

  // Stage 9 for bins 1-256 only, each into its magnitude (stashed in re, scratch after the
  // FFT). Bin 256 is the j = 0 butterfly's difference; bins 1-255 its sums.
  float frameMax = 1e-6f;
  const float r256 = re[0] - re[256];
  const float i256 = im[0] - im[256];
  const float m256 = detmath::SqrtF(r256 * r256 + i256 * i256);
  for (uint32_t j = 1; j < 256u; ++j) {
    const float br = re[j + 256u], bi = im[j + 256u];
    float       r, i;
    if (j == 128u) {  // W^128 = (0, -1)
      r = re[j] + bi;
      i = im[j] - br;
    } else {
      const float wr = twc[j], wi = tws[j];
      const float xr = br * wr - bi * wi;
      const float xi = br * wi + bi * wr;
      r = re[j] + xr;
      i = im[j] + xi;
    }
    const float mag = detmath::SqrtF(r * r + i * i);
    re[j]           = mag;
    if (mag > frameMax) frameMax = mag;
  }
  re[256] = m256;
  if (m256 > frameMax) frameMax = m256;

  // Whitened positive spectral flux (Stowell & Plumbley adaptive whitening:
  // each bin normalized by its own decaying peak memory — this is what makes one
  // sensitivity setting work across single-coil, fuzz, and line level).
  //
  // The peak memory is floored RELATIVE to the frame maximum (S&P's floor
  // parameter, -40 dB here): without it, the leakage skirts of a steady tone —
  // hundreds of near-silent bins — are amplified to O(1) and their window-phase
  // flutter free-ran the detector at every sensitivity (review finding, 242
  // fires/10 s on a held sine). Relative to the frame max, not absolute, so
  // level independence is preserved.
  float* __restrict peakMem = peakMem_;
  float* __restrict prevW   = prevW_;
  float* __restrict prevAvg = prevAvg_;
  const float decay         = whitenDecay_;
  const float floorVal      = frameMax * 0.01f;
  float       flux          = 0.f;
  for (uint32_t k = 1; k < kOnsetBins; ++k) {
    const float mag = re[k];
    float pm        = peakMem[k] * decay;
    if (mag > pm) pm = mag;
    if (pm < floorVal) pm = floorVal;
    peakMem[k]      = pm;
    const float w   = mag / pm;
    // 2-hop averaging on top mops up residual hop-to-hop alternation.
    const float avg = 0.5f * (w + prevW[k]);
    const float d   = avg - prevAvg[k];
    if (d > 0.f) flux += d;
    prevW[k]   = w;
    prevAvg[k] = avg;
  }
  flux_ = flux * (1.0f / static_cast<float>(kOnsetBins - 1));
}

bool OnsetDetector::ProcessSample(float x, int64_t abs) noexcept {
  ana_[anaPos_] = x;
  anaPos_       = (anaPos_ + 1u) & (kOnsetFftSize - 1u);
  hopEnergy_ += x * x;

  // Hop boundaries on the ABSOLUTE grid — split-invariant by construction.
  if ((static_cast<uint64_t>(abs + 1) & (kOnsetHop - 1u)) != 0) return false;

  const float energy = hopEnergy_;
  hopEnergy_         = 0.f;
  AnalyzeHop();

  // Moving-mean baseline over the PREVIOUS mw hops (excluding this one).
  float mean = 0.f;
  if (fluxRingLen_ > 0) {
    for (uint32_t i = 0; i < fluxRingLen_; ++i) mean += fluxRing_[i];
    mean /= static_cast<float>(fluxRingLen_);
  }
  const float lastFlux = prevFlux_;
  prevFlux_            = flux_;
  fluxRing_[fluxRingPos_] = flux_;
  fluxRingPos_            = (fluxRingPos_ + 1u) % kOnsetMeanWin;
  if (fluxRingLen_ < kOnsetMeanWin) ++fluxRingLen_;

  if (warmupHops_ > 0) {
    --warmupHops_;
    return false;
  }
  // Silence gate at -80 dBFS RMS over the hop. Deliberately 10 dB tighter than
  // aubio's -90 dB: this gate is what keeps idle rig hiss from reaching the
  // peak-picker at all (review finding). The calibration routine (research
  // §5.3) will eventually derive it from the measured noise floor.
  if (energy < 1e-8f * static_cast<float>(kOnsetHop)) return false;

  // Dixon's peak-picker (research §2.2): above the moving mean by delta AND
  // rising — plus bonk~-style hysteresis: after a fire, re-arm only once flux
  // falls back near the baseline, which kills min-IOI-rate chatter on held
  // distorted chords (review: a bare threshold fired 179x/5 s there).
  const bool above = flux_ >= mean + delta_;
  if (!armed_) {
    if (flux_ <= mean + 0.4f * delta_) armed_ = true;
    return false;
  }
  if (!above) return false;
  if (flux_ < lastFlux) return false;  // causal rising condition
  if (abs - lastOnsetAbs_ < minIoi_) return false;
  lastOnsetAbs_ = abs;
  armed_        = false;
  return true;
}

}  // namespace brainscape::detail
