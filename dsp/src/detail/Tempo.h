#pragma once
#include <cstdint>

#include "brainscape/PresetState.h"
#include "brainscape/Tempo.h"

// The tempo core (docs/design/clock.md §2, §3, §4.1-§4.2, §6.3): the engine's tempo state, its
// sources and their arbitration, in integer arithmetic only, so it needs no FP environment guard
// and runs from QSPI flash, out of the pedal's ITCM (§9.6). Tempo changes only at event frames
// (§1.4): between them every tempo-derived quantity is a closed-form function of this state, so
// block-split invariance holds by construction.
//
// Frames are absolute engine frames (since Init or Restart), and the core keeps the frame it has
// reached, Frame(): the first frame of the next render span. The engine renders in spans split
// at every event, calls GridFrames once per span, and hands each event to the core at the span
// boundary where it applies (BeforeEvent for every event, ApplyEvent for events 6-10). An entry
// point given a later frame first advances the phasor to it (firing nothing on the way); a frame
// before Frame() is a caller error and applies at Frame().
namespace brainscape {

// One grid position fired in a render span (§6.3): `position` at `frame`.
struct GridHit {
  int64_t frame    = 0;
  int64_t position = 0;
};

namespace tempo {

// P, frames per quarter in unsigned Q32.32, from ns per quarter at the integer rate R (§2.1):
// RoundHalfUp(ns·R·2^32 / 10^9), computed exactly as §2.1 writes it. ns in the tempo range and
// R in 8,000-384,000.
uint64_t PFromNs(uint32_t nsPerQuarter, uint32_t rate) noexcept;
// Back to ns, for Tempo() and displays only: RoundHalfUp(P·10^9 / (R·2^32)).
uint32_t NsFromP(uint64_t p, uint32_t rate) noexcept;
// A tempo-derived duration in exact frames (§2.3): a note value of `noteTicks` 24-ppqn ticks scaled
// by the effective Subdiv's `subdivTicks` (24 at TAP), RoundHalfUp(Pc·noteTicks·subdivTicks /
// (576·2^32)), doubled `octaves` times when positive and halved −`octaves` times when negative
// (§5.3's fold: Pc·2^j in the numerator, 2^k in the denominator). |octaves| ≤ 10.
uint64_t DurationFrames(uint64_t pc, uint32_t noteTicks, uint32_t subdivTicks,
                        int32_t octaves = 0) noexcept;
// §4.1-§4.2: whether an event 6-10's payload is valid. For a valid Start or Locate, *position
// receives its position (0 otherwise). Any other type is not valid.
bool ValidPayload(uint8_t type, uint32_t id, uint32_t valueBits, uint32_t* position) noexcept;

}  // namespace tempo

class TempoCore {
 public:
  static constexpr int64_t  kK               = int64_t{24} << 32;  // one frame in phasor units
  static constexpr uint32_t kMaxClockPerSpan = 4;    // §6.3: three in 512 frames, plus a catch-up
  static constexpr uint32_t kMaxSpanFrames   = 512;  // the engine's largest render span
  static constexpr uint32_t kMinRate         = 8000;
  static constexpr uint32_t kMaxRate         = 384000;
  // The tap (§3.2) and the follower (§3.3, §7.1).
  static constexpr uint32_t kTapIntervals    = 4;    // the mean of up to four intervals
  static constexpr uint32_t kWindowLabels    = 96;   // the fit spans the last 96 labels
  static constexpr uint32_t kRingTicks       = 6;    // the last received ticks
  static constexpr uint32_t kOutlierFloor    = 8;    // N for outliers and dropouts
  static constexpr uint32_t kEarlyTicks      = 12;   // the early commit after a Start
  static constexpr uint32_t kLockTicks       = 24;   // acquisition and the deadband
  static constexpr uint32_t kReacquireRun    = 6;    // outliers of one sign in a row
  static constexpr uint32_t kDriftRun        = 192;  // fitted ticks outside Pc >> 12

