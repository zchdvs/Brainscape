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
  f.U(s.outlierRun); f.I(s.outlierSign); f.U(s.bandRun); f.U(s.driftRun);
  f.U(s.earlyArmed);
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

// Seeded streams of one family (§8.2): tempo changes, Subdiv and time-mode events, taps and
// snapshots (Tempo); tap chains that set, refine, pause, halve, double, bounce and sit on the
// 1.75x and 40 % edges (Taps); clock runs with each jitter model, dropouts, tempo steps, MIDI
// and host transports, FA then FC, gaps with snapshots, Spillover loads and other events inside
// them, invalid and unknown events (Clock); or all of them (Mixed). `seconds` long at `rate`.
enum class Family : uint8_t { Tempo, Taps, Clock, Mixed };

// A random stream of one family, `seconds` long at 48 kHz (or `rate`).
inline Stream MakeStream(Family fam, uint64_t seed, int seconds, uint32_t rate = 48000) {
  Stream s;
  s.rate = rate;
  Rng r(seed);
  const int64_t end = int64_t{rate} * seconds;
  auto at = [&](int64_t lo, int64_t hi) { return r.Range(lo, hi < end - 1 ? hi : end - 1); };
  // ns per quarter of 137.5, 140, 97, 120, 20, 300, 61.3 and 233 BPM, rounded.
  const uint32_t bpmNs[] = {436363636u, 428571429u, 618556701u, 500000000u,
                            3000000000u, 200000000u, 978792822u, 257510730u};
  if (fam == Family::Tempo || fam == Family::Mixed) {
    for (int i = 0; i < 6 * seconds; ++i) {
      const int64_t f = at(1, end - 1);
      switch (r.Next() % 6) {
        case 0: s.Tempo(f, bpmNs[r.Next() % 8]); break;
        case 1: s.Tempo(f, static_cast<uint32_t>(r.Range(tempo::kMinNsPerQuarter, tempo::kMaxNsPerQuarter))); break;
        case 2: s.Subdiv(f, tempo::SubdivField::Subdivision, static_cast<uint8_t>(r.Next() % 6)); break;
        case 3: s.Subdiv(f, tempo::SubdivField::TimeMode, static_cast<uint8_t>(r.Next() % 3)); break;
        case 4: s.Tap(f); break;
        default: s.Probe(f); break;
      }
    }
  }
  if (fam == Family::Taps || fam == Family::Mixed) {
    int64_t f = at(0, int64_t{rate} / 2);
    while (f < end - int64_t{rate} * 4) {
      // A chain: set, refine, then a pause, a halving, a bounce or a new tempo.
      const int64_t iv = r.Range(rate / 5, 3 * rate);
      std::vector<int64_t> ivs;
      const int n = static_cast<int>(r.Range(1, 6));
      for (int i = 0; i < n; ++i) ivs.push_back(iv);
      switch (r.Next() % 5) {
        case 0: ivs.push_back(iv * 2); break;                // a pause (or a halving's first)
        case 1: ivs.push_back(iv / 2); ivs.push_back(iv / 2); break;  // doubling
        case 2: ivs.push_back(r.Range(1, rate / 5 - 1)); break;        // a bounce
        case 3: ivs.push_back(iv * 7 / 4); ivs.push_back(iv * 7 / 4); break;  // the 1.75 edge
        default: ivs.push_back(iv * 7 / 5 + r.Range(-2, 2)); break;   // the 40 % edge
      }
      // Every draw in its own statement: function arguments and `+` operands are evaluated in an
      // unspecified order, and the streams must be the same on every compiler.
      const int64_t spread = r.Range(0, 3) * 48;
      f = s.TapSeries(f, ivs, spread, r);
      f += r.Range(rate / 10, 4 * rate);
    }
    for (int i = 0; i < seconds; ++i) s.Probe(at(0, end - 1));
  }
  if (fam == Family::Clock || fam == Family::Mixed) {
    int64_t f = at(0, int64_t{rate});
    const TickModel models[] = {TickModel::None, TickModel::Hardware, TickModel::Computer};
    while (f < end - int64_t{rate}) {
      const uint32_t ns = r.Chance(30) ? bpmNs[r.Next() % 8]
                                       : static_cast<uint32_t>(r.Range(250000000, 1500000000));
      const int count = static_cast<int>(r.Range(10, 300));
      const TickModel m = models[r.Next() % 3];
      // Transport around the run: MIDI Start / Continue / Locate / Stop, host events; and FA then
      // FC before the ticks, which must start nothing (E10).
      switch (r.Next() % 9) {
        case 7:
          s.Transport(f, tempo::TransportKind::Start, true, 0);
          s.Transport(f + r.Range(0, 400), tempo::TransportKind::Stop, true);
          f += 400;
          break;
        // FA or FB some time before the first tick, so the grid is held over grid points (§3.4,
        // as-built note 26).
        case 0:
          s.Transport(f, tempo::TransportKind::Start, true, 0);
          f += r.Range(0, rate / 2);
          break;
        case 1:
          s.Transport(f, tempo::TransportKind::Continue, true);
          f += r.Range(0, rate / 2);
          break;
        case 2: s.Transport(f, tempo::TransportKind::Locate, true, static_cast<uint32_t>(6 * r.Range(0, 16383))); break;
        case 3: {
          const auto pos = static_cast<uint32_t>(r.Range(0, 1000));
          const auto offset = static_cast<uint32_t>(r.Range(0, 2000));
          s.Transport(f, tempo::TransportKind::Start, false, pos, offset);
          break;
        }
        default: break;
      }
      int64_t dropFrom = -1, dropTo = -1;
      if (r.Chance(30)) {
        dropFrom = f + r.Range(0, int64_t{rate} * 2);
        dropTo = dropFrom + r.Range(rate / 20, rate / 3);
      }
      const int64_t after = s.Ticks(f, ns, count, m, r, dropFrom, dropTo);
      // Mid-run events.
      for (int i = 0; i < 4; ++i) {
        const int64_t g = r.Range(f, after);
        switch (r.Next() % 9) {
          case 0: s.Tap(g); break;
          case 1: s.Tempo(g, bpmNs[r.Next() % 8]); break;
          case 2: s.Transport(g, tempo::TransportKind::Stop, true); break;
          case 3: {
            const bool atNext = r.Chance(50);
            const auto pos = static_cast<uint32_t>(r.Range(0, 5000));
            s.Transport(g, tempo::TransportKind::Locate, atNext, pos);
            break;
          }
          case 4: {
            const auto us = static_cast<uint32_t>(r.Range(200000, 3000000));
            const auto tm = static_cast<uint8_t>(r.Next() % 3);
            const auto sd = static_cast<uint8_t>(r.Next() % 6);
            const bool recall = r.Chance(50);
            s.Spill(g, us, tm, sd, recall);
            break;
          }
          case 5: s.Invalid(g, r); break;
          case 6: s.Unknown(g, static_cast<uint8_t>(r.Range(11, 255))); break;
          case 7: s.Other(g); break;
          default: s.Probe(g); break;
        }
      }
      if (r.Chance(40)) {
        // A gap: a second or more without a tick, and inside it a snapshot, and maybe a Spillover
        // load under recall Preset or another event (each sees the gap first, §3.5).
        const int64_t gapLen = r.Range(int64_t{rate} + 1, int64_t{rate} * 3);
        s.Probe(after + int64_t{rate} + r.Range(0, 2000));
        if (r.Chance(50)) s.Spill(after + int64_t{rate} + r.Range(2001, 4000), 400000, 0, 0, true);
        if (r.Chance(30)) s.Other(after + int64_t{rate} + r.Range(4001, 6000));
        f = after + gapLen;
      } else {
        f = after + r.Range(0, rate / 2);
      }
      if (r.Chance(30)) s.Transport(f, tempo::TransportKind::Stop, r.Chance(70));
    }
  }
  s.Sort();
  // Keep the events inside the render.
  while (!s.ev.empty() && s.ev.back().frame >= end) s.ev.pop_back();
  return s;
}


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
