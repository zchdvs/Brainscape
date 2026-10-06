#include "detail/FpProfilePrivate.h"

#include "detail/Granular.h"

#include <cassert>

#include "detail/DetMath.h"

// Every float-to-int conversion below has a proven range, asserted in Debug builds:
// out-of-range conversions differ by ISA (determinism profile §3.10).
namespace brainscape::detail {

using grainmath::Draw;
using grainmath::RandUnit;

namespace {

constexpr float  kInvScale = 1.0f / 32767.0f;
constexpr double kFix      = 4294967296.0;  // 2^32

inline float ReadLinear(const int16_t* ring, uint32_t mask, uint64_t pos, uint32_t chan) noexcept {
  const auto f0   = static_cast<uint32_t>(pos >> 32) & mask;
  const auto f1   = (f0 + 1u) & mask;
  const float fr  = static_cast<float>(static_cast<uint32_t>(pos)) * (1.0f / 4294967296.0f);
  const float s0  = static_cast<float>(ring[2u * f0 + chan]);
  const float s1  = static_cast<float>(ring[2u * f1 + chan]);
  return (s0 + (s1 - s0) * fr) * kInvScale;
}

inline float ReadHermite(const int16_t* ring, uint32_t mask, uint64_t pos, uint32_t chan) noexcept {
  const auto f1  = static_cast<uint32_t>(pos >> 32);  // masked per tap below
  const float fr = static_cast<float>(static_cast<uint32_t>(pos)) * (1.0f / 4294967296.0f);
  const float xm1 = static_cast<float>(ring[2u * ((f1 - 1u) & mask) + chan]);
  const float x0  = static_cast<float>(ring[2u * (f1 & mask) + chan]);
  const float x1  = static_cast<float>(ring[2u * ((f1 + 1u) & mask) + chan]);
  const float x2  = static_cast<float>(ring[2u * ((f1 + 2u) & mask) + chan]);
  const float c    = (x1 - xm1) * 0.5f;
  const float v    = x0 - x1;
  const float w    = c + v;
  const float a    = w + v + (x2 - x0) * 0.5f;
  const float bNeg = w + a;
  return ((((a * fr) - bNeg) * fr + c) * fr + x0) * kInvScale;
}

// Envelope value with the always-applied LUT morph: at smoothness 0 the morph is
// an exact no-op (env + 0*(lut-env) == env), so no per-sample branch is needed
// and the rectangular/unity null stays bit-exact.
inline float EnvMorphed(const grainmath::EnvSpec& e, float i, float smoothness,
                        const float* lut) noexcept {
  float env = grainmath::EnvValue(e, i);
  const float x   = env * static_cast<float>(kWindowLutSize - 1);
  assert(x >= 0.f && x <= static_cast<float>(kWindowLutSize - 1));  // finite env in [0, 1]
  const auto  i0  = static_cast<uint32_t>(x);
  const auto  i1  = i0 + 1u < kWindowLutSize ? i0 + 1u : i0;
  const float fr  = x - static_cast<float>(i0);
  const float lv  = lut[i0] + (lut[i1] - lut[i0]) * fr;
  env += smoothness * (lv - env);
  return env * e.gain;
}

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
      static_cast<double>(g.total), static_cast<double>(mag) * 0x1p-32, g.inc < 0,
      bufLen > kBlockWriteAheadFrames ? bufLen - kBlockWriteAheadFrames : 0u,
      kGuardMarginFrames);
  return !(rail.hi < rail.lo);
}
#endif

}  // namespace

