#pragma once
#include <cstddef>
#include <cstdint>

#include "brainscape/FpProfile.h"
#include "brainscape/Memory.h"
#include "brainscape/Params.h"
#include "brainscape/PresetState.h"

namespace brainscape {

// Shared build constants (docs/design/grain-engine.md §3): mode files are authored
// against these; they are deliberately NOT EngineConfig fields.
inline constexpr uint32_t kMaxGrains = 64;

// The feedback path re-enters the ring through a FIFO of exactly this many frames,
// so the loop period is base_ms + kFeedbackDelayFrames/sr on EVERY target. Sizing
// the FIFO from maxBlockSize instead made a 100 ms preset repeat at 101 ms on the
// pedal and 110.7 ms in a plugin at a 512 buffer (review finding — contract #6).
// Power of two so the slot index is a mask, not a 64-bit modulo (which compiled to
// two __aeabi_uldivmod calls per sample on Cortex-M7).
inline constexpr uint32_t kFeedbackDelayFrames = 512;

// Opaque storage for the engine state (docs/design/determinism-profile.md §3.5): no
// floating-point code may live in a public header, where a consumer's flags would
// compile it, yet the Engine must not allocate and must fit DTCM on the pedal. Sized
// per pointer width (measured 6,032 B on the M7, 6,168 B on x86-64, plus headroom);
// Engine.cpp static_asserts the fit.
inline constexpr size_t kEngineImplBytes = sizeof(void*) == 4 ? 6656 : 6912;
inline constexpr size_t kEngineImplAlign = 16;

struct EngineConfig {
  double   sampleRate    = 48000.0;   // fixed for the Engine's lifetime (design §9);
                                      // rate changes re-run PlanMemory + Init
  uint32_t maxBlockSize  = 512;       // worst case; firmware passes 48, the plugin 512.
                                      // Must be <= kFeedbackDelayFrames (Init enforces) —
                                      // a wrapper facing larger host buffers chunks them.
  uint32_t historyFrames = 1u << 22;  // power of two in [8, 2^26]; masked indexing
  uint32_t looperFrames  = 0;         // 0 disables the looper subsystem (not yet implemented)
  bool     stereoInput   = true;
  bool     ditherRingWrite = true;    // TPDF dither on the int16 ring write (design §12.3):
                                      // breaks quantization fixed points in the feedback
                                      // loop so the delay decays to true silence. Keyed on
                                      // the sample counter (from the random-number epoch),
                                      // so renders stay reproducible and block-split
                                      // invariant. Disable for the
                                      // bit-exact Tu null mode (design §10 contract #2).
};

// An empty plan for a sample rate Init would refuse (outside 8-384 kHz).
MemoryPlan PlanMemory(const EngineConfig&) noexcept;

// The granular engine: 64-voice pool + scheduler over the int16 history ring,
// inside the lifecycle / memory / parameter / process contracts of
// docs/design/grain-engine.md §9-§10. The clean delay is the degenerate config
// the design predicts (§5 Pattern A): rectangular window, abutting unity grains.
//
// Every entry point that runs floating-point code (Init, Reset, Restart, ClearHistory,
// Process, SetParam, LoadPreset, and PlanMemory, Canonicalize and CheckPreset) installs
// the determinism profile's complete FP control word for its duration and restores the
// caller's (docs/design/determinism-profile.md §4.1): round to nearest, gradual underflow.
// Callers need not set flush modes (JUCE's ScopedNoDenormals is redundant here).
// Input must be finite: wrappers pass live input through SanitizeInput
// (InputCondition.h); Debug builds assert that the output stays finite.
class Engine {
 public:
  Engine() noexcept;
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

  // Non-RT, with Process stopped (like Init): returns a running engine to the exact
  // post-Init state, the exact-restart state of the parity contract, but KEEPS the
  // parameter values (docs/design/determinism-profile.md §5.8). Clears the history ring,
  // the post delay and reverb, the feedback FIFO and the tamer's diffusers; kills voices
  // and marks and re-arms the scheduler; resets the onset detector and every filter and
  // LFO; zeroes the sample counter, the random-number epoch, the write position and the
  // onset and trigger counts; turns freeze off; drains pending parameters and snaps the
  // smoothers to them. Reset is the real-time subset: it keeps the counter, the epoch,
  // freeze and every large buffer, so a DAW's reset() maps to it. Restart begins a new
  // timeline at frame 0: events queued against the old one are cleared with it
  // (EventQueue::Clear), and producers stamp from the restarted counter. On an engine that
  // has rendered no frame since Init, Restart or ClearHistory the ring and post buffers are
  // still clear and are not cleared again: such a Restart costs about what Reset does and
  // may run on the audio thread between Process calls (a wrapper's restore or restart
  // before its first block).
  void Restart() noexcept;

