#pragma once
#include <cstdint>

#include "detail/GrainMath.h"

// Internal granular core (docs/design/grain-engine.md §3-§4). The pool lives inside
// the Engine's opaque storage (which firmware places in DTCM); not part of the
// public API surface and free to change.
namespace brainscape::detail {

inline constexpr uint32_t kGranularMaxGrains = 64;
inline constexpr uint32_t kHiFiGrains        = 8;    // T0: cubic Hermite
inline constexpr uint32_t kWindowLutSize     = 4096;
inline constexpr double   kGuardMarginFrames = 64.0;
// The largest legal block. Pass 1 writes a whole block into the ring before grains
// render, so up to kBlockWriteAheadFrames - 1 frames past the live write head hold
// audio a smaller block would not have written yet. The far rail keeps every read
// out of that window — the condition for block-split invariance (contract #1).
// Engine.cpp static_asserts it covers kFeedbackDelayFrames, which bounds
// maxBlockSize.
inline constexpr uint32_t kBlockWriteAheadFrames = 512;
// A FastCut load's fade, in frames (Engine.cpp checks it equals kFastCutFrames), and a grain's
// fadeStart while it is not fading.
inline constexpr uint32_t kGranularFastCutFrames = 128;
inline constexpr uint32_t kGranularMaxPitch      = 8;  // a pitch set's entries (Mode.h's cap)
inline constexpr uint32_t kNoFade                = 0xFFFFFFFFu;

// Everything a grain needs, resolved once at birth (design §3: resolve-once-at-
// schedule-time; grains never re-read a global parameter).
struct Grain {
  uint64_t pos;        // absolute ring position, 32.32 fixed point (frame << 32 | frac)
  int64_t  inc;        // signed 32.32 increment — pitch AND direction
  int64_t  endAbs;     // absolute engine sample where the grain finishes
  uint32_t rendered;   // output frames rendered so far
  uint32_t total;      // output frames in the grain; a FastCut shortens it to its fade's end
  uint32_t fadeStart;  // the output frame at which a FastCut load began fading the grain
                       // (mode-compiler.md §7.3), or kNoFade
  grainmath::EnvSpec env;
  float    smoothness;  // piecewise->LUT window morph
  float    gainL, gainR;
  uint8_t  tier;       // resolved at birth: 0 = cubic Hermite, 1 = linear
  bool     active;
  bool     unity;      // inc == +1.0 exactly: bit-exact integer read path (Tu)
};
static_assert(sizeof(Grain) <= 128, "grain pool must stay inside the DTCM budget (design §3)");

// Block-rate parameters, resolved from the drained plain values once per block.
struct GranularParams {
  double   baseDelayFrames;  // layer0.position.base_ms in frames
  float    sprayFrames;
  uint32_t totalFrames;      // rounded grain length — spacing and voices derive from
                             // this ONE integer (spacing from the unrounded float
                             // opened whole-grain duty-cycle holes; review finding)
  float    targetVoices;     // effective target: min(kMaxGrains*overlap^3, totalFrames),
                             // since the 1-frame interval floor caps sustainable voices
  float    jitter;           // 0 = periodic, 1 = Poisson inter-arrival
  // Layer 0's pitch set (mode-compiler.md §7.5, R10; sound revision 5): each entry's
  // semitones plus layer0.pitch.transpose_st, before detune and the ±24 st clamp; the weights
  // and their sum; the selection. The default set {0: 1} gives 0 + transpose, which is the
  // transpose bit for bit (sound revision 1's pitch).
  float    pitchSt[kGranularMaxPitch];
  uint16_t pitchWeight[kGranularMaxPitch];  // 1-16
  uint32_t pitchCount;                      // 1-8
  uint32_t pitchWeightSum;
  bool     pitchRandom;                     // `random` selection, else `cycle`
  float    spreadCents;
  float    reverseProb;
  float    sustain, skew, smoothness;
  float    panSpread;
  // From the active mode's structure (mode-compiler.md §7.3), not from leaves since sound
  // revision 2 retired rows 27 and 28 into it:
  bool     onsetTrigger;     // `onset` in scheduler.sources, OR'd with the free-running
                             // scheduler: each detected onset fires a burst (oldest-steal —
                             // explicit triggers never drop, design §4)
  bool     posFromMark;      // layer 0's position.source is `mark`: grains read from the most
                             // recent onset mark (the Strum family's mechanism) instead of
                             // the live position
  // Trigger sources and the scheduler's modifiers (mode-compiler.md §7.5, R9; sound revision 4):
  bool     periodic;         // `periodic` in scheduler.sources: the free-running scheduler runs,
                             // with its one-voice floor; without it only triggers give births
  float    intermittency;    // scheduler.intermittency: a draw below it skips a periodic birth
                             // (which still consumes its interval) or a whole trigger
  uint32_t burstCount;       // scheduler.burst.count, read as an integer (1-16): grains a
                             // trigger births
  uint32_t burstSpacing;     // frames between a burst's grains: max(1, round(spacing_ms * 48))
};

// What the scheduler has done (Engine::Stats): counts since Init, kept by Reset and Restart.
struct GranularStats {
  uint64_t births      = 0;  // grains born, from every source
  uint64_t burstBirths = 0;  // ... of them a burst's second and later grains
  uint64_t skips       = 0;  // periodic births and triggers that intermittency skipped
};

// The same-frame ordinals of the intermittency draws (mode-compiler.md §7.5, R8's key
// extension): a periodic birth, an onset and a manual trigger can each be decided at one frame,
// and their draws must not be one draw.
inline constexpr uint32_t kOrdinalPeriodic = 0;
inline constexpr uint32_t kOrdinalOnset    = 1;
inline constexpr uint32_t kOrdinalManual   = 2;

// External trigger events for one block, collected by the Engine (onset detector,
// manual/MIDI triggers). Offsets are block-relative sample indices, ascending.
struct TriggerEvents {
  static constexpr uint32_t kMaxOnsets = 8;  // > maxBlockSize/kOnsetHop + slack
  uint32_t onsetOffset[kMaxOnsets];
  uint32_t onsetMarkFrame[kMaxOnsets];  // ring frame where the onset's audio starts
  uint32_t onsetCount   = 0;
  uint32_t manualCount  = 0;  // fired at offsets 0,1,2,... (consecutive so the
                              // counter-keyed draws stay distinct per grain)
};

class GranularCore {
 public:
  void Init(const int16_t* ring, uint32_t mask, const float* windowLut) noexcept {
    ring_ = ring;
    mask_ = mask;
    lut_  = windowLut;
    Reset();
  }

