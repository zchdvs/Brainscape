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

constexpr float kInvScale = 1.0f / 32767.0f;

inline int16_t QuantizeS16(float x) noexcept {
  float s = x * 32767.0f;
  if (s > 32767.0f) s = 32767.0f;
  if (s < -32768.0f) s = -32768.0f;
  return static_cast<int16_t>(std::lrintf(s));
}

inline bool IsPowerOfTwo(uint32_t v) noexcept { return v != 0 && (v & (v - 1)) == 0; }

// Per-sample one-pole coefficient from a wall-clock time constant — never per block,
// which would make trajectories depend on host block size (design §3, contract #1).
inline float OnePoleCoef(float tauMs, double sr) noexcept {
  return 1.0f - std::exp(-1.0f / static_cast<float>(tauMs * 0.001 * sr));
}

}  // namespace

const ParamDescriptor* Descriptors(size_t* count) noexcept {
  if (count != nullptr) *count = kNumParams;
  return kParamTable;
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

  const MemoryPlan plan = PlanMemory(cfg);
  for (size_t t = 0; t < kNumTiers; ++t) {
    if (plan.bytes[t] == 0) continue;
    if (arenas.base[t] == nullptr || arenas.bytes[t] < plan.bytes[t]) return false;
  }

  cfg_        = cfg;
  ring_       = static_cast<int16_t*>(arenas.base[static_cast<size_t>(Tier::Bulk)]);
  mask_       = cfg.historyFrames - 1u;
  writeFrame_ = 0;
  sampleCounter_ = 0;

  // The Bulk arena (SDRAM on hardware) has undefined contents at boot. Clear the
  // history ring here — and only the ring; looper buffers are cleared explicitly by
  // the caller when that subsystem lands (design §7).
  std::memset(ring_, 0, static_cast<size_t>(cfg.historyFrames) * 2u * sizeof(int16_t));

  mix_.coef     = OnePoleCoef(10.0f, cfg.sampleRate);
  outGain_.coef = mix_.coef;

  for (size_t i = 0; i < kNumParams; ++i) {
    pending_[i].store(kParamTable[i].def, std::memory_order_relaxed);
    active_[i] = kParamTable[i].def;
    ApplyParam(i, kParamTable[i].def);
  }
  mix_.Prime(mix_.target);
  outGain_.Prime(outGain_.target);

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
}

void Engine::ApplyParam(size_t index, float value) noexcept {
  switch (kParamTable[index].id) {
    case ParamId::DelayMs: {
      const double frames   = static_cast<double>(value) * 0.001 * cfg_.sampleRate;
      const uint32_t maxTap = cfg_.historyFrames - 4u;
      auto d                = static_cast<uint32_t>(std::lrint(frames));
      if (d < 1u) d = 1u;
      if (d > maxTap) d = maxTap;
      delayFrames_ = d;
      break;
    }
    case ParamId::Mix:
      mix_.target = value;
      break;
    case ParamId::Feedback:
      feedback_ = value;
      break;
    case ParamId::OutTrimDb:
      outGain_.target = std::pow(10.0f, value / 20.0f);
      break;
  }
}

void Engine::SetParam(ParamId id, float value, uint32_t /*sampleOffset*/) noexcept {
  const ParamDescriptor* d = FindParam(id);
  if (d == nullptr) return;
  if (value < d->min) value = d->min;
  if (value > d->max) value = d->max;
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
  if (!ready_ || ctx.in == nullptr || ctx.out == nullptr || ctx.numFrames == 0) return;

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

  const float* inL  = ctx.in[0];
  const float* inR  = cfg_.stereoInput ? ctx.in[1] : ctx.in[0];
  float*       outL = ctx.out[0];
  float*       outR = ctx.out[1];
  const uint32_t d  = delayFrames_;
  const float    fb = feedback_;

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
    // until then feedback.amount is capped at 0.95 by its descriptor.
    ring_[2u * writeFrame_]      = QuantizeS16(dryL + fb * wetL);
    ring_[2u * writeFrame_ + 1u] = QuantizeS16(dryR + fb * wetR);
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
