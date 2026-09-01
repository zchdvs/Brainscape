#include "brainscape/detail/PostChain.h"

#include <cmath>

namespace brainscape::detail {

namespace {

// One-pole LP coefficient at control rate only (expm1 for precision — Smoother.h).
inline float LpCoef(double cutoffHz, double sr) noexcept {
  return -static_cast<float>(std::expm1(-6.283185307179586 * cutoffHz / sr));
}

inline uint32_t ScaleLen(uint32_t base32k, double sr) noexcept {
  const auto n = static_cast<uint32_t>(std::lround(static_cast<double>(base32k) * sr / 32000.0));
  return n < 2u ? 2u : n;
}

// Reverb tank lengths at the Clouds reference rate of 32 kHz (topology per
// Dattorro/Griesinger; length set as published in Mutable Instruments Clouds,
// MIT — see docs/research/post-fx-chain-looper-and-system-budget.md §1.3).
constexpr uint32_t kRvAp[4]  = {113, 162, 241, 399};
constexpr uint32_t kRvDap[4] = {1653, 2038, 1913, 1663};
constexpr uint32_t kRvDel[2] = {3411, 4782};
constexpr float    kRvModExcursion32k = 24.0f;  // frames of LFO wobble on the long delays

// Taming diffuser lengths (ms-scale primes, per channel).
constexpr uint32_t kTamerAp32k[4] = {101, 189, 137, 251};  // L0, L1, R0, R1

// Post-delay ceiling: 2 s (the Space-knob delay never needs more; design §2.6).
constexpr double kPostDelayMaxSeconds = 2.0;
// Mod line: 25 ms, center tap 10 ms, max excursion 8 ms.
constexpr double kModLineSeconds   = 0.025;
constexpr double kModCenterSeconds = 0.010;
constexpr double kModMaxSeconds    = 0.008;

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
}

void FeedbackTamer::Reset() noexcept {
  dcL_ = dcR_ = hpL_ = hpR_ = lpL_ = lpR_ = 0.f;
  for (auto& a : apL_) a.Clear();
  for (auto& a : apR_) a.Clear();
}

void FeedbackTamer::SetFeedback(float fbAmount, double sr) noexcept {
  // LP corner rides the feedback amount, 8 kHz down to 4 kHz (design §2.3):
  // higher regeneration = darker loop, which is what keeps >1 feedback musical.
  const double hz = 8000.0 - 4000.0 * static_cast<double>(fbAmount > 1.f ? 1.f : fbAmount);
  lpCoef_         = LpCoef(hz, sr);
}

void FeedbackTamer::ProcessSample(float& l, float& r) noexcept {
  // DC block (one-pole HP at ~8 Hz).
  dcL_ += dcCoef_ * (l - dcL_);
  dcR_ += dcCoef_ * (r - dcR_);
  float xl = l - dcL_;
  float xr = r - dcR_;
  // HP ~100 Hz: stops the low-frequency buildup that actually blows up granular
  // feedback loops (grain-delay-theory.md §3.10).
  hpL_ += hpCoef_ * (xl - hpL_);
  hpR_ += hpCoef_ * (xr - hpR_);
  xl -= hpL_;
  xr -= hpR_;
  // Feedback-dependent LP.
  lpL_ += lpCoef_ * (xl - lpL_);
  lpR_ += lpCoef_ * (xr - lpR_);
  // Soft saturation bounds the loop gain — this is what does the real work at
  // feedback > 1 (grain-delay-theory.md §3.10).
  xl = SoftSat(lpL_);
  xr = SoftSat(lpR_);
  // Short allpass diffusion smears repeats so they blur instead of stacking.
  constexpr float kG = 0.6f;
  xl = apL_[1].Process(apL_[0].Process(xl, kG), kG);
  xr = apR_[1].Process(apR_[0].Process(xr, kG), kG);
  l = xl;
  r = xr;
}

// ── PostChain ───────────────────────────────────────────────────────────────────

uint32_t PostChain::WarmFloats(double sr) noexcept {
  uint32_t total = 0;
  total += 2u * static_cast<uint32_t>(std::lround(kModLineSeconds * sr));  // mod L+R
  for (uint32_t b : kRvAp) total += ScaleLen(b, sr);
  for (uint32_t b : kRvDap) total += ScaleLen(b, sr);
  for (uint32_t b : kRvDel) {
    total += ScaleLen(b, sr) + static_cast<uint32_t>(kRvModExcursion32k * sr / 32000.0) + 4u;
  }
  return total;
}

uint32_t PostChain::BulkFloats(double sr) noexcept {
  return 2u * static_cast<uint32_t>(std::lround(kPostDelayMaxSeconds * sr));
}

void PostChain::Init(float* warm, float* bulk, double sr) noexcept {
  sr_      = sr;
  float* p = warm;
  const auto modLen = static_cast<uint32_t>(std::lround(kModLineSeconds * sr));
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

  const auto pdLen = static_cast<uint32_t>(std::lround(kPostDelayMaxSeconds * sr));
  pdL_.Init(bulk, pdLen);
  pdR_.Init(bulk + pdLen, pdLen);

  modDepthSm_.SetTau(10.0f, sr);
  delayMixSm_.SetTau(10.0f, sr);
  reverbMixSm_.SetTau(10.0f, sr);
  svfCutoffCached_ = -1.f;
  svfResCached_    = -1.f;
  Reset();
}

void PostChain::Reset() noexcept {
  modL_.Clear();
  modR_.Clear();
  pdL_.Clear();
  pdR_.Clear();
  for (auto& a : rvAp_) a.Clear();
  for (auto& a : rvDap_) a.Clear();
  for (auto& d : rvDel_) d.Clear();
  rvBandwidth_ = rvLp_[0] = rvLp_[1] = 0.f;
  lfoPhase_ = rvLfoPhase_ = 0.f;
  svfL_[0] = svfL_[1] = 0.f;
  svfR_[0] = svfR_[1] = 0.f;
  modDepthSm_.Prime(modDepthSm_.target);
  delayMixSm_.Prime(delayMixSm_.target);
  reverbMixSm_.Prime(reverbMixSm_.target);
}

void PostChain::UpdateFilterCoefs(float cutoff, float res) noexcept {
  // DaisySP-style double-sampled SVF coefficients (MIT; post-fx doc §3.1).
  // Control rate only — sinf never runs per sample.
  const double f = static_cast<double>(cutoff) / (sr_ * 2.0);
  svfFreq_ = static_cast<float>(2.0 * std::sin(3.14159265358979 * (f < 0.25 ? f : 0.25)));
  const float r  = res < 0.f ? 0.f : (res > 1.f ? 1.f : res);
  float damp     = 2.0f * (1.0f - std::pow(r, 0.25f));
  const float lim = 2.0f / svfFreq_ - svfFreq_ * 0.5f;
  if (damp > lim) damp = lim;
  if (damp > 2.0f) damp = 2.0f;
  svfDamp_ = damp;
}

void PostChain::Process(const PostParams& p, uint32_t numFrames, float* l, float* r) noexcept {
  modDepthSm_.target   = p.modDepth;
  delayMixSm_.target   = p.delayMix;
  reverbMixSm_.target  = p.reverbMix;
  if (!p.filterBypass && (p.filterCutoff != svfCutoffCached_ || p.filterRes != svfResCached_)) {
    UpdateFilterCoefs(p.filterCutoff, p.filterRes);
    svfCutoffCached_ = p.filterCutoff;
    svfResCached_    = p.filterRes;
  }

  for (uint32_t si = 0; si < kPostStages; ++si) {
    switch (order_[si]) {
      case Stage::Mod: {
        const float rateInc = static_cast<float>(static_cast<double>(p.modRateHz) / sr_);
        const auto  center  = static_cast<float>(kModCenterSeconds * sr_);
        const auto  maxExc  = static_cast<float>(kModMaxSeconds * sr_);
        for (uint32_t n = 0; n < numFrames; ++n) {
          const float depth = modDepthSm_.Next();
          modL_.Write(l[n]);
          modR_.Write(r[n]);
          const float exc  = depth * maxExc;
          const float tapL = modL_.ReadBackLerp(center + exc * CheapSine(lfoPhase_));
          float phR        = lfoPhase_ + 0.25f;
          if (phR >= 1.f) phR -= 1.f;
          const float tapR = modR_.ReadBackLerp(center + exc * CheapSine(phR));
          // depth doubles as the wet mix: depth 0 is exactly transparent
          // (l*1 + tap*0), which the null contracts rely on.
          l[n] = l[n] * (1.0f - depth) + tapL * depth;
          r[n] = r[n] * (1.0f - depth) + tapR * depth;
          lfoPhase_ += rateInc;
          if (lfoPhase_ >= 1.f) lfoPhase_ -= 1.f;
        }
        break;
      }
      case Stage::Delay: {
        auto back = static_cast<uint32_t>(p.delayFrames);
        if (back < 1u) back = 1u;
        if (back > pdL_.len - 1u) back = pdL_.len - 1u;
        const float dfb = p.delayFb;
        for (uint32_t n = 0; n < numFrames; ++n) {
          const float mix  = delayMixSm_.Next();
          const float tapL = pdL_.ReadBack(back);
          const float tapR = pdR_.ReadBack(back);
          pdL_.Write(l[n] + tapL * dfb);
          pdR_.Write(r[n] + tapR * dfb);
          l[n] = l[n] * (1.0f - mix) + tapL * mix;
          r[n] = r[n] * (1.0f - mix) + tapR * mix;
        }
        break;
      }
      case Stage::Reverb: {
        // Decay/damping ride reverbTime (control-block rate, plain arithmetic).
        const float decay = 0.35f + 0.6f * p.reverbTime;
        const float damp  = LpCoef(5500.0, sr_);
        const float bw    = LpCoef(11000.0, sr_);
        const float lfoInc = static_cast<float>(0.5 / sr_);  // 0.5 Hz shimmer
        const auto  exc    = static_cast<float>(kRvModExcursion32k * sr_ / 32000.0);
        const auto  dl0    = static_cast<float>(ScaleLen(kRvDel[0], sr_));
        const auto  dl1    = static_cast<float>(ScaleLen(kRvDel[1], sr_));
        for (uint32_t n = 0; n < numFrames; ++n) {
          const float mix = reverbMixSm_.Next();
          // Input: mono sum -> bandwidth LP -> 4 series diffusers.
          rvBandwidth_ += bw * (0.5f * (l[n] + r[n]) - rvBandwidth_);
          float x = rvBandwidth_;
          x       = rvAp_[0].Process(x, 0.75f);
          x       = rvAp_[1].Process(x, 0.75f);
          x       = rvAp_[2].Process(x, 0.625f);
          x       = rvAp_[3].Process(x, 0.625f);
          // Figure-8 tank, LFO-modulated long-delay reads (kills metallic combs).
          float phB = rvLfoPhase_ + 0.33f;
          if (phB >= 1.f) phB -= 1.f;
          const float t0 = rvDel_[0].ReadBackLerp(dl0 + exc * (0.5f + 0.5f * CheapSine(rvLfoPhase_)));
          const float t1 = rvDel_[1].ReadBackLerp(dl1 + exc * (0.5f + 0.5f * CheapSine(phB)));
          float a = x + t1 * decay;
          a       = rvDap_[0].Process(a, 0.70f);
          a       = rvDap_[1].Process(a, 0.50f);
          rvDel_[0].Write(a);
          rvLp_[0] += damp * (t0 - rvLp_[0]);
          float b = x + rvLp_[0] * decay;
          b       = rvDap_[2].Process(b, 0.70f);
          b       = rvDap_[3].Process(b, 0.50f);
          rvDel_[1].Write(b);
          rvLp_[1] += damp * (t1 - rvLp_[1]);
          const float wetL = t0;
          const float wetR = t1;
          l[n] = l[n] * (1.0f - mix) + wetL * mix;
          r[n] = r[n] * (1.0f - mix) + wetR * mix;
          rvLfoPhase_ += lfoInc;
          if (rvLfoPhase_ >= 1.f) rvLfoPhase_ -= 1.f;
        }
        break;
      }
      case Stage::Filter: {
        if (p.filterBypass) break;  // exact bypass (design §2.6 endpoint semantics)
        const float fq = svfFreq_, dp = svfDamp_;
        const float morph = p.filterMorph < 0.f ? 0.f : (p.filterMorph > 3.f ? 3.f : p.filterMorph);
        const auto  seg   = morph >= 3.f ? 2u : static_cast<uint32_t>(morph);
        const float fr    = morph - static_cast<float>(seg);
        for (uint32_t n = 0; n < numFrames; ++n) {
          auto tick = [&](float in, float* s) {
            // Double-sampled Chamberlin SVF: two half-rate passes, averaged
            // outputs (post-fx doc §3.1) — all four responses fall out.
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
            return outs[seg] + (outs[seg + 1] - outs[seg]) * fr;
          };
          l[n] = tick(l[n], svfL_);
          r[n] = tick(r[n], svfR_);
        }
        break;
      }
    }
  }
}

}  // namespace brainscape::detail