void GranularCore::ScheduleGrain(uint32_t slot, const GranularParams& p, int64_t birthAbs,
                                 uint32_t anchorFrame, uint32_t liveFrame) noexcept {
  uint32_t total = p.totalFrames >= 1u ? p.totalFrames : 1u;
  const int64_t drawKey = birthAbs - drawEpoch_;

  // Resolve everything once (design §3): pitch -> ratio -> signed increment.
  float st = p.ratioBase;
  if (p.spreadCents > 0.f) {
    st += (RandUnit(drawKey, Draw::Detune) * 2.0f - 1.0f) * p.spreadCents * 0.01f;
  }
  // Enforce the design's r_max = 4 ratio ceiling on the COMPOSED value — detune
  // on top of a maxed pitch otherwise exceeds what the guards/budgets assume.
  if (st > 24.f) st = 24.f;
  if (st < -24.f) st = -24.f;
  const float ratio   = grainmath::SemitonesToRatio(st);
  const bool  reverse = p.reverseProb > 0.f && RandUnit(drawKey, Draw::Reverse) < p.reverseProb;

  // Position: POS_LIVE (base delay behind the anchor) or POS_MARK (the most
  // recent eligible onset mark — the Strum family's mechanism, design §4).
  double d          = p.baseDelayFrames;
  bool   markActive = false;
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
    break;
  }
  if (markActive) {
    // Read attack-length earlier so the marked transient lands at the
    // envelope's flat top, not 15-31 dB down the fade-in (review finding).
    d += static_cast<double>(grainmath::MakeEnv(static_cast<float>(total), p.sustain,
                                                p.skew)
                                 .attackEnd);
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
  const uint32_t bufLen = mask_ + 1u;
  auto bounds = grainmath::ComputeDelayBounds(
      static_cast<double>(total), static_cast<double>(ratio), reverse,
      bufLen > kBlockWriteAheadFrames ? bufLen - kBlockWriteAheadFrames : 0u,
      kGuardMarginFrames);
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
    d += static_cast<double>((RandUnit(drawKey, Draw::Spray) * 2.0f - 1.0f) * p.sprayFrames);
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
  assert(ratio >= 0.25f && ratio <= 4.0f);  // the composed pitch is clamped to ±24 st
  auto inc = detmath::RoundHalfAwayI64(static_cast<double>(ratio) * kFix);
  if (reverse) inc = -inc;
  g.inc        = inc;
  g.unity      = (inc == static_cast<int64_t>(1) << 32);
  g.total      = total;
  g.rendered   = 0;
  g.endAbs     = birthAbs + total;
  g.env        = grainmath::MakeEnv(static_cast<float>(total), p.sustain, p.skew);
  g.smoothness = p.smoothness;
  g.tier       = slot < kHiFiGrains ? 0 : 1;

  // Equal-power pan around center, width = panSpread (grain-delay-theory.md §3.9).
  const float pan = 0.5f + p.panSpread * (RandUnit(drawKey, Draw::Pan) - 0.5f);
  double panSin, panCos;
  detmath::SinCosD(static_cast<double>(pan * 1.5707963267948966f), &panSin, &panCos);
  g.gainL = static_cast<float>(panCos);
  g.gainR = static_cast<float>(panSin);
  // Exact unity at center pan so the degenerate-delay null holds bit-exactly.
  if (p.panSpread == 0.0f) {
    g.gainL = 1.0f;
    g.gainR = 1.0f;
  }
  g.active = true;
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

    if (s < endN) {
      uint64_t pos   = g.pos;
      float    env_i = static_cast<float>(g.rendered);
      const float gl = g.gainL, gr = g.gainR, sm = g.smoothness;
#ifndef NDEBUG
      const bool fits = GrainFitsRing(g, mask_ + 1u);
#endif

      // Tier / unity / envelope morph resolved per grain, hoisted out of the
      // per-sample loop as three inner-loop variants (design §3).
      if (g.unity) {
        for (uint32_t n = s; n < endN; ++n) {
          const auto  f   = static_cast<uint32_t>(pos >> 32) & mask_;
          assert(!fits || TapsClearOfWriteAhead(f, 1u, blockRingStart_ + n, mask_));
          const float env = EnvMorphed(g.env, env_i, sm, lut_);
          wetL[n] += static_cast<float>(ring_[2u * f]) * kInvScale * env * gl;
          wetR[n] += static_cast<float>(ring_[2u * f + 1u]) * kInvScale * env * gr;
          pos += static_cast<uint64_t>(g.inc);
          env_i += 1.0f;
        }
      } else if (g.tier == 0) {
        for (uint32_t n = s; n < endN; ++n) {
          assert(!fits || TapsClearOfWriteAhead(static_cast<uint32_t>(pos >> 32) - 1u, 4u,
                                                blockRingStart_ + n, mask_));
          const float env = EnvMorphed(g.env, env_i, sm, lut_);
          wetL[n] += ReadHermite(ring_, mask_, pos, 0) * env * gl;
          wetR[n] += ReadHermite(ring_, mask_, pos, 1) * env * gr;
          pos = static_cast<uint64_t>(static_cast<int64_t>(pos) + g.inc);
          env_i += 1.0f;
        }
      } else {
        for (uint32_t n = s; n < endN; ++n) {
          assert(!fits || TapsClearOfWriteAhead(static_cast<uint32_t>(pos >> 32), 2u,
                                                blockRingStart_ + n, mask_));
          const float env = EnvMorphed(g.env, env_i, sm, lut_);
          wetL[n] += ReadLinear(ring_, mask_, pos, 0) * env * gl;
          wetR[n] += ReadLinear(ring_, mask_, pos, 1) * env * gr;
          pos = static_cast<uint64_t>(static_cast<int64_t>(pos) + g.inc);
          env_i += 1.0f;
        }
      }
      g.pos = pos;
      g.rendered += endN - s;
      if (g.rendered >= g.total) g.active = false;
    }

    if (g.active) order_[w++] = slot;  // compact retired grains out of the list
  }
  orderCount_ = w;
}

