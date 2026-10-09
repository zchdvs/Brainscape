// The onset detector's hop analysis against the textbook form it replaced
// (docs/design/cpu-budget.md §4, step 1): AnalyzeHop's restructured FFT must give the same
// bits as the windowed copy, bit-reversal permutation and nine j-inner butterfly stages it
// replaced, on every hop of a large deterministic set in the detector's input domain (finite,
// within the engine's ±2^16 clamp, Engine.cpp kDetectorBound). Compared each hop: the flux, the
// 256 magnitudes and the three per-bin states (peak memory, previous whitened and averaged
// magnitudes), bit for bit. Two perturbed references, each one rounding away from the textbook
// form, prove that the comparison and the hop set see such a difference.
#include "detail/FpProfilePrivate.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include "catch.hpp"
#include "detail/DetMath.h"
#include "detail/OnsetDetector.h"

namespace brainscape::detail {

// The detector's tables and hop state (a friend of OnsetDetector).
struct OnsetDetectorProbe {
  static const float* Hann(const OnsetDetector& d) { return d.hann_; }
  static const float* TwCos(const OnsetDetector& d) { return d.twCos_; }
  static const float* TwSin(const OnsetDetector& d) { return d.twSin_; }
  static const float* Mags(const OnsetDetector& d) { return d.re_; }  // bins 1-256 after a hop
  static const float* PeakMem(const OnsetDetector& d) { return d.peakMem_; }
  static const float* PrevW(const OnsetDetector& d) { return d.prevW_; }
  static const float* PrevAvg(const OnsetDetector& d) { return d.prevAvg_; }
  static float        WhitenDecay(const OnsetDetector& d) { return d.whitenDecay_; }
  static float        Flux(const OnsetDetector& d) { return d.flux_; }
};

namespace {

#if !defined(BRAINSCAPE_FP_NEGATIVE_CONTROL)  // a contracting build fuses the two forms differently

enum class Perturb {
  None,
  Twiddle,    // stage 6's W^40 one ULP larger in its real part
  FluxOrder,  // the flux summed from bin 256 down
};

// OnsetDetector::AnalyzeHop as it was before the restructuring, verbatim but for the marked
// perturbation lines, over its own copy of the state.
struct ReferenceHop {
  float    hann_[kOnsetFftSize], twCos_[kOnsetFftSize / 2], twSin_[kOnsetFftSize / 2];
  float    ana_[kOnsetFftSize], re_[kOnsetFftSize], im_[kOnsetFftSize];
  float    prevW_[kOnsetBins], prevAvg_[kOnsetBins], peakMem_[kOnsetBins];
  uint32_t anaPos_      = 0;
  float    whitenDecay_ = 0.f;
  float    flux_        = 0.f;
  Perturb  perturb_     = Perturb::None;

  // The tables and the state of a detector just Init'd (OnsetDetector::Init and Reset).
  void Init(const OnsetDetector& d, Perturb p) {
    std::memcpy(hann_, OnsetDetectorProbe::Hann(d), sizeof hann_);
    std::memcpy(twCos_, OnsetDetectorProbe::TwCos(d), sizeof twCos_);
    std::memcpy(twSin_, OnsetDetectorProbe::TwSin(d), sizeof twSin_);
    whitenDecay_ = OnsetDetectorProbe::WhitenDecay(d);
    for (uint32_t i = 0; i < kOnsetFftSize; ++i) ana_[i] = 0.f;
    for (uint32_t k = 0; k < kOnsetBins; ++k) {
      prevW_[k]   = 0.f;
      prevAvg_[k] = 0.f;
      peakMem_[k] = 1e-6f;
    }
    anaPos_  = 0;
    flux_    = 0.f;
    perturb_ = p;
  }

  void Push(float x) {
    ana_[anaPos_] = x;
    anaPos_       = (anaPos_ + 1u) & (kOnsetFftSize - 1u);
  }

