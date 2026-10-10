// The tempo core's rules case by case (docs/design/clock.md §8.2), each against numbers worked by
// hand from the design's text, independently of the reference model: the tap table at both
// thresholds' edges (§3.2), the follower (§3.3), transport (§3.4), gaps (§3.5), the commits and
// classes (§7.1), Restart (§2.5) and the payloads (§4.2). The cases named for a draft-v1 finding
// show draft v1's rule failing them.
#include <cmath>
#include <cstdint>
#include <vector>

#include "TempoStreams.h"
#include "WideInt.h"
#include "brainscape/Tempo.h"
#include "catch.hpp"
#include "detail/IntMath.h"
#include "detail/Tempo.h"

using namespace brainscape;
using namespace brainscape::testing;
using tempo::ClockSource;
using tempo::PcChange;
using tempo::SubdivField;
using tempo::TransportKind;

namespace {

constexpr int64_t  kK      = TempoCore::kK;
constexpr uint64_t kTwo32  = 1ull << 32;
constexpr uint64_t kP120   = 24000 * kTwo32;  // 120 BPM at 48 kHz: 1,000 frames a tick

TempoCore Core(uint32_t rate = 48000, uint32_t us = 500000, uint8_t tm = 0, uint8_t sd = 0) {
  TempoCore c;
  c.Init(rate);
  c.SetStoredPerformance(us, tm, sd);
  c.Restart();
  return c;
}

std::vector<GridHit> RenderTo(TempoCore& c, int64_t end, int64_t span = 48) {
  std::vector<GridHit> out;
  GridHit buf[TempoCore::kMaxClockPerSpan];
  while (c.Frame() < end) {
    const int64_t  e = c.Frame() + span < end ? c.Frame() + span : end;
    const uint32_t n = c.GridFrames(e, buf, TempoCore::kMaxClockPerSpan);
    out.insert(out.end(), buf, buf + n);
  }
  return out;
}

// Each helper renders up to the event's frame first, as the engine will.
void Tap(TempoCore& c, int64_t f) {
  RenderTo(c, f);
  REQUIRE(c.ApplyEvent(f, tempo::kEventTap, 0, 0));
}
void Tick(TempoCore& c, int64_t f) {
  RenderTo(c, f);
  REQUIRE(c.ApplyEvent(f, tempo::kEventClockTick, 0, 0));
}
void TempoEv(TempoCore& c, int64_t f, uint32_t ns) {
  RenderTo(c, f);
  REQUIRE(c.ApplyEvent(f, tempo::kEventTempo, ns, 0));
}
void Transport(TempoCore& c, int64_t f, TransportKind k, bool atNext, uint32_t pos = 0,
               uint32_t offset = 0) {
  RenderTo(c, f);
  const bool hasPos = k == TransportKind::Start || k == TransportKind::Locate;
  REQUIRE(c.ApplyEvent(f, tempo::kEventTransport, tempo::TransportId(k, atNext, offset),
                       hasPos ? tempo::IntegerValueBits(pos) : 0u));
}
void Subdiv(TempoCore& c, int64_t f, SubdivField field, uint8_t code) {
  RenderTo(c, f);
  REQUIRE(c.ApplyEvent(f, tempo::kEventSubdivision, tempo::SubdivisionId(field, code), 0));
}
// n ticks of `period` frames from `start` (no jitter); returns the frame after the last.
int64_t Ticks(TempoCore& c, int64_t start, int64_t period, int n) {
  for (int i = 0; i < n; ++i) Tick(c, start + i * period);
  return start + n * period;
}

}  // namespace

// =================================================================================================
// §3.2 tap
// =================================================================================================

TEST_CASE("Tap: the first tap arms; the second sets the tempo on a downbeat (D16)",
          "[tempo][tap]") {
  TempoCore c = Core();
  Tap(c, 36000);
  REQUIRE(c.P() == kP120);  // nothing changes
  REQUIRE(c.Tick() == 35);
  REQUIRE(c.Stats().taps == 1);
  // 24,000 frames later: 120 BPM again, but placed. At 60,000 boundary 60 lies exactly at the
  // frame: tick 59, acc = P, so x = 11P + P = 12P past beat 48: 2x ≥ 24P, the tie goes up to beat
  // 72; the first apply after an arm then moves it to the next position ≡ 24 (mod 96): 120. The
  // first tap, 24,000 frames back, is beat 96, a bar's first.
  Tap(c, 60000);
  REQUIRE(c.P() == 24000 * kTwo32);
  REQUIRE(c.Pc() == c.P());
  REQUIRE(c.Tick() == 119);
  REQUIRE(c.Acc() == static_cast<int64_t>(c.P()));
  REQUIRE(c.LastFired() == 119);
  const std::vector<GridHit> h = RenderTo(c, 84001);
  REQUIRE(h.size() == 2);
  REQUIRE(h[0].position == 120);
  REQUIRE(h[0].frame == 60000);
  REQUIRE(h[1].position == 144);
  REQUIRE(h[1].frame == 84000);
  // A later tap of the chain re-phases to the nearest beat only: a tap 24,480 frames on (2 %
  // slow) refines the mean to 24,240 and lands nearest beat 144, which fired 480 frames before
  // (no downbeat move, and nothing fires twice).
  Tap(c, 84480);
  REQUIRE(c.P() == 24240 * kTwo32);
  REQUIRE(c.Tick() == 143);
  REQUIRE(c.LastFired() == 144);
  REQUIRE(RenderTo(c, 84480 + 24240 + 1).size() == 1);  // beat 168, a quarter of the new tempo on
  REQUIRE(c.Stats().taps == 3);
  REQUIRE(c.Stats().tapsIgnored == 0);
}

