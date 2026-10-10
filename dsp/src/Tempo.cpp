#include "detail/Tempo.h"

#include <cassert>
#include <cstdint>
#include <new>

#include "detail/IntMath.h"

// Integer-only (docs/design/clock.md §1.4 principle 2): no floating-point arithmetic, so no FP
// guard; compiled -mgeneral-regs-only on the Cortex-M7 and kept out of the pedal's ITCM (§9.6,
// firmware/CMakeLists.txt). Section numbers below are clock.md's.
namespace brainscape {

using intmath::CeilDivI64;
using intmath::FloorDivI64;
using intmath::FloorModI64;
using intmath::MulDivRoundI64;
using intmath::MulDivRoundU64;
using tempo::ClockSource;
using tempo::PcChange;
using tempo::TransportKind;

namespace {

constexpr uint64_t kNsPerSecond = 1000000000u;
constexpr uint64_t kTwo32       = 1ull << 32;
// GridFrames splits a longer span at this many frames, so (frames)·K stays below 2^63.
constexpr int64_t kMaxGridSpan = int64_t{1} << 24;

int64_t Abs64(int64_t v) noexcept { return v < 0 ? -v : v; }
uint64_t AbsDiff(uint64_t a, uint64_t b) noexcept { return a > b ? a - b : b - a; }

bool ValidStored(uint32_t us, uint8_t timeMode, uint8_t subdiv) noexcept {
  return us >= kMinUsPerQuarter && us <= kMaxUsPerQuarter && timeMode < tempo::kTimeModeCodes &&
         subdiv < tempo::kSubdivCodes;
}

}  // namespace

// --- §2.1 units ----------------------------------------------------------------------------------

namespace tempo {

uint64_t PFromNs(uint32_t nsPerQuarter, uint32_t rate) noexcept {
  // ns·R ≤ 3·10^9 · 384,000 < 1.2·10^15 and r·2^32 < 4.3·10^18: both fit uint64_t.
  const uint64_t x = static_cast<uint64_t>(nsPerQuarter) * rate;
  const uint64_t q = x / kNsPerSecond, r = x % kNsPerSecond;
  return (q << 32) + ((r << 32) + kNsPerSecond / 2) / kNsPerSecond;
}

uint32_t NsFromP(uint64_t p, uint32_t rate) noexcept {
  if (rate == 0) return 0;
  const uint64_t ns = MulDivRoundU64(p, kNsPerSecond, static_cast<uint64_t>(rate) << 32);
  return ns > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(ns);
}

uint64_t DurationFrames(uint64_t pc, uint32_t noteTicks, uint32_t subdivTicks,
                        int32_t octaves) noexcept {
  assert(octaves >= -10 && octaves <= 10);
  if (octaves < -10) octaves = -10;
  if (octaves > 10) octaves = 10;
  const uint64_t ts = static_cast<uint64_t>(noteTicks) * subdivTicks;  // ≤ 192·96 = 18,432
  const uint64_t den = 576ull << 32;
  if (octaves >= 0) return MulDivRoundU64(pc << octaves, ts, den);  // Pc < 2^53: Pc·2^10 fits
  return MulDivRoundU64(pc, ts, den << -octaves);                   // 576·2^42 < 2^64
}

bool ValidPayload(uint8_t type, uint32_t id, uint32_t valueBits, uint32_t* position) noexcept {
  *position = 0;
  switch (type) {
    case kEventTap:
    case kEventClockTick:
      return id == 0 && valueBits == 0;
    case kEventTempo:
      return id >= kMinNsPerQuarter && id <= kMaxNsPerQuarter && valueBits == 0;
    case kEventTransport: {
      if ((id & kTransportReservedMask) != 0) return false;
      const auto     kind   = static_cast<TransportKind>(id & kTransportKindMask);
      const bool     atNext = (id & kTransportAtNextTick) != 0;
      const uint32_t offset = id >> kTransportOffsetShift;
      if (offset != 0 && (atNext || kind == TransportKind::Stop)) return false;
      if (kind == TransportKind::Start || kind == TransportKind::Locate)
        return DecodePositionBits(valueBits, position);
      return valueBits == 0;
    }
    case kEventSubdivision: {
      if ((id >> 16) != 0 || valueBits != 0) return false;
      const uint32_t code = id & 0xFFu, field = (id >> 8) & 0xFFu;
      if (field == static_cast<uint32_t>(SubdivField::Subdivision)) return code < kSubdivCodes;
      if (field == static_cast<uint32_t>(SubdivField::TimeMode)) return code < kTimeModeCodes;
      return false;
    }
    default:
      return false;
  }
}

}  // namespace tempo

// --- §2.5 lifecycle ------------------------------------------------------------------------------

void TempoCore::Init(uint32_t rate) noexcept {
  assert(rate >= kMinRate && rate <= kMaxRate);
  rate_ = rate < kMinRate ? kMinRate : (rate > kMaxRate ? kMaxRate : rate);
  pMin_ = tempo::PFromNs(tempo::kMinNsPerQuarter, rate_);
  pMax_ = tempo::PFromNs(tempo::kMaxNsPerQuarter, rate_);
  storedUs_       = 500000;
  storedTimeMode_ = 0;
  storedSubdiv_   = tempo::kSubdivTap;
  stats_          = TempoStats();
  lastClockBirth_ = -1;
  Restart();
}

TempoCore* TempoCore::Create(void* mem, uint32_t rate) noexcept {
  TempoCore* core = ::new (mem) TempoCore();
  core->Init(rate);
  return core;
}

void TempoCore::SetStoredPerformance(const PerformanceState& stored) noexcept {
  SetStoredPerformance(stored.usPerQuarter, static_cast<uint8_t>(stored.timeMode),
                       static_cast<uint8_t>(stored.subdiv));
}

void TempoCore::SetStoredPerformance(uint32_t usPerQuarter, uint8_t timeMode,
                                     uint8_t subdiv) noexcept {
  if (!ValidStored(usPerQuarter, timeMode, subdiv)) {
    usPerQuarter = 500000;
    timeMode     = 0;
    subdiv       = tempo::kSubdivTap;
  }
  storedUs_       = usPerQuarter;
  storedTimeMode_ = timeMode;
  storedSubdiv_   = subdiv;
}

void TempoCore::Restart() noexcept {
  p_ = pc_ = tempo::PFromNs(storedUs_ * 1000u, rate_);  // µs ≤ 3,000,000: ns fits uint32_t
  nsPerQuarter_ = tempo::NsFromP(pc_, rate_);
  timeMode_ = storedTimeMode_;
  subdiv_   = storedSubdiv_;
  frame_    = 0;
  tick_     = -1;  // boundary 0 at frame 0
  acc_      = static_cast<int64_t>(p_);
  lastFired_      = -1;
  lastGridFrame_  = -1;
  lastClockBirth_ = -1;
  pcSerial_       = 0;
  pcChange_       = PcChange::None;

  source_           = ClockSource::Internal;
  masterStopped_    = false;
  resumeRunning_    = false;
  running_          = false;
  armed_            = false;
  armedKind_        = TransportKind::Stop;
  armedPosition_    = 0;
  continuePosition_ = 0;

  haveTap_ = false;
  lastTap_ = 0;
  tapN_    = 0;
  for (int64_t& t : taps_) t = 0;

  ClearFollower();
  labelShift_ = 0;
  haveLabel_  = false;
  lastRaw_    = 0;
  Settle();
}

uint8_t TempoCore::EffectiveSubdiv() const noexcept {
  return timeMode_ == tempo::kTimeModeTempo ? tempo::kSubdivTap : subdiv_;
}

void TempoCore::Settle() noexcept {
  // Every change ends here (an event, a load, Restart): the fast path of GridFrames waits for
  // the full path to bound it again, and the grid's period follows P and G.
  quietUntil_ = kNoQuiet;
  const uint32_t G = GridTicks();
  if (p_ != gridP_ || G != gridG_) {
    gridP_ = p_;
    gridG_ = G;
    // P ≤ the tempo range's largest at R, so the period is at most 96 · 1,152,000 / 24 frames.
    const uint64_t f = MulDivRoundU64(p_, G, static_cast<uint64_t>(kK));
    gridPeriod_ = f > 0xFFFFFFu ? 0xFFFFFFu : static_cast<uint32_t>(f);
  }
}

// --- §2.2 the phasor -----------------------------------------------------------------------------

void TempoCore::AdvanceTo(int64_t frame) noexcept {
  if (frame <= frame_) return;
  const int64_t p = static_cast<int64_t>(p_);
  int64_t       n = frame - frame_;
  while (n > 0) {
    const int64_t step = n < kMaxGridSpan ? n : kMaxGridSpan;
    acc_ += step * kK;
    if (acc_ > p) {
      // Euclidean division: acc_ back into (0, P], whatever the split (§2.2).
      if (acc_ - p <= p) {
        acc_ -= p;
        ++tick_;
      } else {
        const int64_t m = (acc_ - 1) / p;  // acc_ > 0, p > 0
        acc_ -= m * p;
        tick_ += m;
      }
    }
    n -= step;
  }
  frame_ = frame;
}

int64_t TempoCore::FrameOfBoundary(int64_t k) const noexcept {
  return frame_ + CeilDivI64((k - tick_) * static_cast<int64_t>(p_) - acc_, kK);
}

int64_t TempoCore::NearestTick() const noexcept {
  // Ties go up (§3.2, §3.3): 2·acc ≤ 2P < 2^54.
  return 2 * acc_ >= static_cast<int64_t>(p_) ? tick_ + 1 : tick_;
}

uint64_t TempoCore::ClampP(uint64_t p) const noexcept {
  return p < pMin_ ? pMin_ : (p > pMax_ ? pMax_ : p);
}

void TempoCore::PlaceAnchor(int64_t k, uint32_t offset) noexcept {
  // Boundary k at frame_ + offset (§2.2): tick = k − 1 − m, acc = (m + 1)·P − offset·K with m
  // the P's that the design's loop adds back, floor(offset·K / P), so 0 < acc ≤ P.
  const int64_t p  = static_cast<int64_t>(p_);
  const int64_t oK = static_cast<int64_t>(offset) * kK;  // < 2^16 · 2^37
  const int64_t m  = oK / p;
  tick_ = k - 1 - m;
  acc_  = (m + 1) * p - oK;
}

void TempoCore::SetPc(uint64_t pc, bool drift) noexcept {
  if (pc == pc_) return;
  const uint64_t diff = AbsDiff(pc, pc_);
  PcChange       c;
  if (diff > (pc_ >> 5)) {
    c = PcChange::Jump;
    ++stats_.jumps;
  } else if (drift) {
    c = PcChange::Drift;
    ++stats_.slews;
  } else {
    c = PcChange::Step;
  }
  pc_           = pc;
  pcChange_     = c;
  nsPerQuarter_ = tempo::NsFromP(pc_, rate_);  // Info() is read every block (§9.3, §9.5)
  ++pcSerial_;
}

void TempoCore::ApplyInternalTempo(uint64_t p) noexcept {
  // Phase-continuous (§2.2): the fraction of the current tick is kept.
  if (p != p_) {
    const uint64_t a = MulDivRoundU64(static_cast<uint64_t>(acc_), p, p_);  // ≤ P′
    acc_ = a < 1 ? 1 : static_cast<int64_t>(a);
    p_   = p;
  }
  SetPc(p, false);
}

// --- §6.3 the grid -------------------------------------------------------------------------------

uint32_t TempoCore::GridFramesFull(int64_t end, GridHit* hits, uint32_t capacity) noexcept {
  uint32_t n = 0;
  while (frame_ < end) {
    const int64_t s = frame_;
    const int64_t e = end - s > kMaxGridSpan ? s + kMaxGridSpan : end;
    const int64_t G = static_cast<int64_t>(GridTicks());
    const int64_t p = static_cast<int64_t>(p_);
    if (armed_) {
      // The grid is held while a MIDI transport waits for its tick (§3.4, as-built note 26):
      // nothing fires, and every grid position whose first frame lies before e counts as
      // fired. The last position with F(k) < e is tick, unless its boundary lies inside the
      // frame before e (acc < K), which makes F(tick) = e.
      AdvanceTo(e);
      const int64_t last = acc_ >= kK ? tick_ : tick_ - 1;
      const int64_t g    = FloorDivI64(last, G) * G;
      if (g > lastFired_) lastFired_ = g;
      continue;
    }
    // 1. The catch-up: the largest grid position at or below tick, when it has not fired.
    const int64_t g = FloorDivI64(tick_, G) * G;
    if (g > lastFired_ && n < capacity) {
      hits[n].frame    = s;
      hits[n].position = g;
      ++n;
      lastFired_     = g;
      lastGridFrame_ = s;
    }
    // 2. Each grid position after max(tick, lastFired) whose first frame is before e. F(k) < e
    // exactly when (k − tick)·P − acc ≤ (e − s − 1)·K, i.e. k − tick ≤ maxSteps; checking that
    // first also keeps the product below 2^63.
    const int64_t maxSteps = ((e - s - 1) * kK + acc_) / p;
    int64_t       k = (FloorDivI64(tick_ > lastFired_ ? tick_ : lastFired_, G) + 1) * G;
    while (k - tick_ <= maxSteps && n < capacity) {
      const int64_t f  = s + CeilDivI64((k - tick_) * p - acc_, kK);
      hits[n].frame    = f;
      hits[n].position = k;
      ++n;
      lastFired_     = k;
      lastGridFrame_ = f;
      k += G;
    }
    AdvanceTo(e);
  }
  UpdateQuiet();
  return n;
}

void TempoCore::UpdateQuiet() noexcept {
  // The first frame at which a grid position can fire, for GridFrames' fast path: with no
  // catch-up due at frame_ (no grid position at or below tick above lastFired) and the grid not
  // held, the next position to fire is the first grid position above max(tick, lastFired), and
  // nothing fires before its first frame F(k). Otherwise the full path runs next time.
  quietUntil_ = kNoQuiet;
  if (armed_) return;
  const int64_t G = static_cast<int64_t>(GridTicks());
  if (FloorDivI64(tick_, G) * G > lastFired_) return;
  const int64_t k = (FloorDivI64(tick_ > lastFired_ ? tick_ : lastFired_, G) + 1) * G;
  if (k - tick_ > kQuietTicks) return;
  quietUntil_ = FrameOfBoundary(k);
}

void TempoCore::OnGridChange(int64_t oldG) noexcept {
  // A change of the effective grid G (a Subdivision event, a load's stored Subdiv or time mode)
  // starts the new grid at its next position: the new grid's positions whose first frames lie
  // before this frame count as fired, and only one due exactly here (F(g) = frame_: g = tick
  // with acc < K, §2.2) still fires, by the catch-up. §6.3's catch-up is for a jump of the
  // phasor; a grid that changes under a still phasor would otherwise fire the finer grid's last
  // passed position here, up to a whole new-grid period late (as-built note 25).
  const int64_t G = static_cast<int64_t>(GridTicks());
  if (G == oldG) return;
  const int64_t g = FloorDivI64(tick_, G) * G;
  if (g > lastFired_ && (g < tick_ || acc_ >= kK)) lastFired_ = g;
}

// --- §3.5 gaps -----------------------------------------------------------------------------------

void TempoCore::ClearFollower() noexcept {
  winHead_ = winN_ = 0;
  raw0_ = frame0_ = 0;
  sx_ = sy_ = sxx_ = sxy_ = 0;
  fitValid_ = false;
  d_ = a_ = b_ = 0;
  pFit_ = 0;
  ringHead_ = ringN_ = 0;
  haveTickRef_   = false;
  lastTickFrame_ = 0;
  outlierRun_    = 0;
  outlierSign_   = 0;
  driftRun_      = 0;
  earlyArmed_    = false;
}

void TempoCore::ApplyGap(int64_t frame) noexcept {
  if (!GapAt(frame)) return;
  ClearFollower();  // the window, the ring and the dropout reference: a fresh fit next
  ++stats_.gaps;
  if (source_ != ClockSource::Internal) {
    ++stats_.losses;  // P, Pc and the phase kept
    if (source_ == ClockSource::ClockRunning) {
      resumeRunning_ = true;
      running_       = false;
    }
    source_ = ClockSource::Internal;
  }
}

void TempoCore::BeforeEventFull(int64_t frame) noexcept {
  assert(frame >= frame_ && "TempoCore: frames never fall");
  AdvanceTo(frame);
  ApplyGap(frame_);
}

// --- events --------------------------------------------------------------------------------------

bool TempoCore::ApplyEvent(int64_t frame, uint8_t type, uint32_t id, uint32_t valueBits) noexcept {
  BeforeEvent(frame);
  assert(type >= tempo::kEventTap && type <= tempo::kEventLast);
  uint32_t position = 0;
  if (!tempo::ValidPayload(type, id, valueBits, &position)) {
    if (type >= tempo::kEventTap && type <= tempo::kEventLast) ++stats_.invalidEvents;
    return false;
  }
  switch (type) {
    case tempo::kEventTap:
      OnTap(frame_);
      break;
    case tempo::kEventTempo:
      OnTempo(id);
      break;
    case tempo::kEventClockTick:
      OnClockTick(frame_);
      break;
    case tempo::kEventTransport:
      OnTransport(id, position);
      break;
    case tempo::kEventSubdivision:
      OnSubdivision(id);
      break;
    default:
      break;
  }
  Settle();
  return true;
}

void TempoCore::CountUnknownEvent(int64_t frame) noexcept {
  BeforeEvent(frame);
  ++stats_.unknownEvents;
}

void TempoCore::SpilloverLoad(int64_t frame, const PerformanceState& stored,
                              bool recallPreset) noexcept {
  SpilloverLoad(frame, stored.usPerQuarter, static_cast<uint8_t>(stored.timeMode),
                static_cast<uint8_t>(stored.subdiv), recallPreset);
}

void TempoCore::SpilloverLoad(int64_t frame, uint32_t usPerQuarter, uint8_t timeMode,
                              uint8_t subdiv, bool recallPreset) noexcept {
  BeforeEvent(frame);
  const int64_t oldG = static_cast<int64_t>(GridTicks());
  SetStoredPerformance(usPerQuarter, timeMode, subdiv);
  timeMode_ = storedTimeMode_;
  subdiv_   = storedSubdiv_;
  // Under clock the clock wins (§3.6); the source is read after the gap rule.
  if (recallPreset && source_ == ClockSource::Internal)
    ApplyInternalTempo(tempo::PFromNs(storedUs_ * 1000u, rate_));
  // The new grid from its next position, after the recalled tempo has rescaled the phasor (a
  // position the rescale makes due here still fires, as after a Tempo event).
  OnGridChange(oldG);
  Settle();
}

void TempoCore::OnTempo(uint32_t ns) noexcept {
  if (source_ != ClockSource::Internal) {
    ++stats_.tempoIgnored;
    return;
  }
  ++stats_.tempoEvents;
  ApplyInternalTempo(tempo::PFromNs(ns, rate_));
}

void TempoCore::OnSubdivision(uint32_t id) noexcept {
  ++stats_.subdivEvents;
  const int64_t oldG = static_cast<int64_t>(GridTicks());
  const auto    code = static_cast<uint8_t>(id & 0xFFu);
  if (((id >> 8) & 0xFFu) == static_cast<uint32_t>(tempo::SubdivField::TimeMode))
    timeMode_ = code;
  else
    subdiv_ = code;
  OnGridChange(oldG);
}

// --- §3.2 tap ------------------------------------------------------------------------------------

void TempoCore::OnTap(int64_t f) noexcept {
  ++stats_.taps;
  if (source_ == ClockSource::ClockRunning) {  // the master owns tempo and position
    ++stats_.tapsIgnored;
    return;
  }
  if (source_ == ClockSource::ClockFree) {
    // A phase tap: the tick nearest f becomes the nearest multiple of 96. Labels move, times do
    // not; lastFired moves with them, so no hit repeats.
    ++stats_.tapPhases;
    const int64_t kn    = NearestTick();
    const int64_t delta = 96 * FloorDivI64(kn + 48, 96) - kn;
    labelShift_ += delta;
    tick_ += delta;
    lastFired_ += delta;
    return;
  }
  if (!haveTap_) {  // the first tap arms the chain
    haveTap_ = true;
    lastTap_ = f;
    tapN_    = 0;
    return;
  }
  const int64_t I = f - lastTap_;
  const int64_t R = static_cast<int64_t>(rate_);
  if (5 * I < R) {  // a bounce, or above 300 BPM: the last tap stays
    ++stats_.tapsIgnored;
    return;
  }
  int64_t sum = 0;
  for (uint32_t i = 0; i < tapN_; ++i) sum += taps_[i];
  const int64_t n = static_cast<int64_t>(tapN_);
  if (I > 3 * R || (n > 0 && 4 * n * I >= 7 * sum)) {  // a pause: a new chain, armed at f
    lastTap_ = f;
    tapN_    = 0;
    return;
  }
  const bool downbeat = tapN_ == 0;  // the first apply of a chain after an arm (D16)
  if (n > 0 && 5 * Abs64(n * I - sum) > 2 * sum) {  // more than 40 % from the mean
    taps_[0] = I;
    tapN_    = 1;
  } else if (tapN_ < kTapIntervals) {
    taps_[tapN_++] = I;
  } else {
    for (uint32_t i = 1; i < kTapIntervals; ++i) taps_[i - 1] = taps_[i];
    taps_[kTapIntervals - 1] = I;
  }
  lastTap_ = f;
  ApplyTap(downbeat);
}

void TempoCore::ApplyTap(bool downbeat) noexcept {
  const int64_t p = static_cast<int64_t>(p_);
  // 1. The nearest beat, in the old units: x is the phasor's offset past beat 24b.
  const int64_t b = FloorDivI64(tick_, 24);
  const int64_t x = (tick_ - 24 * b) * p + acc_;  // ≤ 24·P < 2^58
  int64_t       B = 2 * x >= 24 * p ? 24 * (b + 1) : 24 * b;
  // 2. The downbeat: the first tap of the chain, I frames back, becomes beat one of a bar, so
  // this one is the bar's second beat: the smallest B' ≥ B with B' ≡ 24 (mod 96).
  if (downbeat) B += FloorModI64(24 - B, 96);
  // 3. The tempo, committed at once: RoundHalfUp((Σ << 32) / n), Σ < 2^23.
  int64_t sum = 0;
  for (uint32_t i = 0; i < tapN_; ++i) sum += taps_[i];
  const uint64_t np = ClampP(MulDivRoundU64(static_cast<uint64_t>(sum), kTwo32, tapN_));
  p_ = np;
  SetPc(np, false);
  // 4. Boundary B at f; a forward jump skips the positions it passed, B fires at f unless it has.
  tick_ = B - 1;
  acc_  = static_cast<int64_t>(np);
  if (lastFired_ < B - 1) lastFired_ = B - 1;
}

// --- §3.4 transport ------------------------------------------------------------------------------

void TempoCore::OnTransport(uint32_t id, uint32_t position) noexcept {
  const auto     kind   = static_cast<TransportKind>(id & tempo::kTransportKindMask);
  const bool     atNext = (id & tempo::kTransportAtNextTick) != 0;
  const uint32_t offset = id >> tempo::kTransportOffsetShift;
  if (atNext) {  // MIDI's kind
    ++stats_.transports;
    switch (kind) {
      case TransportKind::Stop:
        if (armed_) {
          armed_ = false;
          ++stats_.transportsCancelled;
        }
        source_ = ClockSource::Internal;  // phase and tempo kept
        if (haveLabel_) continuePosition_ = lastRaw_ + labelShift_ + 1;
        masterStopped_ = true;
        resumeRunning_ = false;
        running_       = false;
        earlyArmed_    = false;
        break;
      case TransportKind::Start:
      case TransportKind::Continue:
        armed_         = true;  // a later armed event replaces an earlier one
        armedKind_     = kind;
        armedPosition_ = position;
        break;
      case TransportKind::Locate:
        // The continue position either way, so a Continue that replaces an armed Locate before
        // its tick resumes from the Song Position (§3.4: Continue resumes "from the last stop
        // point or Song Position"; as-built note 27).
        continuePosition_ = position;
        if (source_ == ClockSource::ClockRunning) {
          armed_         = true;
          armedKind_     = kind;
          armedPosition_ = position;
        }
        break;
    }
    return;
  }
  // The host's kind: two transports cannot both own the position.
  if (source_ != ClockSource::Internal) {
    ++stats_.transportsIgnored;
    return;
  }
  ++stats_.transports;
  switch (kind) {
    case TransportKind::Stop:
      running_ = false;  // nothing else changes; the grid runs on
      break;
    case TransportKind::Start:
    case TransportKind::Continue:
    case TransportKind::Locate: {
      const int64_t k = kind == TransportKind::Continue ? continuePosition_
                                                        : static_cast<int64_t>(position);
      PlaceAnchor(k, offset);
      lastFired_ = k - 1;
      if (kind != TransportKind::Locate) running_ = true;
      break;
    }
  }
}

// --- §3.3 the follower ---------------------------------------------------------------------------

void TempoCore::WindowAdd(int64_t raw, int64_t frame) noexcept {
  if (winN_ == 0) {
    winHead_ = 0;
    raw0_    = raw;
    frame0_  = frame;
    sx_ = sy_ = sxx_ = sxy_ = 0;
  } else {
    const int64_t x = raw - raw0_;    // 1-95: eviction ran first
    const int64_t y = frame - frame0_;  // below 95·R
    sx_ += x;
    sy_ += y;
    sxx_ += x * x;
    sxy_ += x * y;
  }
  const uint32_t at = winHead_ + winN_;  // below 2·96
  WindowEntry&   e  = window_[at >= kWindowLabels ? at - kWindowLabels : at];
  e.label = static_cast<uint32_t>(static_cast<uint64_t>(raw));
  e.frame = static_cast<uint32_t>(static_cast<uint64_t>(frame));
  ++winN_;
}

void TempoCore::WindowEvict(int64_t newestRaw) noexcept {
  // The fitted ticks whose labels are among the last 96 stay: label > newest − 96 (§3.3).
  const int64_t cut = newestRaw - static_cast<int64_t>(kWindowLabels);  // labels ≤ cut leave
  if (winN_ == 0 || raw0_ > cut) return;
  const WindowEntry& base = window_[winHead_];
  uint32_t           last = winHead_ + winN_ - 1u;
  if (last >= kWindowLabels) last -= kWindowLabels;
  if (raw0_ + static_cast<int32_t>(window_[last].label - base.label) <= cut) {
    winN_ = 0;  // every one of them leaves (a dropout, or a relabel past the window)
    sx_ = sy_ = sxx_ = sxy_ = 0;
    return;
  }
  // The leaving ticks' own terms come out of the sums relative to the current base, the oldest,
  // whose own terms are 0; then the sums re-base once onto the oldest that stays, δ labels and η
  // frames later, from the survivors' sums (§3.3's formulas). The sums are the same exact
  // integers as one re-base per leaving tick gave, at two products per tick instead of six, so a
  // dropout tick that evicts most of the window stays cheap (as-built note 30).
  const uint32_t baseLabel = base.label, baseFrame = base.frame;
  const int64_t  last0     = cut - raw0_;  // an entry δ labels past the base leaves when δ ≤ it
  int64_t        sx = sx_, sy = sy_, sxx = sxx_, sxy = sxy_;
  uint32_t       i    = winHead_;
  uint32_t       gone = 0;
  int64_t        dl   = 0;
  int64_t        df   = 0;
  for (;;) {
    ++gone;
    if (++i == kWindowLabels) i = 0;
    dl = static_cast<int32_t>(window_[i].label - baseLabel);  // exact: below 2^31
    df = static_cast<int32_t>(window_[i].frame - baseFrame);
    if (dl > last0) break;  // the new oldest
    sx -= dl;
    sy -= df;
    sxx -= dl * dl;
    sxy -= dl * df;
  }
  winHead_ = i;
  winN_ -= gone;
  const int64_t N = winN_;
  sx_  = sx - N * dl;
  sy_  = sy - N * df;
  sxx_ = sxx - 2 * dl * sx + N * dl * dl;
  sxy_ = sxy - df * sx - dl * sy + N * dl * df;
  raw0_ += dl;
  frame0_ += df;
}

void TempoCore::Refit() noexcept {
  if (winN_ < 2) {
    fitValid_ = false;
    d_ = a_ = b_ = 0;
    pFit_ = 0;
    return;
  }
  const int64_t N = winN_;
  d_ = N * sxx_ - sx_ * sx_;
  a_ = N * sxy_ - sx_ * sy_;
  b_ = sy_ * sxx_ - sx_ * sxy_;
  fitValid_ = d_ > 0 && a_ > 0;
  pFit_ = fitValid_ ? ClampP(MulDivRoundU64(static_cast<uint64_t>(24 * a_), kTwo32,
                                            static_cast<uint64_t>(d_)))
                    : 0;
}

int64_t TempoCore::Residual(int64_t raw, int64_t frame) const noexcept {
  // ρD(k, f) = (f − frame₀)·D − B − A·(k − label₀) = (f − T̂(k))·D, exact (|ρD| < 2^55).
  return (frame - frame0_) * d_ - b_ - a_ * (raw - raw0_);
}

void TempoCore::RingPush(int64_t raw, int64_t frame) noexcept {
  RingEntry& e = ring_[(ringHead_ + ringN_) % kRingTicks];
  e.label = raw;
  e.frame = frame;
  if (ringN_ < kRingTicks) ++ringN_;
  else ringHead_ = (ringHead_ + 1) % kRingTicks;
}

void TempoCore::Reacquire() noexcept {
  winHead_ = winN_ = 0;
  for (uint32_t i = 0; i < ringN_; ++i) {
    const RingEntry& e = ring_[(ringHead_ + i) % kRingTicks];
    WindowAdd(e.label, e.frame);
  }
}

void TempoCore::PlaceOnLine(int64_t raw) noexcept {
  const int64_t kL = raw + labelShift_;
  if (!fitValid_) {  // N < 2 or no slope: the tick's own frame is its boundary, P unchanged
    tick_ = kL - 1;
    acc_  = static_cast<int64_t>(p_);
    return;
  }
  // d: the signed offset of f past T̂(k_L), in phasor units.
  const int64_t pf = static_cast<int64_t>(pFit_);
  const int64_t d  = MulDivRoundI64(kK, Residual(raw, frame_), d_);
  const int64_t j  = CeilDivI64(d, pf) - 1;
  tick_ = kL + j;
  acc_  = d - j * pf;  // 0 < acc ≤ P_fit
  p_    = pFit_;
}

void TempoCore::OnClockTick(int64_t f) noexcept {
  ++stats_.ticks;
  const uint32_t prevN    = winN_;
  const bool     wasEmpty = winN_ == 0;
  // 1. The label: one per received tick; a dropout inferred only from a valid fit of N ≥ 8, when
  // the gap is at least four ticks and 100 ms, through the clamped P_fit (never by a slope that
  // can be zero).
  int64_t raw;
  if (haveLabel_) {
    raw = lastRaw_ + 1;
    if (fitValid_ && winN_ >= kOutlierFloor && haveTickRef_) {
      const int64_t gap = f - lastTickFrame_;  // below R: the gap rule ran
      if (gap * kK >= 4 * static_cast<int64_t>(pFit_) &&
          10 * gap >= static_cast<int64_t>(rate_)) {
        const int64_t m = static_cast<int64_t>(
                              MulDivRoundU64(static_cast<uint64_t>(gap), kK, pFit_)) - 1;
        raw += m;
        stats_.dropoutTicks += static_cast<uint64_t>(m);
      }
    }
  } else {
    raw = NearestTick() - labelShift_;  // the first label continues the phasor's
  }
  // 2. Outlier: more than a quarter tick and 10 ms off the line, from N ≥ 8.
  bool fitted = true;
  if (fitValid_ && winN_ >= kOutlierFloor) {
    const int64_t rho = Residual(raw, f);
    const int64_t mag = Abs64(rho);
    if (4 * mag > a_ && 100 * mag > static_cast<int64_t>(rate_) * d_) {
      fitted = false;
      ++stats_.tickOutliers;
      const int32_t sign = rho > 0 ? 1 : -1;
      if (outlierRun_ > 0 && sign == outlierSign_) {
        ++outlierRun_;
      } else {
        outlierRun_  = 1;
        outlierSign_ = sign;
      }
      driftRun_ = 0;  // rule 3.2's run is reset by an outlier
    }
  }
  if (fitted) {
    outlierRun_  = 0;
    outlierSign_ = 0;
  }
  RingPush(raw, f);
  lastRaw_       = raw;
  haveLabel_     = true;
  lastTickFrame_ = f;
  haveTickRef_   = true;
  // 3. The window: six outliers of one sign in a row mean the master changed tempo, and the ring
  // replaces the window; a fitted tick joins it; labels out of the last 96 leave.
  if (outlierRun_ >= kReacquireRun) {
    Reacquire();
    ++stats_.reacquires;
    outlierRun_  = 0;
    outlierSign_ = 0;
  } else if (fitted) {
    WindowEvict(raw);
    WindowAdd(raw, f);
  }
  WindowEvict(raw);
  Refit();
  // 4-6. An armed transport applies at this tick; or acquisition under Internal.
  bool entered = false;
  if (armed_) {
    const int64_t p = armedKind_ == TransportKind::Continue ? continuePosition_ : armedPosition_;
    labelShift_ += p - (raw + labelShift_);  // this tick's label becomes p, tempo data kept
    lastFired_     = p - 1;
    masterStopped_ = false;
    resumeRunning_ = false;
    running_       = true;
    earlyArmed_    = wasEmpty;
    source_        = ClockSource::ClockRunning;
    armed_         = false;
    entered        = true;
  } else if (source_ == ClockSource::Internal && !masterStopped_ && winN_ >= kLockTicks &&
             fitValid_) {
    // The newest tick's label becomes the phasor's nearest tick, so the grid does not jump.
    labelShift_ += NearestTick() - (raw + labelShift_);
    if (resumeRunning_) {
      source_  = ClockSource::ClockRunning;  // an implicit Continue
      running_ = true;
      ++stats_.resumes;
    } else {
      source_ = ClockSource::ClockFree;
    }
    entered = true;
  }
  if (source_ != ClockSource::Internal) {
    PlaceOnLine(raw);
    Commit(entered, prevN, fitted);
  }
}

void TempoCore::Commit(bool entered, uint32_t prevN, bool fitted) noexcept {
  if (!fitValid_) return;
  const uint64_t diff = AbsDiff(pFit_, pc_);
  // 1. Acquisition: entering a clock state on a warm window, or the window reaching 24 there.
  if ((entered && winN_ >= kLockTicks) || (prevN < kLockTicks && winN_ >= kLockTicks)) {
    ++stats_.commits;
    SetPc(pFit_, false);
    driftRun_ = 0;
    return;
  }
  // 2. The early commit after a Start or Continue on an empty window, at N = 12.
  if (earlyArmed_ && prevN < kEarlyTicks && winN_ >= kEarlyTicks) {
    earlyArmed_ = false;
    if (diff > (pc_ >> 4)) {
      ++stats_.earlyCommits;
      SetPc(pFit_, false);
      driftRun_ = 0;
    }
    return;
  }
  // 3. The deadband, from N ≥ 24.
  if (winN_ < kLockTicks) return;
  if (diff > (pc_ >> 9)) {
    ++stats_.commits;
    SetPc(pFit_, true);
    driftRun_ = 0;
    return;
  }
  if (!fitted) return;
  if (diff > (pc_ >> 12)) {
    if (++driftRun_ >= kDriftRun) {
      ++stats_.commits;
      SetPc(pFit_, true);
      driftRun_ = 0;
    }
  } else {
    driftRun_ = 0;
  }
}

// --- snapshots -----------------------------------------------------------------------------------

TempoStats TempoCore::Counts(uint64_t clockBirths, uint64_t clockDropped) const noexcept {
  TempoStats s   = stats_;
  s.clockBirths  = clockBirths;
  s.clockDropped = clockDropped;
  return s;
}

TempoCore::State TempoCore::Capture() const noexcept {
  State s;
  s.frame          = frame_;
  s.tick           = tick_;
  s.acc            = acc_;
  s.p              = p_;
  s.pc             = pc_;
  s.lastFired      = lastFired_;
  s.lastGridFrame  = lastGridFrame_;
  s.lastClockBirth = lastClockBirth_;
  s.timeMode       = timeMode_;
  s.subdiv         = subdiv_;
  s.source         = static_cast<uint8_t>(source_);
  s.storedUs       = storedUs_;
  s.storedTimeMode = storedTimeMode_;
  s.storedSubdiv   = storedSubdiv_;
  s.pcSerial       = pcSerial_;
  s.pcChange       = static_cast<uint8_t>(pcChange_);
  s.masterStopped  = masterStopped_;
  s.resumeRunning  = resumeRunning_;
  s.running        = running_;
  s.armed          = armed_;
  s.armedKind      = armed_ ? static_cast<uint8_t>(armedKind_) : 0;
  s.armedPosition  = armed_ ? armedPosition_ : 0;
  s.continuePosition = continuePosition_;
  s.haveTap = haveTap_;
  s.lastTap = haveTap_ ? lastTap_ : 0;
  s.tapN    = tapN_;
  for (uint32_t i = 0; i < tapN_; ++i) s.tapIntervals[i] = taps_[i];
  s.winN = winN_;
  for (uint32_t i = 0; i < winN_; ++i) {
    const WindowEntry& e = window_[(winHead_ + i) % kWindowLabels];
    const WindowEntry& o = window_[winHead_];
    s.winLabel[i] = raw0_ + static_cast<int32_t>(e.label - o.label) + labelShift_;
    s.winFrame[i] = frame0_ + static_cast<int32_t>(e.frame - o.frame);
  }
  s.sx = sx_;
  s.sy = sy_;
  s.sxx = sxx_;
  s.sxy = sxy_;
  s.fitValid = fitValid_;
  s.d    = winN_ >= 2 ? d_ : 0;
  s.a    = winN_ >= 2 ? a_ : 0;
  s.b    = winN_ >= 2 ? b_ : 0;
  s.pFit = fitValid_ ? pFit_ : 0;
  s.ringN = ringN_;
  for (uint32_t i = 0; i < ringN_; ++i) {
    const RingEntry& e = ring_[(ringHead_ + i) % kRingTicks];
    s.ringLabel[i] = e.label + labelShift_;
    s.ringFrame[i] = e.frame;
  }
  s.haveLabel     = haveLabel_;
  s.lastLabel     = haveLabel_ ? lastRaw_ + labelShift_ : 0;
  s.haveTickRef   = haveTickRef_;
  s.lastTickFrame = haveTickRef_ ? lastTickFrame_ : 0;
  s.outlierRun    = outlierRun_;
  s.outlierSign   = outlierSign_;
  s.driftRun      = driftRun_;
  s.earlyArmed    = earlyArmed_;
  return s;
}

}  // namespace brainscape
