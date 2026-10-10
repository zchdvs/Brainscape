// The tempo core (docs/design/clock.md §2, §3, §4.1-§4.2, §6.3), §8.2's unit tests: every rule
// against the reference model of TempoReference.h (closed-form phasor, frame-by-frame grid, direct
// sums, exact 128-bit rounding, gaps at their deadlines) over long random and adversarial streams
// at many block patterns, and case by case against hand-worked numbers. Each comparison also runs
// against a perturbed reference, which it must catch; each case named for a draft-v1 finding
// reproduces that finding's failure on draft v1's rule.
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "TempoReference.h"
#include "TempoStreams.h"
#include "WideInt.h"
#include "brainscape/Tempo.h"
#include "catch.hpp"
#include "detail/IntMath.h"
#include "detail/Tempo.h"

using namespace brainscape;
using namespace brainscape::testing;
using tempo::ClockSource;
using tempo::SubdivField;
using tempo::TransportKind;

namespace {

#if defined(NDEBUG)
constexpr int kScale = 10;  // Release runs the full sizes
#else
constexpr int kScale = 1;   // Debug runs a tenth
#endif

constexpr int64_t kK = TempoCore::kK;

uint32_t NsOfBpm(double bpm) {  // test-side only: exact for the BPMs used
  return static_cast<uint32_t>(60000000000.0 / bpm + 0.5);
}

// ⌈k·P/K⌉ in 128 bits.
int64_t CeilKP(int64_t k, uint64_t p) { return I128::CeilDiv(W(k) * WU(p), W(kK)).ToI64(); }

TempoCore MakeCore(uint32_t rate = 48000, uint32_t us = 500000) {
  TempoCore c;
  c.Init(rate);
  c.SetStoredPerformance(us, 0, 0);
  c.Restart();
  return c;
}

// Renders to `end` in spans of at most `span` frames; returns the hits.
std::vector<GridHit> RenderTo(TempoCore& c, int64_t end, int64_t span = 48) {
  std::vector<GridHit> out;
  GridHit buf[16];
  while (c.Frame() < end) {
    const int64_t e = c.Frame() + span < end ? c.Frame() + span : end;
    const uint32_t n = c.GridFrames(e, buf, 16);
    out.insert(out.end(), buf, buf + n);
  }
  return out;
}

void Tick(TempoCore& c, int64_t f) { c.ApplyEvent(f, tempo::kEventClockTick, 0, 0); }
void Tap(TempoCore& c, int64_t f) { c.ApplyEvent(f, tempo::kEventTap, 0, 0); }
void Tempo(TempoCore& c, int64_t f, uint32_t ns) { c.ApplyEvent(f, tempo::kEventTempo, ns, 0); }
void Transport(TempoCore& c, int64_t f, TransportKind k, bool atNext, uint32_t pos = 0,
               uint32_t offset = 0) {
  const bool hasPos = k == TransportKind::Start || k == TransportKind::Locate;
  c.ApplyEvent(f, tempo::kEventTransport, tempo::TransportId(k, atNext, offset),
               hasPos ? tempo::IntegerValueBits(pos) : 0u);
}

// The streams' comparison: TempoCore at every block pattern against the reference.
struct Compare {
  bool        ok = true;
  std::string why;
};

const std::vector<Blocks>& Patterns() {
  static const std::vector<Blocks> p = {
      Blocks::Const(1),    Blocks::Const(48),  Blocks::Const(441), Blocks::Const(512),
      Blocks::Pattern({48, 1, 127, 32}), Blocks::Pattern({300, 512, 5, 64}),
      Blocks::Random(1),   Blocks::Random(2)};
  return p;
}

// Runs the stream through TempoCore at every pattern (and with gaps applied eagerly) and through
// the reference; true when every run equals the reference in hits, states, probes and counters.
bool CoreMatchesReference(const RunConfig& cfg, const std::vector<StreamEvent>& ev, int64_t end,
                          Perturb perturb, bool literal, std::string* why,
                          bool allPatterns = true) {
  const RunResult ref = RunRef(cfg, ev, end, perturb, literal);
  std::vector<Blocks> pats = allPatterns ? Patterns() : std::vector<Blocks>{Blocks::Const(48)};
  int idx = 0;
  for (Blocks b : pats) {
    for (int eager = 0; eager < 2; ++eager) {
      if (eager == 1 && idx != 1) continue;  // the eager-gap split on one pattern
      const RunResult got = RunCore(cfg, ev, end, b, eager == 1);
      std::string tag = "pattern " + std::to_string(idx) + (eager ? " eager" : "");
      if (got.maxHitsPerSpan > TempoCore::kMaxClockPerSpan) {
        *why = tag + ": more hits in a span than kMaxClockPerSpan";
        return false;
      }
      if (!SameHits(got.hits, ref.hits)) {
        size_t k = 0;
        while (k < got.hits.size() && k < ref.hits.size() && got.hits[k].frame == ref.hits[k].frame &&
               got.hits[k].position == ref.hits[k].position)
          ++k;
        *why = tag + ": hits differ at #" + std::to_string(k) + " of " +
               std::to_string(got.hits.size()) + "/" + std::to_string(ref.hits.size());
        if (k < got.hits.size())
          *why += " core (" + std::to_string(got.hits[k].position) + "@" +
                  std::to_string(got.hits[k].frame) + ")";
        if (k < ref.hits.size())
          *why += " ref (" + std::to_string(ref.hits[k].position) + "@" +
                  std::to_string(ref.hits[k].frame) + ")";
        return false;
      }
      if (got.states != ref.states) {
        size_t k = 0;
        while (k < got.states.size() && k < ref.states.size() && got.states[k] == ref.states[k]) ++k;
        *why = tag + ": state differs after frame group #" + std::to_string(k);
        return false;
      }
      if (got.probes.size() != ref.probes.size()) {
        *why = tag + ": probe count";
        return false;
      }
      for (size_t k = 0; k < got.probes.size(); ++k) {
        if (!SameInfo(got.probes[k], ref.probes[k])) {
          *why = tag + ": Info() differs at probe #" + std::to_string(k);
          return false;
        }
      }
      if (StatsDigest(got.stats) != StatsDigest(ref.stats)) {
        *why = tag + ": counters differ";
        return false;
      }
    }
    ++idx;
  }
  return true;
}

// Explains the first differing state (field by field) for a failing comparison.
std::string ExplainState(const RunConfig& cfg0, const std::vector<StreamEvent>& ev, int64_t end) {
  const RunResult ref = RunRef(cfg0, ev, end);
  const RunResult got = RunCore(cfg0, ev, end, Blocks::Const(48));
  size_t k = 0;
  while (k < got.states.size() && k < ref.states.size() && got.states[k] == ref.states[k]) ++k;
  RunConfig cfg = cfg0;
  cfg.captureGroup = static_cast<int64_t>(k);
  const TempoCore::State a = RunCore(cfg, ev, end, Blocks::Const(48)).captured;
  const TempoCore::State b = RunRef(cfg, ev, end).captured;
  std::string s = "group " + std::to_string(k) + " frame " + std::to_string(a.frame) + "/" +
                  std::to_string(b.frame) + ":";
#define BS_FIELD(f) \
  if (a.f != b.f) s += " " #f " " + std::to_string(a.f) + "/" + std::to_string(b.f);
  BS_FIELD(tick) BS_FIELD(acc) BS_FIELD(p) BS_FIELD(pc) BS_FIELD(lastFired) BS_FIELD(lastGridFrame)
  BS_FIELD(timeMode) BS_FIELD(subdiv) BS_FIELD(source) BS_FIELD(pcSerial) BS_FIELD(pcChange)
  BS_FIELD(masterStopped) BS_FIELD(resumeRunning) BS_FIELD(running) BS_FIELD(armed)
  BS_FIELD(armedKind) BS_FIELD(armedPosition) BS_FIELD(continuePosition) BS_FIELD(haveTap)
  BS_FIELD(lastTap) BS_FIELD(tapN) BS_FIELD(winN) BS_FIELD(sx) BS_FIELD(sy) BS_FIELD(sxx)
  BS_FIELD(sxy) BS_FIELD(fitValid) BS_FIELD(d) BS_FIELD(a) BS_FIELD(b) BS_FIELD(pFit)
  BS_FIELD(ringN) BS_FIELD(haveLabel) BS_FIELD(lastLabel) BS_FIELD(haveTickRef)
  BS_FIELD(lastTickFrame) BS_FIELD(outlierRun) BS_FIELD(outlierSign) BS_FIELD(driftRun)
  BS_FIELD(earlyArmed) BS_FIELD(storedUs)
#undef BS_FIELD
  for (uint32_t i = 0; i < TempoCore::kWindowLabels; ++i)
    if (a.winLabel[i] != b.winLabel[i] || a.winFrame[i] != b.winFrame[i]) {
      s += " win[" + std::to_string(i) + "]";
      break;
    }
  return s;
}

}  // namespace