  // Init: the integer rate R (§1.3; clamped to 8,000-384,000, which Engine::Init enforces), Init's
  // stored performance state (500,000 µs, Free, TAP), then Restart's state; counters zeroed.
  void Init(uint32_t rate) noexcept;
  // The active preset's stored performance state (§2.5), what Restart plays: an Exact load sets
  // it at load step 4 and then restarts (step 5); SpilloverLoad sets it too. Out-of-range fields,
  // which the decoder refuses, fall back to Init's defaults. reverse and byte 3 are not read.
  void SetStoredPerformance(const PerformanceState& stored) noexcept;
  void SetStoredPerformance(uint32_t usPerQuarter, uint8_t timeMode, uint8_t subdiv) noexcept;
  // Restart (§2.5): frame 0; P = Pc = the stored tempo; the stored time mode and subdivision;
  // boundary 0 at frame 0 with nothing fired; the Internal source, and the follower, the tap
  // chain, the armed transport and the transport cleared to Init's. The counters are kept.
  void Restart() noexcept;

  // Before every event of any type at `frame`, and before a direct Spillover load (§3.5):
  // advances to the frame and applies the gap rule. ApplyEvent, CountUnknownEvent and
  // SpilloverLoad call it themselves; calling it again at the same frame changes nothing.
  void BeforeEvent(int64_t frame) noexcept;
  // An event 6-10 at `frame`, its payload as §4.1 lays it out: an invalid payload is ignored and
  // counted (§4.2) and returns false. A type outside 6-10 is the caller's (CountUnknownEvent).
  bool ApplyEvent(int64_t frame, uint8_t type, uint32_t id, uint32_t valueBits) noexcept;
  // An event type above 10 at `frame` (§4.2): the gap rule, then counted.
  void CountUnknownEvent(int64_t frame) noexcept;
  // A Spillover load at `frame` (§2.5): the gap rule; the package's performance state becomes the
  // stored one and its time mode and subdivision apply; with `recallPreset`
  // (global.tempo_recall Preset) under the Internal source its tempo applies as a Tempo event
  // would, phase-continuous and committed at once. The phasor is never moved.
  void SpilloverLoad(int64_t frame, const PerformanceState& stored, bool recallPreset) noexcept;
  void SpilloverLoad(int64_t frame, uint32_t usPerQuarter, uint8_t timeMode, uint8_t subdiv,
                     bool recallPreset) noexcept;

  // One render span [Frame(), end) (§6.3): first the catch-up (g, the largest grid position at or
  // below the phasor's tick, fires at the span's first frame when it is above lastFired), then
  // each grid position k > max(tick, lastFired) with F(k) < end at F(k); then the phasor advances
  // to `end`. Writes at most `capacity` hits, in order, and returns their count; the engine passes
  // kMaxClockPerSpan, which no span of kMaxSpanFrames or fewer at 8 kHz or above can exceed (a
  // position that finds no room is not fired, and a later span's catch-up may fire it). An empty
  // span fires nothing. The grid runs whatever the mode: whether a hit becomes a birth is the
  // engine's (a mode listing `clock`), and two hits at one frame are its to separate (§6.3's
  // one hit per frame).
  uint32_t GridFrames(int64_t end, GridHit* hits, uint32_t capacity) noexcept;

  // The engine reports the frame a CLOCK hit was born at (after jitter and any deferral, §6.3).
  void NoteClockBirth(int64_t frame) noexcept { lastClockBirth_ = frame; }

  // The snapshot (§2.6): at Frame(), §3.5's gap predicate applied without mutating the state.
  TempoInfo         Info() const noexcept;
  const TempoStats& Stats() const noexcept { return stats_; }
  TempoStats&       MutableStats() noexcept { return stats_; }  // the engine's own counters

