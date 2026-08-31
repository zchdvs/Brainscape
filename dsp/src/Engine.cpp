#include "brainscape/Engine.h"

#include <cassert>
#include <cmath>
#include <cstring>

#include "brainscape/DenormalGuard.h"

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

// Per-sample one-pole coefficient from a wall-clock time constant — never per block,
// which would make trajectories depend on host block size (design §3, contract #1).
// expm1, not 1-exp: the subtraction cancels to ~18 mantissa bits and a 1-ulp libm
// difference would eat most of contract #7's cross-build budget (review finding).
inline float OnePoleCoef(float tauMs, double sr) noexcept {
  return -static_cast<float>(std::expm1(-1.0 / (tauMs * 0.001 * sr)));
}

// SplitMix32 — the skeleton's counter-based hash (design §9: RNG keyed on the
// free-running sample counter, independent of transport, split-invariant).
inline uint32_t Hash32(uint32_t x) noexcept {
  x += 0x9E3779B9u;
  x ^= x >> 16;
  x *= 0x21F0AAADu;
  x ^= x >> 15;
  x *= 0x735A2D97u;
  x ^= x >> 15;
  return x;
}

// ±1 LSB TPDF dither in the float domain (design §12.3). Applied to the ring write
// so int16 rounding has no fixed points in the feedback loop — without it a single
// impulse leaves a permanent tone (measured −70 dBFS at fb 0.95; review finding).
inline float Tpdf(uint32_t key) noexcept {
  const float u1 = static_cast<float>(Hash32(key) >> 8) * (1.0f / 16777216.0f);
  const float u2 = static_cast<float>(Hash32(key ^ 0x6A09E667u) >> 8) * (1.0f / 16777216.0f);
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
  // Hot (DTCM-class): grain pool, LUTs, accumulator — lands with the grain engine.
  plan.bytes[static_cast<size_t>(Tier::Hot)] = 0;
  plan.align[static_cast<size_t>(Tier::Hot)] = 16;
  // Warm (AXI-class): reverb tank, onset detector — lands with the post chain.
  plan.bytes[static_cast<size_t>(Tier::Warm)] = 0;
  plan.align[static_cast<size_t>(Tier::Warm)] = 16;
  // Bulk (SDRAM-class): history ring now; looper A+B when the looper lands.
  // Frame counts are bounded by Init (<= 2^26), so these products cannot overflow
  // a 32-bit size_t on the embedded target.
  plan.bytes[static_cast<size_t>(Tier::Bulk)] =
      static_cast<size_t>(cfg.historyFrames) * 2u * sizeof(int16_t) +
      static_cast<size_t>(cfg.looperFrames) * 2u * sizeof(int16_t) * 2u;
  // Cache-line aligned: the SD/DMA coherency rule needs 32-byte-aligned ranges (design §7).
  plan.align[static_cast<size_t>(Tier::Bulk)] = 32;
  return plan;
}

bool Engine::Init(const EngineConfig& cfg, const Arenas& arenas) noexcept {
  ready_ = false;
  if (cfg.sampleRate <= 0.0 || cfg.maxBlockSize == 0) return false;
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

  cfg_           = cfg;
  ring_          = static_cast<int16_t*>(arenas.base[static_cast<size_t>(Tier::Bulk)]);
  mask_          = cfg.historyFrames - 1u;
  writeFrame_    = 0;
  sampleCounter_ = 0;

  // The Bulk arena (SDRAM on hardware) has undefined contents at boot. Clear the
  // history ring here — and only the ring; looper buffers are cleared explicitly by
  // the caller when that subsystem lands (design §7).
  std::memset(ring_, 0, static_cast<size_t>(cfg.historyFrames) * 2u * sizeof(int16_t));

  const float coef = OnePoleCoef(10.0f, cfg.sampleRate);
  mix_.coef        = coef;
  outGain_.coef    = coef;
  feedback_.coef   = coef;

  for (size_t i = 0; i < kNumParams; ++i) {
    pending_[i].store(kParamTable[i].def, std::memory_order_relaxed);
    active_[i] = kParamTable[i].def;
    ApplyParam(i, kParamTable[i].def);
  }
  mix_.Prime(mix_.target);
  outGain_.Prime(outGain_.target);
  feedback_.Prime(feedback_.target);

  ready_ = true;
  return true;
}

void Engine::Reset() noexcept {
  if (!ready_) return;
  for (size_t i = 0; i < kNumParams; ++i) {
    const float p = pending_[i].load(std::memory_order_relaxed);
    active_[i]    = p;
    ApplyParam(i, p);
  }
  mix_.Prime(mix_.target);
  outGain_.Prime(outGain_.target);
  feedback_.Prime(feedback_.target);
}

void Engine::ClearHistory() noexcept {
  if (!ready_) return;
  std::memset(ring_, 0, static_cast<size_t>(cfg_.historyFrames) * 2u * sizeof(int16_t));
}

