#include "brainscape/Engine.h"

#include <cassert>
#include <cmath>
#include <cstring>

#include "brainscape/DenormalGuard.h"
#include "brainscape/GrainMath.h"

namespace brainscape {

namespace {

constexpr bool TableIsContiguous() {
  for (size_t i = 0; i < kNumParams; ++i) {
    if (static_cast<uint32_t>(kParamTable[i].id) != i + 1) return false;
  }
  return true;
}
static_assert(TableIsContiguous(), "kParamTable must be ordered by contiguous ids from 1");

// SetParam/GetParam are documented lock-free from any thread (design §9 threading
// table); make the assumption a compile error on the day it stops being true.
static_assert(std::atomic<float>::is_always_lock_free,
              "SetParam/GetParam must be lock-free on this target");

// The onset event list must cover every hop boundary in the largest legal block
// (maxBlockSize <= kFeedbackDelayFrames, enforced in Init) — if the FIFO constant
// is ever raised, this is the assert that keeps onsets from being dropped.
static_assert(kFeedbackDelayFrames / detail::kOnsetHop + 2u <=
                  detail::TriggerEvents::kMaxOnsets,
              "TriggerEvents::kMaxOnsets must cover the largest legal block");

constexpr float kInvScale = 1.0f / 32767.0f;

// Rounding-mode-independent, NaN-safe int16 quantizer. Not lrintf: lrintf follows
// the dynamic FP rounding mode (which the denormal guard deliberately does not pin)
// and is a libm call in the hot loop on Cortex-M7 (review finding, verified with
// arm-none-eabi-gcc). Round-half-away-from-zero via a plain truncating convert.
// Clamp is symmetric at ±32767 so the ring's float range stays exactly [-1, 1],
// and the negated comparisons map NaN to a defined endpoint on every platform.
inline int16_t QuantizeS16(float x) noexcept {
  float s = x * 32767.0f;
  if (!(s < 32767.0f)) s = 32767.0f;
  if (!(s > -32767.0f)) s = -32767.0f;
  return static_cast<int16_t>(s >= 0.0f ? s + 0.5f : s - 0.5f);
}

inline bool IsPowerOfTwo(uint32_t v) noexcept { return v != 0 && (v & (v - 1)) == 0; }

// ±1 LSB TPDF dither in the float domain (design §12.3). Applied to the ring write
// so int16 rounding has no fixed points in the feedback loop — without it a single
// impulse leaves a permanent tone (measured −70 dBFS at fb 0.95; review finding).
// Keys use the shared grainmath purpose stride so dither draws never collide with
// grain-birth draws.
inline float Tpdf(int64_t absSample, grainmath::Draw purpose) noexcept {
  const auto key =
      static_cast<uint32_t>(absSample) * static_cast<uint32_t>(grainmath::Draw::kCount) +
      static_cast<uint32_t>(purpose);
  const float u1 =
      static_cast<float>(grainmath::Hash32(key) >> 8) * (1.0f / 16777216.0f);
  const float u2 =
      static_cast<float>(grainmath::Hash32(key ^ 0x6A09E667u) >> 8) * (1.0f / 16777216.0f);
  return (u1 + u2 - 1.0f) * kInvScale;
}

}  // namespace

const ParamDescriptor* Descriptors(size_t* count) noexcept {
  if (count != nullptr) *count = kNumParams;
  return kParamTable;
}

const ParamDescriptor* Engine::Descriptors(size_t* count) noexcept {
  return brainscape::Descriptors(count);
}

const ParamDescriptor* FindParam(ParamId id) noexcept {
  const uint32_t raw = static_cast<uint32_t>(id);
  if (raw < 1 || raw > kNumParams) return nullptr;
  return &kParamTable[raw - 1];
}

MemoryPlan PlanMemory(const EngineConfig& cfg) noexcept {
  MemoryPlan plan{};
  // Hot (DTCM-class): window LUT + wet accumulators. The grain pool itself lives
  // inside the Engine object — firmware places the Engine instance in DTCM.
  plan.bytes[static_cast<size_t>(Tier::Hot)] =
      (static_cast<size_t>(detail::kWindowLutSize) + 2u * cfg.maxBlockSize) * sizeof(float);
  plan.align[static_cast<size_t>(Tier::Hot)] = 16;
  // Warm (AXI-class): feedback FIFO + taming diffuser + mod lines + reverb tank
  // + onset-detector analysis/FFT/whitening state.
  plan.bytes[static_cast<size_t>(Tier::Warm)] =
      (static_cast<size_t>(kFeedbackDelayFrames) * 2u +
       detail::FeedbackTamer::WarmFloats(cfg.sampleRate) +
       detail::PostChain::WarmFloats(cfg.sampleRate) + detail::OnsetDetector::WarmFloats()) *
      sizeof(float);
  plan.align[static_cast<size_t>(Tier::Warm)] = 16;
  // Bulk (SDRAM-class): history ring + post-delay buffer; looper A+B when the
  // looper lands. Frame counts are bounded by Init (<= 2^26), so these products
  // cannot overflow a 32-bit size_t on the embedded target.
  plan.bytes[static_cast<size_t>(Tier::Bulk)] =
      static_cast<size_t>(cfg.historyFrames) * 2u * sizeof(int16_t) +
      static_cast<size_t>(cfg.looperFrames) * 2u * sizeof(int16_t) * 2u +
      static_cast<size_t>(detail::PostChain::BulkFloats(cfg.sampleRate)) * sizeof(float);
  // Cache-line aligned: the SD/DMA coherency rule needs 32-byte-aligned ranges (design §7).
  plan.align[static_cast<size_t>(Tier::Bulk)] = 32;
  return plan;
}

bool Engine::Init(const EngineConfig& cfg, const Arenas& arenas) noexcept {
  ready_ = false;
  // Rate bounds: below ~40 Hz the ms-sized post buffers round to zero length and
  // a DelaySlice write walks off its arena (review finding); above 384 kHz is
  // outside anything the design supports.
  if (cfg.sampleRate < 8000.0 || cfg.sampleRate > 384000.0) return false;
  if (cfg.maxBlockSize == 0) return false;
  if (cfg.maxBlockSize > kFeedbackDelayFrames) return false;  // wrappers chunk larger buffers
  if (!IsPowerOfTwo(cfg.historyFrames)) return false;
  // Bounds guard both usefulness (design fixes the ring at 2^22; the ratio ceiling
  // makes anything past 2^26 meaningless) and 32-bit size_t overflow in PlanMemory,
  // which would otherwise wrap to a small plan and bypass arena validation entirely
  // (review finding).
  if (cfg.historyFrames < 8u || cfg.historyFrames > (1u << 26)) return false;
  if (cfg.looperFrames > (1u << 26)) return false;

  const MemoryPlan plan = PlanMemory(cfg);
  for (size_t t = 0; t < kNumTiers; ++t) {
    if (plan.bytes[t] == 0) continue;
    if (arenas.base[t] == nullptr || arenas.bytes[t] < plan.bytes[t]) return false;
    // Alignment is part of the plan, not advice: a misaligned Bulk base violates the
    // SD/DMA cache-coherency rule (design §7) and misaligned int16 access is UB.
    if ((reinterpret_cast<uintptr_t>(arenas.base[t]) & (plan.align[t] - 1u)) != 0) return false;
  }

  cfg_  = cfg;
  ring_ = static_cast<int16_t*>(arenas.base[static_cast<size_t>(Tier::Bulk)]);
  auto* hot  = static_cast<float*>(arenas.base[static_cast<size_t>(Tier::Hot)]);
  windowLut_ = hot;
  wetL_      = hot + detail::kWindowLutSize;
  wetR_      = wetL_ + cfg.maxBlockSize;
  // Warm layout: [feedback FIFO][taming diffuser][mod lines + reverb tank].
  auto* warm = static_cast<float*>(arenas.base[static_cast<size_t>(Tier::Warm)]);
  fbFifo_    = warm;
  warm += static_cast<size_t>(kFeedbackDelayFrames) * 2u;
  tamer_.Init(warm, cfg.sampleRate);
  warm += detail::FeedbackTamer::WarmFloats(cfg.sampleRate);
  // Bulk layout: [history ring][looper A+B (future)][post-delay floats].
  auto* postBulk = reinterpret_cast<float*>(
      reinterpret_cast<char*>(arenas.base[static_cast<size_t>(Tier::Bulk)]) +
      static_cast<size_t>(cfg.historyFrames) * 2u * sizeof(int16_t) +
      static_cast<size_t>(cfg.looperFrames) * 2u * sizeof(int16_t) * 2u);
  post_.Init(warm, postBulk, cfg.sampleRate);
  warm += detail::PostChain::WarmFloats(cfg.sampleRate);
  detector_.Init(warm, cfg.sampleRate);
  mask_          = cfg.historyFrames - 1u;
  writeFrame_    = 0;
  sampleCounter_ = 0;
  frozen_        = false;
  frozenAnchor_  = 0;
  freezePending_.store(false, std::memory_order_relaxed);

  // The Bulk arena (SDRAM on hardware) has undefined contents at boot. Clear the
  // history ring here — and only the ring; looper buffers are cleared explicitly by
  // the caller when that subsystem lands (design §7).
  std::memset(ring_, 0, static_cast<size_t>(cfg.historyFrames) * 2u * sizeof(int16_t));
  std::memset(fbFifo_, 0, static_cast<size_t>(kFeedbackDelayFrames) * 2u * sizeof(float));

  // Half-cosine smoothing LUT: maps the unit-peak piecewise envelope value to its
  // cosine-eased equivalent. Mean over a linear ramp is 0.5 — identical to the
  // raw leg — which is what makes the (1+sustain)/2 window mean exact (GrainMath.h).
  for (uint32_t i = 0; i < detail::kWindowLutSize; ++i) {
    const double x = static_cast<double>(i) / static_cast<double>(detail::kWindowLutSize - 1);
    windowLut_[i]  = static_cast<float>(0.5 * (1.0 - std::cos(3.14159265358979323846 * x)));
  }

  granular_.Init(ring_, mask_, windowLut_);

  mix_.SetTau(10.0f, cfg.sampleRate);
  outGain_.SetTau(10.0f, cfg.sampleRate);
  feedback_.SetTau(10.0f, cfg.sampleRate);
  norm_.SetTau(100.0f, cfg.sampleRate);  // design §3: τ ≈ 100 ms

  for (size_t i = 0; i < kNumParams; ++i) {
    pending_[i].store(kParamTable[i].def, std::memory_order_relaxed);
    active_[i] = kParamTable[i].def;
    ApplyParam(i, kParamTable[i].def);
  }
  RebuildGranularParams();
  RebuildPostParams();
  post_.Reset(pp_);  // primes the post-chain mix smoothers from the ACTUAL params
  granularDirty_ = false;
  postDirty_     = false;
  mix_.Prime(mix_.target);
  outGain_.Prime(outGain_.target);
  feedback_.Prime(feedback_.target);
  norm_.Prime(norm_.target);

  ready_ = true;
  return true;
}

void Engine::Reset() noexcept {
  if (!ready_) return;
  granular_.Reset();
  tamer_.Reset();
  detector_.Reset();
  onsetCount_.store(0u, std::memory_order_relaxed);
  manualTriggers_.store(0u, std::memory_order_relaxed);
  std::memset(fbFifo_, 0, static_cast<size_t>(kFeedbackDelayFrames) * 2u * sizeof(float));
  for (size_t i = 0; i < kNumParams; ++i) {
    const float p = pending_[i].load(std::memory_order_relaxed);
    active_[i]    = p;
    ApplyParam(i, p);
  }
  RebuildGranularParams();
  RebuildPostParams();
  // RT-safe post reset: small state + smoother priming only — clearing the
  // 750 KiB SDRAM post-delay here cost 2-4 consecutive audio deadlines (review
  // finding). The full buffer clear lives in ClearHistory (non-RT).
  post_.Reset(pp_);
  granularDirty_ = false;
  postDirty_     = false;
  mix_.Prime(mix_.target);
  outGain_.Prime(outGain_.target);
  feedback_.Prime(feedback_.target);
  norm_.Prime(norm_.target);
}

void Engine::ClearHistory() noexcept {
  if (!ready_) return;
  std::memset(ring_, 0, static_cast<size_t>(cfg_.historyFrames) * 2u * sizeof(int16_t));
  post_.ClearBuffers();  // the post delay/reverb tails are history too
}

void Engine::ClearLooper() noexcept {
  // No looper buffers yet. When they land, this stays an explicit user gesture —
  // never called from a plugin prepare path (design §7).
}

void Engine::ApplyParam(size_t index, float value) noexcept {
  switch (kParamTable[index].id) {
    case ParamId::Mix:
      mix_.target = value;
      break;
    case ParamId::Feedback:
      feedback_.target = value;
      tamer_.SetFeedback(value, cfg_.sampleRate);  // LP corner rides regeneration
      break;
    case ParamId::OutTrimDb:
      // exp2f, not powf (cheaper, and the pattern to copy is LUT+lerp when
      // SemitonesToRatio lands — schedule-time transcendentals are charged in §8).
      outGain_.target = std::exp2(value * 0.16609640474436813f);  // dB -> linear
      break;
    case ParamId::TriggerSens:
      detector_.SetSensitivity(value);
      break;
    default:
      // Scheduler/voice params vs post-chain params rebuild their own blocks,
      // once, at control rate.
      if (static_cast<uint32_t>(kParamTable[index].id) >=
          static_cast<uint32_t>(ParamId::ModRateHz)) {
        postDirty_ = true;
      } else {
        granularDirty_ = true;
      }
      break;
  }
}

void Engine::RebuildPostParams() noexcept {
  const auto get = [&](ParamId id) {
    return active_[static_cast<uint32_t>(id) - 1u];
  };
  pp_.modRateHz    = get(ParamId::ModRateHz);
  pp_.modDepth     = get(ParamId::ModDepth);
  pp_.delayFrames  = static_cast<float>(get(ParamId::DelayTimeMs) * 0.001 * cfg_.sampleRate);
  pp_.delayFb      = get(ParamId::DelayFb);
  pp_.delayMix     = get(ParamId::DelayMix);
  pp_.reverbTime = get(ParamId::ReverbTime);
  pp_.reverbMix  = get(ParamId::ReverbMix);
  const float rawCutoff = get(ParamId::FilterCutoffHz);
  // Bypass decides on the RAW knob value (fully CW = bypass, design §2.6); the
  // engaged cutoff is clamped against the actual rate so the knob cannot silently
  // pin past sr/4 at low sample rates (review finding).
  pp_.filterBypass = rawCutoff >= FindParam(ParamId::FilterCutoffHz)->max - 0.5f;
  const auto maxHz = static_cast<float>(cfg_.sampleRate * 0.45);
  pp_.filterCutoff = rawCutoff < maxHz ? rawCutoff : maxHz;
  pp_.filterRes    = get(ParamId::FilterRes);
  pp_.filterMorph  = get(ParamId::FilterMorph);
}

void Engine::RebuildGranularParams() noexcept {
  const auto get = [&](ParamId id) {
    return active_[static_cast<uint32_t>(id) - 1u];
  };
  const double sr = cfg_.sampleRate;

  gp_.baseDelayFrames = static_cast<double>(get(ParamId::DelayMs)) * 0.001 * sr;
  gp_.sprayFrames     = static_cast<float>(get(ParamId::SprayMs) * 0.001 * sr);
  // ONE rounded integer drives grain length, spacing, and the voice budget —
  // spacing from the unrounded float opened duty-cycle holes across 80% of the
  // size_ms range (review finding, up to 48.8% silence).
  auto total = static_cast<uint32_t>(std::lround(get(ParamId::GrainSizeMs) * 0.001 * sr));
  if (total < 1u) total = 1u;
  gp_.totalFrames = total;

  const float overlap = get(ParamId::Overlap);
  float target        = static_cast<float>(kMaxGrains) * overlap * overlap * overlap;
  if (target < 1.0f) target = 1.0f;
  if (target > static_cast<float>(kMaxGrains)) target = static_cast<float>(kMaxGrains);
  // The 1-frame inter-arrival floor caps sustainable voices at the grain length:
  // normalizing to an unreachable target read up to -12.9 dB low (review finding).
  if (target > static_cast<float>(total)) target = static_cast<float>(total);
  gp_.targetVoices = target;

  gp_.jitter      = get(ParamId::Jitter);
  gp_.ratioBase   = get(ParamId::PitchSt);
  gp_.spreadCents = get(ParamId::SpreadCents);
  gp_.reverseProb = get(ParamId::ReverseProb);
  gp_.sustain      = get(ParamId::WindowSustain);
  gp_.skew         = get(ParamId::WindowSkew);
  gp_.smoothness   = get(ParamId::WindowSmooth);
  gp_.panSpread    = get(ParamId::PanSpread);
  gp_.onsetTrigger = get(ParamId::OnsetTrigger) >= 0.5f;
  gp_.posFromMark  = get(ParamId::PositionSource) >= 0.5f;

  // Coherence-aware normalization exponent (design §3): unity-rate, zero-spray
  // grains all read the SAME source sample and sum coherently (1/N); anything
  // that spreads their read positions decorrelates them toward 1/sqrt(N).
  // Every term is continuous and expressed as accumulated divergence — the v1
  // heuristic's boolean pitch term was a +17.85 dB cliff at 0.001 st, its
  // frame-keyed spray term saturated at 1.33 ms (and differed per sample rate),
  // and its jitter term had the wrong sign: at unity rate a grain's output is
  // independent of its birth time, so timing jitter decorrelates nothing
  // (review findings, all measured).
  const float ratio        = grainmath::SemitonesToRatio(gp_.ratioBase);
  const float kDecorrFrames = static_cast<float>(0.003 * sr);  // ~3 ms of divergence = full
  auto clamp01 = [](float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); };
  float decorr = 0.f;
  decorr += clamp01(std::fabs(ratio - 1.0f) * static_cast<float>(total) / kDecorrFrames);
  decorr += clamp01(get(ParamId::SprayMs) * (1.0f / 30.0f));  // ms-based: rate-independent
  decorr += clamp01(std::fabs(grainmath::SemitonesToRatio(gp_.spreadCents * 0.01f) - 1.0f) *
                    static_cast<float>(total) / kDecorrFrames);
  decorr += gp_.reverseProb;
  // Freeze pins the anchor, turning identical grains into time-shifted copies of
  // one window — fully decorrelated. Without this term, engaging freeze on a
  // coherent preset dropped the wet path up to 16.7 dB (review finding).
  // POS_MARK is the same geometry (all grains anchored to one ring frame, born
  // at different times) — without its term the Strum family read up to 18 dB
  // quiet across the overlap knob (review finding).
  if (frozen_ || gp_.posFromMark) decorr = 1.0f;
  decorr        = clamp01(decorr);
  const float p = 1.0f - 0.5f * decorr;
  norm_.target  = std::pow(gp_.targetVoices, -p);
}