// =================================================================================================
// §2.1 units and §2.3 durations
// =================================================================================================

TEST_CASE("Tempo: P from ns and back, exactly", "[tempo][units]") {
  Rng r(11);
  const uint32_t rates[] = {8000, 22050, 44100, 48000, 88200, 96000, 192000, 384000};
  for (uint32_t R : rates) {
    TempoRef ref(R);
    for (int i = 0; i < 2000; ++i) {
      uint32_t ns;
      switch (i) {
        case 0: ns = tempo::kMinNsPerQuarter; break;
        case 1: ns = tempo::kMaxNsPerQuarter; break;
        case 2: ns = 500000000u; break;
        case 3: ns = 428571429u; break;  // 140 BPM
        default:
          ns = static_cast<uint32_t>(r.Range(tempo::kMinNsPerQuarter, tempo::kMaxNsPerQuarter));
      }
      const uint64_t p = tempo::PFromNs(ns, R);
      INFO("R=" << R << " ns=" << ns);
      REQUIRE(p == ref.PFromNs(ns));
      REQUIRE(p < (1ull << 53));
      REQUIRE(tempo::NsFromP(p, R) == ns);  // the 0.5-unit rounding of P never moves ns
    }
  }
  // A perturbed conversion (truncation) differs somewhere.
  int differs = 0;
  for (uint32_t ns = 200000000u; ns < 200001000u; ++ns) {
    const uint64_t x = static_cast<uint64_t>(ns) * 44100u;
    const uint64_t trunc = ((x / 1000000000u) << 32) + ((x % 1000000000u) << 32) / 1000000000u;
    if (trunc != tempo::PFromNs(ns, 44100)) ++differs;
  }
  REQUIRE(differs > 100);
}

