#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

#include "TempoReference.h"
#include "brainscape/Tempo.h"
#include "detail/Tempo.h"

// Event streams for the tempo core's tests (docs/design/clock.md §8.2): seeded integer generators
// (ticks with the hardware and computer jitter models of §8.3, dropouts, gaps and tempo steps,
// tap series, tempo, transport, subdivision, Spillover and invalid events), and the two drivers
// that play a stream: RunCore renders TempoCore in blocks split at every event, as the engine
// will; RunRef plays the reference model with no blocks at all. Both report the grid's hits, a
// digest of the whole state after each frame's events, the Info() read at each probe, and the
// counters. No test framework here, so the tempo tool can run the same streams on the emulated
// Cortex-M7.
namespace brainscape::testing {

struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed) {}
  uint64_t Next() {
    uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  int64_t Range(int64_t lo, int64_t hi) {  // inclusive
    return lo + static_cast<int64_t>(Next() % static_cast<uint64_t>(hi - lo + 1));
  }
  bool Chance(uint32_t percent) { return Next() % 100 < percent; }
};

struct Fnv {
  uint64_t h = 0xCBF29CE484222325ull;
  void U(uint64_t v) {
    for (int i = 0; i < 8; ++i) {
      h ^= (v >> (8 * i)) & 0xFFu;
      h *= 0x100000001B3ull;
    }
  }
  void I(int64_t v) { U(static_cast<uint64_t>(v)); }
};

inline uint64_t StateDigest(const TempoCore::State& s) {
  Fnv f;
  f.I(s.frame); f.I(s.tick); f.I(s.acc); f.U(s.p); f.U(s.pc);
  f.I(s.lastFired); f.I(s.lastGridFrame); f.I(s.lastClockBirth);
  f.U(s.timeMode); f.U(s.subdiv); f.U(s.source);
  f.U(s.storedUs); f.U(s.storedTimeMode); f.U(s.storedSubdiv);
  f.U(s.pcSerial); f.U(s.pcChange);
  f.U(s.masterStopped); f.U(s.resumeRunning); f.U(s.running); f.U(s.armed);
  f.U(s.armedKind); f.I(s.armedPosition); f.I(s.continuePosition);
  f.U(s.haveTap); f.I(s.lastTap); f.U(s.tapN);
  for (int64_t v : s.tapIntervals) f.I(v);
  f.U(s.winN);
  for (uint32_t i = 0; i < TempoCore::kWindowLabels; ++i) {
    f.I(s.winLabel[i]);
    f.I(s.winFrame[i]);
  }
  f.I(s.sx); f.I(s.sy); f.I(s.sxx); f.I(s.sxy);
  f.U(s.fitValid); f.I(s.d); f.I(s.a); f.I(s.b); f.U(s.pFit);
  f.U(s.ringN);
  for (uint32_t i = 0; i < TempoCore::kRingTicks; ++i) {
    f.I(s.ringLabel[i]);
    f.I(s.ringFrame[i]);
  }
  f.U(s.haveLabel); f.I(s.lastLabel); f.U(s.haveTickRef); f.I(s.lastTickFrame);
  f.U(s.outlierRun); f.I(s.outlierSign); f.U(s.driftRun); f.U(s.earlyArmed);
  return f.h;
}

inline uint64_t StatsDigest(const TempoStats& t) {
  Fnv f;
  const uint64_t v[] = {t.taps, t.tapsIgnored, t.tapPhases, t.tempoEvents, t.tempoIgnored,
                        t.ticks, t.tickOutliers, t.reacquires, t.dropoutTicks, t.gaps, t.losses,
                        t.resumes, t.transports, t.transportsCancelled, t.transportsIgnored,
                        t.subdivEvents, t.clockBirths, t.clockDeferred, t.clockDeferredFrames,
                        t.clockDropped, t.commits, t.earlyCommits, t.jumps, t.slews, t.crossfades,
                        t.folds, t.invalidEvents, t.unknownEvents};
  for (uint64_t x : v) f.U(x);
  return f.h;
}

inline bool SameInfo(const TempoInfo& a, const TempoInfo& b) {
  return a.position == b.position && a.nsPerQuarter == b.nsPerQuarter && a.source == b.source &&
         a.timeMode == b.timeMode && a.subdiv == b.subdiv && a.flags == b.flags &&
         a.lastGridFrame == b.lastGridFrame && a.lastClockBirth == b.lastClockBirth;
}

// --- streams -------------------------------------------------------------------------------------