void GranularCore::FireExternal(const GranularParams& p, int64_t birthAbs,
                                uint32_t anchorFrame, uint32_t liveFrame,
                                uint32_t* renderedTo, uint32_t n, int64_t absSample,
                                float* wetL, float* wetR) noexcept {
  // Flush up to the trigger sample so a reused/stolen slot's tail is emitted.
  RenderSpan(*renderedTo, n, absSample, wetL, wetR);
  *renderedTo = n;

  uint32_t slot = kGranularMaxGrains;
  for (uint32_t i = 0; i < kGranularMaxGrains; ++i) {
    if (!grains_[i].active || grains_[i].endAbs <= birthAbs) {
      slot = i;
      break;
    }
  }
  if (slot == kGranularMaxGrains) {
    // Oldest-steal (design §4): the order_ list is ascending birth order, so the
    // head is the oldest live voice. Its un-rendered remainder is cut hard —
    // partikkel's documented policy; tight response beats a fade here.
    slot = order_[0];
    for (uint32_t i = 1; i < orderCount_; ++i) order_[i - 1] = order_[i];
    --orderCount_;
  }
  ScheduleGrain(slot, p, birthAbs, anchorFrame, liveFrame);
  order_[orderCount_++] = static_cast<uint8_t>(slot);
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

  const float target  = p.targetVoices >= 1.0f ? p.targetVoices : 1.0f;
  const float spacing = static_cast<float>(p.totalFrames) / target;
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
    // starts at the trigger's frame.
    if (n < ev.manualCount) {
      FireExternal(p, abs, anchor, live, &renderedTo, n, absSample, wetL, wetR);
    }

    // Onset events: record the mark always (POS_MARK feeds on it); fire a grain
    // only when the ONSET trigger source is enabled (OR'd with the free-running
    // scheduler, design §4).
    while (evIdx < ev.onsetCount && ev.onsetOffset[evIdx] == n) {
      marks_[markHead_] = {abs, ev.onsetMarkFrame[evIdx]};
      markHead_         = (markHead_ + 1u) % kMaxMarks;
      if (markCount_ < kMaxMarks) ++markCount_;
      if (p.onsetTrigger) {
        FireExternal(p, abs, anchor, live, &renderedTo, n, absSample, wetL, wetR);
      }
      ++evIdx;
    }

    intervalRemaining_ -= 1.0f;
    if (intervalRemaining_ > 0.0f) continue;

    // Time-aware sweep: a slot is reusable when its grain has fully sounded
    // (endAbs <= abs); the flush below guarantees it is also fully rendered.
    uint32_t slot     = kGranularMaxGrains;
    uint32_t sounding = 0;
    for (uint32_t i = 0; i < kGranularMaxGrains; ++i) {
      const Grain& g = grains_[i];
      if (!g.active || g.endAbs <= abs) {
        if (slot == kGranularMaxGrains) slot = i;
      } else {
        ++sounding;
      }
    }

    // Fractional targets are dithered, not truncated: a fixed integer ceiling
    // against a fractional normalization target produced a 6 dB grain-rate
    // tremolo on coherent presets (review finding). Counter-keyed draw, so
    // split-invariant.
    uint32_t allowed = targetFloor;
    if (targetFrac > 0.f && RandUnit(abs - drawEpoch, Draw::Ceiling) < targetFrac) ++allowed;
    if (allowed < 1u) allowed = 1u;

    if (sounding < allowed && slot != kGranularMaxGrains) {
      // Flush the span up to this birth so a reused slot's tail is emitted first.
      RenderSpan(renderedTo, n, absSample, wetL, wetR);
      renderedTo = n;

      ScheduleGrain(slot, p, abs, anchor, live);
      order_[orderCount_++] = static_cast<uint8_t>(slot);

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