TEST_CASE("Tap: the table's edges", "[tempo][tap]") {
  // A chain of one 24,000-frame interval, then the next tap at f + I.
  auto chainThen = [](int64_t I, TempoCore* out) {
    TempoCore c = Core();
    Tap(c, 48000);
    Tap(c, 72000);  // {24000}
    Tap(c, 72000 + I);
    *out = c;
  };
  TempoCore c;
  // A bounce: I < R/5 = 9,600 ignored, the last tap stays.
  chainThen(9599, &c);
  REQUIRE(c.Stats().tapsIgnored == 1);
  REQUIRE(c.P() == 24000 * kTwo32);
  REQUIRE(c.Capture().lastTap == 72000);
  // 9,600 is no bounce: 5·|9600 − 24000| = 72,000 > 48,000, a new tempo {9600}.
  chainThen(9600, &c);
  REQUIRE(c.Stats().tapsIgnored == 0);
  REQUIRE(c.P() == 9600 * kTwo32);
  REQUIRE(c.Capture().tapN == 1);
  // The 40 % rule: 5·|n·I − Σ| > 2·Σ. 33,600 and 14,400 are exactly 40 % off: the same tempo.
  chainThen(33600, &c);
  REQUIRE(c.Capture().tapN == 2);
  REQUIRE(c.P() == 28800 * kTwo32);
  chainThen(33601, &c);
  REQUIRE(c.Capture().tapN == 1);
  REQUIRE(c.P() == 33601 * kTwo32);
  chainThen(14400, &c);
  REQUIRE(c.Capture().tapN == 2);
  REQUIRE(c.P() == 19200 * kTwo32);
  chainThen(14399, &c);
  REQUIRE(c.Capture().tapN == 1);
  REQUIRE(c.P() == 14399 * kTwo32);
  // The pause: 4·n·I ≥ 7·Σ (I ≥ 1.75 × the mean) re-arms at f and changes nothing.
  chainThen(42000, &c);
  REQUIRE(c.Capture().tapN == 0);
  REQUIRE(c.Capture().lastTap == 114000);
  REQUIRE(c.P() == 24000 * kTwo32);
  chainThen(41999, &c);  // just below: more than 40 % off, a new tempo
  REQUIRE(c.Capture().tapN == 1);
  REQUIRE(c.P() == 41999 * kTwo32);
  // And above 3R (below 20 BPM), whatever the chain.
  {
    TempoCore d = Core();
    Tap(d, 1000);
    Tap(d, 1000 + 144001);
    REQUIRE(d.Capture().tapN == 0);
    REQUIRE(d.P() == kP120);
    Tap(d, 1000 + 144001 + 144000);  // exactly 3R: accepted, 20 BPM
    REQUIRE(d.Capture().tapN == 1);
    REQUIRE(d.P() == 144000 * kTwo32);
  }
  // The mean of the last four: RoundHalfUp((Σ << 32) / n).
  {
    TempoCore d = Core();
    int64_t f = 0;
    const int64_t ivs[] = {20000, 20001, 20003, 20007, 20013};
    Tap(d, f);
    for (int64_t iv : ivs) Tap(d, f += iv);
    REQUIRE(d.Capture().tapN == 4);
    const uint64_t sum = 20001 + 20003 + 20007 + 20013;
    REQUIRE(d.P() == (2 * (sum << 32) + 4) / 8);
  }
}

TEST_CASE("Tap: halving takes three taps, doubling two; a pause re-arms the downbeat",
          "[tempo][tap]") {
  TempoCore c = Core();
  Tap(c, 0);
  Tap(c, 24000);
  Tap(c, 48000);  // 120 BPM, {24000, 24000}
  Tap(c, 96000);  // 48,000: a pause (≥ 1.75 × the mean)
  REQUIRE(c.P() == 24000 * kTwo32);
  Tap(c, 144000);  // the third tap: 60 BPM, the downbeat again
  REQUIRE(c.P() == 48000 * kTwo32);
  REQUIRE(tempo::kPositionModulus % 96 == 0);
  REQUIRE(intmath::FloorModI64(c.Tick() + 1, 96) == 24);
  TempoCore d = Core();
  Tap(d, 0);
  Tap(d, 24000);
  Tap(d, 36000);  // 12,000: 50 % off, a new tempo at once
  REQUIRE(d.P() == 12000 * kTwo32);
}

TEST_CASE("Tap: the beat in the old units near a half beat (E11)", "[tempo][tap]") {
  // A chain on beat B2 at 120 BPM, then a new-tempo tap I frames later (more than 40 % fast):
  // the nearest beat is decided with the old P, x = I·K against 12·P_old, so 11,999 frames is
  // below half a beat and 12,000 the tie, which goes up. Reading x against the new P (I/2 frames)
  // would round both up.
  for (int64_t I : {11999, 12000}) {
    TempoCore c = Core();
    Tap(c, 36000);
    Tap(c, 60000);  // boundary 120 at 60,000 (the downbeat)
    REQUIRE(c.Tick() == 119);
    Tap(c, 60000 + I);
    REQUIRE(c.P() == static_cast<uint64_t>(I) * kTwo32);
    const int64_t correct = I < 12000 ? 120 : 144;
    REQUIRE(c.Tick() == correct - 1);
    // The perturbed reading, for contrast: x (I frames past beat 120, I·K) against the new P
    // rounds both up, 2·I·K ≥ 24·I·2^32 = I·K.
    const int64_t x = I * kK;
    const int64_t perturbed = 2 * x >= 24 * I * static_cast<int64_t>(kTwo32) ? 144 : 120;
    REQUIRE(perturbed == 144);
    if (I == 11999) REQUIRE(perturbed != correct);
  }
}

TEST_CASE("Tap: under ClockFree a tap marks the downbeat only; under ClockRunning it is ignored",
          "[tempo][tap]") {
  TempoCore c = Core();
  // 24 ticks at 125 BPM (960 frames a tick) and no transport: ClockFree.
  int64_t f = Ticks(c, 5000, 960, 24);
  REQUIRE(c.Source() == ClockSource::ClockFree);
  const uint64_t P = c.P(), Pc = c.Pc();
  RenderTo(c, f + 300);
  const int64_t tick = c.Tick(), acc = c.Acc(), last = c.LastFired();
  const int64_t kn = 2 * acc >= static_cast<int64_t>(P) ? tick + 1 : tick;
  const int64_t delta = 96 * intmath::FloorDivI64(kn + 48, 96) - kn;
  const int64_t label = c.Capture().lastLabel;
  Tap(c, f + 300);
  REQUIRE(c.Stats().tapPhases == 1);
  REQUIRE(c.P() == P);
  REQUIRE(c.Pc() == Pc);
  REQUIRE(c.Acc() == acc);
  REQUIRE(c.Tick() == tick + delta);
  REQUIRE(c.LastFired() == last + delta);
  REQUIRE(c.Capture().lastLabel == label + delta);
  REQUIRE(intmath::FloorModI64(kn + delta, 96) == 0);
  REQUIRE(delta >= -47);
  REQUIRE(delta <= 48);
  REQUIRE(c.Capture().tapN == 0);  // the tap chain untouched
  // Under ClockRunning: ignored and counted.
  Transport(c, f + 400, TransportKind::Start, true);
  f = Ticks(c, f + 500, 960, 3);
  REQUIRE(c.Source() == ClockSource::ClockRunning);
  const TempoCore::State before = c.Capture();
  Tap(c, f + 10);
  TempoCore::State after = c.Capture();
  after.frame = before.frame;
  after.tick = before.tick;
  after.acc = before.acc;
  REQUIRE(StateDigest(after) == StateDigest(before));
  REQUIRE(c.Stats().tapsIgnored == 1);
}

// =================================================================================================
// §3.3 the follower
// =================================================================================================