  // Applies a decoded preset in the fixed order of determinism profile §5.10, with the
  // per-kind rules of docs/design/mode-compiler.md §4.1 and §7.3: every Leaf row's
  // default; every stored leaf, canonicalized, in ascending id order (an id that is not a
  // Leaf row is unknown and ignored); freeze off; Global rows (device settings) keep their
  // values; then for Exact a Restart (Process stopped, the event queue cleared; non-RT unless
  // the engine has rendered nothing since its buffers were cleared, see Restart), for
  // Spillover the random-number epoch restarted at the load frame, keeping history,
  // grains, scheduler phase and smoothers. A direct Spillover call applies at the next
  // Process call's first frame and must not race Process (audio thread between blocks, or
  // Process stopped); a live load is a SpilloverLoad event instead, applied as one change
  // at its frame, never as per-parameter stores. Returns true when the load applied and
  // was exact (*report says why not).
  bool LoadPreset(const PresetState& preset, LoadMode mode,
                  LoadReport* report = nullptr) noexcept;

  // External trigger sources (design §4/§9): footswitch, MIDI note, sidechain —
  // the guaranteed-working fallback when onset detection can't hear the source.
  enum class TriggerSource : uint8_t { Footswitch = 0, MidiNote = 1, Sidechain = 2 };

  // Frame-stamped events (determinism profile §5.11): (absolute frame, sequence number,
  // type, id, value). Frames before an event's frame use the old state, the event
  // applies from its frame on, and events stamped at one frame apply in sequence order:
  // at a block's first frame that is exactly "SetParam, then Process". So freeze is a
  // level: the last Freeze event at a frame decides it, and a release and a re-engage at
  // one frame keep the pin, as SetFreeze between split blocks does. The numbering is
  // permanent (events are logged and replayed).
  enum class EventType : uint8_t {
    SetParam      = 0,  // id: ParamId; value: the exact binary32 plain value (canonicalized).
                        // Leaf and Global rows only, as SetParam
    Freeze        = 1,  // value: nonzero engages, zero releases
    Trigger       = 2,  // id: TriggerSource; value: velocity (not yet read)
    SpilloverLoad = 3,  // preset: a staged PresetState, read when the event applies. Its
                        // freeze-off is immediate, so a Freeze after it at its frame pins anew
  };
  // A stamped event, as producers, scripts and the transport (EventQueue.h) carry it.
  struct Event {
    int64_t            frame  = 0;  // absolute engine frame: frames since Init or Restart
    uint32_t           seq    = 0;  // orders the events stamped at one frame
    EventType          type   = EventType::SetParam;
    uint32_t           id     = 0;
    float              value  = 0.f;
    const PresetState* preset = nullptr;  // SpilloverLoad: valid until the event is retired
                                          // (EventQueue::Retired)
  };
  // The same event, stamped with its offset in the block that Process renders.
  struct BlockEvent {
    uint32_t           offset = 0;  // frames from the block's first frame
    uint32_t           seq    = 0;
    EventType          type   = EventType::SetParam;
    uint32_t           id     = 0;
    float              value  = 0.f;
    const PresetState* preset = nullptr;
  };

