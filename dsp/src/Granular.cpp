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
  const float c     = (x1 - xm1) * 0.5f;
  const float v     = x0 - x1;
  const float w     = c + v;
  const float a     = w + v + (x2 - x0) * 0.5f;
  const float bNeg  = w + a;
  return ((((a * fr) - bNeg) * fr + c) * fr + x0) * kInvScale;
}

}  // namespace

uint32_t GranularCore::CountActiveAt(int64_t abs) const noexcept {
  uint32_t n = 0;
  for (const auto& g : grains_) {
    if (g.active && g.endAbs > abs) ++n;
  }
  return n;
}

void GranularCore::ScheduleGrain(const GranularParams& p, int64_t birthAbs,
                                 uint32_t anchorFrame) noexcept {
  // Free slot: low slots first, so T0 (cubic) capacity is used before T1/T2.
  uint32_t slot = kGranularMaxGrains;
  for (uint32_t i = 0; i < kGranularMaxGrains; ++i) {
    if (!grains_[i].active) {
      slot = i;
      break;
    }
  }
  if (slot == kGranularMaxGrains) return;  // don't-fire (design §4 allocation policy)

  const float outFrames = p.sizeFrames >= 1.0f ? p.sizeFrames : 1.0f;
  const auto  total     = static_cast<uint32_t>(outFrames + 0.5f) >= 1u
                              ? static_cast<uint32_t>(outFrames + 0.5f)
                              : 1u;

  // Resolve everything once (design §3): pitch -> ratio -> signed increment.
  float st = p.ratioBase;
  if (p.spreadCents > 0.f) {
    st += (RandUnit(birthAbs, Draw::Detune) * 2.0f - 1.0f) * p.spreadCents * 0.01f;
  }
  const float ratio   = grainmath::SemitonesToRatio(st);
  const bool  reverse = p.reverseProb > 0.f && RandUnit(birthAbs, Draw::Reverse) < p.reverseProb;

  // Position: base delay +/- spray, then the per-direction write-head guards.
  double d = p.baseDelayFrames;
  if (p.sprayFrames > 0.f) {
    d += static_cast<double>((RandUnit(birthAbs, Draw::Spray) * 2.0f - 1.0f) * p.sprayFrames);
  }
  d = grainmath::ClampDelayFrames(d, static_cast<double>(total), static_cast<double>(ratio),
                                  reverse, mask_ + 1u, kGuardMarginFrames);

  Grain& g  = grains_[slot];
  const auto startFrame = (anchorFrame - static_cast<uint32_t>(std::lround(d))) & mask_;
  g.pos     = static_cast<uint64_t>(startFrame) << 32;
  auto inc  = static_cast<int64_t>(std::llround(static_cast<double>(ratio) * kFix));
  if (reverse) inc = -inc;
  g.inc      = inc;
  g.unity    = (inc == static_cast<int64_t>(1) << 32);
  g.total    = total;
  g.rendered = 0;
  g.endAbs   = birthAbs + total;
  g.env      = grainmath::MakeEnv(static_cast<float>(total), p.sustain, p.skew);
  g.smoothness = p.smoothness;

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

void GranularCore::Process(const GranularParams& p, int64_t absSample,
                           uint32_t ringFrameAtBlockStart, bool frozen, uint32_t frozenAnchor,
                           uint32_t numFrames, float* wetL, float* wetR) noexcept {
  for (uint32_t n = 0; n < numFrames; ++n) {
    wetL[n] = 0.f;
    wetR[n] = 0.f;
  }

  // ── Schedule pass (per sample, split-invariant: all state advances per sample
  // and every draw is keyed on the absolute sample index).
  const float spacing = p.sizeFrames / (p.targetVoices >= 1.0f ? p.targetVoices : 1.0f);
  for (uint32_t n = 0; n < numFrames; ++n) {
    intervalRemaining_ -= 1.0f;
    if (intervalRemaining_ > 0.0f) continue;

    const int64_t abs = absSample + n;
    // Don't-fire ceiling (design §4): count voices still sounding at this sample.
    if (CountActiveAt(abs) < static_cast<uint32_t>(std::ceil(p.targetVoices))) {
      const uint32_t anchor =
          frozen ? frozenAnchor : ((ringFrameAtBlockStart + n) & mask_);
      ScheduleGrain(p, abs, anchor);
    }
    // Next inter-arrival: deterministic spacing morphing to an exponential
    // (Poisson) draw — Roads' synchronous<->asynchronous axis on one knob
    // (design §4 jitter). Drawn from the counter RNG, so split-invariant.
    float interval = spacing;
    if (p.jitter > 0.f) {
      const float u   = RandUnit(abs, Draw::Interval);
      const float exp = -std::log(1.0f - u * 0.999f) * spacing;
      interval        = spacing + p.jitter * (exp - spacing);
    }
    intervalRemaining_ += (interval >= 1.0f ? interval : 1.0f);
  }

  // ── Render pass: per-grain over the whole block (design §2 commitment 2).
  for (auto& g : grains_) {
    if (!g.active) continue;
    // A grain born mid-block starts at its birth offset (the design's pre_delay).
    const int64_t birthAbs = g.endAbs - g.total;
    int64_t startN         = birthAbs + g.rendered - absSample;
    if (startN < 0) startN = 0;  // defensive; rendered grains track exactly
    const uint32_t remaining = g.total - g.rendered;
    const uint32_t endN =
        static_cast<uint32_t>(startN) + remaining < numFrames
            ? static_cast<uint32_t>(startN) + remaining
            : numFrames;

    uint64_t pos   = g.pos;
    float    env_i = static_cast<float>(g.rendered);
    for (uint32_t n = static_cast<uint32_t>(startN); n < endN; ++n) {
      float sl, sr;
      if (g.unity) {
        const auto f = static_cast<uint32_t>(pos >> 32) & mask_;
        sl = static_cast<float>(ring_[2u * f]) * kInvScale;
        sr = static_cast<float>(ring_[2u * f + 1u]) * kInvScale;
      } else if (&g - grains_ < static_cast<ptrdiff_t>(kHiFiGrains)) {
        sl = ReadHermite(ring_, mask_, pos, 0);
        sr = ReadHermite(ring_, mask_, pos, 1);
      } else {
        sl = ReadLinear(ring_, mask_, pos, 0);
        sr = ReadLinear(ring_, mask_, pos, 1);
      }

      // Piecewise envelope, morphed toward the half-cosine LUT by smoothness.
      float env = grainmath::EnvValue(g.env, env_i);
      if (g.smoothness > 0.f) {
        const float x   = env * static_cast<float>(kWindowLutSize - 1);
        const auto  i0  = static_cast<uint32_t>(x);
        const float fr  = x - static_cast<float>(i0);
        const float lut = lut_[i0] + (lut_[i0 + 1u < kWindowLutSize ? i0 + 1u : i0] - lut_[i0]) * fr;
        env += g.smoothness * (lut - env);
      }
      env *= g.env.gain;  // exact mean compensation (GrainMath.h)

      wetL[n] += sl * env * g.gainL;
      wetR[n] += sr * env * g.gainR;
      pos = static_cast<uint64_t>(static_cast<int64_t>(pos) + g.inc);
      env_i += 1.0f;
    }
    const auto renderedNow = endN > static_cast<uint32_t>(startN)
                                 ? endN - static_cast<uint32_t>(startN)
                                 : 0u;
    g.pos = pos;
    g.rendered += renderedNow;
    if (g.rendered >= g.total) g.active = false;
  }

  // ── Normalization: target-referenced N^-p, exact 1.0 at a single voice so the
  // degenerate-delay null holds (design §3). Smoothing happens upstream on the
  // parameter, not here — normGain is constant within a block.
  if (p.normGain != 1.0f) {
    for (uint32_t n = 0; n < numFrames; ++n) {
      wetL[n] *= p.normGain;
      wetR[n] *= p.normGain;
    }
  }
}

}  // namespace brainscape::detail
