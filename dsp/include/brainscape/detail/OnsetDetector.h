#pragma once
#include <cstdint>

// Spectral-flux onset detector with adaptive whitening
// (docs/research/onset-detection-on-cortex-m7.md recs #2/#4; design §4 trigger layer).
//
// 512-sample Hann window, 256-sample hop, hop boundaries aligned to the ABSOLUTE
// sample counter — so detection is block-split invariant by construction. The FFT
// is a small in-tree radix-2 (not CMSIS/libm) so firmware and plugin compute
// bit-identical spectra; swapping the M7 build to arm_rfft_fast_f32 is a
// measured-budget decision that would also need the contract-#7 determinism note.
//
// Honesty note carried from the research: no detection function solves both
// distorted-guitar and slow-pad onsets — this layer ships with adaptive
// whitening, a silence gate, min-IOI dedup, and (at the engine level) manual/
// external trigger fallbacks, and is not marketed as "solved".
namespace brainscape::detail {

inline constexpr uint32_t kOnsetFftSize = 512;
inline constexpr uint32_t kOnsetHop     = 256;
inline constexpr uint32_t kOnsetBins    = kOnsetFftSize / 2 + 1;

class OnsetDetector {
 public:
  static uint32_t WarmFloats() noexcept {
    // hann + analysis ring + re + im + twiddle cos/sin + prev whitened + peak memory
    return kOnsetFftSize * 4u + (kOnsetFftSize / 2u) * 2u + kOnsetBins * 2u;
  }

  void Init(float* warm, double sampleRate) noexcept;
  void Reset() noexcept;
  void SetSensitivity(float s01) noexcept;  // control rate; 0 = deaf, 1 = hair trigger

  // Feed one mono sample at absolute engine sample `abs`. Returns true when this
  // sample completes an analysis hop that contains an onset. The onset is
  // attributed to the hop just finished (the detector's inherent ~5-11 ms
  // latency; the research's figure for a causal 512/256 flux detector).
  bool ProcessSample(float x, int64_t abs) noexcept;

 private:
  void AnalyzeHop() noexcept;  // fills flux_ from the last 512 samples

  float* hann_    = nullptr;  // [512]
  float* ana_     = nullptr;  // [512] circular input window
  float* re_      = nullptr;  // [512]
  float* im_      = nullptr;  // [512]
  float* twCos_   = nullptr;  // [256]
  float* twSin_   = nullptr;  // [256]
  float* prevW_   = nullptr;  // [257] previous whitened magnitudes
  float* peakMem_ = nullptr;  // [257] adaptive-whitening peak memory

  double  sr_          = 48000.0;
  float   threshold_   = 0.11f;
  float   whitenDecay_ = 0.997f;  // per hop; ~0.4 s memory at 48 kHz
  float   hopEnergy_   = 0.f;
  float   flux_        = 0.f;
  uint32_t anaPos_     = 0;
  int64_t  lastOnsetAbs_ = -1000000;
  int64_t  minIoi_       = 960;  // 20 ms at 48 kHz (aubio's real-time default)
  int64_t  warmupUntil_  = kOnsetFftSize * 2;
};

}  // namespace brainscape::detail
