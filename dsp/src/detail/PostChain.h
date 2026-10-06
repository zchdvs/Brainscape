#pragma once
#include "detail/FpProfilePrivate.h"

#include <cassert>
#include <cstdint>
#include <cstring>

#include "detail/FlushTiny.h"
#include "detail/Smoother.h"

// Post chain (docs/design/grain-engine.md §2.6) and the fixed feedback taming
// chain (§2.3). Internal; free to change.
//
// Determinism rules (design §10 contracts #1/#7): every per-sample nonlinearity
// and oscillator here is in-tree arithmetic (parabolic sine, Padé tanh) or an
// IEEE-exact operation (sqrtf) — no libm anywhere. Control-rate coefficient
// math (DetMath sin/expm1) runs at Init or when a parameter changes.
namespace brainscape::detail {

// ── Small primitives ────────────────────────────────────────────────────────────

// Parabolic sine approximation of sin(2π·phase) on a [0,1) phase — deterministic,
// in-tree, plenty for LFO duty (peak error ~5.6%, odd-symmetric).
inline float CheapSine(float phase01) noexcept {
  float x = phase01 < 0.5f ? phase01 : phase01 - 1.0f;  // [-0.5, 0.5)
  return x * (8.0f - 16.0f * (x < 0.f ? -x : x));       // peaks exactly ±1 at ±0.25
}

// Padé tanh — bounded soft saturator (design §2.3). C1-continuous at the ±3 seam
// (value and derivative both match), exact 0 at 0.
inline float SoftSat(float x) noexcept {
  if (x > 3.0f) return 1.0f;
  if (x < -3.0f) return -1.0f;
  // Below 2^-63 the square is subnormal, and the flushed tamer state fed in here
  // reaches down to 1e-20 (determinism profile §4.3). 27 absorbs such a square, so
  // squaring 0 gives the same bits without the subnormal operation. Selecting on the
  // bits before the multiply keeps a compiler from speculating x * x.
  uint32_t u;
  std::memcpy(&u, &x, sizeof u);
  if ((u & 0x7FFFFFFFu) < 0x20000000u) u = 0u;
  float xs;
  std::memcpy(&xs, &u, sizeof u);
  const float x2 = xs * xs;
  return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

// Schroeder allpass over a caller-provided buffer slice. The stored state is flushed
// (determinism profile §4.3).
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
    float       w = x + g * y;
    FlushTiny(w);
    buf[pos] = w;
    if (++pos == len) pos = 0;
    return y;
  }
  float TapBack(uint32_t back) const noexcept {  // read inside the line, no state change
    uint32_t i = pos + len - back;
    if (i >= len) i -= len;
    return buf[i];
  }
};

// Plain delay line over a caller-provided slice, with an optional modulated
// (linearly interpolated) read for the reverb tank. Written values are flushed
// (determinism profile §4.3): the post delay and reverb tank recirculate.
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
    FlushTiny(x);
    buf[pos] = x;
    if (++pos == len) pos = 0;
  }
  float ReadBack(uint32_t back) const noexcept {  // back in [1, len]
    uint32_t i = pos + len - back;
    if (i >= len) i -= len;
    return buf[i];
  }
  float ReadBackLerp(float back) const noexcept {  // back in [1, len-1)
    assert(back >= 1.0f && back < static_cast<float>(len - 1u));  // profile §3.10: bounded excursion
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
  void ClearDiffusers() noexcept;  // the allpass buffers: Init and Restart, never Reset
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
  float modDepth     = 0.0f;   // drives excursion AND a wet mix capped at 0.5 so a
                               // dry term always survives (chorus, not vibrato);
                               // 0 = exactly transparent
  float delayFrames  = 16800;  // post.delay.time_ms in frames (snaps; crossfade TODO)
  float delayFb      = 0.3f;
  float delayMix     = 0.0f;   // equal-power; 0 = exactly transparent
  float reverbTime   = 0.5f;
  float reverbMix    = 0.0f;   // equal-power; 0 = exactly transparent
  float filterCutoff = 20000.f;
  float filterRes    = 0.1f;
  float filterMorph  = 0.0f;   // 0..3 continuous LP -> BP -> HP -> Notch (equal-power)
  bool  filterBypass = true;   // cutoff at descriptor max ramps the insert mix to an
                               // EXACT bypass (design §2.6: click-free crossfade,
                               // and the null contracts' transparency)
};