  // Kills all voices and re-arms the scheduler to fire on the next sample.
  void Reset() noexcept {
    for (auto& g : grains_) g.active = false;
    orderCount_ = 0;
    markCount_  = 0;
    markHead_   = 0;
    // 1.0, not 0: the per-sample decrement runs before the fire check, so an
    // initial 0 leaves a -1 residual in the phasor and every subsequent birth
    // lands one sample early — which breaks exact grain abutment.
    intervalRemaining_ = 1.0f;
    ResetSequencing();
  }

  // The sequencing state a load of a different mode resets and a load of the same mode keeps
  // (mode-compiler.md §7.3): the bursts in progress (sound revision 4) and the pitch cycle's
  // position (5).
  void ResetSequencing() noexcept {
    burstCount_ = 0;
    burstDue_   = kNoBurstDue;
    pitchCycle_ = grainmath::PitchCycle{};
  }

  // Counts since Init (Engine::Stats); Reset and Restart keep them.
  const GranularStats& Stats() const noexcept { return stats_; }
  void                 ClearStats() noexcept { stats_ = GranularStats{}; }

  // A FastCut load at absolute frame `abs` (mode-compiler.md §7.3): every grain still sounding
  // there that is not already fading starts a linear fade to zero over kFastCutFrames from
  // that frame, and its life ends with the fade. Every grain is rendered up to `abs` (the
  // engine splits its blocks at events), so each fades from where it is. Several loads within
  // the fade each fade their own grains.
  void FastCut(int64_t abs) noexcept;

  // Schedules and renders one block. wetL/wetR are overwritten (not accumulated).
  // drawEpoch: random draws are keyed on absSample - drawEpoch (determinism profile
  // §5.9); lifetimes and mark ages stay absolute.
  // ringFrameAtBlockStart: the ring frame where absSample's input is written.
  // frozen/frozenAnchor: design §2.4 — positions resolve against the pinned
  // anchor instead of the advancing write position, and POS_MARK uses only marks
  // at or before the pin; the ring keeps recording, so the write-head guards stay
  // relative to the live head. *frozenAnchor is in/out: re-anchor-on-wrap
  // updates it per sample.
  void Process(const GranularParams& p, const TriggerEvents& ev, int64_t absSample,
               int64_t drawEpoch, uint32_t ringFrameAtBlockStart, bool frozen,
               uint32_t* frozenAnchor, uint32_t numFrames, float* wetL, float* wetR) noexcept;