TEST_CASE("Tempo: durations, every code and Subdiv at six tempos", "[tempo][units]") {
  const uint32_t noteTicks[] = {3, 4, 6, 8, 9, 12, 16, 18, 24, 32, 36, 48, 64, 72, 96, 192};
  const double   bpms[]      = {20, 30, 60, 119, 120, 300};
  for (double bpm : bpms) {
    const uint64_t pc = tempo::PFromNs(NsOfBpm(bpm), 48000);
    for (uint8_t sd = 0; sd < tempo::kSubdivCodes; ++sd) {
      const uint32_t s = tempo::SubdivTicks(sd);
      for (uint32_t t : noteTicks) {
        for (int32_t oct = -4; oct <= 4; ++oct) {
          // RoundHalfUp(Pc·t·s·2^oct / (576·2^32)) in 128 bits.
          I128 num = WU(pc) * W(t) * W(s);
          I128 den = W(576) * W(int64_t{1} << 32);
          if (oct > 0) num = num * W(int64_t{1} << oct);
          if (oct < 0) den = den * W(int64_t{1} << -oct);
          const uint64_t want = TempoRef::RoundHalfUp(num, den).ToU64();
          INFO("bpm=" << bpm << " sd=" << int(sd) << " ticks=" << t << " oct=" << oct);
          REQUIRE(tempo::DurationFrames(pc, t, s, oct) == want);
        }
      }
    }
  }
  // Worked values (§5.2, §5.3): a quarter at 120 BPM is 24,000 frames; 2/1 at 119 BPM 193,613;
  // folded once, 96,807; 1/32 under ×8 at 300 BPM 150.
  REQUIRE(tempo::DurationFrames(tempo::PFromNs(500000000u, 48000), 24, 24) == 24000);
  const uint64_t p119 = tempo::PFromNs(NsOfBpm(119), 48000);
  REQUIRE(tempo::DurationFrames(p119, 192, 24) == 193613);
  REQUIRE(tempo::DurationFrames(p119, 192, 24, -1) == 96807);
  REQUIRE(tempo::DurationFrames(tempo::PFromNs(200000000u, 48000), 3, 3) == 150);
  // 1/2d at 50 BPM, 172,800 frames (§5.2).
  REQUIRE(tempo::DurationFrames(tempo::PFromNs(1200000000u, 48000), 72, 24) == 172800);
}

