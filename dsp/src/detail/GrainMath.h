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
  kCount   = 8,  // key stride (power of two): purposes 0-7 fold into the key directly
  // Purposes from 8 on are keyed through the extension below (mode-compiler.md §7.5), which
  // carries purpose >> 3, whichever DrawKey overload is called. Named now, drawn by the waves
  // that build them.
  PitchSelect   = 8,   // W1: a pitch-set entry under `random` selection
  Intermittency = 9,   // W1 (r4): a skipped birth or trigger, ordinal per kind (Granular.h)
  StepShuffle   = 10,  // W2: the step order's shuffle
  StepProb      = 11,  // W2: a step's probability
  MarkWalk      = 12,  // W2: the mark walk
  RandomCutoff  = 13,  // W3: an SVF's random cutoff source
  kMaxPurpose   = 31,  // purpose & 7 and purpose >> 3 (2 bits) cover 0-31
};

// Sound revision 1's key: the absolute sample index times the purpose stride plus a purpose
// below 8. The 64-bit counter is folded in full — truncating it first would repeat the whole
// draw stream every 2^29 samples (~3 h at 48 kHz; review finding). Below 2^29 the key equals
// the truncated one. Only DrawKey calls it, never with a purpose from 8 on: abs·8 + (8 + p) is
// frame abs + 1's key of purpose p.
inline uint32_t FoldedKey(int64_t absSample, uint32_t low) noexcept {
  const auto k64 = static_cast<uint64_t>(absSample) * static_cast<uint32_t>(Draw::kCount) + low;
  return static_cast<uint32_t>(k64) ^ static_cast<uint32_t>(k64 >> 32);
}

// The key extension (mode-compiler.md §7.5, R8): a second layer, several draws of one purpose
// at one frame (step entries sharing a slot) and purposes from 8 on. A 6-bit `ext` packs
//
//   bit 0      the layer (0 or 1)
//   bits 1-3   the same-frame ordinal (0-7)
//   bits 4-5   purpose >> 3 (0-3)
//
// With ext = 0 (layer 0, ordinal 0, a purpose below 8) the key is revision 1's,
// FoldedKey(absSample, purpose), so no key of sound revision 1 changes. Otherwise the frame is
// mixed first and the 9-bit (ext, purpose & 7) after:
//
//   key = Hash32(Hash32(FoldedKey(absSample, 0)) ^ ((ext << 3) | (purpose & 7)))
//
// The 504 extended keys of one frame are distinct: one value XOR-ed with distinct 9-bit values,
// then the bijection Hash32. Keys of two frames meet only where the frames' mixed values
// differ in their low 9 bits alone, by chance (2^-23 per pair of frames), never at a fixed
// frame offset. Mixing the frame in after the extension, Hash32(FoldedKey(abs, p) ^
// Hash32(ext)) as draft v2 of the design had it, leaves the frame linear under the XOR: every
// draw of one ext equalled a draw of another at frame abs ^ (D >> 3), D = Hash32(ext) ^
// Hash32(ext') (lane B review, measured). XOR-ing ext << 56 into the counter before the fold,
// draft v1's scheme, aliased every layer-1 draw with the layer-0 draw 2^21 frames away (record
// §2.8). The packing and this formula become part of the sound with the first wave that draws
// an extended key; test_blob.cpp checks both failure modes.
inline uint32_t DrawKeyExtension(uint32_t layer, uint32_t ordinal, Draw purpose) noexcept {
  const uint32_t high = (static_cast<uint32_t>(purpose) >> 3) & 3u;
  return (layer & 1u) | ((ordinal & 7u) << 1) | (high << 4);
}

inline uint32_t DrawKey(int64_t absSample, Draw purpose, uint32_t layer,
                        uint32_t ordinal) noexcept {
  const uint32_t low = static_cast<uint32_t>(purpose) & 7u;
  const uint32_t ext = DrawKeyExtension(layer, ordinal, purpose);
  if (ext == 0u) return FoldedKey(absSample, low);
  return Hash32(Hash32(FoldedKey(absSample, 0u)) ^ ((ext << 3) | low));
}

// Hash key of the draw for an absolute sample index and a purpose, on layer 0 at ordinal 0:
// revision 1's key for purposes 0-7, the extended key for purposes from 8 on.
inline uint32_t DrawKey(int64_t absSample, Draw purpose) noexcept {
  const auto p = static_cast<uint32_t>(purpose);
  return p < static_cast<uint32_t>(Draw::kCount) ? FoldedKey(absSample, p)
                                                 : DrawKey(absSample, purpose, 0u, 0u);
}

// Uniform [0, 1) from an absolute sample index and a purpose.
inline float RandUnit(int64_t absSample, Draw purpose) noexcept {
  return static_cast<float>(Hash32(DrawKey(absSample, purpose)) >> 8) * (1.0f / 16777216.0f);
}

// Uniform [0, 1) from an extended key: RandUnit above when layer and ordinal are 0.
inline float RandUnit(int64_t absSample, Draw purpose, uint32_t layer,
                      uint32_t ordinal) noexcept {
  return static_cast<float>(Hash32(DrawKey(absSample, purpose, layer, ordinal)) >> 8) *
         (1.0f / 16777216.0f);
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

// The two rails as the table states them; hi < lo means the ring cannot hold the grain.
inline DelayBounds ComputeDelayRails(double outFrames, double ratio, bool reverse,
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
  return b;
}

inline DelayBounds ComputeDelayBounds(double outFrames, double ratio, bool reverse,
                                      uint32_t bufFrames, double marginFrames) noexcept {
  DelayBounds b = ComputeDelayRails(outFrames, ratio, reverse, bufFrames, marginFrames);
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
  float attackInv;   // 1 / attack length (0 if the leg is absent)
  float decayInv;    // 1 / decay length (0 if the leg is absent)
  float gain;        // 1 / window mean = 2 / (1 + sustain)
};

// Shortest envelope leg (determinism profile §3.7); a shorter one is absent. A
// subnormal leg once made 1/a infinite, so EnvValue(0) = 0 * inf = NaN; and with only
// the reciprocal guarded, a subnormal attackEnd put index 0 in the attack leg (0) under
// gradual underflow but past it (1) under a flush mode. Zeroing the leg itself makes
// the geometry the same in every mode.
inline constexpr float kMinEnvLeg = 0x1p-20f;

inline EnvSpec MakeEnv(float outFrames, float sustain, float skew) noexcept {
  if (sustain < 0.f) sustain = 0.f;
  if (sustain > 1.f) sustain = 1.f;
  if (skew < 0.f) skew = 0.f;
  if (skew > 1.f) skew = 1.f;
  const float legs = (1.0f - sustain) * outFrames;
  EnvSpec e;
  float a = legs * skew;
  if (!(a >= kMinEnvLeg)) a = 0.f;
  float dcy = legs - a;
  if (!(dcy >= kMinEnvLeg)) dcy = 0.f;
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