namespace {

// A stream of ticks through TempoCore; returns the core.
TempoCore PlayTicks(const Stream& s, int64_t end, uint32_t rate = 48000) {
  TempoCore c = Core(rate);
  GridHit buf[TempoCore::kMaxClockPerSpan];
  for (const StreamEvent& e : s.ev) {
    while (c.Frame() < e.frame) {
      const int64_t to = e.frame - c.Frame() > 512 ? c.Frame() + 512 : e.frame;
      c.GridFrames(to, buf, TempoCore::kMaxClockPerSpan);
    }
    RunResult sink;
    ApplyToCore(c, e, &sink);
  }
  while (c.Frame() < end) c.GridFrames(c.Frame() + 512 < end ? c.Frame() + 512 : end, buf, 4);
  return c;
}

double RelErr(uint64_t got, uint64_t want) {
  return (static_cast<double>(got) - static_cast<double>(want)) / static_cast<double>(want);
}

}  // namespace

TEST_CASE("Follower: lock and accuracy at 20, 120 and 300 BPM, hardware and computer jitter",
          "[tempo][follower]") {
  struct Case {
    double    bpm;
    TickModel model;
    double    bound;  // |P_fit / P − 1|, about five σ of the 96-tick fit (§3.3)
  };
  const Case cases[] = {{20, TickModel::Hardware, 0.0001},  {120, TickModel::Hardware, 0.00025},
                        {300, TickModel::Hardware, 0.0007}, {20, TickModel::Computer, 0.0006},
                        {120, TickModel::Computer, 0.003},  {300, TickModel::Computer, 0.008}};
  for (const Case& k : cases) {
    for (uint64_t seed = 1; seed <= 3; ++seed) {
      Stream s;
      Rng r(seed * 1000 + static_cast<uint64_t>(k.bpm));
      const uint32_t ns = static_cast<uint32_t>(60000000000.0 / k.bpm + 0.5);
      const int64_t end = s.Ticks(1000, ns, 400, k.model, r);
      const TempoCore c = PlayTicks(s, end);
      const uint64_t truth = tempo::PFromNs(ns, 48000);
      INFO("bpm " << k.bpm << " model " << int(k.model) << " seed " << seed << " P err "
                  << RelErr(c.P(), truth) << " Pc err " << RelErr(c.Pc(), truth));
      REQUIRE(c.Source() == ClockSource::ClockFree);
      REQUIRE(c.Stats().commits >= 1);
      REQUIRE(std::abs(RelErr(c.P(), truth)) < k.bound);
      // Pc within the deadband of P_fit, never further than the coarse band.
      REQUIRE(std::abs(RelErr(c.Pc(), c.P())) <= 1.0 / 512 + 1e-9);
      if (k.model == TickModel::Hardware) REQUIRE(c.Stats().tickOutliers == 0);
      // The commits after the acquisition's (§7.4's budget): see "[experiment]" below.
      INFO("bpm " << k.bpm << " model " << int(k.model) << " seed " << seed << ": commits "
                  << c.Stats().commits << " slews " << c.Stats().slews << " P err "
                  << RelErr(c.P(), truth));
    }
  }
}

// §7.4's commit budgets, measured: ten minutes of each clock model, commits counted after the
// window first fills (N = 96) and after the acquisition (N = 24). A diagnostic (hidden: run it
// with "[experiment]"), since the budgets are the golden counters' to hold (§8.3).
TEST_CASE("Follower: commits over ten minutes per clock model", "[.][experiment]") {
  for (TickModel m : {TickModel::Hardware, TickModel::Computer}) {
    for (double bpm : {60.0, 120.0, 140.0, 300.0}) {
      for (uint64_t seed = 1; seed <= 3; ++seed) {
        Stream s;
        Rng r(seed);
        const uint32_t ns = static_cast<uint32_t>(60000000000.0 / bpm + 0.5);
        const int ticks = static_cast<int>(bpm * 24 * 10);
        const int64_t end = s.Ticks(1000, ns, ticks, m, r);
        TempoCore c = Core();
        GridHit buf[4];
        uint64_t commitsAt24 = 0, commitsAt96 = 0, maxStepPpm = 0;
        uint64_t lastPc = c.Pc();
        bool full = false;
        const double truth = static_cast<double>(tempo::PFromNs(ns, 48000));
        double sq = 0;
        uint64_t nsq = 0, coarse = 0;
        for (const StreamEvent& e : s.ev) {
          while (c.Frame() < e.frame)
            c.GridFrames(e.frame - c.Frame() > 512 ? c.Frame() + 512 : e.frame, buf, 4);
          c.ApplyEvent(e.frame, e.type, e.id, e.value);
          if (c.Capture().winN >= 96) full = true;
          if (full) {
            const double rel = static_cast<double>(c.P()) / truth - 1.0;
            sq += rel * rel;
            ++nsq;
          }
          if (c.Pc() != lastPc) {
            if (c.Stats().commits > 1) {
              ++commitsAt24;
              if (full) ++commitsAt96;
              const uint64_t step =
                  (c.Pc() > lastPc ? c.Pc() - lastPc : lastPc - c.Pc()) * 1000000 / lastPc;
              if (step > maxStepPpm) maxStepPpm = step;
              if (step * 512 > 1000000) ++coarse;  // rule 3.1's band (Pc >> 9)
            }
            lastPc = c.Pc();
          }
        }
        (void)end;
        WARN("model " << int(m) << " bpm " << bpm << " seed " << seed << ": commits after N=24 "
                      << commitsAt24 << " (" << coarse << " by rule 3.1), after N=96 "
                      << commitsAt96 << ", largest " << maxStepPpm << " ppm; P_fit rms "
                      << std::sqrt(sq / (nsq ? nsq : 1)) * 100 << " %; outliers "
                      << c.Stats().tickOutliers << ", reacquires " << c.Stats().reacquires);
      }
    }
  }
}

TEST_CASE("Follower: acquisition at 24 ticks, the nearest tick's tie up", "[tempo][follower]") {
  // 120 BPM grid (boundary k at 1,000k). Ticks at 1,000j + 500 put the 24th at 23,500, where the
  // phasor has tick 23 and acc = P/2: the tie goes up, so the newest tick is labelled 24.
  TempoCore c = Core();
  for (int j = 0; j < 23; ++j) Tick(c, 1000 * j + 500);
  REQUIRE(c.Source() == ClockSource::Internal);
  RenderTo(c, 23500);
  REQUIRE(c.Tick() == 23);
  REQUIRE(2 * c.Acc() == static_cast<int64_t>(c.P()));
  Tick(c, 23500);
  REQUIRE(c.Source() == ClockSource::ClockFree);
  REQUIRE(c.Capture().lastLabel == 24);
  // Placed on the fitted line, exactly 1,000 frames a tick: boundary 24 at 23,500.
  REQUIRE(c.P() == kP120);
  REQUIRE(c.FrameOfBoundary(25) == 24500);
  REQUIRE(c.Stats().commits == 1);
  REQUIRE(c.Pc() == kP120);
}

