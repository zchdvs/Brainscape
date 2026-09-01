#pragma once
#include <cmath>
#include <cstdint>

// Pure grain arithmetic (docs/design/grain-engine.md §3-§4): counter-based RNG,
// pitch ratios, write-head guards, and envelope geometry. Everything here is a
// stateless function of its arguments so it can be unit-tested directly and is
// split-invariant by construction.
namespace brainscape::grainmath {

// SplitMix32 — the counter-based hash behind every random draw (design §9: RNG
// keyed on the free-running sample counter, never a stateful stream).
inline uint32_t Hash32(uint32_t x) noexcept {
  x += 0x9E3779B9u;
  x ^= x >> 16;
  x *= 0x21F0AAADu;
  x ^= x >> 15;
  x *= 0x735A2D97u;
  x ^= x >> 15;
  return x;
}

// Draw purposes keep simultaneous draws from colliding (design §9). One grain
// birth per sample per layer at most, so (sample, purpose) is a unique key.
enum class Draw : uint32_t {
  Interval = 0,
  Spray    = 1,
  Detune   = 2,
  Reverse  = 3,
  Pan      = 4,
  DitherL  = 5,  // owned by the ring-write dither in Engine.cpp
  DitherR  = 6,
  kCount   = 8,  // key stride (power of two)
};

// Uniform [0, 1) from an absolute sample index and a purpose.
inline float RandUnit(int64_t absSample, Draw purpose) noexcept {
  const auto key = static_cast<uint32_t>(absSample) * static_cast<uint32_t>(Draw::kCount) +
                   static_cast<uint32_t>(purpose);
  return static_cast<float>(Hash32(key) >> 8) * (1.0f / 16777216.0f);
}

inline float SemitonesToRatio(float st) noexcept {
  // exp2f at grain-birth rate; the M7 build swaps this for LUT + lerp when the
  // §8 schedule-time budget is measured (design §3).
  return std::exp2(st * (1.0f / 12.0f));
}

// Write-head guards (design §3, per-direction table). d = scheduled delay in
// frames behind the anchor; outFrames = grain length in OUTPUT samples; ratio =
// |playback rate|. Reverse convention: the grain starts at its scheduled position
// and reads backward (receding from the write head), so its near guard is trivial
// and its far guard must cover L*(1+r) of recession.
inline double ClampDelayFrames(double d, double outFrames, double ratio, bool reverse,
                               uint32_t bufFrames, double marginFrames) noexcept {
  double lo, hi;
  if (reverse) {
    lo = marginFrames;
    hi = static_cast<double>(bufFrames) - outFrames * (1.0 + ratio) - marginFrames;
  } else {
    lo = outFrames * (ratio > 1.0 ? ratio - 1.0 : 0.0) + marginFrames;
    hi = static_cast<double>(bufFrames) - outFrames * (ratio < 1.0 ? 1.0 - ratio : 0.0) -
         marginFrames;
  }
  if (hi < lo) hi = lo;  // degenerate config: near guard wins (validator warns upstream)
  if (d < lo) d = lo;
  if (d > hi) d = hi;
  return d;
}

// Envelope geometry (design §3 windowing): piecewise attack / flat / decay with
// both legs normalized to unit peak. sustain = flat-top fraction, skew splits the
// non-flat portion between attack and decay. With the half-cosine smoothing LUT
// (mean 0.5, same as a linear leg), the window mean is (1 + sustain)/2 regardless
// of skew and smoothness — so the level compensation is exact, not estimated.
struct EnvSpec {
  float total;       // grain length in output frames
  float attackEnd;   // output-frame index where the attack leg ends
  float decayStart;  // output-frame index where the decay leg starts
  float attackInv;   // 1 / attack length (0 if no attack)
  float decayInv;    // 1 / decay length (0 if no decay)
  float gain;        // 1 / window mean = 2 / (1 + sustain)
};

inline EnvSpec MakeEnv(float outFrames, float sustain, float skew) noexcept {
  if (sustain < 0.f) sustain = 0.f;
  if (sustain > 1.f) sustain = 1.f;
  if (skew < 0.f) skew = 0.f;
  if (skew > 1.f) skew = 1.f;
  const float legs = (1.0f - sustain) * outFrames;
  EnvSpec e;
  const float a   = legs * skew;
  const float dcy = legs - a;
  e.total      = outFrames;
  e.attackEnd  = a;
  e.decayStart = outFrames - dcy;
  e.attackInv  = a > 0.f ? 1.0f / a : 0.f;
  e.decayInv   = dcy > 0.f ? 1.0f / dcy : 0.f;
  e.gain       = 2.0f / (1.0f + sustain);
  return e;
}

// Unit-peak piecewise envelope value at output-frame index i (before LUT morph).
inline float EnvValue(const EnvSpec& e, float i) noexcept {
  if (i < e.attackEnd) return i * e.attackInv;
  if (i > e.decayStart) {
    const float v = (e.total - i) * e.decayInv;
    return v > 0.f ? (v < 1.f ? v : 1.f) : 0.f;
  }
  return 1.0f;
}

}  // namespace brainscape::grainmath