  uint32_t           Rate() const noexcept { return rate_; }
  int64_t            Frame() const noexcept { return frame_; }
  uint64_t           P() const noexcept { return p_; }    // drives the grid
  uint64_t           Pc() const noexcept { return pc_; }  // the committed tempo: durations read it
  int64_t            Tick() const noexcept { return tick_; }
  int64_t            Acc() const noexcept { return acc_; }
  int64_t            LastFired() const noexcept { return lastFired_; }
  tempo::ClockSource Source() const noexcept { return source_; }
  uint8_t            TimeMode() const noexcept { return timeMode_; }
  uint8_t            Subdiv() const noexcept { return subdiv_; }
  // §2.4: the stored or live subdivision, except that Tempo time mode forces TAP.
  uint8_t  EffectiveSubdiv() const noexcept;
  uint32_t GridTicks() const noexcept { return tempo::SubdivTicks(EffectiveSubdiv()); }
  // Every change of Pc raises the serial and records its class (§7.1); Init and Restart reset
  // both, the engine rebuilding everything then.
  uint32_t        PcSerial() const noexcept { return pcSerial_; }
  tempo::PcChange LastPcChange() const noexcept { return pcChange_; }
  // F(k) for k ≥ Tick() (§2.2): the first frame at or after boundary k. (k − Tick())·P must stay
  // below 2^63 (k within about 2,000 ticks of Tick()).
  int64_t FrameOfBoundary(int64_t k) const noexcept;
  // The frame at which §3.5's gap falls due (the last tick's frame + R), or false while no tick is
  // pending: for tests that apply gaps at their deadline.
  bool GapDeadline(int64_t* frame) const noexcept {
    *frame = lastTickFrame_ + static_cast<int64_t>(rate_);
    return haveTickRef_;
  }

  // The whole tempo state in a representation-independent form, for the tests' reference models
  // and §8.2's Restart test: labels and frames absolute, the window and ring oldest first, every
  // unused slot zero. The counters are not part of it.
  struct State {
    int64_t  frame = 0, tick = 0, acc = 0;
    uint64_t p = 0, pc = 0;
    int64_t  lastFired = 0, lastGridFrame = 0, lastClockBirth = 0;
    uint8_t  timeMode = 0, subdiv = 0, source = 0;
    uint32_t storedUs = 0;
    uint8_t  storedTimeMode = 0, storedSubdiv = 0;
    uint32_t pcSerial = 0;
    uint8_t  pcChange = 0;
    // Transport (§3.4).
    bool    masterStopped = false, resumeRunning = false, running = false, armed = false;
    uint8_t armedKind = 0;
    int64_t armedPosition = 0, continuePosition = 0;
    // Tap (§3.2).
    bool     haveTap = false;
    int64_t  lastTap = 0;
    uint32_t tapN = 0;
    int64_t  tapIntervals[kTapIntervals] = {};
    // Follower (§3.3).
    uint32_t winN = 0;
    int64_t  winLabel[kWindowLabels] = {}, winFrame[kWindowLabels] = {};
    int64_t  sx = 0, sy = 0, sxx = 0, sxy = 0;
    bool     fitValid = false;
    int64_t  d = 0, a = 0, b = 0;
    uint64_t pFit = 0;
    uint32_t ringN = 0;
    int64_t  ringLabel[kRingTicks] = {}, ringFrame[kRingTicks] = {};
    bool     haveLabel = false;
    int64_t  lastLabel = 0;
    bool     haveTickRef = false;
    int64_t  lastTickFrame = 0;
    uint32_t outlierRun = 0;
    int32_t  outlierSign = 0;
    uint32_t driftRun = 0;
    bool     earlyArmed = false;
  };
  State Capture() const noexcept;

 private:
  // A fitted tick in the window: the low 32 bits of its raw label (its label − labelShift_) and of
  // its frame. Inside the window labels differ by at most 95 and frames by less than 95·R
  // (§3.3), so differences of the low words are exact.
  struct WindowEntry {
    uint32_t label = 0;
    uint32_t frame = 0;
  };
  struct RingEntry {
    int64_t label = 0;  // raw
    int64_t frame = 0;
  };