TEST_CASE("Follower: a tempo step re-acquires within six outliers", "[tempo][follower]") {
  TempoCore c = Core();
  int64_t f = Ticks(c, 1000, 1000, 120);  // 120 BPM, locked
  REQUIRE(c.Source() == ClockSource::ClockFree);
  REQUIRE(c.Stats().tickOutliers == 0);
  // 150 BPM: 800 frames a tick. Each tick falls 200 frames further ahead of the line.
  int n = 0;
  while (c.Stats().reacquires == 0 && n < 40) {
    Tick(c, f + 800 * n);
    ++n;
  }
  REQUIRE(c.Stats().reacquires == 1);
  REQUIRE(c.Stats().tickOutliers == 6);  // six in a row, of one sign
  REQUIRE(c.Capture().winN == 6);        // the ring
  // Then 24 ticks commit the new tempo (an acquisition commit, a Jump).
  const uint32_t serial = c.PcSerial();
  for (int i = 0; i < 30; ++i) Tick(c, f + 800 * (n + i));
  REQUIRE(c.Pc() == 19200 * kTwo32);
  REQUIRE(c.PcSerial() > serial);
  REQUIRE(c.Stats().jumps >= 1);
}

TEST_CASE("Follower: a 200 ms dropout infers its ticks; a 38 ms burst does not",
          "[tempo][follower]") {
  {
    TempoCore c = Core();
    int64_t f = Ticks(c, 1000, 1000, 100);
    const int64_t label = c.Capture().lastLabel;
    // 200 ms without ticks: nine ideal ticks missing (f .. f + 8,000), the next at f + 9,000.
    Tick(c, f + 9000);
    REQUIRE(c.Stats().dropoutTicks == 9);
    REQUIRE(c.Capture().lastLabel == label + 10);
    REQUIRE(c.Stats().tickOutliers == 0);
  }
  {
    TempoCore c = Core();
    int64_t f = Ticks(c, 1000, 1000, 100);
    const int64_t label = c.Capture().lastLabel;
    Tick(c, f + 1843);  // a held tick, 38.4 ms late
    Tick(c, f + 1843);  // the next bunched behind it
    Tick(c, f + 2000);
    REQUIRE(c.Stats().dropoutTicks == 0);
    REQUIRE(c.Stats().reacquires == 0);
    // The held one, and the bunched one 843 frames late: both past a quarter tick and 10 ms.
    REQUIRE(c.Stats().tickOutliers == 2);
    REQUIRE(c.Capture().lastLabel == label + 3);
  }
}

TEST_CASE("Follower: ticks at one frame and then a slow gap infer nothing, divide by nothing (E5)",
          "[tempo][follower]") {
  TempoCore c = Core();
  for (int i = 0; i < 30; ++i) Tick(c, 5000);  // every tick at one frame: A = 0
  TempoCore::State s = c.Capture();
  REQUIRE(s.winN == 30);
  REQUIRE(s.a == 0);
  REQUIRE_FALSE(s.fitValid);
  REQUIRE(c.Source() == ClockSource::Internal);  // no acquisition without a slope
  Tick(c, 5000 + 47000);  // 0.98 s later, below the gap
  REQUIRE(c.Stats().dropoutTicks == 0);
  REQUIRE(c.Stats().gaps == 0);
  // A valid fit of eight, then many ticks at one frame and a slow gap: the clamped P_fit infers.
  TempoCore d = Core();
  int64_t f = Ticks(d, 1000, 6000, 10);  // 20 BPM
  for (int i = 0; i < 5; ++i) Tick(d, f);
  REQUIRE(d.Capture().fitValid);
  Tick(d, f + 47000);
  REQUIRE(d.Stats().dropoutTicks < 120);
}

TEST_CASE("Follower: incremental sums equal direct sums (H4)", "[tempo][follower]") {
#if defined(NDEBUG)
  const int kTicks = 1000000;
#else
  const int kTicks = 100000;
#endif
  TempoCore c = Core();
  Rng r(77);
  GridHit buf[4];
  int64_t f = 1000;
  int checked = 0;
  for (int i = 0; i < kTicks; ++i) {
    // Ticks 300-900 frames apart, 2 % dropouts of 1-5 ticks' time, 1 % outliers.
    int64_t step = r.Range(300, 900);
    if (r.Chance(2)) step += r.Range(8, 20) * 600;  // 100-250 ms: inferred
    if (r.Chance(1)) step += r.Range(-250, 250);
    if (step < 0) step = 0;
    f += step;
    while (c.Frame() < f) c.GridFrames(f - c.Frame() > 512 ? c.Frame() + 512 : f, buf, 4);
    c.ApplyEvent(f, tempo::kEventClockTick, 0, 0);
    if (i % 7 != 0) continue;
    const TempoCore::State s = c.Capture();
    int64_t sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (uint32_t k = 0; k < s.winN; ++k) {
      const int64_t x = s.winLabel[k] - s.winLabel[0], y = s.winFrame[k] - s.winFrame[0];
      if (x < 0 || x > 95) FAIL("window label out of range");
      sx += x;
      sy += y;
      sxx += x * x;
      sxy += x * y;
    }
    if (sx != s.sx || sy != s.sy || sxx != s.sxx || sxy != s.sxy) {
      INFO("tick " << i);
      REQUIRE(sx == s.sx);
      REQUIRE(sy == s.sy);
      REQUIRE(sxx == s.sxx);
      REQUIRE(sxy == s.sxy);
    }
    if (s.winN >= 2) {
      const int64_t N = s.winN;
      if (s.d != N * sxx - sx * sx || s.a != N * sxy - sx * sy || s.b != sy * sxx - sx * sxy)
        FAIL("fit numerators differ at tick " << i);
    }
    ++checked;
  }
  REQUIRE(checked > kTicks / 8);
  REQUIRE(c.Stats().dropoutTicks > 0);
  REQUIRE(c.Stats().tickOutliers > 0);
}

