#include "detail/FpProfilePrivate.h"

#include "detail/PostChain.h"

#include "detail/DetMath.h"

namespace brainscape::detail {

namespace {

// One-pole LP coefficient at control rate only (expm1 for precision — Smoother.h).
inline float LpCoef(double cutoffHz, double sr) noexcept {
  return -static_cast<float>(detmath::Expm1D(-6.283185307179586 * cutoffHz / sr));
}

inline uint32_t ScaleLen(uint32_t base32k, double sr) noexcept {
  const auto n =
      static_cast<uint32_t>(detmath::RoundHalfAwayI32(static_cast<double>(base32k) * sr / 32000.0));
  return n < 2u ? 2u : n;
}

// Reverb tank lengths at the Clouds reference rate of 32 kHz (topology per
// Dattorro/Griesinger; length set as published in Mutable Instruments Clouds,
// MIT — see docs/research/post-fx-chain-looper-and-system-budget.md §1.3).
constexpr uint32_t kRvAp[4]  = {113, 162, 241, 399};
constexpr uint32_t kRvDap[4] = {1653, 2038, 1913, 1663};
constexpr uint32_t kRvDel[2] = {3411, 4782};
constexpr float    kRvModExcursion32k = 24.0f;  // frames of LFO wobble on the long delays

// Wet output taps inside the tank (32 kHz frames): staggered primes so both
// channels get early energy from ~8 ms and decorrelated reflections — the raw
// line ends alone were 107/150 ms of silence then two slaps (review finding).
constexpr uint32_t kRvTapL32k[3] = {266, 1913, 3300};  // del0, dap1, del1
constexpr uint32_t kRvTapR32k[3] = {353, 1228, 2673};  // del1, dap3, del0
constexpr float    kRvTapGain[3] = {0.6f, -0.35f, 0.4f};

// Taming diffuser lengths (ms-scale primes, per channel).
constexpr uint32_t kTamerAp32k[4] = {101, 189, 137, 251};  // L0, L1, R0, R1

// Post-delay ceiling: 2 s (the Space-knob delay never needs more; design §2.6).
constexpr double kPostDelayMaxSeconds = 2.0;
// Mod line: 25 ms, center tap 10 ms, max excursion 4 ms (8 ms at full depth read
// as seasick vibrato; review finding).
constexpr double kModLineSeconds   = 0.025;
constexpr double kModCenterSeconds = 0.010;
constexpr double kModMaxSeconds    = 0.004;

}  // namespace

// ── FeedbackTamer ───────────────────────────────────────────────────────────────

uint32_t FeedbackTamer::WarmFloats(double sr) noexcept {
  uint32_t total = 0;
  for (uint32_t b : kTamerAp32k) total += ScaleLen(b, sr);
  return total;
}

void FeedbackTamer::Init(float* warm, double sr) noexcept {
  float* p = warm;
  for (int i = 0; i < 2; ++i) {
    const uint32_t n = ScaleLen(kTamerAp32k[i], sr);
    apL_[i].Init(p, n);
    p += n;
  }
  for (int i = 0; i < 2; ++i) {
    const uint32_t n = ScaleLen(kTamerAp32k[2 + i], sr);
    apR_[i].Init(p, n);
    p += n;
  }
  dcCoef_ = LpCoef(8.0, sr);    // DC blocker corner
  hpCoef_ = LpCoef(100.0, sr);  // design §2.3: HP 80-120 Hz
  lpCoef_ = LpCoef(6000.0, sr);
  Reset();
  for (auto& a : apL_) a.Clear();
  for (auto& a : apR_) a.Clear();
}

void FeedbackTamer::Reset() noexcept {
  // RT-safe: filter states only (the diffuser buffers are ~4 KiB and cleared at
  // Init; stale content decays through the loop naturally).
  dcL_ = dcR_ = hpL_ = hpR_ = lpL_ = lpR_ = 0.f;
}

void FeedbackTamer::SetFeedback(float fbAmount, double sr) noexcept {
  // LP corner rides the feedback amount, 8 kHz down to 4 kHz (design §2.3):
  // higher regeneration = darker loop, which is what keeps >1 feedback musical.
  const double hz = 8000.0 - 4000.0 * static_cast<double>(fbAmount > 1.f ? 1.f : fbAmount);
  lpCoef_         = LpCoef(hz, sr);
}

void FeedbackTamer::ProcessSample(float& l, float& r) noexcept {
  dcL_ += dcCoef_ * (l - dcL_);
  dcR_ += dcCoef_ * (r - dcR_);
  float xl = l - dcL_;
  float xr = r - dcR_;
  hpL_ += hpCoef_ * (xl - hpL_);
  hpR_ += hpCoef_ * (xr - hpR_);
  xl -= hpL_;
  xr -= hpR_;
  lpL_ += lpCoef_ * (xl - lpL_);
  lpR_ += lpCoef_ * (xr - lpR_);
  xl = SoftSat(lpL_);
  xr = SoftSat(lpR_);
  constexpr float kG = 0.6f;
  xl = apL_[1].Process(apL_[0].Process(xl, kG), kG);
  xr = apR_[1].Process(apR_[0].Process(xr, kG), kG);
  l = xl;
  r = xr;
}

// ── PostChain ───────────────────────────────────────────────────────────────────

uint32_t PostChain::WarmFloats(double sr) noexcept {
  uint32_t total = 0;
  total += 2u * static_cast<uint32_t>(detmath::RoundHalfAwayI32(kModLineSeconds * sr));  // mod L+R
  for (uint32_t b : kRvAp) total += ScaleLen(b, sr);
  for (uint32_t b : kRvDap) total += ScaleLen(b, sr);
  for (uint32_t b : kRvDel) {
    total += ScaleLen(b, sr) + static_cast<uint32_t>(kRvModExcursion32k * sr / 32000.0) + 4u;
  }
  return total;
}

uint32_t PostChain::BulkFloats(double sr) noexcept {
  return 2u * static_cast<uint32_t>(detmath::RoundHalfAwayI32(kPostDelayMaxSeconds * sr));
}

void PostChain::Init(float* warm, float* bulk, double sr) noexcept {
  sr_      = sr;
  float* p = warm;
  auto modLen = static_cast<uint32_t>(detmath::RoundHalfAwayI32(kModLineSeconds * sr));
  if (modLen < 2u) modLen = 2u;
  modL_.Init(p, modLen);
  p += modLen;
  modR_.Init(p, modLen);
  p += modLen;
  for (int i = 0; i < 4; ++i) {
    const uint32_t n = ScaleLen(kRvAp[i], sr);
    rvAp_[i].Init(p, n);
    p += n;
  }
  for (int i = 0; i < 4; ++i) {
    const uint32_t n = ScaleLen(kRvDap[i], sr);
    rvDap_[i].Init(p, n);
    p += n;
  }
  for (int i = 0; i < 2; ++i) {
    const uint32_t n =
        ScaleLen(kRvDel[i], sr) + static_cast<uint32_t>(kRvModExcursion32k * sr / 32000.0) + 4u;
    rvDel_[i].Init(p, n);
    p += n;
  }

  auto pdLen = static_cast<uint32_t>(detmath::RoundHalfAwayI32(kPostDelayMaxSeconds * sr));
  if (pdLen < 2u) pdLen = 2u;
  pdL_.Init(bulk, pdLen);
  pdR_.Init(bulk + pdLen, pdLen);

  // Everything that depends only on the (lifetime-fixed) sample rate is computed
  // once here — the reverb prologue used to call expm1/lround every block
  // (review finding, and this file's own header rule).
  modCenter_ = static_cast<float>(kModCenterSeconds * sr);
  modMaxExc_ = static_cast<float>(kModMaxSeconds * sr);
  pdLpCoef_  = LpCoef(6000.0, sr);
  pdDcCoef_  = LpCoef(20.0, sr);
  rvDamp_    = LpCoef(5500.0, sr);
  rvBw_      = LpCoef(11000.0, sr);
  rvExc_     = static_cast<float>(kRvModExcursion32k * sr / 32000.0);
  rvLfoInc_  = static_cast<float>(0.5 / sr);
  rvDl0_     = static_cast<float>(ScaleLen(kRvDel[0], sr));
  rvDl1_     = static_cast<float>(ScaleLen(kRvDel[1], sr));
  for (int i = 0; i < 3; ++i) {
    rvTapL_[i] = ScaleLen(kRvTapL32k[i], sr);
    rvTapR_[i] = ScaleLen(kRvTapR32k[i], sr);
  }

  modDepthSm_.SetTau(10.0f, sr);
  delayMixSm_.SetTau(10.0f, sr);
  reverbMixSm_.SetTau(10.0f, sr);
  filterMixSm_.SetTau(10.0f, sr);
  svfCutoffCached_ = -1.f;
  svfResCached_    = -1.f;
  svfMorphCached_  = -1.f;
  ClearBuffers();
  Reset(PostParams{});
}

void PostChain::ClearBuffers() noexcept {
  modL_.Clear();
  modR_.Clear();
  pdL_.Clear();
  pdR_.Clear();
  for (auto& a : rvAp_) a.Clear();
  for (auto& a : rvDap_) a.Clear();
  for (auto& d : rvDel_) d.Clear();
}

void PostChain::Reset(const PostParams& p) noexcept {
  rvBandwidth_ = rvLp_[0] = rvLp_[1] = 0.f;
  pdLpL_ = pdLpR_ = pdDcL_ = pdDcR_ = 0.f;
  lfoPhase_ = rvLfoPhase_ = 0.f;
  svfL_[0] = svfL_[1] = 0.f;
  svfR_[0] = svfR_[1] = 0.f;
  svfCleared_ = true;
  // Prime from the CALLER'S params — priming to the smoothers' own stale targets
  // left Reset leaking 37% of the un-delayed signal through a delayMix=1 insert
  // (review finding).
  modDepthSm_.Prime(p.modDepth * 0.5f);
  delayMixSm_.Prime(p.delayMix);
  reverbMixSm_.Prime(p.reverbMix);
  filterMixSm_.Prime(p.filterBypass ? 0.f : 1.f);
}

void PostChain::UpdateFilterCoefs(float cutoff, float res, float morph) noexcept {
  // DaisySP-style double-sampled SVF coefficients (MIT; post-fx doc §3.1).
  // Control rate only — sinf/cosf never run per sample.
  const double f = static_cast<double>(cutoff) / (sr_ * 2.0);
  svfFreq_ = static_cast<float>(2.0 * detmath::SinD(3.14159265358979 * (f < 0.25 ? f : 0.25)));
  float r = res < 0.f ? 0.f : (res > 1.f ? 1.f : res);
  // Floor the damping: at res exactly 1 damp hits 0 and (with the reference's
  // cubic drive term not ported) nothing bounds the resonator — measured +76 dB
  // runaway at the knob stop (review finding). 0.995 is already an extreme
  // (+52 dB) resonance.
  if (r > 0.995f) r = 0.995f;
  // r^0.25 as sqrt(sqrt(r)) in binary64 (both correctly rounded), one final rounding.
  const float r4  = static_cast<float>(detmath::SqrtD(detmath::SqrtD(static_cast<double>(r))));
  float damp      = 2.0f * (1.0f - r4);
  const float lim = 2.0f / svfFreq_ - svfFreq_ * 0.5f;
  if (damp > lim) damp = lim;
  if (damp > 2.0f) damp = 2.0f;
  svfDamp_ = damp;

  // Equal-power morph weights (LP/BP are in quadrature — a linear blend dipped
  // 3.1 dB at segment midpoints; review finding).
  const float m   = morph < 0.f ? 0.f : (morph > 3.f ? 3.f : morph);
  svfSeg_         = m >= 3.f ? 2u : static_cast<uint32_t>(m);
  const float fr  = m - static_cast<float>(svfSeg_);
  double ws, wc;
  detmath::SinCosD(static_cast<double>(fr * 1.5707963267948966f), &ws, &wc);
  svfWa_ = static_cast<float>(wc);
  svfWb_ = static_cast<float>(ws);
}

void PostChain::Process(const PostParams& p, uint32_t numFrames, float* l, float* r) noexcept {
  modDepthSm_.target  = p.modDepth * 0.5f;  // wet capped at 0.5: dry always survives
  delayMixSm_.target  = p.delayMix;
  reverbMixSm_.target = p.reverbMix;
  filterMixSm_.target = p.filterBypass ? 0.f : 1.f;
  if (!p.filterBypass && (p.filterCutoff != svfCutoffCached_ || p.filterRes != svfResCached_ ||
                          p.filterMorph != svfMorphCached_)) {
    UpdateFilterCoefs(p.filterCutoff, p.filterRes, p.filterMorph);
    svfCutoffCached_ = p.filterCutoff;
    svfResCached_    = p.filterRes;
    svfMorphCached_  = p.filterMorph;
  }

  for (uint32_t si = 0; si < kPostStages; ++si) {
    switch (order_[si]) {
      case Stage::Mod: {
        // Early-out when settled at exactly 0 (stall-snap makes that reachable):
        // a bypassed stage costs nothing and its lines freeze — design §2.6.
        if (modDepthSm_.target == 0.f && modDepthSm_.value == 0.f) break;
        const float rateInc = static_cast<float>(static_cast<double>(p.modRateHz) / sr_);
        for (uint32_t n = 0; n < numFrames; ++n) {
          const float wet = modDepthSm_.Next();  // <= 0.5
          // Frozen-bypass gate must be PER SAMPLE: the smoother reaches 0 at an
          // absolute sample, and buffer/LFO state must freeze from exactly that
          // sample under every block splitting (contract #1). A block-start
          // check alone would freeze at split-dependent boundaries.
          if (wet == 0.0f && modDepthSm_.target == 0.0f) continue;
          const float exc = (wet * 2.0f) * modMaxExc_;  // excursion follows depth
          modL_.Write(l[n]);
          modR_.Write(r[n]);
          const float tapL = modL_.ReadBackLerp(modCenter_ + exc * CheapSine(lfoPhase_));
          float phR        = lfoPhase_ + 0.25f;
          if (phR >= 1.f) phR -= 1.f;
          const float tapR = modR_.ReadBackLerp(modCenter_ + exc * CheapSine(phR));
          l[n] = l[n] * (1.0f - wet) + tapL * wet;
          r[n] = r[n] * (1.0f - wet) + tapR * wet;
          lfoPhase_ += rateInc;
          if (lfoPhase_ >= 1.f) lfoPhase_ -= 1.f;
        }
        break;
      }
      case Stage::Delay: {
        if (delayMixSm_.target == 0.f && delayMixSm_.value == 0.f) break;
        auto back = static_cast<uint32_t>(p.delayFrames);
        if (back < 1u) back = 1u;
        if (back > pdL_.len - 1u) back = pdL_.len - 1u;
        const float dfb = p.delayFb;
        for (uint32_t n = 0; n < numFrames; ++n) {
          const float mix = delayMixSm_.Next();
          if (mix == 0.0f && delayMixSm_.target == 0.0f) continue;  // per-sample gate
          // Equal-power crossfade: delay wet is decorrelated from dry, and the
          // linear law scooped the Space macro 5 dB mid-knob (review finding).
          // sqrtf is IEEE-exact, so 0 and 1 stay exact endpoints.
          const float gw   = detmath::SqrtF(mix);
          const float gd   = detmath::SqrtF(1.0f - mix);
          const float tapL = pdL_.ReadBack(back);
          const float tapR = pdR_.ReadBack(back);
          // Damped, DC-blocked regeneration (bare recirculation measured x9.9 DC
          // gain and full-bandwidth repeats forever; review finding).
          pdDcL_ += pdDcCoef_ * (tapL - pdDcL_);
          pdDcR_ += pdDcCoef_ * (tapR - pdDcR_);
          pdLpL_ += pdLpCoef_ * ((tapL - pdDcL_) - pdLpL_);
          pdLpR_ += pdLpCoef_ * ((tapR - pdDcR_) - pdLpR_);
          pdL_.Write(l[n] + pdLpL_ * dfb);
          pdR_.Write(r[n] + pdLpR_ * dfb);
          l[n] = l[n] * gd + tapL * gw;
          r[n] = r[n] * gd + tapR * gw;
        }
        break;
      }
      case Stage::Reverb: {
        if (reverbMixSm_.target == 0.f && reverbMixSm_.value == 0.f) break;
        // Decay floor lowered: 0.35 put RT60 at 2.8 s with the knob at ZERO
        // (review finding); this maps ~0.6 s at 0 to ~10 s at 1.
        const float decay = 0.10f + 0.78f * p.reverbTime;
        for (uint32_t n = 0; n < numFrames; ++n) {
          const float mix = reverbMixSm_.Next();
          if (mix == 0.0f && reverbMixSm_.target == 0.0f) continue;  // per-sample gate
          const float gw  = detmath::SqrtF(mix);
          const float gd  = detmath::SqrtF(1.0f - mix);
          rvBandwidth_ += rvBw_ * (0.5f * (l[n] + r[n]) - rvBandwidth_);
          float x = rvBandwidth_;
          x       = rvAp_[0].Process(x, 0.75f);
          x       = rvAp_[1].Process(x, 0.75f);
          x       = rvAp_[2].Process(x, 0.625f);
          x       = rvAp_[3].Process(x, 0.625f);
          float phB = rvLfoPhase_ + 0.33f;
          if (phB >= 1.f) phB -= 1.f;
          const float t0 = rvDel_[0].ReadBackLerp(rvDl0_ + rvExc_ * (0.5f + 0.5f * CheapSine(rvLfoPhase_)));
          const float t1 = rvDel_[1].ReadBackLerp(rvDl1_ + rvExc_ * (0.5f + 0.5f * CheapSine(phB)));
          // BOTH halves of the figure-8 take their cross-feed damped — rvLp_[1]
          // was a proven-dead store and branch A ran undamped (review finding).
          rvLp_[0] += rvDamp_ * (t0 - rvLp_[0]);
          rvLp_[1] += rvDamp_ * (t1 - rvLp_[1]);
          float a = x + rvLp_[1] * decay;
          a       = rvDap_[0].Process(a, 0.70f);
          a       = rvDap_[1].Process(a, 0.50f);
          rvDel_[0].Write(a);
          float b = x + rvLp_[0] * decay;
          b       = rvDap_[2].Process(b, 0.70f);
          b       = rvDap_[3].Process(b, 0.50f);
          rvDel_[1].Write(b);
          // Multi-tap wet: early energy from ~8 ms in both channels.
          const float wetL = kRvTapGain[0] * rvDel_[0].ReadBack(rvTapL_[0]) +
                             kRvTapGain[1] * rvDap_[1].TapBack(rvTapL_[1]) +
                             kRvTapGain[2] * rvDel_[1].ReadBack(rvTapL_[2]);
          const float wetR = kRvTapGain[0] * rvDel_[1].ReadBack(rvTapR_[0]) +
                             kRvTapGain[1] * rvDap_[3].TapBack(rvTapR_[1]) +
                             kRvTapGain[2] * rvDel_[0].ReadBack(rvTapR_[2]);
          l[n] = l[n] * gd + wetL * gw;
          r[n] = r[n] * gd + wetR * gw;
          rvLfoPhase_ += rvLfoInc_;
          if (rvLfoPhase_ >= 1.f) rvLfoPhase_ -= 1.f;
        }
        break;
      }
      case Stage::Filter: {
        // Smoothed insert mix: an unramped bypass was a 21.6x amplitude step at
        // the knob stop, and frozen state discharged as a 3.9-peak burst on
        // re-engage (review findings). At a settled 0 the stage is skipped
        // exactly and its state cleared once.
        if (filterMixSm_.target == 0.f && filterMixSm_.value == 0.f) break;
        const float fq  = svfFreq_, dp = svfDamp_;
        const float wa  = svfWa_, wb = svfWb_;
        const auto  seg = svfSeg_;
        for (uint32_t n = 0; n < numFrames; ++n) {
          const float fm = filterMixSm_.Next();
          if (fm == 0.0f && filterMixSm_.target == 0.0f) {
            // State clears at the invariant absolute sample where the ramp lands
            // on exactly 0 — so re-engage starts from silence on every split
            // (frozen state discharged as a 3.9-peak burst; review finding).
            if (!svfCleared_) {
              svfL_[0] = svfL_[1] = 0.f;
              svfR_[0] = svfR_[1] = 0.f;
              svfCleared_ = true;
            }
            continue;
          }
          svfCleared_ = false;
          auto tick = [&](float in, float* s) {
            float outs[4] = {0.f, 0.f, 0.f, 0.f};
            for (int pass = 0; pass < 2; ++pass) {
              const float notch = in - dp * s[1];
              s[0] += fq * s[1];
              const float high = notch - s[0];
              s[1] += fq * high;
              outs[0] += 0.5f * s[0];
              outs[1] += 0.5f * s[1];
              outs[2] += 0.5f * high;
              outs[3] += 0.5f * notch;
            }
            return outs[seg] * wa + outs[seg + 1] * wb;
          };
          l[n] = l[n] * (1.0f - fm) + tick(l[n], svfL_) * fm;
          r[n] = r[n] * (1.0f - fm) + tick(r[n], svfR_) * fm;
        }
        break;
      }
    }
  }
}

}  // namespace brainscape::detail