  void AnalyzeHop() {
    // Windowed copy of the last 512 samples (anaPos_ points one past the newest).
    for (uint32_t i = 0; i < kOnsetFftSize; ++i) {
      const uint32_t src = (anaPos_ + i) & (kOnsetFftSize - 1u);
      re_[i]             = ana_[src] * hann_[i];
      im_[i]             = 0.f;
    }

    // In-place iterative radix-2 FFT (decimation in time), bit-reversed input.
    for (uint32_t i = 1, j = 0; i < kOnsetFftSize; ++i) {
      uint32_t bit = kOnsetFftSize >> 1;
      for (; j & bit; bit >>= 1) j ^= bit;
      j ^= bit;
      if (i < j) {
        const float tr = re_[i];
        re_[i]         = re_[j];
        re_[j]         = tr;
        const float ti = im_[i];
        im_[i]         = im_[j];
        im_[j]         = ti;
      }
    }
    for (uint32_t len = 2; len <= kOnsetFftSize; len <<= 1) {
      const uint32_t half = len >> 1;
      const uint32_t step = kOnsetFftSize / len;
      for (uint32_t i = 0; i < kOnsetFftSize; i += len) {
        for (uint32_t j = 0; j < half; ++j) {
          float wr = twCos_[j * step];
          if (perturb_ == Perturb::Twiddle && len == 64u && j == 5u) wr = NextUp(wr);  // control
          const float wi = twSin_[j * step];
          const uint32_t a = i + j, b = i + j + half;
          const float xr = re_[b] * wr - im_[b] * wi;
          const float xi = re_[b] * wi + im_[b] * wr;
          re_[b] = re_[a] - xr;
          im_[b] = im_[a] - xi;
          re_[a] += xr;
          im_[a] += xi;
        }
      }
    }

    float frameMax = 1e-6f;
    for (uint32_t k = 1; k < kOnsetBins; ++k) {
      const float mag = detmath::SqrtF(re_[k] * re_[k] + im_[k] * im_[k]);
      re_[k]          = mag;  // stash magnitudes (re_ is scratch after the FFT)
      if (mag > frameMax) frameMax = mag;
    }
    const float floorVal = frameMax * 0.01f;
    float       flux     = 0.f;
    float       ds[kOnsetBins] = {};  // control: the terms, for the reversed sum
    for (uint32_t k = 1; k < kOnsetBins; ++k) {
      const float mag = re_[k];
      float pm        = peakMem_[k] * whitenDecay_;
      if (mag > pm) pm = mag;
      if (pm < floorVal) pm = floorVal;
      peakMem_[k]     = pm;
      const float w   = mag / pm;
      // 2-hop averaging on top mops up residual hop-to-hop alternation.
      const float avg = 0.5f * (w + prevW_[k]);
      const float d   = avg - prevAvg_[k];
      if (d > 0.f) flux += d;
      if (d > 0.f) ds[k] = d;  // control
      prevW_[k]   = w;
      prevAvg_[k] = avg;
    }
    if (perturb_ == Perturb::FluxOrder) {  // control
      flux = 0.f;
      for (uint32_t k = kOnsetBins - 1u; k >= 1u; --k) flux += ds[k];
    }
    flux_ = flux * (1.0f / static_cast<float>(kOnsetBins - 1));
  }

  static float NextUp(float x) {
    uint32_t u;
    std::memcpy(&u, &x, sizeof u);
    ++u;  // x is a positive normal twiddle here
    std::memcpy(&x, &u, sizeof u);
    return x;
  }
};

uint32_t Bits(float f) {
  uint32_t u;
  std::memcpy(&u, &f, sizeof u);
  return u;
}

// Any bit of the hop's results that differs between the detector and the reference.
bool HopDiffers(const OnsetDetector& d, const ReferenceHop& r) {
  if (Bits(OnsetDetectorProbe::Flux(d)) != Bits(r.flux_)) return true;
  const float* mags = OnsetDetectorProbe::Mags(d);
  const float* pm   = OnsetDetectorProbe::PeakMem(d);
  const float* pw   = OnsetDetectorProbe::PrevW(d);
  const float* pa   = OnsetDetectorProbe::PrevAvg(d);
  for (uint32_t k = 1; k < kOnsetBins; ++k) {
    if (Bits(mags[k]) != Bits(r.re_[k]) || Bits(pm[k]) != Bits(r.peakMem_[k]) ||
        Bits(pw[k]) != Bits(r.prevW_[k]) || Bits(pa[k]) != Bits(r.prevAvg_[k])) {
      return true;
    }
  }
  return false;
}

// Input streams: integer-seeded, the same samples on every platform.
struct Source {
  uint64_t s;
  float    env  = 0.f;
  float    gain = 1.f;

  uint64_t Next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }
  // Uniform on the codec's 24-bit grid in [-1, 1).
  float Uni() { return static_cast<float>(static_cast<int32_t>(Next() >> 40) - 0x800000) * 0x1p-23f; }
  static float Pow2(int e) {  // 2^e for a normal exponent
    const uint32_t u = static_cast<uint32_t>(127 + e) << 23;
    float          f;
    std::memcpy(&f, &u, sizeof f);
    return f;
  }
  static float Sine(double cyclesPerSample, uint64_t t) {
    double s, c;
    detmath::SinCosPi(2.0 * cyclesPerSample * static_cast<double>(t % 48000u), &s, &c);
    return static_cast<float>(s);
  }