TEST_CASE("Follower: rule 3.2's run of 192 fitted ticks, reset by an outlier", "[tempo][follower]") {
  // Lock at 1,000 frames a tick, then 1,000.6: 0.06 %, between Pc >> 12 (0.024 %) and Pc >> 9
  // (0.195 %), so only rule 3.2 commits, after 192 consecutive fitted ticks outside the band.
  auto run = [](bool outlier, int64_t* commitTick) {
    TempoCore c = Core();
    int64_t f = Ticks(c, 1000, 1000, 100);
    REQUIRE(c.Stats().commits == 1);
    const uint32_t serial = c.PcSerial();
    int64_t ideal = f * 10;  // in tenths of a frame
    *commitTick = -1;
    for (int i = 0; i < 800 && *commitTick < 0; ++i) {
      int64_t at = ideal / 10;
      if (outlier && i == 150) at += 1000;  // a tick 1,000 frames late: an outlier
      Tick(c, at);
      ideal += 10006;
      if (c.PcSerial() != serial) {
        *commitTick = i;
        REQUIRE(c.LastPcChange() == PcChange::Drift);
        REQUIRE(c.Stats().slews == 1);
      }
    }
  };
  int64_t plain, reset;
  run(false, &plain);
  run(true, &reset);
  REQUIRE(plain > 0);
  REQUIRE(reset > plain);  // the outlier restarted the run
  REQUIRE(reset >= 150 + 192);
}

// =================================================================================================
// §3.4 transport
// =================================================================================================

TEST_CASE("Transport: Start and Continue at the next tick; Stop's continue position",
          "[tempo][transport]") {
  TempoCore c = Core();
  Transport(c, 900, TransportKind::Start, true, 0);  // FA: armed
  REQUIRE(c.Source() == ClockSource::Internal);
  REQUIRE(c.Capture().armed);
  Tick(c, 1000);  // the next tick: label 0, ClockRunning
  REQUIRE(c.Source() == ClockSource::ClockRunning);
  REQUIRE(c.Capture().lastLabel == 0);
  REQUIRE(c.LastFired() == -1);
  REQUIRE(c.FrameOfBoundary(0) == 1000);  // N < 2: the tick's own frame is its boundary
  std::vector<GridHit> h = RenderTo(c, 1001);
  REQUIRE(h.size() == 1);
  REQUIRE(h[0].position == 0);
  REQUIRE(h[0].frame == 1000);
  REQUIRE(c.Capture().earlyArmed);  // the window was empty before that tick
  int64_t f = Ticks(c, 2000, 1000, 40);
  REQUIRE(c.Capture().lastLabel == 40);
  // Stop: Internal, phase and tempo kept, continue position = last label + 1.
  const uint64_t P = c.P(), Pc = c.Pc();
  Transport(c, f, TransportKind::Stop, true);
  REQUIRE(c.Source() == ClockSource::Internal);
  REQUIRE(c.P() == P);
  REQUIRE(c.Pc() == Pc);
  REQUIRE(c.Capture().continuePosition == 41);
  REQUIRE(c.Capture().masterStopped);
  // Ticks while stopped keep the window warm and drive nothing.
  f = Ticks(c, f + 500, 1000, 30);
  REQUIRE(c.Source() == ClockSource::Internal);
  REQUIRE(c.Capture().winN >= 24);
  // Continue at the next tick: that tick is labelled 41, lastFired 40, ClockRunning.
  Transport(c, f, TransportKind::Continue, true);
  Tick(c, f + 100);
  REQUIRE(c.Source() == ClockSource::ClockRunning);
  REQUIRE(c.Capture().lastLabel == 41);
  REQUIRE(c.LastFired() == 40);
  REQUIRE_FALSE(c.Capture().masterStopped);
  REQUIRE(c.Stats().commits >= 2);  // a Continue on a warm window commits at once
}

TEST_CASE("Transport: Song Position while stopped and while running", "[tempo][transport]") {
  TempoCore c = Core();
  // While stopped (Internal): the continue position at once.
  Transport(c, 100, TransportKind::Locate, true, 6 * 32);
  REQUIRE(c.Capture().continuePosition == 192);
  REQUIRE_FALSE(c.Capture().armed);
  Transport(c, 200, TransportKind::Continue, true);
  Tick(c, 1000);
  REQUIRE(c.Capture().lastLabel == 192);
  REQUIRE(c.LastFired() == 191);
  int64_t f = Ticks(c, 2000, 1000, 30);
  // While running: armed, applied at the next tick as Start.
  Transport(c, f, TransportKind::Locate, true, 6 * 64);
  REQUIRE(c.Capture().armed);
  Tick(c, f + 1000);
  REQUIRE(c.Capture().lastLabel == 384);
  REQUIRE(c.LastFired() == 383);
  REQUIRE(c.Source() == ClockSource::ClockRunning);
}

TEST_CASE("Transport: FA, FC, F8 starts nothing (E10); Stop cancels and counts",
          "[tempo][transport]") {
  TempoCore c = Core();
  Transport(c, 100, TransportKind::Start, true, 0);
  Transport(c, 200, TransportKind::Stop, true);
  REQUIRE_FALSE(c.Capture().armed);
  REQUIRE(c.Stats().transportsCancelled == 1);
  Ticks(c, 1000, 1000, 60);  // a master that sends clock while stopped
  REQUIRE(c.Source() == ClockSource::Internal);
  REQUIRE(c.Capture().masterStopped);
  REQUIRE_FALSE(c.Capture().running);
}

TEST_CASE("Transport: host events anchor, and are ignored under clock (E9)", "[tempo][transport]") {
  TempoCore c = Core();
  // A host Start at the block's first frame, position 96, its boundary 300 frames in.
  RenderTo(c, 5000);
  Transport(c, 5000, TransportKind::Start, false, 96, 300);
  const std::vector<GridHit> h = RenderTo(c, 40000);
  REQUIRE(h.front().position == 96);  // no hit before the anchor
  REQUIRE(h.front().frame == 5300);
  REQUIRE(c.Capture().running);
  Transport(c, 40000, TransportKind::Stop, false);
  REQUIRE_FALSE(c.Capture().running);
  // Under clock, host events are ignored and counted.
  TempoCore d = Core();
  int64_t f = Ticks(d, 1000, 1000, 30);
  REQUIRE(d.Source() == ClockSource::ClockFree);
  RenderTo(d, f);
  const int64_t tick = d.Tick();
  Transport(d, f, TransportKind::Start, false, 0, 10);
  Transport(d, f, TransportKind::Locate, false, 480);
  Transport(d, f, TransportKind::Stop, false);
  REQUIRE(d.Stats().transportsIgnored == 3);
  REQUIRE(d.Tick() == tick);
  REQUIRE(d.Source() == ClockSource::ClockFree);
}