void Engine::ClearLooper() noexcept {
  // No looper buffers yet. When they land, this stays an explicit user gesture —
  // never called from a plugin prepare path (design §7).
}

void Engine::ApplyParam(size_t index, float value) noexcept {
  switch (kParamTable[index].id) {
    case ParamId::DelayMs: {
      // std::lround, not lrint: half-away-from-zero regardless of the dynamic
      // rounding mode, so the tap count is identical across builds (contract #6).
      const double frames = static_cast<double>(value) * 0.001 * cfg_.sampleRate;
      // Read-before-write makes historyFrames-1 the exact upper bound for the
      // unity tap; the grain engine's §3 guard table replaces this. Lower bound
      // wins if they ever conflict (Init floors historyFrames at 8).
      // TODO(design §3): enforce d_min_fb (~5 ms) when feedback > 0 once the
      // taming chain lands — a near-zero delay with feedback is a high-Q comb.
      const uint32_t maxTap = cfg_.historyFrames - 1u;
      auto d                = static_cast<uint32_t>(std::lround(frames));
      if (d > maxTap) d = maxTap;
      if (d < 1u) d = 1u;
      delayFrames_ = d;
      break;
    }
    case ParamId::Mix:
      mix_.target = value;
      break;
    case ParamId::Feedback:
      feedback_.target = value;
      break;
    case ParamId::OutTrimDb:
      // exp2f, not powf (cheaper, and the pattern to copy is LUT+lerp when
      // SemitonesToRatio lands — schedule-time transcendentals are charged in §8).
      outGain_.target = std::exp2(value * 0.16609640474436813f);  // dB -> linear
      break;
  }
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
  assert(ready_);
  assert(ctx.numFrames >= 1 && ctx.numFrames <= cfg_.maxBlockSize);
  if (!ready_ || ctx.in == nullptr || ctx.out == nullptr || ctx.numFrames == 0) {
    // Never hand back stale host memory: an Init failure must sound like silence,
    // not like uninitialized buffers at full volume (review finding).
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
  // with the scheduler).
  for (size_t i = 0; i < kNumParams; ++i) {
    const float p = pending_[i].load(std::memory_order_relaxed);
    if (p != active_[i]) {
      active_[i] = p;
      ApplyParam(i, p);
    }
  }

  const float* inL    = ctx.in[0];
  const float* inR    = cfg_.stereoInput ? ctx.in[1] : ctx.in[0];
  float*       outL   = ctx.out[0];
  float*       outR   = ctx.out[1];
  const uint32_t d    = delayFrames_;
  const bool   dither = cfg_.ditherRingWrite;

  for (uint32_t n = 0; n < ctx.numFrames; ++n) {
    const float dryL = inL[n];
    const float dryR = inR[n];

    // Read the tap before writing this frame: with d >= 1 the read frame is always
    // strictly older than the write frame, so an impulse at frame 0 emerges at
    // exactly frame d.
    const uint32_t readFrame = (writeFrame_ - d) & mask_;
    const float wetL = static_cast<float>(ring_[2u * readFrame]) * kInvScale;
    const float wetR = static_cast<float>(ring_[2u * readFrame + 1u]) * kInvScale;

    // Feedback topology (A): wet summed into the record path (design §2.3).
    // The taming chain (DC/HP/LP/saturator/diffuser) lands with the grain engine;
    // until then feedback.amount is capped below unity by its descriptor.
    const float fb = feedback_.Next();
    float wrL      = dryL + fb * wetL;
    float wrR      = dryR + fb * wetR;
    if (dither) {
      // Gated on non-zero so true silence stays bit-zero (no idle noise floor):
      // the write value performs a downward random walk absorbed at 0 instead of
      // latching on a quantization fixed point.
      const auto key = static_cast<uint32_t>(sampleCounter_ + n) * 2u;
      if (wrL != 0.0f) wrL += Tpdf(key);
      if (wrR != 0.0f) wrR += Tpdf(key + 1u);
    }
    ring_[2u * writeFrame_]      = QuantizeS16(wrL);
    ring_[2u * writeFrame_ + 1u] = QuantizeS16(wrR);
    writeFrame_ = (writeFrame_ + 1u) & mask_;

    const float mix = mix_.Next();
    const float g   = outGain_.Next();
    // Linear wet/dry crossfade (grain-delay-theory.md §3.11); dry is never delayed.
    // Two-multiply form, not dry + mix*(wet-dry): the lerp form is not bit-exact at
    // the endpoints, which would break the Tu null contract (design §10 #2).
    outL[n] = (dryL * (1.0f - mix) + wetL * mix) * g;
    outR[n] = (dryR * (1.0f - mix) + wetR * mix) * g;
  }

  sampleCounter_ += ctx.numFrames;
}

}  // namespace brainscape
