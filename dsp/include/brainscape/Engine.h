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
  uint32_t historyFrames = 1u << 22;  // power of two in [8, 2^26]; masked indexing
  uint32_t looperFrames  = 0;         // 0 disables the looper subsystem (not yet implemented)
  bool     stereoInput   = true;
  bool     ditherRingWrite = true;    // TPDF dither on the int16 ring write (design §12.3):
                                      // breaks quantization fixed points in the feedback
                                      // loop so the delay decays to true silence. Keyed on
                                      // the sample counter, so renders stay reproducible
                                      // and block-split invariant. Disable for the
                                      // bit-exact Tu null mode (design §10 contract #2).
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

  // Lifecycle. Init does not allocate; it validates the arenas (size AND alignment)
  // against PlanMemory() and clears the history ring ONLY (never looper buffers —
  // design §7). Non-RT: the ring clear is a multi-MiB memset.
  bool Init(const EngineConfig&, const Arenas&) noexcept;

  // Audio thread only (or with Process quiesced). Drains pending parameters and
  // snaps smoothers to their targets; will also kill voices and zero post/feedback
  // state as those subsystems land (design §9). Keeps the history ring intact.
  void Reset() noexcept;

  // Non-RT: re-clears the history ring (multi-MiB memset).
  void ClearHistory() noexcept;
  // Non-RT, explicit gesture only. A plugin's prepare/Init path must NOT call this —
  // it would destroy the user's recorded loop on every host re-prepare (design §7).
  // No-op until the looper subsystem lands.
  void ClearLooper() noexcept;

  struct ProcessContext {
    const float* const* in  = nullptr;  // planar; in[0]=L, in[1]=R (unused if !stereoInput)
    float* const*       out = nullptr;  // planar stereo
    uint32_t numFrames      = 0;        // 1..maxBlockSize, varies freely block to block
    double   tempoBpm       = 120.0;
    int64_t  timelinePos    = 0;
    bool     transportPlaying = false;
  };
  // Audio thread only. No allocation, no locks, no syscalls, no exceptions, no RTTI.
  // On an invalid call (not Init'd, null buffers) the outputs are zero-filled —
  // never left with stale host memory.
  void Process(const ProcessContext&) noexcept;

  // Any thread; lock-free. Non-finite values are mapped to the descriptor minimum
  // (NaN must never reach the smoothers — it is an absorbing state there).
  // sampleOffset is accepted for API stability but the skeleton applies changes at
  // the next Process() start — the sample-accurate SPSC event queue lands with the
  // scheduler (design §9 threading table), at which point the currently hidden
  // "[.pending-spsc]" split-invariance test becomes the acceptance criterion.
  // Known limitation: Mix / Feedback / OutTrim are smoothed per-sample; DelayMs
  // snaps at the block boundary (audible splice on knob moves) until the delay
  // crossfade lands with the grain engine.
  void  SetParam(ParamId id, float plainValue, uint32_t sampleOffset = 0) noexcept;
  float GetParam(ParamId id) const noexcept;  // returns the pending (target) plain value

  // Shared descriptor table (design §9): also available as brainscape::Descriptors().
  static const ParamDescriptor* Descriptors(size_t* count) noexcept;

  // Audio thread only (plain int64: an atomic 8-byte load is not lock-free on
  // Cortex-M7, so cross-thread readers wait for SaveState to land instead).
  // Free-running, advanced by numFrames every Process regardless of transport —
  // the future counter-based RNG key (design §9); also keys the ring-write dither.
  int64_t SampleCounter() const noexcept { return sampleCounter_; }

  // Dry path is never block-delayed (design §2.5).
  uint32_t LatencySamples() const noexcept { return 0; }

 private:
  struct Smoother {
    float value = 0.f, target = 0.f, coef = 1.f;
    void  Prime(float v) noexcept { value = target = v; }
    float Next() noexcept {
      // Snap on stall: the bare one-pole freezes short of its target once the
      // increment rounds to a no-op (~1.4e-5 short at this coefficient — review
      // finding), which would leave Mix=1.0 never exactly 1.0 and break the Tu
      // null on the ordinary SetParam path. Detecting `next == value` catches the
      // stall exactly at any magnitude, since the stall point scales with ULP.
      const float next = value + coef * (target - value);
      value            = (next == value) ? target : next;
      return value;
    }
  };

  void ApplyParam(size_t index, float value) noexcept;

  EngineConfig cfg_{};
  int16_t*     ring_        = nullptr;  // interleaved stereo, historyFrames frames
  uint32_t     mask_        = 0;
  uint32_t     writeFrame_  = 0;
  uint32_t     delayFrames_ = 1;
  Smoother     mix_, outGain_, feedback_;
  int64_t      sampleCounter_ = 0;
  bool         ready_         = false;

  std::atomic<float> pending_[kNumParams]{};
  float              active_[kNumParams]{};
};

}  // namespace brainscape