TEST_CASE("Transport: the early commit at N = 12 after a Start on an empty window (P4)",
          "[tempo][transport]") {
  // Pc at 120 BPM; the song starts at 150 BPM (25 % away, above Pc >> 4).
  TempoCore c = Core();
  Transport(c, 500, TransportKind::Start, true);
  int64_t f = 1000;
  for (int i = 0; i < 11; ++i) Tick(c, f + 800 * i);
  REQUIRE(c.Pc() == kP120);
  Tick(c, f + 800 * 11);  // the 12th
  REQUIRE(c.Stats().earlyCommits == 1);
  REQUIRE(c.Pc() == 19200 * kTwo32);
  REQUIRE(c.LastPcChange() == PcChange::Jump);
  // Within 6.25 %: no early commit, Pc waits for N = 24.
  TempoCore d = Core();
  Transport(d, 500, TransportKind::Start, true);
  for (int i = 0; i < 12; ++i) Tick(d, f + 960 * i);  // 125 BPM, 4 %
  REQUIRE(d.Stats().earlyCommits == 0);
  REQUIRE(d.Pc() == kP120);
  for (int i = 12; i < 24; ++i) Tick(d, f + 960 * i);
  REQUIRE(d.Pc() == 23040 * kTwo32);
  REQUIRE(d.Stats().commits == 1);
}

// =================================================================================================
// §3.5 gaps
// =================================================================================================

TEST_CASE("Gaps: the snapshot shows Internal after a gap with no event (E2)", "[tempo][gap]") {
  TempoCore c = Core();
  int64_t f = Ticks(c, 1000, 1000, 30);
  const int64_t last = f - 1000;
  REQUIRE(c.Info().source == static_cast<uint8_t>(ClockSource::ClockFree));
  REQUIRE((c.Info().flags & kTempoFlagLocked) != 0);
  RenderTo(c, last + 47999);  // just short of a second since the last tick
  REQUIRE(c.Info().source == static_cast<uint8_t>(ClockSource::ClockFree));
  RenderTo(c, last + 48000);
  REQUIRE(c.Info().source == static_cast<uint8_t>(ClockSource::Internal));
  REQUIRE((c.Info().flags & kTempoFlagLocked) == 0);
  REQUIRE(c.Source() == ClockSource::ClockFree);  // read-only: nothing applied yet
  REQUIRE(c.Stats().gaps == 0);
  // A Spillover load under recall Preset after the lost clock sees the gap first, so the stored
  // tempo applies (draft v1 saw the clock still).
  RenderTo(c, f + 60000);
  c.SpilloverLoad(f + 60000, 400000, 0, 0, true);
  REQUIRE(c.Stats().gaps == 1);
  REQUIRE(c.Stats().losses == 1);
  REQUIRE(c.Source() == ClockSource::Internal);
  REQUIRE(c.Pc() == tempo::PFromNs(400000000u, 48000));
}

TEST_CASE("Gaps: a window cleared under Internal after a clock-silent stop (P4)", "[tempo][gap]") {
  TempoCore c = Core();
  Transport(c, 500, TransportKind::Start, true);
  int64_t f = Ticks(c, 1000, 1000, 50);
  Transport(c, f, TransportKind::Stop, true);
  REQUIRE(c.Capture().winN == 50);
  // The master stops sending clock: the next event of any type clears the window.
  RenderTo(c, f + 49000);
  c.BeforeEvent(f + 49000);
  REQUIRE(c.Capture().winN == 0);
  REQUIRE(c.Stats().gaps == 1);
  REQUIRE(c.Stats().losses == 0);  // Internal already
}

TEST_CASE("Gaps: a loss mid-song resumes ClockRunning after 24 ticks", "[tempo][gap]") {
  TempoCore c = Core();
  Transport(c, 500, TransportKind::Start, true);
  int64_t f = Ticks(c, 1000, 1000, 60);
  REQUIRE(c.Source() == ClockSource::ClockRunning);
  // 1.5 s without clock, then the ticks come back on the master's grid.
  const int64_t back = f - 1000 + 72000;
  RenderTo(c, back);
  const int64_t expectLabel = c.Tick() + 1;  // the phasor ran on at P_fit: the nearest tick
  for (int i = 0; i < 23; ++i) Tick(c, back + 1000 * i);
  REQUIRE(c.Source() == ClockSource::Internal);
  REQUIRE(c.Stats().losses == 1);
  REQUIRE(c.Capture().resumeRunning);
  Tick(c, back + 23000);
  REQUIRE(c.Source() == ClockSource::ClockRunning);
  REQUIRE(c.Stats().resumes == 1);
  REQUIRE(c.Capture().lastLabel == expectLabel + 23);
}

TEST_CASE("Gaps: ticks resumed after twelve hours at 384 kHz overflow nothing", "[tempo][gap]") {
  TempoCore c = Core(384000);
  const int64_t tick = 8000;  // 120 BPM at 384 kHz
  int64_t f = Ticks(c, 1000, tick, 200);
  REQUIRE(c.Source() == ClockSource::ClockFree);
  // Twelve hours of silence, rendered in one-second spans (two quarters each).
  GridHit buf[8];
  const int64_t later = f + int64_t{384000} * 3600 * 12;
  while (c.Frame() < later)
    c.GridFrames(c.Frame() + 384000 < later ? c.Frame() + 384000 : later, buf, 8);
  REQUIRE(later > (int64_t{1} << 33));
  f = Ticks(c, later, tick, 200);
  REQUIRE(c.Source() == ClockSource::ClockFree);
  REQUIRE(c.P() == 192000 * kTwo32);
  REQUIRE(c.Stats().gaps == 1);
  const TempoCore::State s = c.Capture();
  REQUIRE(s.winN == 96);
  REQUIRE(s.winFrame[95] - s.winFrame[0] == 95 * tick);
  // And a continuous clock across the 2^32-frame boundary of the window's 32-bit storage.
  TempoCore d = Core(384000);
  const int64_t start = (int64_t{1} << 32) - 50 * tick;
  while (d.Frame() < start)
    d.GridFrames(d.Frame() + 384000 < start ? d.Frame() + 384000 : start, buf, 8);
  Ticks(d, start, tick, 200);
  REQUIRE(d.P() == 192000 * kTwo32);
  REQUIRE(d.Capture().winFrame[95] - d.Capture().winFrame[0] == 95 * tick);
}

// =================================================================================================
// §2.5 Restart (E3), §7.1 classes, §4.2 payloads
// =================================================================================================