// =================================================================================================
// §2.2 the phasor
// =================================================================================================

TEST_CASE("Tempo phasor: span-split invariance and the closed form", "[tempo][phasor]") {
  Rng r(3);
  for (int trial = 0; trial < 4 * kScale; ++trial) {
    const uint32_t R  = trial % 2 ? 48000 : static_cast<uint32_t>(r.Range(8000, 384000));
    const uint32_t ns = static_cast<uint32_t>(r.Range(tempo::kMinNsPerQuarter, tempo::kMaxNsPerQuarter));
    TempoCore a = MakeCore(R), b = MakeCore(R);
    Tempo(a, 0, ns);
    Tempo(b, 0, ns);
    TempoRef ref(R);
    ref.Event([&] {
      StreamEvent e;
      e.frame = 0;
      e.type = tempo::kEventTempo;
      e.id = ns;
      return e;
    }());
    GridHit buf[64];
    const int64_t total = 1000000;
    // a: random spans; b: one call per 2^20 frames.
    while (a.Frame() < total) {
      const int64_t e = std::min<int64_t>(a.Frame() + r.Range(1, 512), total);
      (void)a.GridFrames(e, buf, 64);
      if (r.Chance(5)) {
        int64_t tick, acc;
        ref.TickAcc(a.Frame(), &tick, &acc);
        REQUIRE(a.Tick() == tick);
        REQUIRE(a.Acc() == acc);
      }
    }
    (void)b.GridFrames(total, buf, 64);
    REQUIRE(a.Tick() == b.Tick());
    REQUIRE(a.Acc() == b.Acc());
    REQUIRE(a.Acc() > 0);
    REQUIRE(static_cast<uint64_t>(a.Acc()) <= a.P());
  }
}

TEST_CASE("Tempo phasor: ten hours at 140 BPM, every beat on the closed form", "[tempo][phasor]") {
  TempoCore c = MakeCore(48000);
  const uint32_t ns = 428571429u;  // 140 BPM to the ns
  Tempo(c, 0, ns);
  const uint64_t P = c.P();
  REQUIRE(P == tempo::PFromNs(ns, 48000));
  const int64_t total = int64_t{48000} * 3600 * (kScale == 10 ? 10 : 1);
  GridHit buf[8];
  int64_t beats = 0;
  bool    ok = true;
  while (c.Frame() < total) {
    const int64_t e = std::min<int64_t>(c.Frame() + 4800, total);
    const uint32_t n = c.GridFrames(e, buf, 8);
    for (uint32_t i = 0; i < n; ++i) {
      if (buf[i].position != 24 * beats || buf[i].frame != CeilKP(buf[i].position, P)) ok = false;
      ++beats;
    }
  }
  REQUIRE(ok);
  // Every beat whose first frame lies before the end fired, and no other: F(24b) ≤ total − 1, so
  // b ≤ (total − 1)·K / (24·P). 140 a minute: the ns rounding puts the last beat just past the end.
  REQUIRE(beats == I128::FloorDiv(W(total - 1) * W(kK), WU(P) * W(24)).ToI64() + 1);
  REQUIRE(beats == 140 * 60 * (kScale == 10 ? 10 : 1));
}