class PostChain {
 public:
  // Warm arena: mod lines + reverb tank. Bulk arena: the stereo post-delay buffer.
  static uint32_t WarmFloats(double sampleRate) noexcept;
  static uint32_t BulkFloats(double sampleRate) noexcept;

  void Init(float* warm, float* bulk, double sampleRate) noexcept;
  // RT-safe: clears small filter/LFO state and primes smoothers from the given
  // params. Does NOT memset the delay/reverb buffers (750 KiB of SDRAM — review
  // finding: that made Engine::Reset miss 2-4 audio deadlines); stale tails are
  // masked by the mix ramps and fully cleared by the non-RT ClearBuffers().
  void Reset(const PostParams& p) noexcept;
  void ClearBuffers() noexcept;  // non-RT: memsets mod/delay/reverb storage
  // In-place over the block. Stage mixes are smoothed per sample internally; a
  // stage whose mix target and smoothed value are both exactly 0 is skipped
  // (its buffers freeze while bypassed — normal bypass semantics).
  void Process(const PostParams& p, uint32_t numFrames, float* l, float* r) noexcept;

 private:
  void UpdateFilterCoefs(float cutoff, float res, float morph) noexcept;  // control rate

  double sr_ = 48000.0;

  // Mod (stereo chorus): one LFO, quadrature R, modulated tap; wet capped at 0.5.
  DelaySlice modL_{}, modR_{};
  float      lfoPhase_  = 0.f;
  float      modCenter_ = 0.f, modMaxExc_ = 0.f;  // frames, fixed at Init
  Smoother   modDepthSm_{};

  // Post delay (stereo, Bulk) with damped, DC-blocked regeneration.
  DelaySlice pdL_{}, pdR_{};
  float      pdLpL_ = 0.f, pdLpR_ = 0.f;  // loop damping LP state
  float      pdDcL_ = 0.f, pdDcR_ = 0.f;  // loop DC-blocker state
  float      pdLpCoef_ = 1.f, pdDcCoef_ = 0.f;
  Smoother   delayMixSm_{};

  // Reverb: Clouds-style Dattorro/Griesinger — 4 input diffusers, figure-8 tank
  // (both halves damped), slow LFO on the long delays, multi-tap wet outputs
  // (raw line-end outputs were two discrete slaps with 107 ms of silence first —
  // review finding).
  float      rvBandwidth_ = 0.f;
  Allpass    rvAp_[4]{};
  Allpass    rvDap_[4]{};
  DelaySlice rvDel_[2]{};
  float      rvLp_[2]     = {0.f, 0.f};
  float      rvLfoPhase_  = 0.f;
  float      rvDamp_ = 0.f, rvBw_ = 0.f, rvExc_ = 0.f, rvLfoInc_ = 0.f;  // fixed at Init
  float      rvDl0_ = 0.f, rvDl1_ = 0.f;                                  // fixed at Init
  uint32_t   rvTapL_[3] = {0, 0, 0}, rvTapR_[3] = {0, 0, 0};              // fixed at Init
  Smoother   reverbMixSm_{};

  // Filter: stereo double-sampled SVF, equal-power morph, smoothed insert mix.
  float    svfFreq_ = 0.f, svfDamp_ = 0.f;
  float    svfWa_ = 1.f, svfWb_ = 0.f;  // equal-power morph weights (control rate)
  uint32_t svfSeg_ = 0;
  float    svfCutoffCached_ = -1.f, svfResCached_ = -1.f, svfMorphCached_ = -1.f;
  float    svfL_[2] = {0.f, 0.f};
  float    svfR_[2] = {0.f, 0.f};
  Smoother filterMixSm_{};
  bool     svfCleared_ = true;

  Stage order_[kPostStages] = {Stage::Mod, Stage::Delay, Stage::Reverb, Stage::Filter};
};

}  // namespace brainscape::detail
