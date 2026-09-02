#include "brainscape/detail/OnsetDetector.h"

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
  peakMem_ = p;

  sr_     = sampleRate;
  minIoi_ = static_cast<int64_t>(0.020 * sampleRate);  // 20 ms

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
    peakMem_[k] = 1e-6f;
  }
  hopEnergy_    = 0.f;
  anaPos_       = 0;
  lastOnsetAbs_ = -1000000;
  warmupUntil_  = kOnsetFftSize * 2;
}

void OnsetDetector::SetSensitivity(float s01) noexcept {
  if (s01 < 0.f) s01 = 0.f;
  if (s01 > 1.f) s01 = 1.f;
  // Higher sensitivity = lower flux threshold. Midpoint lands near aubio's
  // tuned default region for whitened flux (empirically calibrated in tests).
  threshold_ = 0.02f + (1.0f - s01) * 0.18f;
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
  float flux = 0.f;
  for (uint32_t k = 1; k < kOnsetBins; ++k) {
    const float mag = std::sqrt(re_[k] * re_[k] + im_[k] * im_[k]);
    float pm        = peakMem_[k] * whitenDecay_;
    if (mag > pm) pm = mag;
    if (pm < 1e-6f) pm = 1e-6f;
    peakMem_[k]   = pm;
    const float w = mag / pm;
    const float d = w - prevW_[k];
    if (d > 0.f) flux += d;
    prevW_[k] = w;
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

  if (abs < warmupUntil_) return false;
  // Silence gate ~-80 dBFS RMS over the hop (aubio's -90 dB class of gate).
  if (energy < 1e-8f * static_cast<float>(kOnsetHop)) return false;
  if (flux_ < threshold_) return false;
  if (abs - lastOnsetAbs_ < minIoi_) return false;
  lastOnsetAbs_ = abs;
  return true;
}

}  // namespace brainscape::detail