TEST_CASE("Tempo phasor: tempo changes keep the tick fraction; offset placement", "[tempo][phasor]") {
  TempoCore c = MakeCore(48000);  // 120 BPM: 1,000 frames a tick
  RenderTo(c, 10250);             // tick 10, a quarter of the way in
  REQUIRE(c.Tick() == 10);
  REQUIRE(c.Acc() == 250 * kK);
  Tempo(c, 10250, 250000000u);  // 240 BPM: 500 frames a tick
  REQUIRE(c.Tick() == 10);
  REQUIRE(c.Acc() == 125 * kK);  // the same fraction
  REQUIRE(c.FrameOfBoundary(11) == 10250 + 375);
  // The rounding: acc′ = max(1, round(acc·P′/P)), never 0.
  TempoCore d = MakeCore(48000);
  RenderTo(d, 1);  // acc = K
  Tempo(d, 1, 200000000u);
  REQUIRE(d.Acc() == I128::FloorDiv(W(2) * W(kK) * WU(d.P()) + WU(tempo::PFromNs(500000000u, 48000)),
                                    W(2) * WU(tempo::PFromNs(500000000u, 48000)))
                         .ToI64());
  // A host Start with an offset of up to two ticks places boundary p at f + o.
  for (uint32_t o : {0u, 1u, 999u, 1000u, 1001u, 1999u}) {
    TempoCore e = MakeCore(48000);
    RenderTo(e, 5000);
    Transport(e, 5000, TransportKind::Start, false, 96, o);
    REQUIRE(e.FrameOfBoundary(96) == 5000 + o);
    REQUIRE(e.Acc() > 0);
    REQUIRE(static_cast<uint64_t>(e.Acc()) <= e.P());
    REQUIRE(e.LastFired() == 95);
    const std::vector<GridHit> h = RenderTo(e, 5000 + o + 30000);
    REQUIRE_FALSE(h.empty());
    REQUIRE(h.front().position == 96);  // nothing before the anchor (E9)
    REQUIRE(h.front().frame == 5000 + o);
  }
}

// =================================================================================================
// §6.3 grid firing (E1)
// =================================================================================================

namespace {

// Draft v1's GridFrames (record §2.6): positions k > max(tick, lastFired) with F(k) < e, no
// catch-up. The perturbed control of E1.
int DraftV1Quarters(uint64_t P, int64_t total, int64_t block) {
  int64_t tick = -1, acc = static_cast<int64_t>(P), last = -1, s = 0;
  int     fired = 0;
  while (s < total) {
    const int64_t e = std::min(s + block, total);
    int64_t k = std::max(tick, last) + 1;
    for (;;) {
      const int64_t F = s + intmath::CeilDivI64((k - tick) * static_cast<int64_t>(P) - acc, kK);
      if (F >= e) break;
      if (k % 24 == 0) {
        ++fired;
        last = k;
      }
      ++k;
    }
    acc += (e - s) * kK;
    while (acc > static_cast<int64_t>(P)) {
      acc -= static_cast<int64_t>(P);
      ++tick;
    }
    s = e;
  }
  return fired;
}

int QuartersIn60s(uint32_t ns, int64_t block) {
  TempoCore c = MakeCore(48000);
  Tempo(c, 0, ns);
  const std::vector<GridHit> h = RenderTo(c, 48000 * 60, block);
  return static_cast<int>(h.size());
}

}  // namespace

TEST_CASE("Tempo grid (E1): quarters at 140 and 137.5 BPM at every block size", "[tempo][grid]") {
  for (int64_t block : {1, 48, 64, 441, 512}) {
    INFO("block " << block);
    REQUIRE(QuartersIn60s(428571429u, block) == 140);  // positions 0..139: 140 at 140 BPM
    REQUIRE(QuartersIn60s(436363636u, block) == 138);  // 137.5 BPM: 0..137
    REQUIRE(QuartersIn60s(500000000u, block) == 120);
  }
  // Draft v1's rule loses the hits whose boundary lies inside the frame before a span start.
  const uint64_t p140  = (uint64_t{48000} * 60 << 32) / 140;
  const uint64_t p1375 = (uint64_t{48000} * 60 * 2 << 32) / 275;
  CHECK(DraftV1Quarters(p140, 48000 * 60, 48) == 121);
  CHECK(DraftV1Quarters(p1375, 48000 * 60, 48) == 126);
  REQUIRE(DraftV1Quarters(p140, 48000 * 60, 48) < 140);
  REQUIRE(DraftV1Quarters(p140, 48000 * 60, 1) < 10);  // only the hit at frame 0, and a few exact
  REQUIRE(DraftV1Quarters((uint64_t{48000} * 60 << 32) / 120, 48000 * 60, 48) == 120);  // hid it
}

