#include "brainscape/detail/Granular.h"

#include <cmath>

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
  const auto  i0  = static_cast<uint32_t>(x);
  const auto  i1  = i0 + 1u < kWindowLutSize ? i0 + 1u : i0;
  const float fr  = x - static_cast<float>(i0);
  const float lv  = lut[i0] + (lut[i1] - lut[i0]) * fr;
  env += smoothness * (lv - env);
  return env * e.gain;
}

}  // namespace

void GranularCore::ScheduleGrain(uint32_t slot, const GranularParams& p, int64_t birthAbs,
                                 uint32_t anchorFrame) noexcept {
  const uint32_t total = p.totalFrames >= 1u ? p.totalFrames : 1u;

  // Resolve everything once (design §3): pitch -> ratio -> signed increment.
  float st = p.ratioBase;
  if (p.spreadCents > 0.f) {
    st += (RandUnit(birthAbs, Draw::Detune) * 2.0f - 1.0f) * p.spreadCents * 0.01f;
  }
  // Enforce the design's r_max = 4 ratio ceiling on the COMPOSED value — detune
  // on top of a maxed pitch otherwise exceeds what the guards/budgets assume.
  if (st > 24.f) st = 24.f;
  if (st < -24.f) st = -24.f;
  const float ratio   = grainmath::SemitonesToRatio(st);
  const bool  reverse = p.reverseProb > 0.f && RandUnit(birthAbs, Draw::Reverse) < p.reverseProb;

  // Position: base delay ± spray, reflected (not clamped) into the per-direction
  // write-head guard bounds so spray keeps a distribution instead of piling onto
  // the margin rail (review finding).
  const auto bounds = grainmath::ComputeDelayBounds(
      static_cast<double>(total), static_cast<double>(ratio), reverse, mask_ + 1u,
      kGuardMarginFrames);
  double d = p.baseDelayFrames;
  if (p.sprayFrames > 0.f) {
    d += static_cast<double>((RandUnit(birthAbs, Draw::Spray) * 2.0f - 1.0f) * p.sprayFrames);
    d = grainmath::ReflectIntoBounds(d, bounds);
  } else {
    d = d < bounds.lo ? bounds.lo : (d > bounds.hi ? bounds.hi : d);
  }

  Grain& g = grains_[slot];
  const auto startFrame = (anchorFrame - static_cast<uint32_t>(std::lround(d))) & mask_;
  g.pos    = static_cast<uint64_t>(startFrame) << 32;
  auto inc = static_cast<int64_t>(std::llround(static_cast<double>(ratio) * kFix));
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
  const float pan = 0.5f + p.panSpread * (RandUnit(birthAbs, Draw::Pan) - 0.5f);
  g.gainL = std::cos(pan * 1.5707963267948966f);
  g.gainR = std::sin(pan * 1.5707963267948966f);
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

      // Tier / unity / envelope morph resolved per grain, hoisted out of the
      // per-sample loop as three inner-loop variants (design §3).
      if (g.unity) {
        for (uint32_t n = s; n < endN; ++n) {
          const auto  f   = static_cast<uint32_t>(pos >> 32) & mask_;
          const float env = EnvMorphed(g.env, env_i, sm, lut_);
          wetL[n] += static_cast<float>(ring_[2u * f]) * kInvScale * env * gl;
          wetR[n] += static_cast<float>(ring_[2u * f + 1u]) * kInvScale * env * gr;
          pos += static_cast<uint64_t>(g.inc);
          env_i += 1.0f;
        }
      } else if (g.tier == 0) {
        for (uint32_t n = s; n < endN; ++n) {
          const float env = EnvMorphed(g.env, env_i, sm, lut_);
          wetL[n] += ReadHermite(ring_, mask_, pos, 0) * env * gl;
          wetR[n] += ReadHermite(ring_, mask_, pos, 1) * env * gr;
          pos = static_cast<uint64_t>(static_cast<int64_t>(pos) + g.inc);
          env_i += 1.0f;
        }
      } else {
        for (uint32_t n = s; n < endN; ++n) {
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

void GranularCore::Process(const GranularParams& p, int64_t absSample,
                           uint32_t ringFrameAtBlockStart, bool frozen, uint32_t frozenAnchor,
                           uint32_t numFrames, float* wetL, float* wetR) noexcept {
  for (uint32_t n = 0; n < numFrames; ++n) {
    wetL[n] = 0.f;
    wetR[n] = 0.f;
  }

  const float target  = p.targetVoices >= 1.0f ? p.targetVoices : 1.0f;
  const float spacing = static_cast<float>(p.totalFrames) / target;
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

  for (uint32_t n = 0; n < numFrames; ++n) {
    intervalRemaining_ -= 1.0f;
    if (intervalRemaining_ > 0.0f) continue;

    const int64_t abs = absSample + n;

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
    if (targetFrac > 0.f && RandUnit(abs, Draw::Ceiling) < targetFrac) ++allowed;
    if (allowed < 1u) allowed = 1u;

    if (sounding < allowed && slot != kGranularMaxGrains) {
      // Flush the span up to this birth so a reused slot's tail is emitted first.
      RenderSpan(renderedTo, n, absSample, wetL, wetR);
      renderedTo = n;

      const uint32_t anchor = frozen ? frozenAnchor : ((ringFrameAtBlockStart + n) & mask_);
      ScheduleGrain(slot, p, abs, anchor);
      order_[orderCount_++] = static_cast<uint8_t>(slot);

      // Next inter-arrival: deterministic spacing morphing to an exponential
      // (Poisson) draw — Roads' synchronous<->asynchronous axis (design §4).
      float interval = spacing;
      if (p.jitter > 0.f) {
        const float u   = RandUnit(abs, Draw::Interval);
        const float exp = -std::log(1.0f - u * 0.999f) * spacing;
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
}

}  // namespace brainscape::detail