  float Sample(int kind, uint64_t t) {
    switch (kind) {
      case 0: return Uni();  // full-scale noise
      case 1:                // noise at a level from 2^-40 to 2^15, new every 4,096 samples
        if (t % 4096u == 0u) gain = Pow2(static_cast<int>(Next() % 56u) - 40);
        return Uni() * gain;
      case 2: return Next() % 97u == 0u ? Uni() : 0.f;  // sparse spikes among exact zeros
      case 3:                                           // decaying plucks over a quiet tone
        if (Next() % 3000u == 0u) env = 1.f;
        env *= 0.9995f;
        return env * static_cast<float>(t % 109u) * (1.0f / 109.0f) + 1e-4f * Uni();
      case 4: return Uni() * 0x1p-140f;  // subnormal samples
      case 5: return 0.25f;              // DC
      case 6: return t % 2048u == 0u ? 1.f : 0.f;  // an impulse train
      case 7: {  // a bin-centred tone and an off-bin one on the 24-bit grid
        const float v = 0.5f * Sine(1500.0 / 48000.0, t) + 0.25f * Sine(1234.5 / 48000.0, t);
        return static_cast<float>(detmath::RoundHalfAwayI32(static_cast<double>(v) * 0x1p23)) *
               0x1p-23f;
      }
      case 8:  // bursts at the clamp (exactly ±2^16 now and then) between silences
        if ((t / 256u) % 2u == 0u) return 0.f;
        if (Next() % 61u == 0u) return Next() & 1u ? 65536.f : -65536.f;
        return Uni() * 65536.f;
      case 9: return (t >> 9) & 1u ? -0.f : 0.f;  // signed zeros only
      case 10: {  // any finite float within the clamp: every exponent, subnormals, both zeros
        const uint64_t r = Next();
        const uint32_t e = static_cast<uint32_t>(r % 144u);  // 2^-126 (or subnormal) to 2^16
        const uint32_t u = static_cast<uint32_t>((r >> 8) & 0x80000000u) | (e << 23) |
                           (e == 143u ? 0u : static_cast<uint32_t>(r >> 33) & 0x7FFFFFu);
        float f;
        std::memcpy(&f, &u, sizeof f);
        return f;
      }
      default: return Sample(static_cast<int>((t / 1024u) % 11u), t);  // the others in turn
    }
  }
};

constexpr int      kKinds       = 12;
constexpr uint32_t kHopsPerKind = 2048;
constexpr uint32_t kControlHops = 512;  // per kind, for each perturbed reference

#endif

}  // namespace

#if !defined(BRAINSCAPE_FP_NEGATIVE_CONTROL)
TEST_CASE("onset hop analysis: the restructured FFT is bit-identical to the textbook form") {
  // The twiddles AnalyzeHop does not multiply by: W^0 = (1, -0) and W^128 = (0, -1).
  {
    std::vector<float> warm(OnsetDetector::WarmFloats());
    OnsetDetector      d;
    d.Init(warm.data(), 48000.0);
    REQUIRE(OnsetDetectorProbe::TwCos(d)[0] == 1.0f);
    REQUIRE(OnsetDetectorProbe::TwSin(d)[0] == 0.0f);
    REQUIRE(OnsetDetectorProbe::TwCos(d)[kOnsetFftSize / 4u] == 0.0f);
    REQUIRE(OnsetDetectorProbe::TwSin(d)[kOnsetFftSize / 4u] == -1.0f);
  }

  uint64_t hops = 0, mismatches = 0, controlHops = 0, twiddleMisses = 0, orderMisses = 0;
  for (int kind = 0; kind < kKinds; ++kind) {
    std::vector<float> warm(OnsetDetector::WarmFloats());
    OnsetDetector      d;
    d.Init(warm.data(), 48000.0);
    ReferenceHop ref, twiddle, order;
    ref.Init(d, Perturb::None);
    twiddle.Init(d, Perturb::Twiddle);
    order.Init(d, Perturb::FluxOrder);
    Source   src{0x9E3779B97F4A7C15ull + static_cast<uint64_t>(kind) * 0x2545F4914F6CDD1Dull};
    uint32_t kindMismatches = 0;
    for (uint64_t t = 0; t < uint64_t{kHopsPerKind} * kOnsetHop; ++t) {
      const float x = src.Sample(kind, t);
      d.ProcessSample(x, static_cast<int64_t>(t));  // analyses when t + 1 ends a hop
      ref.Push(x);
      const uint64_t hop = t / kOnsetHop;
      const bool     ctl = hop < kControlHops;
      if (ctl) {
        twiddle.Push(x);
        order.Push(x);
      }
      if (((t + 1u) & (kOnsetHop - 1u)) != 0u) continue;
      ref.AnalyzeHop();
      ++hops;
      if (HopDiffers(d, ref)) ++kindMismatches;
      if (ctl) {
        twiddle.AnalyzeHop();
        order.AnalyzeHop();
        ++controlHops;
        twiddleMisses += HopDiffers(d, twiddle) ? 1u : 0u;
        orderMisses += HopDiffers(d, order) ? 1u : 0u;
      }
    }
    INFO("input kind " << kind);
    CHECK(kindMismatches == 0u);
    mismatches += kindMismatches;
  }
  INFO("hops " << hops << ", mismatches " << mismatches << "; control hops " << controlHops
               << ", twiddle control mismatches " << twiddleMisses << ", flux-order control "
               << orderMisses);
  REQUIRE(hops == uint64_t{kKinds} * kHopsPerKind);
  REQUIRE(mismatches == 0u);
  // The controls: one rounding away from the textbook form must show on many hops.
  REQUIRE(controlHops == uint64_t{kKinds} * kControlHops);
  REQUIRE(twiddleMisses >= controlHops / 2u);
  REQUIRE(orderMisses >= controlHops / 4u);
}
#endif

}  // namespace brainscape::detail
