#include "detail/FpProfilePrivate.h"

#include "detail/OnsetDetector.h"

#include <cmath>

namespace brainscape::detail {

namespace {
constexpr double kPi = 3.14159265358979323846;
}

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
  const auto ioiHops = static_cast<int64_t>(std::ceil(0.020 * sampleRate / kOnsetHop));
  minIoi_            = ioiHops * kOnsetHop;
  // Whitening memory ~0.4 s in TIME regardless of rate (review: the fixed 0.997
  // constant was a 1.78 s memory that suppressed quiet notes after loud ones).
  whitenDecay_ = static_cast<float>(std::exp(-(static_cast<double>(kOnsetHop) / sampleRate) / 0.4));

  // Tables at Init only (libm at init is fine; cross-build 1-ULP table drift is
  // covered by the same contract-#7 TODO as SemitonesToRatio).
  for (uint32_t i = 0; i < kOnsetFftSize; ++i) {
    hann_[i] = static_cast<float>(
        0.5 * (1.0 - std::cos(2.0 * kPi * static_cast<double>(i) / kOnsetFftSize)));
  }
  for (uint32_t k = 0; k < kOnsetFftSize / 2; ++k) {
    twCos_[k] = static_cast<float>(std::cos(2.0 * kPi * k / kOnsetFftSize));
    twSin_[k] = static_cast<float>(-std::sin(2.0 * kPi * k / kOnsetFftSize));
  }
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

void OnsetDetector::AnalyzeHop() noexcept {
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
        const float wr = twCos_[j * step];
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
  float frameMax = 1e-6f;
  for (uint32_t k = 1; k < kOnsetBins; ++k) {
    const float mag = std::sqrt(re_[k] * re_[k] + im_[k] * im_[k]);
    re_[k]          = mag;  // stash magnitudes (re_ is scratch after the FFT)
    if (mag > frameMax) frameMax = mag;
  }
  const float floorVal = frameMax * 0.01f;
  float       flux     = 0.f;
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
    prevW_[k]   = w;
    prevAvg_[k] = avg;
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