TEST_CASE("Tempo grid: constant tempo, every hit on ceil(k*P/K), every Subdiv and rate",
          "[tempo][grid]") {
  const double   bpms[]  = {20, 97, 137.5, 140, 300};
  const uint32_t rates[] = {8000, 44100, 48000, 96000, 384000};
  for (uint32_t R : rates) {
    for (double bpm : bpms) {
      for (uint8_t sd = 0; sd < tempo::kSubdivCodes; ++sd) {
        TempoCore c = MakeCore(R);
        Tempo(c, 0, NsOfBpm(bpm));
        c.ApplyEvent(0, tempo::kEventSubdivision,
                     tempo::SubdivisionId(SubdivField::Subdivision, sd), 0);
        const int64_t G = tempo::SubdivTicks(sd);
        const std::vector<GridHit> h = RenderTo(c, int64_t{R} * 8, 512);
        INFO("R=" << R << " bpm=" << bpm << " sd=" << int(sd));
        REQUIRE_FALSE(h.empty());
        for (size_t i = 0; i < h.size(); ++i) {
          REQUIRE(h[i].position == static_cast<int64_t>(i) * G);
          REQUIRE(h[i].frame == CeilKP(h[i].position, c.P()));
        }
      }
    }
  }
}

TEST_CASE("Tempo grid: a rescale or a tap that makes a position due fires it at the event frame",
          "[tempo][grid]") {
  // 120 BPM: boundary 24 at frame 24,000. Render to 24,000.5's frame: the span [23999, 24001) is
  // split by a tempo change at 24,000, where boundary 24 lies exactly: it fires there.
  TempoCore c = MakeCore(48000);
  std::vector<GridHit> h = RenderTo(c, 24000);
  REQUIRE(h.size() == 1);  // position 0
  Tempo(c, 24000, 400000000u);
  h = RenderTo(c, 24001, 1);
  REQUIRE(h.size() == 1);
  REQUIRE(h[0].position == 24);
  REQUIRE(h[0].frame == 24000);
  // A boundary inside the frame before the event (acc < K): the catch-up fires it at the event.
  TempoCore d = MakeCore(48000);
  Tempo(d, 0, NsOfBpm(140));  // 857.142857 frames a tick: boundary 24 at 20,571.43
  h = RenderTo(d, 20573);
  REQUIRE(h.size() == 2);
  REQUIRE(h[1].frame == 20572);
  TempoCore e = MakeCore(48000);
  Tempo(e, 0, NsOfBpm(140));
  RenderTo(e, 20572 - 1);
  RenderTo(e, 20572, 1);  // the span ends at 20,572: boundary 24 not yet fired
  REQUIRE(e.LastFired() == 0);
  Tempo(e, 20572, NsOfBpm(90));  // rescaled at the frame after the boundary
  h = RenderTo(e, 20573, 1);
  REQUIRE(h.size() == 1);
  REQUIRE(h[0].position == 24);
  REQUIRE(h[0].frame == 20572);
}

// =================================================================================================
// The streams: TempoCore against the reference, at every block pattern
// =================================================================================================