TEST_CASE("Restart equals Init and an Exact load of the active preset (E3)", "[tempo][restart]") {
  // A tapped tempo, a fitted clock, a Stop and live Subdivision events, after a Spillover load
  // that made a preset with its own performance state the active one.
  TempoCore c = Core();
  c.SpilloverLoad(10, 437500, 1, 3, false);
  Tap(c, 1000);
  Tap(c, 25000);
  Transport(c, 26000, TransportKind::Start, true);
  int64_t f = Ticks(c, 27000, 900, 30);
  f = Ticks(c, f, 901, 10);  // a drift inside the deadband: P_fit moves, Pc does not
  Transport(c, f, TransportKind::Stop, true);
  Subdiv(c, f + 10, SubdivField::Subdivision, 5);
  Subdiv(c, f + 20, SubdivField::TimeMode, 2);
  REQUIRE(c.P() != c.Pc());  // P and Pc differ after a Stop
  const TempoStats counters = c.Stats();
  c.Restart();
  TempoCore d;
  d.Init(48000);
  d.SetStoredPerformance(437500, 1, 3);  // load step 4, then step 5's Restart
  d.Restart();
  REQUIRE(StateDigest(c.Capture()) == StateDigest(d.Capture()));
  REQUIRE(c.TimeMode() == 1);
  REQUIRE(c.Subdiv() == 3);
  REQUIRE(c.Pc() == tempo::PFromNs(437500000u, 48000));
  REQUIRE(StatsDigest(c.Stats()) == StatsDigest(counters));  // Restart keeps the counters
}

TEST_CASE("Classes: each change of Pc lands in its class (§7.1)", "[tempo][classes]") {
  // A tap starting a new chain: a Jump. A tap refining its chain: a Step.
  TempoCore c = Core();
  Tap(c, 0);
  Tap(c, 30000);  // 96 BPM from 120: 20 %
  REQUIRE(c.LastPcChange() == PcChange::Jump);
  Tap(c, 60300);  // 1 % slow: the chain refined, the mean 0.5 % slower
  REQUIRE(c.LastPcChange() == PcChange::Step);
  Tap(c, 60300 + 15000);  // a new chain at twice the tempo
  REQUIRE(c.LastPcChange() == PcChange::Jump);
  REQUIRE(c.Stats().jumps == 2);
  // A Tempo-knob step of 1 %: a Step; a host jump: a Jump; Pc >> 5 is the edge.
  TempoCore d = Core();
  const uint64_t p0 = d.Pc();
  TempoEv(d, 100, 505000000u);
  REQUIRE(d.LastPcChange() == PcChange::Step);
  TempoEv(d, 200, 400000000u);
  REQUIRE(d.LastPcChange() == PcChange::Jump);
  // A recall under Preset: a Jump; a drift commit: a Drift (Follower test above).
  d.SpilloverLoad(300, 600000, 0, 0, true);
  REQUIRE(d.LastPcChange() == PcChange::Jump);
  const uint64_t jumps = d.Stats().jumps;
  REQUIRE(jumps == 2);
  (void)p0;
}

TEST_CASE("Payloads: every invalid form ignored and counted; unknown types counted",
          "[tempo][payload]") {
  TempoCore c = Core();
  Tick(c, 10);  // something to move
  const uint64_t before = StateDigest(c.Capture());
  struct Bad {
    uint8_t  type;
    uint32_t id, value;
  };
  const uint32_t pos = tempo::IntegerValueBits(48);
  const Bad bad[] = {
      {6, 1, 0},           {6, 0, 0x80000000u},  {6, 0, 0x3F800000u},
      {7, 199999999u, 0},  {7, 3000000001u, 0},  {7, 500000000u, 0x3F800000u},
      {8, 2, 0},           {8, 0, 1},
      {9, tempo::TransportId(TransportKind::Start, true, 1), pos},     // an offset with AtNextTick
      {9, tempo::TransportId(TransportKind::Stop, false, 1), 0},       // an offset on a Stop
      {9, tempo::TransportId(TransportKind::Stop, true) | 0x4u, 0},    // reserved bit 2
      {9, tempo::TransportId(TransportKind::Stop, true) | 0x200u, 0},  // reserved bit 9
      {9, tempo::TransportId(TransportKind::Start, false), 0x3FC00000u},  // 1.5
      {9, tempo::TransportId(TransportKind::Start, false), 0xBF800000u},  // −1
      {9, tempo::TransportId(TransportKind::Locate, false), 0x80000000u},  // −0
      {9, tempo::TransportId(TransportKind::Locate, true), tempo::IntegerValueBits(6291456u)},
      {9, tempo::TransportId(TransportKind::Locate, true), 0x7FC00000u},  // NaN
      {9, tempo::TransportId(TransportKind::Continue, true), pos},     // a value on Continue
      {9, tempo::TransportId(TransportKind::Stop, true), pos},         // and on Stop
      {10, tempo::SubdivisionId(SubdivField::Subdivision, 6), 0},
      {10, tempo::SubdivisionId(SubdivField::TimeMode, 3), 0},
      {10, 0x200u, 0},
      {10, 0x10000u, 0},
      {10, 0, 0x3F800000u},
  };
  int n = 0;
  for (const Bad& b : bad) {
    INFO("case " << n);
    REQUIRE_FALSE(c.ApplyEvent(c.Frame(), b.type, b.id, b.value));
    ++n;
  }
  REQUIRE(c.Stats().invalidEvents == static_cast<uint64_t>(n));
  REQUIRE(StateDigest(c.Capture()) == before);
  for (int t = 11; t < 256; ++t) c.CountUnknownEvent(c.Frame());
  REQUIRE(c.Stats().unknownEvents == 245);
  REQUIRE(StateDigest(c.Capture()) == before);
  // And the valid edges.
  REQUIRE(c.ApplyEvent(c.Frame(), 7, tempo::kMinNsPerQuarter, 0));
  REQUIRE(c.ApplyEvent(c.Frame(), 7, tempo::kMaxNsPerQuarter, 0));
  REQUIRE(c.ApplyEvent(c.Frame(), 9, tempo::TransportId(TransportKind::Locate, false, 0xFFFF),
                       tempo::IntegerValueBits(6291455u)));
  REQUIRE(c.ApplyEvent(c.Frame(), 10, tempo::SubdivisionId(SubdivField::TimeMode, 2), 0));
}

TEST_CASE("Effective subdivision: Tempo time mode forces TAP; leaving it restores the kept value",
          "[tempo][subdiv]") {
  TempoCore c = Core();
  Subdiv(c, 10, SubdivField::Subdivision, 1);  // ×1/4: whole notes
  REQUIRE(c.GridTicks() == 96);
  Subdiv(c, 20, SubdivField::TimeMode, 2);
  REQUIRE(c.EffectiveSubdiv() == 0);
  REQUIRE(c.GridTicks() == 24);
  REQUIRE(c.Subdiv() == 1);
  Subdiv(c, 30, SubdivField::TimeMode, 1);
  REQUIRE(c.GridTicks() == 96);
}

// =================================================================================================
// §3.3's bounds at their extremes (overflow, division by zero), hours of running
// =================================================================================================