enum class TickModel : uint8_t { None, Hardware, Computer };

struct Stream {
  std::vector<StreamEvent> ev;
  uint32_t rate = 48000;

  void Add(int64_t f, uint8_t type, uint32_t id = 0, uint32_t value = 0) {
    StreamEvent e;
    e.frame = f;
    e.kind  = StreamEvent::Kind::Tempo;
    e.type  = type;
    e.id    = id;
    e.value = value;
    ev.push_back(e);
  }
  void Tap(int64_t f) { Add(f, tempo::kEventTap); }
  void Tempo(int64_t f, uint32_t ns) { Add(f, tempo::kEventTempo, ns); }
  void Tick(int64_t f) { Add(f, tempo::kEventClockTick); }
  void Transport(int64_t f, tempo::TransportKind k, bool atNext, uint32_t pos = 0,
                 uint32_t offset = 0) {
    const bool hasPos = k == tempo::TransportKind::Start || k == tempo::TransportKind::Locate;
    Add(f, tempo::kEventTransport, tempo::TransportId(k, atNext, offset),
        hasPos ? tempo::IntegerValueBits(pos) : 0u);
  }
  void Subdiv(int64_t f, tempo::SubdivField field, uint8_t code) {
    Add(f, tempo::kEventSubdivision, tempo::SubdivisionId(field, code));
  }
  void Spill(int64_t f, uint32_t us, uint8_t tm, uint8_t sd, bool recall) {
    StreamEvent e;
    e.frame = f;
    e.kind = StreamEvent::Kind::Spillover;
    e.us = us;
    e.timeMode = tm;
    e.subdiv = sd;
    e.recall = recall;
    ev.push_back(e);
  }
  void Kind(int64_t f, StreamEvent::Kind k, uint8_t type = 0) {
    StreamEvent e;
    e.frame = f;
    e.kind = k;
    e.type = type;
    ev.push_back(e);
  }
  void Probe(int64_t f) { Kind(f, StreamEvent::Kind::Probe); }
  void Other(int64_t f) { Kind(f, StreamEvent::Kind::Other); }
  void Unknown(int64_t f, uint8_t type) { Kind(f, StreamEvent::Kind::Unknown, type); }

  // An invalid event of types 6-10 (§4.2's forms), chosen by r.
  void Invalid(int64_t f, Rng& r) {
    switch (r.Next() % 12) {
      case 0: Add(f, tempo::kEventTap, 1); break;
      case 1: Add(f, tempo::kEventTap, 0, 0x80000000u); break;  // −0 where +0 is required
      case 2: Add(f, tempo::kEventTempo, tempo::kMinNsPerQuarter - 1); break;
      case 3: Add(f, tempo::kEventTempo, tempo::kMaxNsPerQuarter + 1); break;
      case 4: Add(f, tempo::kEventClockTick, 0, 0x3F800000u); break;
      case 5:  // an offset with AtNextTick
        Add(f, tempo::kEventTransport, tempo::TransportId(tempo::TransportKind::Start, true, 5));
        break;
      case 6:  // an offset on a Stop
        Add(f, tempo::kEventTransport, tempo::TransportId(tempo::TransportKind::Stop, false, 5));
        break;
      case 7:  // a fractional position
        Add(f, tempo::kEventTransport, tempo::TransportId(tempo::TransportKind::Locate, false),
            0x3FC00000u);
        break;
      case 8:  // a position out of range
        Add(f, tempo::kEventTransport, tempo::TransportId(tempo::TransportKind::Start, true),
            tempo::IntegerValueBits(tempo::kPositionModulus));
        break;
      case 9:  // a reserved bit
        Add(f, tempo::kEventTransport, tempo::TransportId(tempo::TransportKind::Stop, true) | 4u);
        break;
      case 10:  // a code out of range
        Add(f, tempo::kEventSubdivision, tempo::SubdivisionId(tempo::SubdivField::Subdivision, 6));
        break;
      default:  // a field out of range
        Add(f, tempo::kEventSubdivision, 0x200u);
        break;
    }
  }