TEST_CASE("Tempo streams: TempoCore equals the reference at every block pattern",
          "[tempo][reference]") {
  const Family fams[] = {Family::Tempo, Family::Taps, Family::Clock, Family::Mixed};
  const char*  names[] = {"tempo", "taps", "clock", "mixed"};
  int checked = 0;
  TempoStats sum;  // what the streams exercised, summed
  for (int fi = 0; fi < 4; ++fi) {
    for (uint64_t seed = 1; seed <= static_cast<uint64_t>(kScale); ++seed) {
      const int seconds = fi == 2 || fi == 3 ? 40 : 30;
      const Stream s = MakeStream(fams[fi], seed * 7919 + static_cast<uint64_t>(fi), seconds);
      RunConfig cfg;
      std::string why;
      const bool ok = CoreMatchesReference(cfg, s.ev, int64_t{48000} * seconds, Perturb::None,
                                           false, &why);
      const std::string detail = ok ? std::string() : ExplainState(cfg, s.ev, int64_t{48000} * seconds);
      INFO(names[fi] << " seed " << seed << " (" << s.ev.size() << " events): " << why << " "
                     << detail);
      REQUIRE(ok);
      ++checked;
      const TempoStats t = RunCore(cfg, s.ev, int64_t{48000} * seconds, Blocks::Const(48)).stats;
      sum.taps += t.taps; sum.tapsIgnored += t.tapsIgnored; sum.tapPhases += t.tapPhases;
      sum.tempoEvents += t.tempoEvents; sum.tempoIgnored += t.tempoIgnored; sum.ticks += t.ticks;
      sum.tickOutliers += t.tickOutliers; sum.reacquires += t.reacquires;
      sum.dropoutTicks += t.dropoutTicks; sum.gaps += t.gaps; sum.losses += t.losses;
      sum.resumes += t.resumes; sum.transports += t.transports;
      sum.transportsCancelled += t.transportsCancelled;
      sum.transportsIgnored += t.transportsIgnored; sum.subdivEvents += t.subdivEvents;
      sum.commits += t.commits; sum.earlyCommits += t.earlyCommits; sum.jumps += t.jumps;
      sum.slews += t.slews; sum.invalidEvents += t.invalidEvents;
      sum.unknownEvents += t.unknownEvents;
    }
  }
  REQUIRE(checked == 4 * kScale);
  // Every rule ran: the minimums prove the streams reached it.
  INFO("taps " << sum.taps << " ignored " << sum.tapsIgnored << " phases " << sum.tapPhases
               << " tempo " << sum.tempoEvents << " ignored " << sum.tempoIgnored << " ticks "
               << sum.ticks << " outliers " << sum.tickOutliers << " reacquires " << sum.reacquires
               << " dropouts " << sum.dropoutTicks << " gaps " << sum.gaps << " losses "
               << sum.losses << " resumes " << sum.resumes << " transports " << sum.transports
               << " cancelled " << sum.transportsCancelled << " ignored "
               << sum.transportsIgnored << " subdiv " << sum.subdivEvents << " commits "
               << sum.commits << " early " << sum.earlyCommits << " jumps " << sum.jumps
               << " slews " << sum.slews << " invalid " << sum.invalidEvents << " unknown "
               << sum.unknownEvents);
  if (kScale < 10) return;  // Debug's tenth of the streams need not reach every rule
  CHECK(sum.tapsIgnored > 0);
  CHECK(sum.tapPhases > 0);
  CHECK(sum.tempoIgnored > 0);
  CHECK(sum.tickOutliers > 0);
  CHECK(sum.reacquires > 0);
  CHECK(sum.dropoutTicks > 0);
  CHECK(sum.losses > 0);
  CHECK(sum.resumes > 0);
  CHECK(sum.transportsCancelled > 0);
  CHECK(sum.transportsIgnored > 0);
  CHECK(sum.commits > 0);
  CHECK(sum.earlyCommits > 0);
  CHECK(sum.jumps > 0);
  CHECK(sum.slews > 0);
  CHECK(sum.invalidEvents > 0);
  CHECK(sum.unknownEvents > 0);
}

TEST_CASE("Tempo streams: a snapshot after the last event reads the gap", "[tempo][reference]") {
  // §2.6: the snapshot applies §3.5's gap predicate without mutating, so a display never shows a
  // clock that has gone, even when no event follows the gap to apply it. A probe more than a second
  // after the last tick, with nothing after it, under ClockFree (no transport) and ClockRunning
  // (FA first), and the same probe followed by an event that applies the gap.
  for (int variant = 0; variant < 4; ++variant) {
    const bool running = (variant & 1) != 0, later = (variant & 2) != 0;
    Stream s;
    Rng r(17);
    if (running) s.Transport(500, TransportKind::Start, true, 0);
    const int64_t after = s.Ticks(1000, 500000000u, 60, TickModel::Hardware, r);
    const int64_t lastTick = s.ev.back().frame;
    s.Probe(lastTick + 48000 - 1);  // just inside the second: still the clock's
    s.Probe(lastTick + 48000);      // at the deadline: Internal
    s.Probe(lastTick + 48000 + 700);
    if (later) s.Other(lastTick + 48000 + 900);
    const int64_t end = after + 48000 * 2;
    std::string why;
    const bool ok = CoreMatchesReference(RunConfig(), s.ev, end, Perturb::None, false, &why);
    INFO("variant " << variant << ": " << why);
    REQUIRE(ok);
    const RunResult got = RunCore(RunConfig(), s.ev, end, Blocks::Const(48));
    REQUIRE(got.probes.size() == 3);
    const auto clock = static_cast<uint8_t>(running ? ClockSource::ClockRunning
                                                    : ClockSource::ClockFree);
    REQUIRE(got.probes[0].source == clock);
    REQUIRE((got.probes[0].flags & kTempoFlagLocked) != 0);
    REQUIRE(((got.probes[0].flags & kTempoFlagRunning) != 0) == running);
    for (size_t k = 1; k < 3; ++k) {
      REQUIRE(got.probes[k].source == static_cast<uint8_t>(ClockSource::Internal));
      REQUIRE(got.probes[k].flags == 0);
    }
    REQUIRE(got.stats.gaps == (later ? 1u : 0u));  // the snapshot itself changes nothing
  }
}