  void    AdvanceTo(int64_t frame) noexcept;
  void    ApplyGap(int64_t frame) noexcept;
  bool    GapAt(int64_t frame) const noexcept;
  void    ClearFollower() noexcept;
  void    SetPc(uint64_t pc, bool drift) noexcept;
  void    ApplyInternalTempo(uint64_t p) noexcept;
  void    PlaceAnchor(int64_t k, uint32_t offset) noexcept;
  void    PlaceOnLine(int64_t raw) noexcept;
  int64_t NearestTick() const noexcept;
  uint64_t ClampP(uint64_t p) const noexcept;

  void OnTap(int64_t frame) noexcept;
  void ApplyTap(bool downbeat) noexcept;
  void OnTempo(uint32_t ns) noexcept;
  void OnClockTick(int64_t frame) noexcept;
  void OnTransport(uint32_t id, uint32_t position) noexcept;
  void OnSubdivision(uint32_t id) noexcept;
  void Commit(bool entered, uint32_t prevN, bool fitted) noexcept;

  // The window (§3.3).
  void    WindowAdd(int64_t raw, int64_t frame) noexcept;
  void    WindowEvict(int64_t newestRaw) noexcept;
  void    Refit() noexcept;
  void    Reacquire() noexcept;
  int64_t Residual(int64_t raw, int64_t frame) const noexcept;  // ρD (§3.3)
  void    RingPush(int64_t raw, int64_t frame) noexcept;

  // Configuration.
  uint32_t rate_ = 48000;
  uint64_t pMin_ = 0, pMax_ = 0;
  uint32_t storedUs_ = 500000;
  uint8_t  storedTimeMode_ = 0, storedSubdiv_ = 0;

  // The phasor (§2.2) at frame_: 0 < acc_ ≤ p_.
  int64_t  frame_ = 0;
  int64_t  tick_  = -1;
  int64_t  acc_   = 1;
  uint64_t p_     = 1;
  uint64_t pc_    = 1;
  int64_t  lastFired_      = -1;
  int64_t  lastGridFrame_  = -1;
  int64_t  lastClockBirth_ = -1;
  uint32_t        pcSerial_ = 0;
  tempo::PcChange pcChange_ = tempo::PcChange::None;

  // The performance state (§2.4).
  uint8_t timeMode_ = 0;
  uint8_t subdiv_   = 0;

  // Source and transport (§3.1, §3.4).
  tempo::ClockSource   source_           = tempo::ClockSource::Internal;
  bool                 masterStopped_    = false;
  bool                 resumeRunning_    = false;
  bool                 running_          = false;
  bool                 armed_            = false;
  tempo::TransportKind armedKind_        = tempo::TransportKind::Stop;
  int64_t              armedPosition_    = 0;
  int64_t              continuePosition_ = 0;

  // The tap chain (§3.2): intervals oldest first.
  bool     haveTap_ = false;
  int64_t  lastTap_ = 0;
  uint32_t tapN_    = 0;
  int64_t  taps_[kTapIntervals] = {};

  // The follower (§3.3). A label is a raw label plus labelShift_, so a relabel moves every label
  // of the window, the ring and the last tick at once.
  WindowEntry window_[kWindowLabels];
  uint32_t    winHead_ = 0, winN_ = 0;
  int64_t     raw0_ = 0, frame0_ = 0;  // the oldest fitted tick
  int64_t     sx_ = 0, sy_ = 0, sxx_ = 0, sxy_ = 0;
  bool        fitValid_ = false;
  int64_t     d_ = 0, a_ = 0, b_ = 0;
  uint64_t    pFit_ = 0;
  RingEntry   ring_[kRingTicks];
  uint32_t    ringHead_ = 0, ringN_ = 0;
  int64_t     labelShift_ = 0;
  bool        haveLabel_ = false;
  int64_t     lastRaw_ = 0;
  bool        haveTickRef_ = false;  // the dropout and gap reference
  int64_t     lastTickFrame_ = 0;
  uint32_t    outlierRun_ = 0;
  int32_t     outlierSign_ = 0;
  uint32_t    driftRun_ = 0;
  bool        earlyArmed_ = false;

  TempoStats stats_;
};

}  // namespace brainscape
