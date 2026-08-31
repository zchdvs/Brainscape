#pragma once
#include <atomic>
#include <cstdint>

#include "brainscape/Memory.h"
#include "brainscape/Params.h"

namespace brainscape {

// Shared build constants (docs/design/grain-engine.md §3): mode files are authored
// against these; they are deliberately NOT EngineConfig fields.
inline constexpr uint32_t kMaxGrains = 64;

struct EngineConfig {
  double   sampleRate    = 48000.0;   // fixed for the Engine's lifetime (design §9);
                                      // rate changes re-run PlanMemory + Init
  uint32_t maxBlockSize  = 512;       // worst case; firmware passes 48, plugin the host max
  uint32_t historyFrames = 1u << 22;  // power of two; masked indexing
  uint32_t looperFrames  = 0;         // 0 disables the looper subsystem (not yet implemented)
  bool     stereoInput   = true;
};

MemoryPlan PlanMemory(const EngineConfig&) noexcept;

// Walking-skeleton Engine: the history ring (int16 interleaved stereo, Bulk arena)
// plus one unity-rate tap — the design doc's `Tu` degenerate case, a clean delay —
// inside the exact lifecycle / memory / parameter / process contracts the grain
// engine will grow into. See docs/design/grain-engine.md §9-§10.
class Engine {
 public:
  Engine() noexcept = default;
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  // Lifecycle. Init does not allocate; it validates the arenas against PlanMemory()
  // and clears the history ring ONLY (never looper buffers — design §7). Non-RT.
  bool Init(const EngineConfig&, const Arenas&) noexcept;

  // RT-safe: drains pending parameters, snaps smoothers to their targets.
  // Keeps the history ring intact.
  void Reset() noexcept;

  struct ProcessContext {
    const float* const* in  = nullptr;  // planar; in[0]=L, in[1]=R (unused if !stereoInput)
    float* const*       out = nullptr;  // planar stereo
    uint32_t numFrames      = 0;        // 1..maxBlockSize, varies freely block to block
    double   tempoBpm       = 120.0;
    int64_t  timelinePos    = 0;
    bool     transportPlaying = false;
  };
  // Audio thread only. No allocation, no locks, no syscalls, no exceptions, no RTTI.
  void Process(const ProcessContext&) noexcept;

  // Any thread; lock-free. sampleOffset is accepted for API stability but the
  // skeleton applies changes at the next Process() start — the sample-accurate
  // SPSC event queue lands with the scheduler (design §9 threading table).
  void  SetParam(ParamId id, float plainValue, uint32_t sampleOffset = 0) noexcept;
  float GetParam(ParamId id) const noexcept;  // returns the pending (target) plain value

  // Free-running, advanced by numFrames every Process regardless of transport —
  // the future counter-based RNG key (design §9); saved with state.
  int64_t SampleCounter() const noexcept { return sampleCounter_; }

  // Dry path is never block-delayed (design §2.5).
  uint32_t LatencySamples() const noexcept { return 0; }

 private:
  struct Smoother {
    float value = 0.f, target = 0.f, coef = 1.f;
    void  Prime(float v) noexcept { value = target = v; }
    float Next() noexcept {
      value += coef * (target - value);
      return value;
    }
  };

  void ApplyParam(size_t index, float value) noexcept;

  EngineConfig cfg_{};
  int16_t*     ring_        = nullptr;  // interleaved stereo, historyFrames frames
  uint32_t     mask_        = 0;
  uint32_t     writeFrame_  = 0;
  uint32_t     delayFrames_ = 1;
  float        feedback_    = 0.f;
  Smoother     mix_, outGain_;
  int64_t      sampleCounter_ = 0;
  bool         ready_         = false;

  std::atomic<float> pending_[kNumParams]{};
  float              active_[kNumParams]{};
};

}  // namespace brainscape