  struct ProcessContext {
    const float* const* in  = nullptr;  // planar; in[0]=L, in[1]=R (unused if !stereoInput)
    float* const*       out = nullptr;  // planar stereo
    uint32_t numFrames      = 0;        // 1..maxBlockSize, varies freely block to block
    // The block's events in the order they apply: offsets (0..numFrames-1) never fall, and
    // at one offset the sequence numbers rise, except at offset 0, where the transport
    // also puts late events in stamp order. Process splits the block at each event's
    // offset, so its output equals a wrapper that splits the block there and applies the
    // events between the parts. An event out of order applies where it is reached, and
    // an offset past the block after the block's last frame: both are caller errors,
    // asserted in Debug, and never dropped. A call that renders nothing (a zero-frame or
    // oversized block, null buffers) still applies its events, after its frames.
    const BlockEvent* events    = nullptr;
    uint32_t          numEvents = 0;
    // Reserved for the CLOCK trigger source (design §4) — not yet read by the engine.
    double   tempoBpm       = 120.0;
    int64_t  timelinePos    = 0;
    bool     transportPlaying = false;
  };
  // Audio thread only. No allocation, no locks, no syscalls, no exceptions, no RTTI.
  // On an invalid call (not Init'd, null buffers, a block size outside 1..maxBlockSize)
  // the outputs are zero-filled — never left with stale host memory.
  void Process(const ProcessContext&) noexcept;

  // SetParam, SetFreeze and Trigger are the unstamped path: they apply at the first
  // frame of the next Process call, before that block's events, and are kept for
  // callers that split blocks themselves. Sample-accurate changes are events.
  //
  // Any thread; lock-free. Stores Canonicalize(id, plainValue) (Params.h) for a Leaf or
  // Global row: NaN and ±inf become the descriptor minimum, ±0 and subnormals +0, then the
  // clamp. Any other row (Macro, Performance, Reserved, Retired) or unknown id is a no-op
  // (mode-compiler.md §4.1). sampleOffset is ignored (a SetParam event carries the offset).
  // Mix / Feedback / trim / normalization are smoothed per-sample; scheduler and per-grain
  // values apply to grains born after the change (resolve-at-birth — design §6 automation
  // semantics). Restart keeps every stored value; a preset load keeps Global rows.
  void  SetParam(ParamId id, float plainValue, uint32_t sampleOffset = 0) noexcept;
  // The pending (target) plain value of a Leaf or Global row; 0 for any other id.
  float GetParam(ParamId id) const noexcept;

  // Any thread; applied at the next Process() start. Freeze pins the grain
  // position anchor (design §2.4) — the ring keeps recording, so a freeze held
  // longer than the ring length (~87 s at the default config) is overwritten by
  // wraparound (documented ceiling).
  void SetFreeze(bool on) noexcept;
  bool GetFreeze() const noexcept;

  // Any thread; lock-free. Fires a grain (oldest-steal; a surplus beyond one
  // block's frames carries to the next block — explicit triggers never drop).
  // Triggers due at one frame fire on consecutive frames, one birth per frame.
  // src and velocity are not yet read (source routing and velocity land with the
  // mode system); sampleOffset is ignored (a Trigger event carries the offset). A
  // SIDECHAIN audio input is not yet expressible through ProcessContext at all
  // (reserved, like tempoBpm).
  void Trigger(TriggerSource src = TriggerSource::Footswitch, float velocity = 1.f,
               uint32_t sampleOffset = 0) noexcept;

  // Any thread; atomic exchange(0). Counts onsets since the last call — counted,
  // not boolean, so fast passages stay individually visible on the trigger LED
  // (design §9; the LED driver stretches each to a visible minimum).
  uint32_t ConsumeOnsetCount() noexcept;

  // Shared descriptor table (design §9): also available as brainscape::Descriptors().
  static const ParamDescriptor* Descriptors(size_t* count) noexcept;

  // Audio thread only (plain int64: an atomic 8-byte load is not lock-free on
  // Cortex-M7, so cross-thread readers wait for SaveState to land instead).
  // Free-running from Init or Restart, advanced by numFrames every Process regardless
  // of transport: the absolute frame of event stamps, grain lifetimes, mark ages, the
  // feedback FIFO slot and the onset hop grid.
  int64_t SampleCounter() const noexcept;
  // Audio thread only. The random-number epoch (determinism profile §5.9): every random
  // draw (design §9), the ring-write dither included, is keyed on SampleCounter() minus
  // this frame. Init and Restart set it to 0, a Spillover load to its load frame.
  int64_t EpochStart() const noexcept;

  // Dry path is never block-delayed (design §2.5).
  uint32_t LatencySamples() const noexcept { return 0; }

 private:
  struct Impl;
  Impl&       impl() noexcept;
  const Impl& impl() const noexcept;

  alignas(kEngineImplAlign) unsigned char impl_[kEngineImplBytes];
};

}  // namespace brainscape
