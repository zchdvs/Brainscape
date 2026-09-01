#pragma once
#include <atomic>
#include <cstdint>

#include "brainscape/Memory.h"
#include "brainscape/Params.h"
#include "brainscape/detail/Granular.h"
#include "brainscape/detail/PostChain.h"
#include "brainscape/detail/Smoother.h"

namespace brainscape {

// Shared build constants (docs/design/grain-engine.md §3): mode files are authored
// against these; they are deliberately NOT EngineConfig fields.
inline constexpr uint32_t kMaxGrains = detail::kGranularMaxGrains;

// The feedback path re-enters the ring through a FIFO of exactly this many frames,
// so the loop period is base_ms + kFeedbackDelayFrames/sr on EVERY target. Sizing
// the FIFO from maxBlockSize instead made a 100 ms preset repeat at 101 ms on the
// pedal and 110.7 ms in a plugin at a 512 buffer (review finding — contract #6).
// Power of two so the slot index is a mask, not a 64-bit modulo (which compiled to
// two __aeabi_uldivmod calls per sample on Cortex-M7).
inline constexpr uint32_t kFeedbackDelayFrames = 512;

struct EngineConfig {
  double   sampleRate    = 48000.0;   // fixed for the Engine's lifetime (design §9);
                                      // rate changes re-run PlanMemory + Init
  uint32_t maxBlockSize  = 512;       // worst case; firmware passes 48, plugin the host max.
                                      // Must be <= kFeedbackDelayFrames (Init enforces) —
                                      // a wrapper facing larger host buffers chunks them.
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

// The granular engine: 64-voice pool + scheduler over the int16 history ring,
// inside the lifecycle / memory / parameter / process contracts of
// docs/design/grain-engine.md §9-§10. The clean delay is the degenerate config
// the design predicts (§5 Pattern A): rectangular window, abutting unity grains.
class Engine {
 public:
  Engine() noexcept = default;
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  // Lifecycle. Init does not allocate; it validates the arenas (size AND alignment)
  // against PlanMemory(), builds the window LUT, and clears the history ring ONLY
  // (never looper buffers — design §7). Non-RT: the ring clear is a multi-MiB memset.
  bool Init(const EngineConfig&, const Arenas&) noexcept;

  // Audio thread only (or with Process quiesced). RT-safe: kills all grain
  // voices, clears the feedback path and small post-chain state, drains pending
  // parameters and snaps smoothers to their targets (design §9). Keeps the
  // history ring AND the large post delay/reverb buffers intact (their stale
  // tails are masked by the mix ramps; ClearHistory does the full non-RT clear).
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
    // Reserved for the CLOCK trigger source (design §4) — not yet read by the engine.
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
  // the next Process() start — the sample-accurate SPSC event queue lands next
  // (design §9 threading table); the hidden "[.pending-spsc]" test is its
  // acceptance criterion. Mix / Feedback / OutTrim / normalization are smoothed
  // per-sample; scheduler and per-grain values apply to grains born after the
  // change (resolve-at-birth — design §6 automation semantics).
  void  SetParam(ParamId id, float plainValue, uint32_t sampleOffset = 0) noexcept;
  float GetParam(ParamId id) const noexcept;  // returns the pending (target) plain value

  // Any thread; applied at the next Process() start. Freeze pins the grain
  // position anchor (design §2.4) — the ring keeps recording, so a freeze held
  // longer than the ring length (~87 s at the default config) is overwritten by
  // wraparound (documented ceiling).
  void SetFreeze(bool on) noexcept { freezePending_.store(on, std::memory_order_relaxed); }
  bool GetFreeze() const noexcept { return freezePending_.load(std::memory_order_relaxed); }

  // Shared descriptor table (design §9): also available as brainscape::Descriptors().
  static const ParamDescriptor* Descriptors(size_t* count) noexcept;

  // Audio thread only (plain int64: an atomic 8-byte load is not lock-free on
  // Cortex-M7, so cross-thread readers wait for SaveState to land instead).
  // Free-running, advanced by numFrames every Process regardless of transport —
  // keys every random draw (design §9) including the ring-write dither.
  int64_t SampleCounter() const noexcept { return sampleCounter_; }

  // Dry path is never block-delayed (design §2.5).
  uint32_t LatencySamples() const noexcept { return 0; }

 private:
  using Smoother = detail::Smoother;

  void ApplyParam(size_t index, float value) noexcept;
  void RebuildGranularParams() noexcept;  // control-rate; runs only when a granular
                                          // param actually changed (keeps exp2/pow
                                          // off the steady-state audio path)
  void RebuildPostParams() noexcept;      // same discipline for the post chain

  EngineConfig cfg_{};
  int16_t*     ring_       = nullptr;  // interleaved stereo, historyFrames frames
  float*       windowLut_  = nullptr;  // Hot arena: kWindowLutSize half-cosine entries
  float*       wetL_       = nullptr;  // Hot arena: maxBlockSize each
  float*       wetR_       = nullptr;
  float*       fbFifo_     = nullptr;  // Warm arena: interleaved stereo,
                                       // kFeedbackDelayFrames frames (NOT maxBlockSize —
                                       // see the constant's rationale above)
  uint32_t     mask_       = 0;
  uint32_t     writeFrame_ = 0;
  Smoother     mix_, outGain_, feedback_, norm_;
  int64_t      sampleCounter_ = 0;
  bool         ready_         = false;
  bool         granularDirty_ = true;
  bool         postDirty_     = true;
  bool         frozen_        = false;
  uint32_t     frozenAnchor_  = 0;

  detail::GranularCore   granular_;
  detail::GranularParams gp_{};
  detail::PostChain      post_;
  detail::PostParams     pp_{};
  detail::FeedbackTamer  tamer_;

  std::atomic<float> pending_[kNumParams]{};
  std::atomic<bool>  freezePending_{false};
  float              active_[kNumParams]{};
};

}  // namespace brainscape
