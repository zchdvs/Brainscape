#pragma once
#include <cstdint>

// Spectral-flux onset detector with adaptive whitening and Dixon's peak-picker
// (docs/research/onset-detection-on-cortex-m7.md recs #2/#4 and §2.2; design §4).
//
// 512-sample Hann window, 256-sample hop, hop boundaries aligned to the ABSOLUTE
// sample counter — so detection is block-split invariant by construction. The FFT
// is a small in-tree radix-2 (not CMSIS/libm) so firmware and plugin compute
// bit-identical spectra; swapping the M7 build to arm_rfft_fast_f32 is a
// measured-budget decision that would also need the contract-#7 determinism note.
//
// The decision layer is Dixon's peak-picker (moving-mean baseline + margin +
// causal rising condition — the research says the moving mean "does most of the
// work"), plus bonk~-style growth hysteresis to stop chatter on sustained
// material. A bare fixed threshold on whitened flux free-ran at the min-IOI rate
// on any broadband or held-distorted source (review finding, measured 231
// fires/5 s on hiss); the sensitivity knob maps to Dixon's margin delta.
//
// Calibration is for 44.1/48 kHz: window and hop are fixed in SAMPLES, so time
// and frequency resolution scale with rate (at 96 kHz+ the window stops
// resolving guitar fundamentals). The whitening memory IS rate-derived (~0.4 s).
//
// Honesty note carried from the research: no detection function solves both
// heavily-distorted sustained material and slow-attack pads — this layer ships
// detection + a visible onset count + always-working manual/external fallbacks,
// and is not marketed as "solved".
namespace brainscape::detail {

inline constexpr uint32_t kOnsetFftSize = 512;
inline constexpr uint32_t kOnsetHop     = 256;
inline constexpr uint32_t kOnsetBins    = kOnsetFftSize / 2 + 1;
inline constexpr uint32_t kOnsetMeanWin = 9;  // Dixon's mw — hops in the moving mean

class OnsetDetector {
 public:
  static uint32_t WarmFloats() noexcept {
    // hann + analysis ring + re + im + twiddle cos/sin
    // + prev raw whitened + prev averaged whitened + peak memory
    return kOnsetFftSize * 4u + (kOnsetFftSize / 2u) * 2u + kOnsetBins * 3u;
  }

  void Init(float* warm, double sampleRate) noexcept;
  void Reset() noexcept;
  void SetSensitivity(float s01) noexcept;  // control rate; maps to Dixon's delta

  // Feed one mono sample at absolute engine sample `abs`. Returns true when this
  // sample completes an analysis hop that contains an onset. Detection latency
  // measured at ~5.3 ms end-to-end (review), inside the research's 11-16 ms
  // budget for a causal 512/256 detector.
  bool ProcessSample(float x, int64_t abs) noexcept;

 private:
  void AnalyzeHop() noexcept;  // fills flux_ from the last 512 samples

  float* hann_    = nullptr;  // [512]
  float* ana_     = nullptr;  // [512] circular input window
  float* re_      = nullptr;  // [512]
  float* im_      = nullptr;  // [512]
  float* twCos_   = nullptr;  // [256]
  float* twSin_   = nullptr;  // [256]
  float* prevW_   = nullptr;  // [257] previous hop's RAW whitened magnitudes
  float* prevAvg_ = nullptr;  // [257] previous 2-hop-averaged whitened magnitudes.
                              // The flux differences 2-hop AVERAGES: a steady
                              // non-bin-aligned tone's leakage alternates with the
                              // window phase (period-2 flutter) and free-ran the
                              // detector once the whitening memory decayed into it
                              // (review finding, 242 fires/10 s); averaging two
                              // hops cancels the alternation exactly and costs one
                              // hop of onset smear inside the min-IOI window.
  float* peakMem_ = nullptr;  // [257] adaptive-whitening peak memory

  double  sr_          = 48000.0;
  float   delta_       = 0.15f;   // Dixon margin over the moving mean
  float   whitenDecay_ = 0.987f;  // per hop, derived at Init: ~0.4 s 1/e memory
                                  // (research Open Question #3 — the S&P primary
                                  // value is unverified; 0.4 s measurably recovers
                                  // quiet-after-loud, review finding)
  float   hopEnergy_   = 0.f;
  float   flux_        = 0.f;
  float   prevFlux_    = 0.f;
  float   fluxRing_[kOnsetMeanWin] = {};  // last mw flux values (moving-mean baseline)
  uint32_t fluxRingPos_ = 0;
  uint32_t fluxRingLen_ = 0;
  bool     armed_       = true;  // bonk~-style growth hysteresis: re-arm only after
                                 // flux falls back under the baseline
  uint32_t anaPos_      = 0;
  int64_t  lastOnsetAbs_ = -1000000;
  int64_t  minIoi_       = 1024;  // rounded to whole hops at Init (aubio's 20 ms
                                  // quantizes to 21.3 ms on the 256-sample hop grid)
  uint32_t warmupHops_   = 4;     // RELATIVE to Reset — an absolute index re-armed a
                                  // spurious onset on every mid-stream Reset (review)
};

}  // namespace brainscape::detail
