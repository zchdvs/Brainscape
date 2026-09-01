#pragma once
#include <cstdint>

#include "brainscape/GrainMath.h"

// Internal granular core (docs/design/grain-engine.md §3-§4). Included by
// Engine.h so the pool can live inside the Engine object (which firmware places
// in DTCM); not part of the public API surface and free to change.
namespace brainscape::detail {

inline constexpr uint32_t kGranularMaxGrains = 64;
inline constexpr uint32_t kHiFiGrains        = 8;    // T0: cubic Hermite
inline constexpr uint32_t kWindowLutSize     = 4096;
inline constexpr double   kGuardMarginFrames = 64.0;

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
    // 1.0, not 0: the per-sample decrement runs before the fire check, so an
    // initial 0 leaves a -1 residual in the phasor and every subsequent birth
    // lands one sample early — which breaks exact grain abutment.
    intervalRemaining_ = 1.0f;
  }

  // Schedules and renders one block. wetL/wetR are overwritten (not accumulated).
  // ringFrameAtBlockStart: the ring frame where absSample's input is written.
  // frozen/frozenAnchor: design §2.4 — positions resolve against the pinned
  // anchor instead of the advancing write position; the ring keeps recording.
  void Process(const GranularParams& p, int64_t absSample, uint32_t ringFrameAtBlockStart,
               bool frozen, uint32_t frozenAnchor, uint32_t numFrames, float* wetL,
               float* wetR) noexcept;

 private:
  void ScheduleGrain(uint32_t slot, const GranularParams& p, int64_t birthAbs,
                     uint32_t anchorFrame) noexcept;
  // Renders every live voice over [from, to) in BIRTH order (the canonical
  // per-sample summation order — see Process), retiring finished grains.
  void RenderSpan(uint32_t from, uint32_t to, int64_t absSample, float* wetL,
                  float* wetR) noexcept;

  const int16_t* ring_ = nullptr;
  const float*   lut_  = nullptr;
  uint32_t       mask_ = 0;
  float          intervalRemaining_ = 1.0f;  // frames until the next scheduled birth
  Grain          grains_[kGranularMaxGrains]{};
  uint8_t        order_[kGranularMaxGrains]{};  // slot indices in ascending birth order
  uint32_t       orderCount_ = 0;
};

}  // namespace brainscape::detail