  // `count` clock ticks at ns per quarter from `start`, with a jitter model; returns the frame
  // after the last. Ticks whose ideal frame lies in [dropFrom, dropTo) are dropped.
  int64_t Ticks(int64_t start, uint32_t ns, int count, TickModel model, Rng& r,
                int64_t dropFrom = -1, int64_t dropTo = -1) {
    // The ideal tick i at start + floor(i·ns·R / (24·10^9)), exactly.
    const uint64_t num = static_cast<uint64_t>(ns) * rate;
    const uint64_t den = 24000000000ull;
    const uint64_t q = num / den, rem = num % den;
    int64_t last = ev.empty() ? 0 : ev.back().frame;
    int64_t f = start;
    for (int i = 0; i < count; ++i) {
      const int64_t ideal = start + static_cast<int64_t>(q * static_cast<uint64_t>(i) +
                                                         (rem * static_cast<uint64_t>(i)) / den);
      if (ideal >= dropFrom && ideal < dropTo) continue;
      f = ideal;
      if (model == TickModel::Hardware) {
        f = (ideal + 47) / 48 * 48;  // stamped on the pedal's 48-frame block grid
      } else if (model == TickModel::Computer) {
        int64_t j = 0;
        for (int k = 0; k < 4; ++k) j += r.Range(-141, 141);  // σ ≈ 163 frames (3.4 ms)
        f = ideal + j;
        if (i % 500 == 499) f = ideal + 1843;  // a held tick, 38.4 ms
      }
      if (f < last) f = last;  // frames never fall: bunched behind a late one
      if (f < 0) f = 0;
      Tick(f);
      last = f;
    }
    return start + static_cast<int64_t>(q * static_cast<uint64_t>(count) +
                                        (rem * static_cast<uint64_t>(count)) / den);
  }

  // Taps at start and then after each interval, each moved by up to ±spread frames.
  int64_t TapSeries(int64_t start, const std::vector<int64_t>& intervals, int64_t spread, Rng& r) {
    int64_t f = start;
    Tap(f);
    for (int64_t iv : intervals) {
      f += iv;
      Tap(spread > 0 ? f + r.Range(-spread, spread) : f);
    }
    return f;
  }

  // Stable sort by frame: events at one frame keep their order.
  void Sort() {
    std::stable_sort(ev.begin(), ev.end(),
                     [](const StreamEvent& a, const StreamEvent& b) { return a.frame < b.frame; });
  }
};

// Block sizes: a constant, a repeating pattern, or seeded random sizes 1-512.
struct Blocks {
  std::vector<uint32_t> pattern;
  bool     random = false;
  Rng      rng{1};
  size_t   i = 0;
  static Blocks Const(uint32_t n) { Blocks b; b.pattern = {n}; return b; }
  static Blocks Pattern(std::vector<uint32_t> p) { Blocks b; b.pattern = std::move(p); return b; }
  static Blocks Random(uint64_t seed) { Blocks b; b.random = true; b.rng = Rng(seed); return b; }
  uint32_t Next() {
    if (random) return static_cast<uint32_t>(rng.Range(1, 512));
    const uint32_t n = pattern[i % pattern.size()];
    ++i;
    return n;
  }
};

struct RunConfig {
  uint32_t rate = 48000;
  uint32_t us = 500000;
  uint8_t  timeMode = 0, subdiv = 0;
  int64_t  captureGroup = -1;  // the index of the frame group whose full state to keep
};

struct RunResult {
  std::vector<GridHit>  hits;
  std::vector<uint64_t> states;  // StateDigest after each frame's events
  std::vector<TempoInfo> probes;
  TempoStats            stats;
  uint64_t              finalState = 0;
  uint32_t              maxHitsPerSpan = 0;
  TempoCore::State      captured;
};

inline bool SameHits(const std::vector<GridHit>& a, const std::vector<GridHit>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (a[i].frame != b[i].frame || a[i].position != b[i].position) return false;
  return true;
}

// The index of the last event that is not a probe, or ev.size() when there is none.
inline size_t LastNonProbe(const std::vector<StreamEvent>& ev) {
  for (size_t k = ev.size(); k > 0; --k)
    if (ev[k - 1].kind != StreamEvent::Kind::Probe) return k - 1;
  return ev.size();
}

inline void ApplyToCore(TempoCore& core, const StreamEvent& e, RunResult* out) {
  switch (e.kind) {
    case StreamEvent::Kind::Tempo: core.ApplyEvent(e.frame, e.type, e.id, e.value); break;
    case StreamEvent::Kind::Unknown: core.CountUnknownEvent(e.frame); break;
    case StreamEvent::Kind::Other: core.BeforeEvent(e.frame); break;
    case StreamEvent::Kind::Spillover:
      core.SpilloverLoad(e.frame, e.us, e.timeMode, e.subdiv, e.recall);
      break;
    case StreamEvent::Kind::Probe:
      // A read between renders: Frame() is the probe's frame, nothing mutates.
      out->probes.push_back(core.Info());
      break;
  }
}

