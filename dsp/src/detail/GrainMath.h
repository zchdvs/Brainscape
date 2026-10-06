#pragma once
#include "detail/FpProfilePrivate.h"

#include <cstdint>

#include "detail/DetMath.h"

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
  Ceiling  = 7,  // fractional-target voice-ceiling dither
  kCount   = 8,  // key stride (power of two)
};

// Hash key of the draw for an absolute sample index and a purpose. The 64-bit
// counter is folded in full — truncating it first would repeat the whole draw
// stream every 2^29 samples (~3 h at 48 kHz; review finding). Below 2^29 the key
// equals the truncated one.
inline uint32_t DrawKey(int64_t absSample, Draw purpose) noexcept {
  const auto k64 = static_cast<uint64_t>(absSample) * static_cast<uint32_t>(Draw::kCount) +
                   static_cast<uint32_t>(purpose);
  return static_cast<uint32_t>(k64) ^ static_cast<uint32_t>(k64 >> 32);
}

// Uniform [0, 1) from an absolute sample index and a purpose.
inline float RandUnit(int64_t absSample, Draw purpose) noexcept {
  return static_cast<float>(Hash32(DrawKey(absSample, purpose)) >> 8) * (1.0f / 16777216.0f);
}

inline float SemitonesToRatio(float st) noexcept {
  // exp2 at grain-birth rate, in-tree: a 1-ULP libm difference here was measured to
  // null at only -108.7 dBFS across builds (review finding). Polynomial kernel or
  // DetMath-built table is decided by DWT of ScheduleGrain (determinism profile §3.9).
  return detmath::Exp2F(st * (1.0f / 12.0f));
}

// Write-head guard bounds (design §3, per-direction table). d = scheduled delay in
// frames behind the anchor; outFrames = grain length in OUTPUT samples; ratio =
// |playback rate|. Reverse convention: the grain starts at its scheduled position
// and reads backward (receding from the write head), so its near guard is trivial
// and its far guard must cover L*(1+r) of recession.
struct DelayBounds {
  double lo, hi;
};

inline DelayBounds ComputeDelayBounds(double outFrames, double ratio, bool reverse,
                                      uint32_t bufFrames, double marginFrames) noexcept {
  DelayBounds b;
  if (reverse) {
    b.lo = marginFrames;
    b.hi = static_cast<double>(bufFrames) - outFrames * (1.0 + ratio) - marginFrames;
  } else {
    b.lo = outFrames * (ratio > 1.0 ? ratio - 1.0 : 0.0) + marginFrames;
    b.hi = static_cast<double>(bufFrames) - outFrames * (ratio < 1.0 ? 1.0 - ratio : 0.0) -
           marginFrames;
  }
  if (b.hi < b.lo) b.hi = b.lo;  // degenerate config: near guard wins
  return b;
}

inline double ClampDelayFrames(double d, double outFrames, double ratio, bool reverse,
                               uint32_t bufFrames, double marginFrames) noexcept {
  const DelayBounds b = ComputeDelayBounds(outFrames, ratio, reverse, bufFrames, marginFrames);
  if (d < b.lo) d = b.lo;
  if (d > b.hi) d = b.hi;
  return d;
}

// Fold a sprayed delay back into the guard bounds by reflection. Clamping instead
// piles a large fraction of the population onto the margin rail as one coherent
// tap (measured 50% of draws at base 1 ms / spray 50 ms — review finding);
// reflection keeps a distribution. Clamp remains only as the final backstop for
// draws beyond one full reflection.
inline double ReflectIntoBounds(double d, const DelayBounds& b) noexcept {
  if (d < b.lo) d = b.lo + (b.lo - d);
  if (d > b.hi) d = b.hi - (d - b.hi);
  if (d < b.lo) d = b.lo;
  if (d > b.hi) d = b.hi;
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
