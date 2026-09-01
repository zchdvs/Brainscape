#pragma once
#include <cstdint>

#include "brainscape/detail/Smoother.h"

// Post chain (docs/design/grain-engine.md §2.6) and the fixed feedback taming
// chain (§2.3). Internal — included by Engine.h; free to change.
//
// Determinism rules (design §10 contracts #1/#7): every per-sample nonlinearity
// and oscillator here is in-tree arithmetic (parabolic sine, Padé tanh) — no
// libm in the audio path. Control-rate coefficient math (sinf/expm1) runs only
// when a parameter actually changes.
namespace brainscape::detail {

// ── Small primitives ────────────────────────────────────────────────────────────

// Parabolic sine approximation of sin(2π·phase) on a [0,1) phase — deterministic,
// in-tree, plenty for LFO duty (peak error ~5.6%, odd-symmetric).
inline float CheapSine(float phase01) noexcept {
  float x = phase01 < 0.5f ? phase01 : phase01 - 1.0f;  // [-0.5, 0.5)
  return x * (8.0f - 16.0f * (x < 0.f ? -x : x));       // peaks exactly ±1 at ±0.25
}

// Padé tanh — bounded soft saturator (design §2.3), exact 0 at 0.
inline float SoftSat(float x) noexcept {
  if (x > 3.0f) return 1.0f;
  if (x < -3.0f) return -1.0f;
  const float x2 = x * x;
  return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

// Schroeder allpass over a caller-provided buffer slice.
struct Allpass {
  float*   buf = nullptr;
  uint32_t len = 0, pos = 0;
  void Init(float* b, uint32_t n) noexcept {
    buf = b;
    len = n;
    pos = 0;
  }
  void Clear() noexcept {
    for (uint32_t i = 0; i < len; ++i) buf[i] = 0.f;
    pos = 0;
  }
  float Process(float x, float g) noexcept {
    const float v = buf[pos];
    const float y = v - g * x;
    buf[pos]      = x + g * y;
    if (++pos == len) pos = 0;
    return y;
  }
};

// Plain delay line over a caller-provided slice, with an optional modulated
// (linearly interpolated) read for the reverb tank.
struct DelaySlice {
  float*   buf = nullptr;
  uint32_t len = 0, pos = 0;
  void Init(float* b, uint32_t n) noexcept {
    buf = b;
    len = n;
    pos = 0;
  }
  void Clear() noexcept {
    for (uint32_t i = 0; i < len; ++i) buf[i] = 0.f;
    pos = 0;
  }
  void  Write(float x) noexcept {
    buf[pos] = x;
    if (++pos == len) pos = 0;
  }
  float ReadBack(uint32_t back) const noexcept {  // back in [1, len]
    uint32_t i = pos + len - back;
    if (i >= len) i -= len;
    return buf[i];
  }
  float ReadBackLerp(float back) const noexcept {  // back in [1, len-1)
    const auto  b0 = static_cast<uint32_t>(back);
    const float fr = back - static_cast<float>(b0);
    const float a  = ReadBack(b0);
    const float b  = ReadBack(b0 + 1u);
    return a + (b - a) * fr;
  }
};

// ── Feedback taming chain (design §2.3; fixed order, not user-reorderable) ─────
// DC block -> HP ~100 Hz -> LP 8k->4k (feedback-dependent) -> soft saturation ->
// 2-stage allpass diffusion per channel. This is what makes feedback > 1.0 a
// bounded self-oscillation feature instead of a rail.
class FeedbackTamer {
 public:
  static uint32_t WarmFloats(double sampleRate) noexcept;
  void Init(float* warm, double sampleRate) noexcept;
  void Reset() noexcept;
  // Per-block coefficient update (control rate; fbAmount sets the LP corner).
  void SetFeedback(float fbAmount, double sampleRate) noexcept;
  void ProcessSample(float& l, float& r) noexcept;

 private:
  float   dcL_ = 0.f, dcR_ = 0.f;      // DC-blocker LP state
  float   hpL_ = 0.f, hpR_ = 0.f;      // 100 Hz HP (via one-pole LP state)
  float   lpL_ = 0.f, lpR_ = 0.f;      // damping LP state
  float   dcCoef_ = 0.f, hpCoef_ = 0.f, lpCoef_ = 1.f;
  Allpass apL_[2]{}, apR_[2]{};
};

// ── Post chain: ordered, bypassable stage list (design §2.6) ───────────────────

enum class Stage : uint8_t { Mod = 0, Delay = 1, Reverb = 2, Filter = 3 };
inline constexpr uint32_t kPostStages = 4;

struct PostParams {
  float modRateHz    = 0.4f;
  float modDepth     = 0.0f;   // 0 = exactly transparent (dry term is x*1 + wet*0)
  float delayFrames  = 16800;  // post.delay.time_ms in frames (snaps; crossfade TODO)
  float delayFb      = 0.3f;
  float delayMix     = 0.0f;   // 0 = exactly transparent
  float reverbTime   = 0.5f;
  float reverbMix    = 0.0f;   // 0 = exactly transparent
  float filterCutoff = 20000.f;
  float filterRes    = 0.1f;
  float filterMorph  = 0.0f;   // 0..3 continuous LP -> BP -> HP -> Notch
  bool  filterBypass = true;   // cutoff at descriptor max = exact bypass (design §2.6
                               // endpoint semantics; also what keeps the null bit-exact)
};

class PostChain {
 public:
  // Warm arena: mod lines + reverb tank. Bulk arena: the stereo post-delay buffer.
  static uint32_t WarmFloats(double sampleRate) noexcept;
  static uint32_t BulkFloats(double sampleRate) noexcept;

  void Init(float* warm, float* bulk, double sampleRate) noexcept;
  void Reset() noexcept;
  // In-place over the block. Stage mixes are smoothed per sample internally.
  void Process(const PostParams& p, uint32_t numFrames, float* l, float* r) noexcept;

 private:
  void UpdateFilterCoefs(float cutoff, float res) noexcept;  // control rate (sinf)

  double sr_ = 48000.0;

  // Mod (stereo chorus-class): one LFO, quadrature R, modulated tap.
  DelaySlice modL_{}, modR_{};
  float      lfoPhase_ = 0.f;
  Smoother   modDepthSm_{};

  // Post delay (stereo, Bulk).
  DelaySlice pdL_{}, pdR_{};
  Smoother   delayMixSm_{};

  // Reverb: Clouds-style Dattorro/Griesinger — 4 input diffusers, figure-8 tank
  // (2 x (AP, AP, delay) with damping LP and decay), slow LFO on the long delays.
  float      rvBandwidth_ = 0.f;  // input LP state
  Allpass    rvAp_[4]{};
  Allpass    rvDap_[4]{};
  DelaySlice rvDel_[2]{};
  float      rvLp_[2] = {0.f, 0.f};
  float      rvLfoPhase_ = 0.f;
  Smoother   reverbMixSm_{};

  // Filter: stereo double-sampled SVF with continuous morph (post-fx doc §3.1 —
  // all outputs are computed by the recurrence anyway, so the morph is free).
  float svfFreq_ = 0.f, svfDamp_ = 0.f;
  float svfCutoffCached_ = -1.f, svfResCached_ = -1.f;
  float svfL_[3] = {0.f, 0.f, 0.f};  // low, band, (notch derived)
  float svfR_[3] = {0.f, 0.f, 0.f};

  Stage order_[kPostStages] = {Stage::Mod, Stage::Delay, Stage::Reverb, Stage::Filter};
};

}  // namespace brainscape::detail