void Engine::SetParam(ParamId id, float value, uint32_t /*sampleOffset*/) noexcept {
  const ParamDescriptor* d = FindParam(id);
  if (d == nullptr) return;
  // Negated comparisons: NaN fails both, so a non-finite value maps to the minimum
  // instead of slipping through into the smoothers, where NaN is an absorbing
  // state recoverable only by Reset()/Init() (review finding, verified).
  if (!(value >= d->min)) value = d->min;
  if (!(value <= d->max)) value = d->max;
  pending_[static_cast<uint32_t>(id) - 1u].store(value, std::memory_order_relaxed);
}

float Engine::GetParam(ParamId id) const noexcept {
  const ParamDescriptor* d = FindParam(id);
  if (d == nullptr) return 0.f;
  return pending_[static_cast<uint32_t>(id) - 1u].load(std::memory_order_relaxed);
}

void Engine::Process(const ProcessContext& ctx) noexcept {
  // No assert on ready_: the documented contract IS the zero-fill below, and an
  // assert here killed the whole suite on the Debug/sanitizer CI leg (review
  // finding). The block-size assert stays — that one is a genuine caller bug.
  assert(ctx.numFrames >= 1 && ctx.numFrames <= cfg_.maxBlockSize);
  if (!ready_ || ctx.in == nullptr || ctx.out == nullptr || ctx.numFrames == 0 ||
      ctx.numFrames > cfg_.maxBlockSize) {
    // Never hand back stale host memory — and never overrun the Hot-arena wet
    // buffers on an oversized block (review finding: hosts do hand out blocks
    // larger than the prepared maximum; that was a silent heap overflow).
    if (ctx.out != nullptr) {
      for (uint32_t n = 0; n < ctx.numFrames; ++n) {
        if (ctx.out[0] != nullptr) ctx.out[0][n] = 0.f;
        if (ctx.out[1] != nullptr) ctx.out[1][n] = 0.f;
      }
    }
    return;
  }

  ScopedDenormalGuard guard;

  // Drain pending parameter changes at block start (sample-accurate queue lands
  // with the scheduler's event queue). Freeze drains FIRST because the
  // normalization exponent depends on it (frozen grains are decorrelated).
  const bool freezeReq = freezePending_.load(std::memory_order_relaxed);
  if (freezeReq != frozen_) {
    frozen_ = freezeReq;
    if (frozen_) frozenAnchor_ = writeFrame_;  // pin the anchor at engage (design §2.4)
    granularDirty_ = true;
  }
  if (frozen_) {
    // Re-anchor-on-wrap (design §2.4 decided behavior): once the live write head
    // has consumed 3/4 of the ring behind the pin, re-pin to the present. The
    // splice is audible and documented; the alternative was the frozen window
    // silently degrading into delayed live signal as it is overwritten.
    const uint32_t age = (writeFrame_ - frozenAnchor_) & mask_;
    if (age > cfg_.historyFrames - (cfg_.historyFrames >> 2)) {
      frozenAnchor_ = writeFrame_;
    }
  }
  for (size_t i = 0; i < kNumParams; ++i) {
    const float p = pending_[i].load(std::memory_order_relaxed);
    if (p != active_[i]) {
      active_[i] = p;
      ApplyParam(i, p);
    }
  }
  if (granularDirty_) {
    RebuildGranularParams();
    granularDirty_ = false;
  }
  if (postDirty_) {
    RebuildPostParams();
    postDirty_ = false;
  }

  const float* inL  = ctx.in[0];
  const float* inR  = cfg_.stereoInput ? ctx.in[1] : ctx.in[0];
  float*       outL = ctx.out[0];
  float*       outR = ctx.out[1];

  const uint32_t ringStart = writeFrame_;
  const bool     dither    = cfg_.ditherRingWrite;

  // ── Pass 1: write input (+ feedback) into the ring, per sample, and feed the
  // onset detector (hop boundaries on the absolute grid, so detection is
  // split-invariant). The feedback signal is the wet output delayed by exactly
  // kFeedbackDelayFrames through a fixed-length FIFO — a shared build constant,
  // so the loop period is identical on firmware and plugin (contracts #1/#6).
  // Slot index is a mask of the absolute sample (power-of-two length) — a 64-bit
  // modulo compiled to two __aeabi_uldivmod calls per sample on Cortex-M7.
  // Deliver at most numFrames manual triggers this block and CARRY the surplus —
  // draining the counter dropped every trigger past numFrames, silently breaking
  // the "explicit triggers never drop" contract on small blocks (review finding).
  detail::TriggerEvents ev;
  {
    const uint32_t queued = manualTriggers_.load(std::memory_order_relaxed);
    ev.manualCount        = queued < ctx.numFrames ? queued : ctx.numFrames;
    if (ev.manualCount > 0) {
      manualTriggers_.fetch_sub(ev.manualCount, std::memory_order_relaxed);
    }
  }
  for (uint32_t n = 0; n < ctx.numFrames; ++n) {
    const int64_t abs  = sampleCounter_ + n;
    const auto    slot = static_cast<uint32_t>(abs) & (kFeedbackDelayFrames - 1u);
    const float   fb   = feedback_.Next();
    float wrL          = inL[n] + fb * fbFifo_[2u * slot];
    float wrR          = inR[n] + fb * fbFifo_[2u * slot + 1u];

    // Detector listens to the raw mono input (pre-feedback: regenerated wet must
    // not re-trigger grains — that would be a trigger feedback loop). It runs
    // unconditionally even when nothing consumes onsets: the trigger LED must
    // stay live for sensitivity calibration (research rec #7), and gating it on
    // parameters would make the whitening state parameter-history dependent.
    if (detector_.ProcessSample(0.5f * (inL[n] + inR[n]), abs)) {
      onsetCount_.fetch_add(1u, std::memory_order_relaxed);  // LED even if the
                                                             // event list is full
      if (ev.onsetCount < detail::TriggerEvents::kMaxOnsets) {
        ev.onsetOffset[ev.onsetCount] = n;
        // Attribute the onset's audio to the start of the hop just analyzed.
        ev.onsetMarkFrame[ev.onsetCount] =
            (ringStart + n + 1u - detail::kOnsetHop) & mask_;
        ++ev.onsetCount;
      }
    }
    if (dither) {
      // Gated at a quarter LSB, not at zero: below half an LSB plain rounding
      // already absorbs to exact 0 (no fixed points exist down there), while a
      // zero gate let the taming chain's asymptotic filter tail hold the gate
      // open and sustain an LSB-level dither loop forever. Above the gate, the
      // write value performs a downward random walk absorbed at 0 instead of
      // latching on a quantization fixed point.
      constexpr float kDitherGate = 0.25f / 32767.0f;
      if (wrL > kDitherGate || wrL < -kDitherGate) wrL += Tpdf(abs, grainmath::Draw::DitherL);
      if (wrR > kDitherGate || wrR < -kDitherGate) wrR += Tpdf(abs, grainmath::Draw::DitherR);
    }
    ring_[2u * writeFrame_]      = QuantizeS16(wrL);
    ring_[2u * writeFrame_ + 1u] = QuantizeS16(wrR);
    writeFrame_ = (writeFrame_ + 1u) & mask_;
  }

  // ── Pass 2: schedule + render the grain block (per-grain over the whole block).
  granular_.Process(gp_, ev, sampleCounter_, ringStart, frozen_, frozenAnchor_,
                    ctx.numFrames, wetL_, wetR_);

  // ── Pass 3a: normalization (smoothed), then the feedback tap — TAMED wet into
  // the FIFO (design §2.3: DC/HP/LP/saturator/diffuser sit inside the loop; the
  // tap is pre-post-chain per the §2 diagram). The tamer always runs so its
  // filter state stays split-invariant regardless of the feedback amount.
  for (uint32_t n = 0; n < ctx.numFrames; ++n) {
    const int64_t abs  = sampleCounter_ + n;
    const auto    slot = static_cast<uint32_t>(abs) & (kFeedbackDelayFrames - 1u);
    const float   nrm  = norm_.Next();
    const float   wl   = wetL_[n] * nrm;
    const float   wr   = wetR_[n] * nrm;
    wetL_[n] = wl;
    wetR_[n] = wr;
    float tl = wl, tr = wr;
    tamer_.ProcessSample(tl, tr);
    fbFifo_[2u * slot]      = tl;
    fbFifo_[2u * slot + 1u] = tr;
  }

  // ── Pass 3b: the post chain, in place on the wet buffers (design §2.6:
  // mod -> delay -> reverb -> filter, ordered and bypassable).
  post_.Process(pp_, ctx.numFrames, wetL_, wetR_);

  // ── Pass 3c: wet/dry mix and output trim.
  for (uint32_t n = 0; n < ctx.numFrames; ++n) {
    const float mix = mix_.Next();
    const float g   = outGain_.Next();
    // Linear wet/dry crossfade (grain-delay-theory.md §3.11); dry is never delayed.
    // Two-multiply form, not dry + mix*(wet-dry): the lerp form is not bit-exact at
    // the endpoints, which would break the Tu null contract (design §10 #2).
    // Both dry reads precede either write: hosts process in place (in[0] == out[0])
    // and mono input aliases inR to inL, so writing outL first corrupted every outR.
    const float dryL = inL[n];
    const float dryR = inR[n];
    outL[n] = (dryL * (1.0f - mix) + wetL_[n] * mix) * g;
    outR[n] = (dryR * (1.0f - mix) + wetR_[n] * mix) * g;
  }

  sampleCounter_ += ctx.numFrames;
}

}  // namespace brainscape