// TempoCore rendered in blocks, each split at the events inside it, GridFrames once per span.
// With eagerGaps the driver also splits at every gap deadline and applies the gap there.
inline RunResult RunCore(const RunConfig& cfg, const std::vector<StreamEvent>& ev, int64_t end,
                         Blocks blocks, bool eagerGaps = false) {
  RunResult out;
  TempoCore core;
  core.Init(cfg.rate);
  core.SetStoredPerformance(cfg.us, cfg.timeMode, cfg.subdiv);
  core.Restart();
  GridHit buf[TempoCore::kMaxClockPerSpan];
  const size_t lastEvent = LastNonProbe(ev);
  int64_t f = 0;
  size_t  i = 0;
  int64_t group = 0;
  while (f < end) {
    const int64_t bEnd = std::min<int64_t>(f + blocks.Next(), end);
    while (f < bEnd) {
      bool any = false;  // a probe reads, so a frame of probes alone records no state
      while (i < ev.size() && ev[i].frame == f) {
        ApplyToCore(core, ev[i], &out);
        if (ev[i].kind != StreamEvent::Kind::Probe) any = true;
        ++i;
      }
      if (any) {
        const TempoCore::State s = core.Capture();
        out.states.push_back(StateDigest(s));
        if (group == cfg.captureGroup) out.captured = s;
        ++group;
      }
      int64_t next = bEnd;
      if (i < ev.size() && ev[i].frame < next) next = ev[i].frame;
      // Gaps at their deadlines, while an event is still to come (with none, the lazy rule never
      // applies one, and the counters would differ for that alone; a probe does not count).
      int64_t deadline;
      const bool more = lastEvent < ev.size() && i <= lastEvent;
      if (eagerGaps && more && core.GapDeadline(&deadline) && deadline > f && deadline < next) {
        next = deadline;
      }
      const uint32_t n = core.GridFrames(next, buf, TempoCore::kMaxClockPerSpan);
      if (n > out.maxHitsPerSpan) out.maxHitsPerSpan = n;
      out.hits.insert(out.hits.end(), buf, buf + n);
      f = next;
      if (eagerGaps && more && core.GapDeadline(&deadline) && deadline == f) core.BeforeEvent(f);
    }
  }
  out.stats = core.Stats();
  out.finalState = StateDigest(core.Capture());
  return out;
}

// The reference, with no blocks: the grid fired between events (literal: frame by frame), gaps
// applied at their deadlines (unless the perturbation says otherwise).
inline RunResult RunRef(const RunConfig& cfg, const std::vector<StreamEvent>& ev, int64_t end,
                        Perturb perturb = Perturb::None, bool literal = false) {
  RunResult out;
  TempoRef ref(cfg.rate, perturb);
  ref.SetStored(cfg.us, cfg.timeMode, cfg.subdiv);
  ref.Restart();
  const bool lazyGap = perturb == Perturb::GapOnlyBeforeTempo;
  const size_t lastEvent = LastNonProbe(ev);
  int64_t t0 = 0;
  size_t  i = 0;
  int64_t group = 0;
  for (;;) {
    const int64_t t1 = i < ev.size() ? ev[i].frame : end;
    int64_t d;
    // A gap at its deadline, while an event is still to come (see RunCore).
    if (!lazyGap && lastEvent < ev.size() && i <= lastEvent && ref.GapDue(&d) && d <= t1) {
      ref.Fire(t0, d, &out.hits, literal);
      ref.Gap();
      t0 = d;
    }
    ref.Fire(t0, t1, &out.hits, literal);
    if (i >= ev.size()) break;
    bool any = false;
    while (i < ev.size() && ev[i].frame == t1) {
      const StreamEvent& e = ev[i];
      if (lazyGap && e.kind == StreamEvent::Kind::Tempo && ref.GapDue(&d) && d <= t1) ref.Gap();
      ref.Event(e);
      if (e.kind == StreamEvent::Kind::Probe) out.probes.push_back(ref.Info(t1));
      else any = true;
      ++i;
    }
    if (any) {
      const TempoCore::State s = ref.Capture();
      out.states.push_back(StateDigest(s));
      if (group == cfg.captureGroup) out.captured = s;
      ++group;
    }
    t0 = t1;
  }
  out.stats = ref.Stats();
  out.finalState = 0;  // the reference's frame is the last event's, not `end`: compare states
  return out;
}

}  // namespace brainscape::testing