TEST_CASE("Tempo streams: frame-by-frame reference on short streams", "[tempo][reference]") {
  for (uint64_t seed = 1; seed <= static_cast<uint64_t>(kScale); ++seed) {
    for (Family fam : {Family::Tempo, Family::Mixed}) {
      const Stream s = MakeStream(fam, seed * 104729 + 3, 4);
      RunConfig cfg;
      std::string why;
      const bool ok = CoreMatchesReference(cfg, s.ev, 48000 * 4, Perturb::None, /*literal=*/true,
                                           &why, false);
      const std::string detail = ok ? std::string() : ExplainState(cfg, s.ev, 48000 * 4);
      INFO("seed " << seed << ": " << why << " " << detail);
      REQUIRE(ok);
    }
  }
}

TEST_CASE("Tempo streams: other rates, stored states and the 2^32-frame boundary",
          "[tempo][reference]") {
  struct Case {
    uint32_t rate, us;
    uint8_t  tm, sd;
  };
  const Case cases[] = {{44100, 500000, 0, 0}, {8000, 200000, 1, 5}, {384000, 3000000, 2, 1},
                        {96000, 436364, 0, 3}};
  for (const Case& c : cases) {
    const Stream s = MakeStream(Family::Mixed, c.rate + c.us, 12, c.rate);
    RunConfig cfg;
    cfg.rate = c.rate;
    cfg.us = c.us;
    cfg.timeMode = c.tm;
    cfg.subdiv = c.sd;
    std::string why;
    INFO("rate " << c.rate);
    REQUIRE(CoreMatchesReference(cfg, s.ev, int64_t{c.rate} * 12, Perturb::None, false, &why));
  }
}

TEST_CASE("Tempo streams: the perturbed references are caught", "[tempo][reference][control]") {
  struct Ctl {
    Perturb p;
    Family  fam;
    const char* name;
  };
  const Ctl ctls[] = {
      {Perturb::NoCatchUp, Family::Tempo, "no catch-up (E1)"},
      {Perturb::RescaleTruncates, Family::Tempo, "rescale truncates"},
      {Perturb::TapMeanFloors, Family::Taps, "tap mean floors"},
      {Perturb::NoDownbeat, Family::Taps, "no downbeat (D16)"},
      {Perturb::StopKeepsArmed, Family::Clock, "Stop keeps an armed Start (E10)"},
      {Perturb::DropoutFloors, Family::Clock, "dropout count floors"},
      {Perturb::GapOnlyBeforeTempo, Family::Clock, "gap only before tempo events (E2)"},
      {Perturb::OutlierBound, Family::Clock, "outlier bound 11 ms"},
  };
  for (const Ctl& c : ctls) {
    bool caught = false;
    for (uint64_t seed = 1; seed <= 6 && !caught; ++seed) {
      const Stream s = MakeStream(c.fam, seed * 31 + static_cast<uint64_t>(c.p), 40);
      std::string why;
      if (!CoreMatchesReference(RunConfig(), s.ev, 48000 * 40, c.p, false, &why, false))
        caught = true;
    }
    INFO(c.name);
    REQUIRE(caught);
  }
}
