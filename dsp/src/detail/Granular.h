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

// Everything a grain needs, resolved once at birth (design §3: resolve-once-at-
// schedule-time; grains never re-read a global parameter).
struct Grain {
  uint64_t pos;        // absolute ring position, 32.32 fixed point (frame << 32 | frac)
  int64_t  inc;        // signed 32.32 increment — pitch AND direction
  int64_t  endAbs;     // absolute engine sample where the grain finishes
  uint32_t rendered;   // output frames rendered so far
  uint32_t total;      // output frames in the grain
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
  float    ratioBase;        // from layer0.pitch.st (semitones)
  float    spreadCents;
  float    reverseProb;
  float    sustain, skew, smoothness;
  float    panSpread;
  bool     onsetTrigger;     // OR'd ONSET source: each detected onset fires a grain
                             // (oldest-steal — explicit triggers never drop, design §4)
  bool     posFromMark;      // POS_MARK: grains read from the most recent onset mark
                             // (the Strum family's mechanism) instead of POS_LIVE
};

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
  }

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

  // anchorFrame: the position reference (the pin while frozen).
  // liveFrame: ring frame Pass 1 wrote at birthAbs — the write-head guard reference.
  void ScheduleGrain(uint32_t slot, const GranularParams& p, int64_t birthAbs,
                     uint32_t anchorFrame, uint32_t liveFrame) noexcept;
  // Fire an explicit trigger: free slot if available, else steal the OLDEST voice
  // (design §4 allocation policy — explicit triggers never drop a hit).
  void FireExternal(const GranularParams& p, int64_t birthAbs, uint32_t anchorFrame,
                    uint32_t liveFrame, uint32_t* renderedTo, uint32_t n,
                    int64_t absSample, float* wetL, float* wetR) noexcept;
  // Renders every live voice over [from, to) in BIRTH order (the canonical
  // per-sample summation order — see Process), retiring finished grains.
  void RenderSpan(uint32_t from, uint32_t to, int64_t absSample, float* wetL,
                  float* wetR) noexcept;

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
};

}  // namespace brainscape::detail
