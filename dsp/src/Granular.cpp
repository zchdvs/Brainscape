#include "detail/FpProfilePrivate.h"

#include "detail/Granular.h"

#include <cassert>
#include <cstring>
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>  // _BitScanForward
#endif

#include "detail/DetMath.h"
#include "detail/Placement.h"

// Every float-to-int conversion below has a proven range, asserted in Debug builds:
// out-of-range conversions differ by ISA (determinism profile §3.10).
namespace brainscape::detail {

using grainmath::Draw;
using grainmath::RandUnit;

namespace {

constexpr float kInvScale = 1.0f / 32767.0f;

// A ratio's signed 32.32 increment, ratio * 2^32: the ratio is a normal float m * 2^(E - 150)
// (m its 24-bit significand, E its biased exponent), so the product is m * 2^(E - 118), an
// integer, exactly what RoundHalfAwayI64(double(ratio) * 2^32) returned. Built from the bits, it
// needs no conversion between int64 and double (no __aeabi_d2lz or __aeabi_l2d on the M7).
inline int64_t RatioToInc(float ratio) noexcept {
  assert(ratio >= 0.25f && ratio <= 4.0f);  // the composed pitch is clamped to ±24 st
  uint32_t bits;
  std::memcpy(&bits, &ratio, sizeof bits);
  const uint32_t e = bits >> 23;  // the sign is 0; 125 to 129 for [0.25, 4]
  const auto     m = static_cast<int64_t>((bits & 0x7FFFFFu) | 0x800000u);
  return m << (e - 118u);
}

// One out-of-line copy of MakeEnv for the birth's two uses (ITCM): a grain of `frames` frames.
BRAINSCAPE_NOINLINE grainmath::EnvSpec EnvOf(uint32_t frames, float sustain, float skew) noexcept {
  return grainmath::MakeEnv(static_cast<float>(frames), sustain, skew);
}

// The always-applied LUT morph of a unit-peak envelope value (grainmath::EnvValue), times the
// window's gain: at smoothness 0 the morph is an exact no-op (env + 0*(lut-env) == env), so no
// per-sample branch is needed and the rectangular/unity null stays bit-exact.
inline float MorphEnv(float env, float smoothness, float gain, const float* lut) noexcept {
  const float x  = env * static_cast<float>(kWindowLutSize - 1);
  assert(x >= 0.f && x <= static_cast<float>(kWindowLutSize - 1));  // finite env in [0, 1]
  const auto  i0 = static_cast<uint32_t>(x);
  const auto  i1 = i0 + 1u < kWindowLutSize ? i0 + 1u : i0;
  const float fr = x - static_cast<float>(i0);
  const float lv = lut[i0] + (lut[i1] - lut[i0]) * fr;
  env += smoothness * (lv - env);
  return env * gain;
}

// A run's envelope is computed in chunks of this many frames into a stack buffer (DTCM on the
// pedal), ahead of the reads (cpu-budget.md §4.3, step 4).
constexpr uint32_t kEnvChunk = 64;

// env[k] = MorphEnv(EnvValue(e, i0 + k), sm, e.gain, lut) for k in [0, count): the same
// operations on the same operands as frame by frame, with the legs bounded once. For an integer
// index i (exact in binary32: below 2^24), EnvValue's `i < attackEnd` holds exactly for
// i < A = ceil(attackEnd), and its `i > decayStart` for i >= floor(decayStart) + 1; attack is
// tested first, so the decay leg starts at max(A, that). Each leg runs four independent frames
// at a time.
void FillEnvelope(const grainmath::EnvSpec& e, float sm, const float* lut, uint32_t i0,
                  uint32_t count, float* __restrict env) noexcept {
  assert(e.attackEnd >= 0.f && e.attackEnd <= e.total && e.total < 16777216.f);
  assert(e.decayStart >= 0.f && e.decayStart <= e.total);
  auto A = static_cast<uint32_t>(e.attackEnd);
  if (static_cast<float>(A) < e.attackEnd) ++A;
  auto D = static_cast<uint32_t>(e.decayStart) + 1u;
  if (D < A) D = A;
  const float    aInv = e.attackInv, dInv = e.decayInv, tot = e.total, gain = e.gain;
  const uint32_t iEnd = i0 + count;
  uint32_t       k    = 0;
  float          fi   = static_cast<float>(i0);  // i0 + k, exact
  // Attack: i * attackInv.
  uint32_t stop = A < iEnd ? (A > i0 ? A - i0 : 0u) : count;
  for (; k + 4u <= stop; k += 4u) {
    const float f1 = fi + 1.0f, f2 = fi + 2.0f, f3 = fi + 3.0f;
    const float e0 = MorphEnv(fi * aInv, sm, gain, lut);
    const float e1 = MorphEnv(f1 * aInv, sm, gain, lut);
    const float e2 = MorphEnv(f2 * aInv, sm, gain, lut);
    const float e3 = MorphEnv(f3 * aInv, sm, gain, lut);
    env[k]      = e0;
    env[k + 1u] = e1;
    env[k + 2u] = e2;
    env[k + 3u] = e3;
    fi += 4.0f;
  }
  for (; k < stop; ++k) {
    env[k] = MorphEnv(fi * aInv, sm, gain, lut);
    fi += 1.0f;
  }
  // Flat top: EnvValue is exactly 1, so every frame morphs to one value.
  stop = D < iEnd ? (D > i0 ? D - i0 : 0u) : count;
  if (k < stop) {
    const float flat = MorphEnv(1.0f, sm, gain, lut);
    for (; k < stop; ++k) env[k] = flat;
    fi = static_cast<float>(i0 + k);
  }
  // Decay: (total - i) * decayInv clamped to [0, 1]. The index stays below the pass's length,
  // e.total, so total - i >= 1 exactly, and a decay frame exists only with a decay leg, whose
  // decayInv is positive and finite: the value is positive, and the clamp is its upper half.
  for (; k + 4u <= count; k += 4u) {
    const float f1 = fi + 1.0f, f2 = fi + 2.0f, f3 = fi + 3.0f;
    const float v0 = (tot - fi) * dInv, v1 = (tot - f1) * dInv;
    const float v2 = (tot - f2) * dInv, v3 = (tot - f3) * dInv;
    assert(v0 > 0.f && v1 > 0.f && v2 > 0.f && v3 > 0.f);
    const float e0 = MorphEnv(v0 < 1.f ? v0 : 1.f, sm, gain, lut);
    const float e1 = MorphEnv(v1 < 1.f ? v1 : 1.f, sm, gain, lut);
    const float e2 = MorphEnv(v2 < 1.f ? v2 : 1.f, sm, gain, lut);
    const float e3 = MorphEnv(v3 < 1.f ? v3 : 1.f, sm, gain, lut);
    env[k]      = e0;
    env[k + 1u] = e1;
    env[k + 2u] = e2;
    env[k + 3u] = e3;
    fi += 4.0f;
  }
  for (; k < count; ++k) {
    const float v = (tot - fi) * dInv;
    assert(v > 0.f);
    env[k] = MorphEnv(v < 1.f ? v : 1.f, sm, gain, lut);
    fi += 1.0f;
  }
}

// Unroll the next loop by two where the compiler takes a request (GCC and Clang; MSVC decides
// alone). It changes only the schedule: each frame's operations stay in order.
#define BS_PRAGMA(x) _Pragma(#x)
#if defined(__clang__)
#define BS_UNROLL2 BS_PRAGMA(clang loop unroll_count(2))
#elif defined(__GNUC__)
#define BS_UNROLL2 BS_PRAGMA(GCC unroll 2)
#else
#define BS_UNROLL2
#endif

#ifndef NDEBUG
// Contract #1's ring invariant (determinism-profile.md §5.7): Pass 1 writes the
// whole block before grains render, so at output sample n the frames just past
// the live head W(n) hold new input in a large block but one-ring-old audio in a
// small one. No interpolation tap (numTaps frames from firstTap) may land within
// [1, kBlockWriteAheadFrames] ahead of W(n); W(n) itself was written by sample n
// under any split, so it is legal.
bool TapsClearOfWriteAhead(uint32_t firstTap, uint32_t numTaps, uint32_t liveFrame,
                           uint32_t mask) noexcept {
  for (uint32_t t = 0; t < numTaps; ++t) {
    const uint32_t ahead = (firstTap + t - liveFrame) & mask;
    if (ahead >= 1u && ahead <= kBlockWriteAheadFrames) return false;
  }
  return true;
}

// The invariant needs a ring that holds the grain: the write-ahead window, the grain's
// span and both margins. On a smaller ring ComputeDelayBounds lets the near guard win
// and the grain reads inside the window: a legal configuration (Init accepts 8 frames)
// that no rail can make block-split invariant (determinism-profile.md §5.7, "Not
// covered"), so the assertion skips those grains. g.inc is the float ratio times 2^32,
// exact, so these are the rails ScheduleGrain computed.
bool GrainFitsRing(const Grain& g, uint32_t bufLen) noexcept {
  const int64_t mag  = g.inc < 0 ? -g.inc : g.inc;
  const auto    rail = grainmath::ComputeDelayRails(
      static_cast<double>(g.passLen), static_cast<double>(mag) * 0x1p-32, g.inc < 0,
      bufLen > kBlockWriteAheadFrames ? bufLen - kBlockWriteAheadFrames : 0u,
      kGuardMarginFrames,
      static_cast<double>(g.passes - 1u) * static_cast<double>(g.passLen));
  return !(rail.hi < rail.lo);
}
#endif

}  // namespace

void GranularCore::ScheduleGrain(uint32_t slot, const GranularParams& p, int64_t birthAbs,
                                 uint32_t anchorFrame, uint32_t liveFrame,
                                 uint32_t drawOrdinal) noexcept {
  // One pass's length L (the grain length); a voice reads its region `passes` times
  // (mode-compiler.md §7.5, R11), so its life is passes * L.
  uint32_t       total   = p.totalFrames >= 1u ? p.totalFrames : 1u;
  const uint32_t passes  = p.repeat >= 1u ? p.repeat : 1u;
  // The envelope of a grain of the block's length, cached while the length, sustain and skew stay
  // (cpu-budget.md §4.3, step 7): MakeEnv is a function of them.
  if (envTotal_ != total || std::memcmp(&envSustain_, &p.sustain, sizeof(float)) != 0 ||
      std::memcmp(&envSkew_, &p.skew, sizeof(float)) != 0) {
    envTotal_   = total;
    envSustain_ = p.sustain;
    envSkew_    = p.skew;
    env_        = EnvOf(total, p.sustain, p.skew);
  }
  const int64_t  drawKey = birthAbs - drawEpoch_;

  // Resolve everything once (design §3): pitch -> ratio -> signed increment. The pitch is the
  // set's entry plus the transpose leaf, then detune, then the clamp (mode-compiler.md §7.5).
  // Each draw at the birth's key and ordinal (kOrdinalClock for a CLOCK hit, else 0, whose keys
  // are revision 1's: RandUnit(key, purpose, 0, 0) is RandUnit(key, purpose)).
  const uint32_t ord   = drawOrdinal;
  const uint32_t entry = PickPitch(p, drawKey, ord);
  float          st    = p.pitchSt[entry];
  const bool     detune = p.spreadCents > 0.f;
  if (detune) {
    st += (RandUnit(drawKey, Draw::Detune, 0u, ord) * 2.0f - 1.0f) * p.spreadCents * 0.01f;
  }
  // Enforce the design's r_max = 4 ratio ceiling on the COMPOSED value — detune
  // on top of a maxed pitch otherwise exceeds what the guards/budgets assume.
  if (st > 24.f) st = 24.f;
  if (st < -24.f) st = -24.f;
  // Without detune an entry's ratio repeats birth after birth: it is cached per entry, keyed by
  // the semitones' bits (cpu-budget.md §4.3, step 7). SemitonesToRatio is a function of them.
  uint32_t key;
  std::memcpy(&key, &st, sizeof key);
  float ratio = ratioOf_[entry];
  if (detune || ratioKey_[entry] != key) {
    ratio = grainmath::SemitonesToRatio(st);
    if (!detune) {
      ratioKey_[entry] = key;
      ratioOf_[entry]  = ratio;
    }
  }
  const bool  reverse =
      p.reverseProb > 0.f && RandUnit(drawKey, Draw::Reverse, 0u, ord) < p.reverseProb;

  // Position: POS_LIVE (base delay behind the anchor) or POS_MARK (the most
  // recent eligible onset mark — the Strum family's mechanism, design §4).
  double  d          = p.baseDelayFrames;
  bool    markActive = false;
  int64_t markAbs    = 0;  // the frame the mark was recorded at, for its age
  for (uint32_t k = 0; p.posFromMark && k < markCount_; ++k) {
    const Mark& m = marks_[(markHead_ + kMaxMarks - 1u - k) % kMaxMarks];
    // Staleness guard: once the write head has lapped the ring, the modular
    // distance aliases to a small value and the "mark" is live audio (review
    // finding) — fall back to the base position instead. Older marks are staler.
    if (birthAbs - m.abs >= static_cast<int64_t>(mask_)) break;
    // Pin-eligible marks (design §2.4): while frozen, a mark recorded after the
    // pin is skipped, so freeze holds the Strum position too. Measured from the
    // pin, such a mark wrapped to ~one ring and clamped onto the far rail.
    // Unfrozen, the two distances are equal and the newest mark wins.
    const uint32_t dPin = (anchorFrame - m.frame) & mask_;
    if (dPin > ((liveFrame - m.frame) & mask_)) continue;
    d          = static_cast<double>(dPin);
    markActive = true;
    markAbs    = m.abs;
    break;
  }
  if (markActive) {
    // Read attack-length earlier so the marked transient lands at the
    // envelope's flat top, not 15-31 dB down the fade-in (review finding).
    d += static_cast<double>(env_.attackEnd);
    // Upward pitch inflates the near guard past the mark distance and would
    // silently clamp the grain onto pre-onset material (review finding): cap
    // the grain length so the guard fits, instead of losing the transient.
    if (ratio > 1.0f) {
      const double maxOut = (d - kGuardMarginFrames) / (static_cast<double>(ratio) - 1.0);
      if (maxOut < static_cast<double>(total)) {  // converted only in [16, total)
        total = maxOut >= 16.0 ? static_cast<uint32_t>(maxOut) : 16u;
      }
    }
  }
  // Spray is reflected (not clamped) into the per-direction guard bounds so it
  // keeps a distribution instead of piling onto the margin rail (review finding).
  // The far rail also excludes the frames Pass 1 has already written past the
  // live head this block (up to kBlockWriteAheadFrames - 1): the 64-frame margin
  // alone left far-rail reads block-size dependent. A shared build constant,
  // never cfg.maxBlockSize, so pedal and plugin clamp identically.
  // The far rail covers the whole life: the write head moves (passes - 1) * L further on while
  // the later passes re-read the region (mode-compiler.md §7.5).
  const uint32_t bufLen = mask_ + 1u;
  auto bounds = grainmath::ComputeDelayBounds(
      static_cast<double>(total), static_cast<double>(ratio), reverse,
      bufLen > kBlockWriteAheadFrames ? bufLen - kBlockWriteAheadFrames : 0u,
      kGuardMarginFrames, static_cast<double>(passes - 1u) * static_cast<double>(total));
  // The guards protect against the LIVE write head, but d is measured from the
  // anchor. While frozen the live head keeps recording `age` frames past the pin,
  // so the far rail moves age frames closer; guards measured from the pin let
  // grains overtake, or start ahead of, the live head. The near rail stays
  // pin-relative (the grain stays inside the pinned window); if the two cross,
  // the live-head rail wins — a clamp changes the sound, a write-ahead read
  // breaks block-split invariance.
  const uint32_t age = (liveFrame - anchorFrame) & mask_;
  if (age != 0) {
    bounds.hi -= static_cast<double>(age);
    if (bounds.hi < bounds.lo) bounds.lo = bounds.hi;
  }
  if (p.sprayFrames > 0.f) {
    d += static_cast<double>((RandUnit(drawKey, Draw::Spray, 0u, ord) * 2.0f - 1.0f) *
                             p.sprayFrames);
    d = grainmath::ReflectIntoBounds(d, bounds);
  } else {
    d = d < bounds.lo ? bounds.lo : (d > bounds.hi ? bounds.hi : d);
  }

  Grain& g = grains_[slot];
  // d is at most the larger of the ring and the near guard, L * (r - 1) + margin
  // with r <= 4. It is at least the margin minus the pin's age (< ring <= 2^26):
  // while frozen the live-head rail can pull the far rail below the near one, and
  // on a small ring with long grains below zero (the grain then starts ahead of the
  // pin, still behind the live head). RoundHalfAwayI32 needs only |d| < 2^31; the
  // negative result wraps modulo 2^32 under the mask as intended.
  assert(d > kGuardMarginFrames - static_cast<double>(bufLen) && d <= 0x1p27);
  const auto startFrame =
      (anchorFrame - static_cast<uint32_t>(detmath::RoundHalfAwayI32(d))) & mask_;
  g.pos    = static_cast<uint64_t>(startFrame) << 32;
  int64_t inc = RatioToInc(ratio);
  if (reverse) inc = -inc;
  g.inc        = inc;
  g.passLen    = total;
  g.passes     = static_cast<uint8_t>(passes);
  g.pass       = 0;
  g.startFrame = startFrame;
  g.total      = passes * total;  // at most 16 * 24,000 frames
  g.rendered   = 0;
  g.fadeStart  = kNoFade;
  g.endAbs     = birthAbs + g.total;
  // A mark-capped grain is shorter than the block's length: its own envelope.
  g.env = total == envTotal_ ? env_ : EnvOf(total, p.sustain, p.skew);
  // Decay (mode-compiler.md §7.5): the position reference's age at birth, a mark's (a live
  // position is the present, age 0), then at each pass's start.
  assert(!markActive || (birthAbs >= markAbs && birthAbs - markAbs < static_cast<int64_t>(mask_)));
  g.age0      = markActive ? static_cast<uint32_t>(birthAbs - markAbs) : 0u;
  g.decayRate = p.decayRate;
  g.decayGain = grainmath::DecayGain(g.age0, g.decayRate);
  g.smoothness = p.smoothness;
  g.tier       = slot < kHiFiGrains ? 0 : 1;

  // Equal-power pan around center, width = panSpread (grain-delay-theory.md §3.9), and exact
  // unity at center pan so the degenerate-delay null holds bit-exactly: without spread the
  // gains are 1, so neither the pan's draw (a pure function of its key) nor SinCosD is made.
  if (p.panSpread == 0.0f) {
    g.gainL = 1.0f;
    g.gainR = 1.0f;
  } else {
    const float pan = 0.5f + p.panSpread * (RandUnit(drawKey, Draw::Pan, 0u, ord) - 0.5f);
    double      panSin, panCos;
    detmath::SinCosD(static_cast<double>(pan * 1.5707963267948966f), &panSin, &panCos);
    g.gainL = static_cast<float>(panCos);
    g.gainR = static_cast<float>(panSin);
  }
  g.active = true;
  HeapPlace(slot);
}

uint32_t GranularCore::PickPitch(const GranularParams& p, int64_t drawKey,
                                 uint32_t drawOrdinal) noexcept {
  // `random`: the birth's own draw (purpose 8, R8's extended key), so births at one frame share
  // it as they share every draw, but for a CLOCK hit's, at its own ordinal; `cycle`: every birth
  // advances it, from any source.
  if (p.pitchRandom) {
    const uint32_t bits = grainmath::RandBits24(drawKey, Draw::PitchSelect, 0u, drawOrdinal);
    return grainmath::RandomEntry(bits, p.pitchWeight,
                                  p.pitchCount, p.pitchWeightSum);
  }
  return grainmath::NextCycleEntry(&pitchCycle_, p.pitchWeight, p.pitchCount);
}

// Out of line and not cloned: RenderSpan calls it for a run without and a run with a fade, and
// one copy of its three frame loops is enough (ITCM, cpu-budget.md §7.3); the call is per run,
// not per frame.
#if defined(__GNUC__) && !defined(__clang__)
#define BS_ONE_COPY BRAINSCAPE_NOINLINE __attribute__((noclone))
#else
#define BS_ONE_COPY BRAINSCAPE_NOINLINE
#endif
BS_ONE_COPY void GranularCore::RenderRun(Grain& g, uint32_t s, uint32_t e, bool fade, float* wetL,
                                         float* wetR) noexcept {
  // Everything the frame loops read, in locals: the wet buffers are float stores that could
  // otherwise alias the Grain and force its fields to be reloaded every frame.
  uint64_t             pos  = g.pos;
  const int64_t        inc  = g.inc;
  const int16_t* const ring = ring_;
  const uint32_t       mask = mask_;
  // The pass's decay gain on the pan gains: exactly them when it is 1, as without decay.
  const float gl = g.gainL * g.decayGain, gr = g.gainR * g.decayGain;
  // The envelope runs over one pass: its index restarts with each. The life frame counts on
  // over the passes, for a FastCut's fade (mode-compiler.md §7.3): 1 at fadeStart, then down by
  // 1/128 a frame; the grain ends where it would reach 0. Exact multiples of 2^-7.
  uint32_t        envIdx    = g.rendered - static_cast<uint32_t>(g.pass) * g.passLen;
  uint32_t        idx       = g.rendered;
  const uint32_t  fadeEnd   = fade ? g.fadeStart + kGranularFastCutFrames : 0u;
  constexpr float kFadeStep = 1.0f / static_cast<float>(kGranularFastCutFrames);
  constexpr float kFrac     = 1.0f / 4294967296.0f;
  // Tier / unity resolved per grain (design §3): three frame loops.
  const int kind = inc == kUnityInc ? 0 : (g.tier == 0 ? 1 : 2);
#ifndef NDEBUG
  const bool fits = GrainFitsRing(g, mask_ + 1u);
#endif
  // The two Hot-arena accumulators never overlap each other or the envelope buffer.
  float* __restrict const outL = wetL;
  float* __restrict const outR = wetR;
  float                   env[kEnvChunk];

  for (uint32_t n0 = s; n0 < e; n0 += kEnvChunk) {
    const uint32_t cnt = e - n0 < kEnvChunk ? e - n0 : kEnvChunk;
    FillEnvelope(g.env, g.smoothness, lut_, envIdx, cnt, env);
    if (fade) {
      for (uint32_t k = 0; k < cnt; ++k) {
        env[k] = env[k] * (static_cast<float>(fadeEnd - (idx + k)) * kFadeStep);
      }
    }
    float* __restrict const l = outL + n0;
    float* __restrict const r = outR + n0;
    if (kind == 0) {
      BS_UNROLL2
      for (uint32_t k = 0; k < cnt; ++k) {
        const auto f = static_cast<uint32_t>(pos >> 32) & mask;
        assert(!fits || TapsClearOfWriteAhead(f, 1u, blockRingStart_ + n0 + k, mask_));
        const float ev = env[k];
        l[k] += static_cast<float>(ring[2u * f]) * kInvScale * ev * gl;
        r[k] += static_cast<float>(ring[2u * f + 1u]) * kInvScale * ev * gr;
        pos += static_cast<uint64_t>(inc);
      }
    } else if (kind == 1) {
      BS_UNROLL2
      for (uint32_t k = 0; k < cnt; ++k) {
        const auto f1 = static_cast<uint32_t>(pos >> 32);  // masked per tap
        assert(!fits || TapsClearOfWriteAhead(f1 - 1u, 4u, blockRingStart_ + n0 + k, mask_));
        const float    fr = static_cast<float>(static_cast<uint32_t>(pos)) * kFrac;
        const uint32_t am = 2u * ((f1 - 1u) & mask), a0 = 2u * (f1 & mask);
        const uint32_t a1 = 2u * ((f1 + 1u) & mask), a2 = 2u * ((f1 + 2u) & mask);
        const float    ev = env[k];
        {  // ReadHermite's left channel
          const float xm1  = static_cast<float>(ring[am]);
          const float x0   = static_cast<float>(ring[a0]);
          const float x1   = static_cast<float>(ring[a1]);
          const float x2   = static_cast<float>(ring[a2]);
          const float c    = (x1 - xm1) * 0.5f;
          const float v    = x0 - x1;
          const float w    = c + v;
          const float a    = w + v + (x2 - x0) * 0.5f;
          const float bNeg = w + a;
          l[k] += ((((a * fr) - bNeg) * fr + c) * fr + x0) * kInvScale * ev * gl;
        }
        {  // and its right
          const float xm1  = static_cast<float>(ring[am + 1u]);
          const float x0   = static_cast<float>(ring[a0 + 1u]);
          const float x1   = static_cast<float>(ring[a1 + 1u]);
          const float x2   = static_cast<float>(ring[a2 + 1u]);
          const float c    = (x1 - xm1) * 0.5f;
          const float v    = x0 - x1;
          const float w    = c + v;
          const float a    = w + v + (x2 - x0) * 0.5f;
          const float bNeg = w + a;
          r[k] += ((((a * fr) - bNeg) * fr + c) * fr + x0) * kInvScale * ev * gr;
        }
        pos = static_cast<uint64_t>(static_cast<int64_t>(pos) + inc);
      }
    } else {
      BS_UNROLL2
      for (uint32_t k = 0; k < cnt; ++k) {
        const auto f0 = static_cast<uint32_t>(pos >> 32) & mask;
        assert(!fits || TapsClearOfWriteAhead(static_cast<uint32_t>(pos >> 32), 2u,
                                              blockRingStart_ + n0 + k, mask_));
        const auto  f1 = (f0 + 1u) & mask;
        const float fr = static_cast<float>(static_cast<uint32_t>(pos)) * kFrac;
        const float ev = env[k];
        const float l0 = static_cast<float>(ring[2u * f0]);
        const float l1 = static_cast<float>(ring[2u * f1]);
        const float r0 = static_cast<float>(ring[2u * f0 + 1u]);
        const float r1 = static_cast<float>(ring[2u * f1 + 1u]);
        l[k] += (l0 + (l1 - l0) * fr) * kInvScale * ev * gl;
        r[k] += (r0 + (r1 - r0) * fr) * kInvScale * ev * gr;
        pos = static_cast<uint64_t>(static_cast<int64_t>(pos) + inc);
      }
    }
    envIdx += cnt;
    idx += cnt;
  }
  g.pos = pos;
  g.rendered += e - s;
}

// ── The sounding voices (cpu-budget.md §4.3, step 6) ─────────────────────────────────────
// A binary min-heap of the slots whose voices sound, keyed by endAbs, with each slot's place in
// it, and the same slots as a 64-bit mask. The order among equal ends is immaterial: every query
// first retires all the voices that ended by its frame.

namespace {
// The index of the lowest set bit of x != 0.
inline uint32_t LowestSetBit(uint32_t x) noexcept {
  assert(x != 0u);
#if defined(_MSC_VER) && !defined(__clang__)
  unsigned long i;
  _BitScanForward(&i, x);
  return static_cast<uint32_t>(i);
#else
  return static_cast<uint32_t>(__builtin_ctz(x));
#endif
}
}  // namespace

uint32_t GranularCore::LowestFreeSlot() const noexcept {
  if (live_[0] != 0xFFFFFFFFu) return LowestSetBit(~live_[0]);
  if (live_[1] != 0xFFFFFFFFu) return 32u + LowestSetBit(~live_[1]);
  return kGranularMaxGrains;
}

void GranularCore::HeapSiftUp(uint32_t i) noexcept {
  const uint8_t slot = heap_[i];
  const int64_t end  = grains_[slot].endAbs;
  while (i > 0u) {
    const uint32_t up = (i - 1u) >> 1;
    const uint8_t  s  = heap_[up];
    if (!(end < grains_[s].endAbs)) break;
    heap_[i]    = s;
    heapPos_[s] = static_cast<uint8_t>(i);
    i           = up;
  }
  heap_[i]       = slot;
  heapPos_[slot] = static_cast<uint8_t>(i);
}

BRAINSCAPE_NOINLINE void GranularCore::HeapSiftDown(uint32_t i) noexcept {
  const uint8_t slot = heap_[i];
  const int64_t end  = grains_[slot].endAbs;
  for (;;) {
    uint32_t c = 2u * i + 1u;
    if (c >= heapCount_) break;
    if (c + 1u < heapCount_ && grains_[heap_[c + 1u]].endAbs < grains_[heap_[c]].endAbs) ++c;
    const uint8_t s = heap_[c];
    if (!(grains_[s].endAbs < end)) break;
    heap_[i]    = s;
    heapPos_[s] = static_cast<uint8_t>(i);
    i           = c;
  }
  heap_[i]       = slot;
  heapPos_[slot] = static_cast<uint8_t>(i);
}

BRAINSCAPE_NOINLINE void GranularCore::HeapPlace(uint32_t slot) noexcept {
  assert(slot < kGranularMaxGrains);
  uint32_t&      word = live_[slot >> 5];
  const uint32_t bit  = 1u << (slot & 31u);
  if ((word & bit) == 0u) {  // a voice in a free slot joins at the bottom
    word |= bit;
    heap_[heapCount_]  = static_cast<uint8_t>(slot);
    heapPos_[slot]     = static_cast<uint8_t>(heapCount_);
    ++heapCount_;
  }
  // Up, or down for a stolen slot's later end (a no-op at the bottom).
  HeapSiftUp(heapPos_[slot]);
  HeapSiftDown(heapPos_[slot]);
}

#ifndef NDEBUG
// The 64-slot sweep the heap replaced: the lowest slot without a sounding voice at abs, and how
// many sound. Debug builds check every query against it.
bool GranularCore::SweepAgrees(int64_t abs) const noexcept {
  uint32_t slot = kGranularMaxGrains, sounding = 0;
  for (uint32_t i = 0; i < kGranularMaxGrains; ++i) {
    const Grain& g = grains_[i];
    if (!g.active || g.endAbs <= abs) {
      if (slot == kGranularMaxGrains) slot = i;
    } else {
      ++sounding;
    }
  }
  return slot == LowestFreeSlot() && sounding == heapCount_;
}
#endif

// Out of line: the scheduler and the triggers query it, and one copy suffices (ITCM).
BRAINSCAPE_NOINLINE void GranularCore::Expire(int64_t abs) noexcept {
  while (heapCount_ != 0u && grains_[heap_[0]].endAbs <= abs) {
    const uint8_t slot = heap_[0];
    live_[slot >> 5] &= ~(1u << (slot & 31u));
    if (--heapCount_ != 0u) {
      heap_[0] = heap_[heapCount_];
      HeapSiftDown(0u);
    }
  }
}

void GranularCore::RenderSpan(uint32_t from, uint32_t to, int64_t absSample, float* wetL,
                              float* wetR) noexcept {
  if (from >= to) return;
  // Iterate in BIRTH order (order_ list): per output sample, contributions always
  // sum oldest-grain-first regardless of how the stream is chopped into blocks or
  // segments — float addition is not associative, so a canonical order is what
  // makes contract #1's bit-exactness hold (review round 2).
  uint32_t w = 0;
  for (uint32_t oi = 0; oi < orderCount_; ++oi) {
    const uint8_t slot = order_[oi];
    Grain&        g    = grains_[slot];
    const int64_t birthAbs = g.endAbs - g.total;
    int64_t startN         = birthAbs + g.rendered - absSample;
    if (startN < static_cast<int64_t>(from)) startN = from;  // defensive; tracks exactly
    const auto s = static_cast<uint32_t>(startN < 0 ? 0 : startN);
    const uint32_t remaining = g.total - g.rendered;
    const uint32_t endN =
        s + remaining < to ? s + remaining : to;

    // Split at the life's pass boundaries (a repeat voice, mode-compiler.md §7.5) and at a
    // FastCut's fade start: both are absolute frames, so the splits are block-invariant.
    for (uint32_t n = s; n < endN;) {
      const uint32_t passEnd  = (static_cast<uint32_t>(g.pass) + 1u) * g.passLen;
      const int64_t  passEndN = birthAbs + passEnd - absSample;  // > n: rendered < passEnd
      const uint32_t segEnd =
          passEndN < static_cast<int64_t>(endN) ? static_cast<uint32_t>(passEndN) : endN;
      // A grain a FastCut load cut renders its frames before fadeStart as any other, then
      // the rest with the fade.
      uint32_t fadeN = segEnd;
      if (g.fadeStart != kNoFade) {
        const int64_t at = birthAbs + g.fadeStart - absSample;
        fadeN = at <= static_cast<int64_t>(n)
                    ? n
                    : (at < static_cast<int64_t>(segEnd) ? static_cast<uint32_t>(at) : segEnd);
      }
      if (n < fadeN) RenderRun(g, n, fadeN, false, wetL, wetR);
      if (fadeN < segEnd) RenderRun(g, fadeN, segEnd, true, wetL, wetR);
      n = segEnd;
      if (g.rendered == passEnd && g.rendered < g.total) NextPass(g);
    }
    if (s < endN && g.rendered >= g.total) g.active = false;

    if (g.active) order_[w++] = slot;  // compact retired grains out of the list
  }
  orderCount_ = w;
}

void GranularCore::NextPass(Grain& g) noexcept {
  ++g.pass;
  g.pos       = static_cast<uint64_t>(g.startFrame) << 32;
  // age0 < the ring <= 2^26, plus at most 15 passes of at most 24,000 frames: below 2^27.
  g.decayGain = grainmath::DecayGain(g.age0 + static_cast<uint32_t>(g.pass) * g.passLen,
                                     g.decayRate);
  ++stats_.repeatPasses;
}

void GranularCore::FastCut(int64_t abs) noexcept {
  for (uint32_t oi = 0; oi < orderCount_; ++oi) {
    Grain& g = grains_[order_[oi]];
    if (!g.active || g.endAbs <= abs || g.fadeStart != kNoFade) continue;
    const int64_t birthAbs = g.endAbs - g.total;
    if (abs < birthAbs) continue;  // not born yet: grains are born before the frame that cuts
    const auto at = static_cast<uint32_t>(abs - birthAbs);  // < total, since endAbs > abs
    g.fadeStart   = at;
    if (g.total - at > kGranularFastCutFrames) {
      g.total  = at + kGranularFastCutFrames;  // the grain ends with its fade, freeing its voice
      g.endAbs = birthAbs + g.total;
      HeapPlace(order_[oi]);  // an earlier end: up the heap
    }
  }
}

void GranularCore::FireExternal(const GranularParams& p, int64_t birthAbs,
                                uint32_t anchorFrame, uint32_t liveFrame,
                                uint32_t* renderedTo, uint32_t n, int64_t absSample,
                                float* wetL, float* wetR, uint32_t drawOrdinal) noexcept {
  // Flush up to the trigger sample so a reused/stolen slot's tail is emitted. Every grain that
  // ended by birthAbs is then retired, so orderCount_ counts the voices sounding there.
  RenderSpan(*renderedTo, n, absSample, wetL, wetR);
  *renderedTo = n;

  // voice_count (mode-compiler.md §7.5, R12): with that many voices sounding the trigger takes
  // the oldest, as it does when all 64 are, so at 64 this is revision 6's allocation.
  uint32_t slot = kGranularMaxGrains;
  if (orderCount_ < p.voiceCount) {
    Expire(birthAbs);
    assert(SweepAgrees(birthAbs));
    slot = LowestFreeSlot();  // the lowest slot whose voice is not sounding at birthAbs
  }
  if (slot == kGranularMaxGrains) {
    ++stats_.steals;
    // Oldest-steal (design §4): the order_ list is ascending birth order, so the
    // head is the oldest live voice. Its un-rendered remainder is cut hard —
    // partikkel's documented policy; tight response beats a fade here.
    slot = order_[0];
    for (uint32_t i = 1; i < orderCount_; ++i) order_[i - 1] = order_[i];
    --orderCount_;
  }
  ScheduleGrain(slot, p, birthAbs, anchorFrame, liveFrame, drawOrdinal);
  order_[orderCount_++] = static_cast<uint8_t>(slot);
  ++stats_.births;
}

bool GranularCore::FireTrigger(const GranularParams& p, uint32_t ordinal, int64_t birthAbs,
                               uint32_t anchorFrame, uint32_t liveFrame, uint32_t* renderedTo,
                               uint32_t n, int64_t absSample, float* wetL, float* wetR) noexcept {
  // Intermittency skips the whole trigger, its burst included (mode-compiler.md §7.5).
  if (p.intermittency > 0.f &&
      RandUnit(birthAbs - drawEpoch_, Draw::Intermittency, 0u, ordinal) < p.intermittency) {
    ++stats_.skips;
    return false;
  }
  // A CLOCK hit's grain draws at its own ordinal (docs/design/clock.md §6.3); an onset's and a
  // manual trigger's at 0, as before sound revision 8.
  FireExternal(p, birthAbs, anchorFrame, liveFrame, renderedTo, n, absSample, wetL, wetR,
               ordinal == kOrdinalClock ? kOrdinalClock : 0u);
  if (p.burstCount > 1u) AddBurst(birthAbs + p.burstSpacing, p.burstSpacing, p.burstCount - 1u);
  return true;
}

void GranularCore::AddClockHit(int64_t due) noexcept {
  if (clockPending_ == kMaxClockPending) {
    ++stats_.clockDropped;
    return;
  }
  uint32_t i = clockPending_++;
  for (; i > 0u && clockDue_[i - 1u] > due; --i) clockDue_[i] = clockDue_[i - 1u];
  clockDue_[i] = due;
}

void GranularCore::AddBurst(int64_t next, uint32_t spacing, uint32_t remaining) noexcept {
  if (burstCount_ == kMaxBursts) {  // the oldest gives way
    for (uint32_t i = 1; i < burstCount_; ++i) bursts_[i - 1] = bursts_[i];
    --burstCount_;
  }
  bursts_[burstCount_++] = Burst{next, spacing, remaining};
  UpdateBurstDue();
}

void GranularCore::UpdateBurstDue() noexcept {
  burstDue_ = kNoBurstDue;
  for (uint32_t i = 0; i < burstCount_; ++i) {
    if (bursts_[i].next < burstDue_) burstDue_ = bursts_[i].next;
  }
}

void GranularCore::FireBurst(const GranularParams& p, int64_t birthAbs, uint32_t anchorFrame,
                             uint32_t liveFrame, uint32_t* renderedTo, uint32_t n,
                             int64_t absSample, float* wetL, float* wetR) noexcept {
  for (uint32_t i = 0; i < burstCount_; ++i) {
    Burst& b = bursts_[i];
    if (b.next > birthAbs) continue;
    // Each grain with its own frame's draws; a deferred grain keeps the spacing from where it
    // fired.
    FireExternal(p, birthAbs, anchorFrame, liveFrame, renderedTo, n, absSample, wetL, wetR, 0u);
    ++stats_.burstBirths;
    if (--b.remaining == 0u) {
      for (uint32_t k = i + 1u; k < burstCount_; ++k) bursts_[k - 1u] = bursts_[k];
      --burstCount_;
    } else {
      b.next = birthAbs + b.spacing;
    }
    break;
  }
  UpdateBurstDue();
}

void GranularCore::Process(const GranularParams& p, const TriggerEvents& ev, int64_t absSample,
                           int64_t drawEpoch, uint32_t ringFrameAtBlockStart, bool frozen,
                           uint32_t* frozenAnchor, uint32_t numFrames, float* wetL,
                           float* wetR) noexcept {
  blockRingStart_ = ringFrameAtBlockStart;
  drawEpoch_      = drawEpoch;
  for (uint32_t n = 0; n < numFrames; ++n) {
    wetL[n] = 0.f;
    wetR[n] = 0.f;
  }

  // Births are spaced by a voice's whole life, repeat passes of the grain length (sound
  // revision 6), so target voices sound at once whatever the repeat.
  const float target = p.targetVoices >= 1.0f ? p.targetVoices : 1.0f;
  const float spacing =
      static_cast<float>(p.totalFrames * (p.repeat >= 1u ? p.repeat : 1u)) / target;
  assert(target <= static_cast<float>(kGranularMaxGrains));
  const auto  targetFloor = static_cast<uint32_t>(target);
  const float targetFrac  = target - static_cast<float>(targetFloor);

  // Segmented schedule/render: every birth first renders all live voices up to
  // the birth sample. This is what lets a slot be reused the moment its grain's
  // last sample is emitted (endAbs-based liveness) WITHOUT destroying the dying
  // grain's un-rendered tail, and — combined with birth-order summation — keeps
  // the output independent of host block size (review round 2: the naive
  // endAbs-only reuse lost block-boundary-dependent tails; the render-pass-swept
  // `active` flag made the population itself block-size dependent, -3.5 dB
  // between block 1 and 512). At extreme birth rates segments approach one
  // sample; TODO(§8): revisit segment batching before the M7 budget pass.
  uint32_t renderedTo = 0;
  uint32_t evIdx      = 0;
  uint32_t clockIdx   = 0;
  // The CLOCK hit's jitter (docs/design/clock.md §6.3): half the grid's period, exact (an integer
  // below 2^24), for (jitter * u) * half below, in binary32 in that order.
  const float clockHalf = 0.5f * static_cast<float>(ev.clockGridFrames);

  // Re-anchor-on-wrap (design §2.4 decided behavior): once the live write head
  // has consumed 3/4 of the ring behind the pin, re-pin to the present. The
  // splice is audible and documented; the alternative was the frozen window
  // silently degrading into delayed live signal as it is overwritten. Decided
  // per sample, so the splice lands on the same absolute sample for every block
  // size (deciding it at block start moved it by up to one block).
  uint32_t       pin         = *frozenAnchor;
  const uint32_t bufLen      = mask_ + 1u;
  const uint32_t reanchorAge = bufLen - (bufLen >> 2);

  for (uint32_t n = 0; n < numFrames; ++n) {
    const int64_t  abs  = absSample + n;
    const uint32_t live = (ringFrameAtBlockStart + n) & mask_;  // written by Pass 1 at abs
    if (frozen && ((live - pin) & mask_) > reanchorAge) pin = live;
    const uint32_t anchor = frozen ? pin : live;

    // Manual/MIDI triggers fire INSIDE the per-sample loop, one per consecutive
    // sample — a pre-loop fired them out of birth order relative to same-block
    // scheduler/onset births, breaking order_'s ascending-birth invariant that
    // both the canonical summation order and oldest-steal rely on (review
    // finding). The Engine splits its block at every trigger event, so the block
    // starts at the trigger's frame, and it passes only the triggers whose source the
    // mode lists (mode-compiler.md §7.5).
    bool fired = false;  // a trigger's grain was born at this frame
    if (n < ev.manualCount) {
      fired = FireTrigger(p, kOrdinalManual, abs, anchor, live, &renderedTo, n, absSample, wetL,
                          wetR);
    }

    // Onset events: record the mark always (POS_MARK feeds on it); fire a burst
    // only when the ONSET trigger source is enabled (OR'd with the free-running
    // scheduler, design §4).
    while (evIdx < ev.onsetCount && ev.onsetOffset[evIdx] == n) {
      marks_[markHead_] = {abs, ev.onsetMarkFrame[evIdx]};
      markHead_         = (markHead_ + 1u) % kMaxMarks;
      if (markCount_ < kMaxMarks) ++markCount_;
      if (p.onsetTrigger &&
          FireTrigger(p, kOrdinalOnset, abs, anchor, live, &renderedTo, n, absSample, wetL, wetR)) {
        fired = true;
      }
      ++evIdx;
    }

    // CLOCK (docs/design/clock.md §6.3, sound revision 8). A grid position enters the queue at
    // its grid frame, delayed by the jitter: a draw u at that frame (purpose Interval, ordinal
    // 3), (jitter * u) * (gridFrames / 2) frames, never early and never accumulating. Then the
    // earliest hit due fires as a trigger (manual/MIDI, onset, clock, burst, scheduler: D13's
    // order), one per frame: a second due at this frame waits for the next. Intermittency
    // skips a whole hit, its burst included. No admission control exists yet, so every hit is
    // born at its (jittered) frame: the cost governor (cpu-budget.md §5) and its 48-frame
    // lateness cap for CLOCK hits (kClockLateFrames, clockDeferred, clock.md §6.3) land with
    // whichever of the two revisions comes second (§11.3).
    for (; clockIdx < ev.clockCount && ev.clockOffset[clockIdx] == n; ++clockIdx) {
      uint32_t delay = 0;
      if (p.jitter > 0.f) {
        const float u = RandUnit(abs - drawEpoch, Draw::Interval, 0u, kOrdinalClock);
        const float d = (p.jitter * u) * clockHalf;
        assert(d >= 0.f && d < 8388608.f);  // jitter, u in [0, 1]; the half below 2^23
        delay = static_cast<uint32_t>(d);
      }
      AddClockHit(abs + delay);
    }
    if (clockPending_ != 0u && clockDue_[0] <= abs) {
      for (uint32_t i = 1; i < clockPending_; ++i) clockDue_[i - 1u] = clockDue_[i];
      --clockPending_;
      if (FireTrigger(p, kOrdinalClock, abs, anchor, live, &renderedTo, n, absSample, wetL,
                      wetR)) {
        fired = true;
        ++stats_.clockBirths;
        stats_.lastClockBirth = abs;
      }
    }

    // A burst's next grain, unless a trigger fired at this frame: it waits for the next.
    if (burstCount_ != 0u && abs >= burstDue_ && !fired) {
      FireBurst(p, abs, anchor, live, &renderedTo, n, absSample, wetL, wetR);
    }

    // Without `periodic` the free-running scheduler does not run (mode-compiler.md §7.5): its
    // phase holds until a mode with it is loaded.
    if (!p.periodic) continue;

    intervalRemaining_ -= 1.0f;
    if (intervalRemaining_ > 0.0f) continue;

    // Time-aware: a slot is reusable when its grain has fully sounded (endAbs <= abs); the
    // flush below guarantees it is also fully rendered. The heap retires the voices that ended
    // by abs, so the mask holds exactly the sounding ones: the lowest clear bit is the slot the
    // 64-slot sweep chose, and the heap's size its count (cpu-budget.md §4.3, step 6).
    Expire(abs);
    assert(SweepAgrees(abs));
    const uint32_t slot     = LowestFreeSlot();
    const uint32_t sounding = heapCount_;

    // Fractional targets are dithered, not truncated: a fixed integer ceiling
    // against a fractional normalization target produced a 6 dB grain-rate
    // tremolo on coherent presets (review finding). Counter-keyed draw, so
    // split-invariant.
    uint32_t allowed = targetFloor;
    if (targetFrac > 0.f && RandUnit(abs - drawEpoch, Draw::Ceiling) < targetFrac) ++allowed;
    if (allowed < 1u) allowed = 1u;

    if (sounding < allowed && slot != kGranularMaxGrains) {
      // Intermittency (mode-compiler.md §7.5): a skipped birth still consumes its interval.
      if (p.intermittency > 0.f &&
          RandUnit(abs - drawEpoch, Draw::Intermittency, 0u, kOrdinalPeriodic) < p.intermittency) {
        ++stats_.skips;
      } else {
        // Flush the span up to this birth so a reused slot's tail is emitted first.
        RenderSpan(renderedTo, n, absSample, wetL, wetR);
        renderedTo = n;

        ScheduleGrain(slot, p, abs, anchor, live, 0u);
        order_[orderCount_++] = static_cast<uint8_t>(slot);
        ++stats_.births;
      }

      // Next inter-arrival: deterministic spacing morphing to an exponential
      // (Poisson) draw — Roads' synchronous<->asynchronous axis (design §4).
      float interval = spacing;
      if (p.jitter > 0.f) {
        const float u   = RandUnit(abs - drawEpoch, Draw::Interval);
        const float exp = -detmath::LogF(1.0f - u * 0.999f) * spacing;
        interval        = spacing + p.jitter * (exp - spacing);
      }
      intervalRemaining_ += (interval >= 1.0f ? interval : 1.0f);
    } else {
      // Refused (ceiling or exhausted pool): retry next sample. Consuming a full
      // fresh interval here turned near-misses into whole-grain silence holes
      // and thinned jittered density ~40% below target (review finding).
      intervalRemaining_ += 1.0f;
    }
  }

  RenderSpan(renderedTo, numFrames, absSample, wetL, wetR);
  *frozenAnchor = pin;
}

}  // namespace brainscape::detail