 private:
  struct Mark {
    int64_t  abs;
    uint32_t frame;
  };
  static constexpr uint32_t kMaxMarks = 16;

  // A trigger's burst in progress (mode-compiler.md §7.5): its second and later grains.
  struct Burst {
    int64_t  next;       // absolute frame its next grain is due
    uint32_t spacing;    // frames between its grains
    uint32_t remaining;  // grains still to fire
  };
  // Bursts kept at once; a trigger past them drops the oldest. They overlap only when triggers
  // come faster than a burst lasts, (count - 1) * spacing frames (7.5 s at most).
  static constexpr uint32_t kMaxBursts  = 8;
  static constexpr int64_t  kNoBurstDue = INT64_MAX;

  // anchorFrame: the position reference (the pin while frozen).
  // liveFrame: ring frame Pass 1 wrote at birthAbs — the write-head guard reference.
  void ScheduleGrain(uint32_t slot, const GranularParams& p, int64_t birthAbs,
                     uint32_t anchorFrame, uint32_t liveFrame) noexcept;
  // Fire an explicit trigger: free slot if available, else steal the OLDEST voice
  // (design §4 allocation policy — explicit triggers never drop a hit).
  void FireExternal(const GranularParams& p, int64_t birthAbs, uint32_t anchorFrame,
                    uint32_t liveFrame, uint32_t* renderedTo, uint32_t n,
                    int64_t absSample, float* wetL, float* wetR) noexcept;
  // A trigger (an onset or a manual trigger, `ordinal` saying which) at birthAbs: skipped by
  // intermittency, or its burst's first grain now and the rest queued. True when it fired.
  bool FireTrigger(const GranularParams& p, uint32_t ordinal, int64_t birthAbs,
                   uint32_t anchorFrame, uint32_t liveFrame, uint32_t* renderedTo, uint32_t n,
                   int64_t absSample, float* wetL, float* wetR) noexcept;
  // The oldest due burst's next grain: at most one a frame, and none at a frame a trigger
  // already fired at, so no two of the frame's births share the frame's draws.
  void FireBurst(const GranularParams& p, int64_t birthAbs, uint32_t anchorFrame,
                 uint32_t liveFrame, uint32_t* renderedTo, uint32_t n, int64_t absSample,
                 float* wetL, float* wetR) noexcept;
  void AddBurst(int64_t next, uint32_t spacing, uint32_t remaining) noexcept;
  // The pitch-set entry a birth plays (mode-compiler.md §7.5, R10): the next of the cycle, or a
  // weighted draw at the birth's key.
  uint32_t PickPitch(const GranularParams& p, int64_t drawKey) noexcept;
  void UpdateBurstDue() noexcept;
  // Renders every live voice over [from, to) in BIRTH order (the canonical
  // per-sample summation order — see Process), retiring finished grains.
  void RenderSpan(uint32_t from, uint32_t to, int64_t absSample, float* wetL,
                  float* wetR) noexcept;
  // One grain over block frames [s, e), from its current read position and frame count;
  // kFade multiplies its envelope by a FastCut's linear fade (only frames past fadeStart).
  template <bool kFade>
  void RenderRun(Grain& g, uint32_t s, uint32_t e, float* wetL, float* wetR) noexcept;

  const int16_t* ring_ = nullptr;
  const float*   lut_  = nullptr;
  uint32_t       mask_ = 0;
  uint32_t       blockRingStart_ = 0;  // ring frame of the block's first sample; read
                                       // only by the Debug write-ahead assertion
  int64_t        drawEpoch_      = 0;  // this block's random-number epoch
  float          intervalRemaining_ = 1.0f;  // frames until the next scheduled birth
  Grain          grains_[kGranularMaxGrains]{};
  uint8_t        order_[kGranularMaxGrains]{};  // slot indices in ascending birth order
  uint32_t       orderCount_ = 0;
  Mark           marks_[kMaxMarks]{};  // recent onset marks (ring of kMaxMarks)
  uint32_t       markHead_  = 0;
  uint32_t       markCount_ = 0;
  Burst          bursts_[kMaxBursts]{};   // in progress, oldest first
  uint32_t       burstCount_ = 0;
  int64_t        burstDue_   = kNoBurstDue;  // the earliest `next` among them
  grainmath::PitchCycle pitchCycle_;  // `cycle` selection's position, over every birth
  GranularStats  stats_;
};

}  // namespace brainscape::detail