namespace {

// The window's sums against direct sums; the fit's numerators against the definitions.
bool SumsExact(const TempoCore::State& s) {
  int64_t sx = 0, sy = 0, sxx = 0, sxy = 0;
  for (uint32_t k = 0; k < s.winN; ++k) {
    const int64_t x = s.winLabel[k] - s.winLabel[0], y = s.winFrame[k] - s.winFrame[0];
    if (x < 0 || x > 95 || y < 0) return false;
    sx += x;
    sy += y;
    sxx += x * x;
    sxy += x * y;
  }
  if (sx != s.sx || sy != s.sy || sxx != s.sxx || sxy != s.sxy) return false;
  if (s.winN < 2) return true;
  const int64_t N = s.winN;
  return s.d == N * sxx - sx * sx && s.a == N * sxy - sx * sy && s.b == sy * sxx - sx * sxy;
}

}  // namespace

TEST_CASE("Follower: the bounds' extremes at 384 kHz and 8 kHz", "[tempo][follower][bounds]") {
  // Ticks just under a second apart at 384 kHz (the gap rule's limit): the slope clamps to 20 BPM
  // and inference labels the gaps; y stays inside int32 and nothing asserts (Debug checks every
  // multiply-divide's range).
  {
    TempoCore c = Core(384000);
    int64_t f = 1000;
    for (int i = 0; i < 300; ++i) {
      Tick(c, f);
      f += 383999 - (i % 7) * 1000;
      REQUIRE(SumsExact(c.Capture()));
    }
    REQUIRE(c.Stats().gaps == 0);
    REQUIRE(c.P() <= tempo::PFromNs(tempo::kMaxNsPerQuarter, 384000));
    REQUIRE(c.P() >= tempo::PFromNs(tempo::kMinNsPerQuarter, 384000));
  }
  // The fastest: 300 BPM at 8 kHz, 66.67 frames a tick, ×8: grid points 200 frames apart, never
  // more than kMaxClockPerSpan in a 512-frame span.
  {
    TempoCore c = Core(8000);
    Subdiv(c, 0, SubdivField::Subdivision, 5);
    Rng r(9);
    int64_t ideal3 = 3000;  // in thirds of a frame
    uint32_t maxHits = 0;
    GridHit buf[TempoCore::kMaxClockPerSpan];
    for (int i = 0; i < 3000; ++i) {
      const int64_t f = ideal3 / 3;
      while (c.Frame() < f) {
        const int64_t e = std::min<int64_t>(c.Frame() + r.Range(1, 512), f);
        const uint32_t n = c.GridFrames(e, buf, TempoCore::kMaxClockPerSpan);
        if (n > maxHits) maxHits = n;
      }
      c.ApplyEvent(f, tempo::kEventClockTick, 0, 0);
      ideal3 += 200;
    }
    REQUIRE(c.Source() == ClockSource::ClockFree);
    REQUIRE(c.P() == tempo::PFromNs(tempo::kMinNsPerQuarter, 8000));
    REQUIRE(maxHits <= TempoCore::kMaxClockPerSpan);
    REQUIRE(SumsExact(c.Capture()));
  }
}

TEST_CASE("Follower: hours of continuous clock at 384 kHz across 2^31 and 2^32 frames",
          "[tempo][follower][bounds]") {
#if defined(NDEBUG)
  const int64_t hours = 4;
#else
  const int64_t hours = 1;
#endif
  TempoCore c = Core(384000);
  Transport(c, 100, TransportKind::Start, true);
  GridHit buf[8];
  const int64_t tick = 8000;  // 120 BPM
  const int64_t ticks = hours * 3600 * 48;
  int64_t hits = 0;
  bool exact = true;
  for (int64_t i = 0; i < ticks; ++i) {
    const int64_t f = 1000 + i * tick;
    while (c.Frame() < f) hits += c.GridFrames(f, buf, 8);
    c.ApplyEvent(f, tempo::kEventClockTick, 0, 0);
    if (i % 4096 == 0 && !SumsExact(c.Capture())) exact = false;
  }
  REQUIRE(exact);
  REQUIRE(c.Source() == ClockSource::ClockRunning);
  REQUIRE(c.Stats().reacquires == 0);
  REQUIRE(c.Stats().tickOutliers == 0);
  REQUIRE(c.Stats().commits == 1);
  REQUIRE(c.P() == 192000 * kTwo32);
  REQUIRE(c.Capture().lastLabel == ticks - 1);
  // One hit per quarter from the Start tick to the last tick (Init's position 0 at frame 0 fired
  // in the Transport helper's render, not counted here).
  REQUIRE(hits == ticks / 24);
  if (hours >= 4) REQUIRE(c.Frame() > (int64_t{1} << 32));
}

TEST_CASE("TempoCore fits §2.6's Warm-arena estimate", "[tempo][placement]") {
  // §2.6: the follower's window (96 × two 32-bit words), the 6-tick ring, the tap chain and the
  // scalars, about 1.1 KiB, plus the counters; inside the Warm arena's 7,968 spare bytes.
  INFO("sizeof(TempoCore) = " << sizeof(TempoCore));
  REQUIRE(sizeof(TempoCore) <= 1536);
}

TEST_CASE("The Tempo knob: 20 to 300 BPM, exponential, guarded (§6.4, D15)", "[tempo][knob]") {
  using brainscape::tempo::TempoNsFromKnob;
  REQUIRE(TempoNsFromKnob(0.0f) == 3000000000u);  // 20 BPM fully counter-clockwise
  REQUIRE(TempoNsFromKnob(1.0f) == 200000000u);   // 300 BPM fully clockwise
  // 3·10^9 / sqrt(15) = 774,596,669.24: equal turns, equal tempo ratios.
  REQUIRE(TempoNsFromKnob(0.5f) == 774596669u);
  // Canonicalized as a macro position: NaN, ±inf and negatives to 0, above 1 to 1.
  REQUIRE(TempoNsFromKnob(std::nanf("")) == 3000000000u);
  REQUIRE(TempoNsFromKnob(-0.25f) == 3000000000u);
  REQUIRE(TempoNsFromKnob(1.5f) == 200000000u);
  REQUIRE(TempoNsFromKnob(INFINITY) == 3000000000u);
  // Monotone over every pot step of a 12-bit knob, always in range, and each step's ratio near
  // 15^(1/4095).
  uint32_t last = TempoNsFromKnob(0.0f);
  for (int k = 1; k <= 4095; ++k) {
    const uint32_t ns = TempoNsFromKnob(static_cast<float>(k) / 4095.0f);
    REQUIRE(ns < last);
    REQUIRE(ns >= brainscape::tempo::kMinNsPerQuarter);
    REQUIRE(ns <= brainscape::tempo::kMaxNsPerQuarter);
    const double ratio = static_cast<double>(last) / static_cast<double>(ns);
    REQUIRE(std::fabs(ratio - std::pow(15.0, 1.0 / 4095.0)) < 1e-6);
    last = ns;
  }
}
