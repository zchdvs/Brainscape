// The tempo core wired into the engine (sound revision 8, docs/design/clock.md §2.5, §2.6, §3.6,
// §4.2, §6.3, §8.2): events 6-10 through the event list and the per-event split, CLOCK births on
// the grid as triggers, the stored performance state played, Restart, Spillover recall, and the
// snapshot and counters. TempoCore's own rules are test_tempo.cpp's and test_tempo_rules.cpp's;
// these cases check what the engine does with them.
#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "brainscape/Engine.h"
#include "brainscape/HostArenas.h"
#include "brainscape/Mode.h"
#include "brainscape/Preset.h"
#include "brainscape/Tempo.h"
#include "brainscape/TestSignal.h"
#include "catch.hpp"
#include "detail/IntMath.h"
#include "detail/Tempo.h"

using namespace brainscape;

namespace {

using Ev     = Engine::Event;
using EvType = Engine::EventType;
using Params = std::vector<std::pair<ParamId, float>>;
using tempo::SubdivField;
using tempo::TransportKind;

constexpr uint32_t kClockOnly = kSourceClock | kSourceFootswitch | kSourceMidiNote;
constexpr uint32_t kUs140     = 428571;     // stored: 140 BPM, 857.14 frames per tick
constexpr uint32_t kNs140     = 428571429;  // as an event

struct Stereo {
  std::vector<float> l, r;
};

bool Same(const Stereo& a, const Stereo& b) {
  return a.l.size() == b.l.size() &&
         std::memcmp(a.l.data(), b.l.data(), a.l.size() * sizeof(float)) == 0 &&
         std::memcmp(a.r.data(), b.r.data(), a.r.size() * sizeof(float)) == 0;
}

// Plucks every 100 ms over a quiet noise floor.
Stereo Plucks(size_t frames) {
  uint32_t x    = 0x13579BDFu;
  auto     next = [&x] {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return static_cast<float>(x & 0xFFFFFFu) / 8388608.0f - 1.0f;
  };
  Stereo s{std::vector<float>(frames), std::vector<float>(frames)};
  for (size_t i = 0; i < frames; ++i) {
    s.l[i] = 0.001f * next();
    s.r[i] = 0.001f * next();
  }
  for (size_t at = 1200; at + 2400 < frames; at += 4800) {
    float env = 0.6f;
    for (size_t i = 0; i < 2400; ++i, env *= 0.998f) {
      const float v = env * next();
      s.l[at + i] += v;
      s.r[at + i] += 0.8f * v;
    }
  }
  return s;
}

EngineConfig Config() {
  EngineConfig cfg;
  cfg.historyFrames = 1u << 16;
  return cfg;
}

struct Rig {
  host::HeapArenas arenas;
  Engine           engine;
  explicit Rig(const EngineConfig& cfg = Config()) : arenas(PlanMemory(cfg)) {
    REQUIRE(arenas.ok());
    REQUIRE(engine.Init(cfg, arenas.get()));
  }
};

// A complete preset of the default mode with `sources`, `params` over the defaults, and the
// stored performance state given.
std::unique_ptr<PresetState> Preset(const Params& params, uint8_t sources = kClockOnly,
                                    uint32_t us = kUs140, uint8_t subdiv = 0, uint8_t mode = 0) {
  auto s = std::make_unique<PresetState>();
  for (size_t i = 0; i < kNumLeafParams; ++i) {
    s->leaves[i] = {static_cast<uint32_t>(LeafId(i)), FindParam(LeafId(i))->def};
  }
  s->leafCount = static_cast<uint32_t>(kNumLeafParams);
  for (const auto& p : params) s->leaves[LeafIndex(p.first)].value = p.second;
  s->mode.schedule.sources  = sources;
  s->mode.features          = RequiredModeFeatures(s->mode);
  s->performance.usPerQuarter = us;
  s->performance.subdiv       = static_cast<Subdivision>(subdiv);
  s->performance.timeMode     = static_cast<TimeMode>(mode);
  REQUIRE(ComputeModeHash(s->mode, &s->mode.modeHash));
  PresetDiagnostic d;
  REQUIRE(ValidateMode(*s, &d));
  LoadReport report;
  REQUIRE(CheckPreset(*s, &report));
  return s;
}

// Grains: 30 ms, live, no spray, no randomness unless asked.
const Params kPlain = {{ParamId::GrainSizeMs, 30.0f}, {ParamId::SprayMs, 0.0f},
                       {ParamId::Jitter, 0.0f},       {ParamId::DelayMs, 100.0f},
                       {ParamId::Mix, 1.0f}};

Params With(Params p, const Params& more) {
  for (const auto& kv : more) p.push_back(kv);
  return p;
}

Ev Event(int64_t frame, uint32_t seq, EvType type, uint32_t id = 0, float value = 0.f) {
  Ev e;
  e.frame = frame;
  e.seq   = seq;
  e.type  = type;
  e.id    = id;
  e.value = value;
  return e;
}
Ev Tap(int64_t f, uint32_t seq = 0) { return Event(f, seq, EvType::Tap); }
Ev TempoNs(int64_t f, uint32_t ns, uint32_t seq = 0) { return Event(f, seq, EvType::Tempo, ns); }
Ev Tick(int64_t f, uint32_t seq = 0) { return Event(f, seq, EvType::ClockTick); }
Ev Transport(int64_t f, TransportKind k, uint32_t pos, bool atNext, uint32_t seq = 0) {
  return Event(f, seq, EvType::Transport, tempo::TransportId(k, atNext),
               static_cast<float>(pos));
}
Ev Subdiv(int64_t f, SubdivField field, uint8_t code, uint32_t seq = 0) {
  return Event(f, seq, EvType::Subdivision, tempo::SubdivisionId(field, code));
}
Ev Set(int64_t f, ParamId id, float v, uint32_t seq = 0) {
  return Event(f, seq, EvType::SetParam, static_cast<uint32_t>(id), v);
}
Ev Load(int64_t f, const PresetState* preset, uint32_t seq = 0) {
  Ev e     = Event(f, seq, EvType::SpilloverLoad, static_cast<uint32_t>(SwitchStyle::Trails));
  e.preset = preset;
  return e;
}

// Renders `in` from the engine's current frame in blocks of `pattern` (repeated), with `events`
// (sorted by frame, then sequence) stamped into each block. With `births`, block size 1 is
// required and the frame of every grain born (Engine::Stats) is recorded.
Stereo Render(Engine& e, const Stereo& in, const std::vector<Ev>& events,
              const std::vector<uint32_t>& pattern = {48}, std::vector<int64_t>* births = nullptr,
              std::vector<int64_t>* clockBirths = nullptr) {
  const int64_t start = e.SampleCounter();
  Stereo        out{std::vector<float>(in.l.size()), std::vector<float>(in.l.size())};
  std::vector<Engine::BlockEvent> block;
  size_t next = 0, bi = 0;
  for (size_t pos = 0; pos < in.l.size();) {
    const size_t  n  = std::min<size_t>(pattern[bi++ % pattern.size()], in.l.size() - pos);
    const int64_t f0 = start + static_cast<int64_t>(pos);
    block.clear();
    while (next < events.size() && events[next].frame < f0 + static_cast<int64_t>(n)) {
      const Ev&          ev = events[next++];
      Engine::BlockEvent b;
      b.offset = static_cast<uint32_t>(ev.frame - f0);
      b.seq    = ev.seq;
      b.type   = ev.type;
      b.id     = ev.id;
      b.value  = ev.value;
      b.preset = ev.preset;
      block.push_back(b);
    }
    const float* ins[2]  = {in.l.data() + pos, in.r.data() + pos};
    float*       outs[2] = {out.l.data() + pos, out.r.data() + pos};
    Engine::ProcessContext ctx;
    ctx.in        = ins;
    ctx.out       = outs;
    ctx.numFrames = static_cast<uint32_t>(n);
    ctx.events    = block.data();
    ctx.numEvents = static_cast<uint32_t>(block.size());
    const uint64_t b0 = e.Stats().births, c0 = e.TempoCounts().clockBirths;
    e.Process(ctx);
    if (births != nullptr) {
      REQUIRE(n == 1u);
      for (uint64_t k = b0; k < e.Stats().births; ++k) births->push_back(f0);
    }
    if (clockBirths != nullptr) {
      REQUIRE(n == 1u);
      for (uint64_t k = c0; k < e.TempoCounts().clockBirths; ++k) clockBirths->push_back(f0);
    }
    pos += n;
  }
  return out;
}

// F(k) at constant tempo from boundary 0 at frame 0 (§2.2): the first frame at or after k·P/K,
// for positions k·P below 2^63.
int64_t GridFrame(uint64_t p, uint64_t position) {
  const uint64_t num = position * p;  // frames × 24·2^32
  const uint64_t k   = uint64_t{24} << 32;
  return static_cast<int64_t>((num + k - 1u) / k);
}

// Every grid position of `ticks` from 0 whose frame is below `frames`.
std::vector<int64_t> Grid(uint32_t us, uint32_t ticks, int64_t frames) {
  const uint64_t       p = tempo::PFromNs(us * 1000u, 48000u);
  std::vector<int64_t> f;
  for (uint64_t k = 0;; k += ticks) {
    const int64_t at = GridFrame(p, k);
    if (at >= frames) break;
    f.push_back(at);
  }
  return f;
}

std::vector<uint32_t> RandomPattern(uint32_t seed) {
  std::vector<uint32_t> p;
  for (uint32_t i = 0; i < 257; ++i) p.push_back(1u + testsignal::SplitMix32(seed, i) % 512u);
  return p;
}

}  // namespace

TEST_CASE("CLOCK: a hit at every grid position's frame F(k), from frame 0 (§6.3)", "[clock]") {
  const Stereo in = Plucks(3 * 48000);
  for (const uint8_t subdiv : {0, 3, 5, 1}) {  // TAP, ×2, ×8, ×1/4
    INFO("subdiv " << static_cast<int>(subdiv));
    auto preset = Preset(kPlain, kClockOnly, kUs140, subdiv);
    Rig  rig;
    REQUIRE(rig.engine.LoadPreset(*preset, LoadMode::Exact));
    std::vector<int64_t> births;
    Render(rig.engine, in, {}, {1}, &births);
    const std::vector<int64_t> want = Grid(kUs140, tempo::SubdivTicks(subdiv), 3 * 48000);
    CHECK(births == want);
    const TempoInfo info = rig.engine.Tempo();
    CHECK(info.lastGridFrame == want.back());
    CHECK(info.lastClockBirth == want.back());
    CHECK(rig.engine.TempoCounts().clockBirths == want.size());
    CHECK(info.nsPerQuarter == kUs140 * 1000u);
    CHECK(info.subdiv == subdiv);
    CHECK(info.source == static_cast<uint8_t>(tempo::ClockSource::Internal));
  }
}

TEST_CASE("CLOCK: hits are block-split invariant with events 6-10 at any frames (§1.4)",
          "[clock]") {
  const Stereo in     = Plucks(4 * 48000);
  auto         preset = Preset(With(kPlain, {{ParamId::Jitter, 0.6f},
                                             {ParamId::Intermittency, 0.3f},
                                             {ParamId::BurstCount, 2.0f},
                                             {ParamId::BurstSpacingMs, 15.0f}}),
                               kClockOnly | kSourcePeriodic, 436364, 3, 1);  // 137.5 BPM, ×2
  auto other = Preset(kPlain, kClockOnly, 600000, 1);                        // 100 BPM, ×1/4
  // Every type at frames on and off any block grid: taps setting 90 BPM, a tempo, a Subdiv
  // stepped, the time mode to Tempo and back, a tempo-recalling Spillover, MIDI ticks with a
  // Start and a Stop, an invalid payload and an unknown type.
  std::vector<Ev> ev = {Tap(10001), Tap(42001), Tap(74001), Subdiv(80003, SubdivField::Subdivision, 5),
                        TempoNs(95017, kNs140), Subdiv(101111, SubdivField::TimeMode, 2),
                        Subdiv(110000, SubdivField::TimeMode, 0),
                        Set(115007, ParamId::TempoRecall, 1.0f), Load(120013, other.get()),
                        Event(121000, 0, EvType::Tempo, 5u), Event(121001, 0, static_cast<EvType>(13)),
                        Transport(122222, TransportKind::Start, 0, true)};
  for (int64_t k = 0; k < 60; ++k) ev.push_back(Tick(123000 + k * 857 + (k % 3)));
  ev.push_back(Transport(123000 + 61 * 857, TransportKind::Stop, 0, true));
  ev.push_back(Subdiv(180001, SubdivField::Subdivision, 4));
  std::sort(ev.begin(), ev.end(), [](const Ev& a, const Ev& b) { return a.frame < b.frame; });
  for (uint32_t i = 0; i < ev.size(); ++i) ev[i].seq = i;

  Rig rig;
  REQUIRE(rig.engine.LoadPreset(*preset, LoadMode::Exact));
  const Stereo     ref    = Render(rig.engine, in, ev, {1});
  const TempoStats counts = rig.engine.TempoCounts();
  CHECK(counts.taps == 3u);
  CHECK(counts.ticks == 60u);
  CHECK(counts.invalidEvents == 1u);
  CHECK(counts.unknownEvents == 1u);
  CHECK(counts.clockBirths > 10u);
  for (const std::vector<uint32_t>& pattern :
       {std::vector<uint32_t>{48}, {441}, {512}, {48, 1, 127, 32}, {300, 512, 5, 64},
        RandomPattern(7)}) {
    INFO("pattern starting " << pattern[0]);
    Rig other2;
    REQUIRE(other2.engine.LoadPreset(*preset, LoadMode::Exact));
    CHECK(Same(Render(other2.engine, in, ev, pattern), ref));
    const TempoStats c = other2.engine.TempoCounts();
    CHECK(c.clockBirths == counts.clockBirths);
    CHECK(c.commits == counts.commits);
    CHECK(c.jumps == counts.jumps);
  }
}

TEST_CASE("CLOCK: hits are triggers, never capped by overlap, stealing at voice_count (D13)",
          "[clock]") {
  const Stereo in = Plucks(2 * 48000);
  // One free-running voice at most (overlap 0), 500 ms grains, a hit every 2,571 frames (×8 at
  // 140 BPM): the hits fill voice_count and then take the oldest.
  auto preset = Preset({{ParamId::GrainSizeMs, 500.0f}, {ParamId::Overlap, 0.0f},
                        {ParamId::VoiceCount, 4.0f}, {ParamId::Jitter, 0.0f}},
                       kClockOnly | kSourcePeriodic, kUs140, 5);
  Rig rig;
  REQUIRE(rig.engine.LoadPreset(*preset, LoadMode::Exact));
  std::vector<int64_t> clock;
  Render(rig.engine, in, {}, {1}, nullptr, &clock);
  CHECK(clock == Grid(kUs140, 3, 2 * 48000));  // every hit born, none refused
  CHECK(rig.engine.Stats().steals >= 30u);  // after the first four voices, every hit steals
}

TEST_CASE("CLOCK: jitter delays a hit by at most half a grid period, never early (§6.3)",
          "[clock]") {
  const Stereo in     = Plucks(3 * 48000);
  auto         preset = Preset(With(kPlain, {{ParamId::Jitter, 1.0f}}), kClockOnly, kUs140, 3);
  Rig          rig;
  REQUIRE(rig.engine.LoadPreset(*preset, LoadMode::Exact));
  std::vector<int64_t> clock;
  Render(rig.engine, in, {}, {1}, nullptr, &clock);
  const std::vector<int64_t> grid = Grid(kUs140, 12, 3 * 48000);
  // gridFrames = MulDivRoundU64(P, G, K), an integer: the jitter's scale (§6.3).
  const auto gridFrames = static_cast<int64_t>(intmath::MulDivRoundU64(
      tempo::PFromNs(kUs140 * 1000u, 48000u), 12u, uint64_t{24} << 32));
  REQUIRE(clock.size() + 1u >= grid.size());  // the last hits may wait past the render's end
  bool moved = false;
  for (size_t i = 0; i < clock.size(); ++i) {
    CHECK(clock[i] >= grid[i]);
    CHECK(clock[i] - grid[i] <= gridFrames / 2);
    moved = moved || clock[i] != grid[i];
  }
  CHECK(moved);
}

TEST_CASE("CLOCK: a mode without clock births nothing on the grid; switching to one fires no "
          "stale hit (§6.3)",
          "[clock]") {
  const Stereo in    = Plucks(2 * 48000);
  auto         none  = Preset(kPlain, kSourceFootswitch | kSourceMidiNote);
  auto         clock = Preset(kPlain, kClockOnly);
  Rig          rig;
  REQUIRE(rig.engine.LoadPreset(*none, LoadMode::Exact));
  const uint32_t switches = rig.engine.ModeSwitches();
  // The switch lands 300 frames after a quarter at 140 BPM: that position is not fired late.
  const std::vector<int64_t> grid = Grid(kUs140, 24, 2 * 48000);
  const int64_t              at   = grid[2] + 300;
  std::vector<int64_t>       clock2;
  Render(rig.engine, in, {Load(at, clock.get())}, {1}, nullptr, &clock2);
  std::vector<int64_t> want;
  for (const int64_t f : grid) {
    if (f >= at) want.push_back(f);
  }
  CHECK(clock2 == want);
  CHECK(rig.engine.ModeSwitches() == switches + 1u);
}

TEST_CASE("Restart equals Init and an Exact load of the active preset, tempo and output (E3)",
          "[clock]") {
  const Stereo in     = Plucks(48000);
  auto         stored = Preset(kPlain, kClockOnly, 436364, 3, 1);  // 137.5 BPM, ×2, Subdiv mode
  // Taps, ticks, a Stop and live Subdivision events move everything Restart must clear.
  std::vector<Ev> ev = {Tap(1001, 0), Tap(25001, 1), Tap(49001, 2),
                        Subdiv(50003, SubdivField::Subdivision, 5, 3),
                        Subdiv(50003, SubdivField::TimeMode, 2, 4)};
  for (int64_t k = 0; k < 30; ++k) ev.push_back(Tick(52000 + k * 800, static_cast<uint32_t>(5 + k)));
  ev.push_back(Transport(52000 + 30 * 800, TransportKind::Stop, 0, true, 40));
  Rig used;
  REQUIRE(used.engine.LoadPreset(*stored, LoadMode::Exact));
  Render(used.engine, in, ev);
  CHECK(used.engine.Tempo().nsPerQuarter != 436364000u);  // the taps moved it
  used.engine.Restart();
  Rig fresh;
  REQUIRE(fresh.engine.LoadPreset(*stored, LoadMode::Exact));
  const TempoInfo a = used.engine.Tempo(), b = fresh.engine.Tempo();
  CHECK(a.position == b.position);
  CHECK(a.nsPerQuarter == 436364000u);
  CHECK(a.nsPerQuarter == b.nsPerQuarter);
  CHECK(a.source == b.source);
  CHECK(a.timeMode == b.timeMode);
  CHECK(a.subdiv == b.subdiv);
  CHECK(a.flags == b.flags);
  CHECK(a.lastGridFrame == b.lastGridFrame);
  CHECK(a.lastClockBirth == b.lastClockBirth);
  const std::vector<Ev> after = {Tap(3001, 0), Tap(31001, 1), Subdiv(40000, SubdivField::Subdivision, 4, 2)};
  CHECK(Same(Render(used.engine, in, after), Render(fresh.engine, in, after)));
}

TEST_CASE("Spillover loads: the stored time mode and subdivision apply; the tempo under Preset "
          "with the internal source only (§2.5, §3.6)",
          "[clock]") {
  const Stereo in      = Plucks(4800);
  auto         start   = Preset(kPlain, kClockOnly, kUs140, 0, 0);
  auto         recall  = Preset(kPlain, kClockOnly, 600000, 2, 1);  // 100 BPM, ×1/2, Subdiv
  Rig          rig;
  REQUIRE(rig.engine.LoadPreset(*start, LoadMode::Exact));
  // Keep (the default): the running tempo crosses the load.
  Render(rig.engine, in, {TempoNs(100, 500000000)});
  REQUIRE(rig.engine.LoadPreset(*recall, LoadMode::Spillover));
  CHECK(rig.engine.Tempo().nsPerQuarter == 500000000u);
  CHECK(rig.engine.Tempo().subdiv == 2u);
  CHECK(rig.engine.Tempo().timeMode == 1u);
  // Preset: the stored tempo, at once (a jump), the phase kept.
  rig.engine.SetParam(ParamId::TempoRecall, 1.0f);
  Render(rig.engine, in, {});
  REQUIRE(rig.engine.LoadPreset(*start, LoadMode::Spillover));
  CHECK(rig.engine.Tempo().nsPerQuarter == kUs140 * 1000u);
  REQUIRE(rig.engine.LoadPreset(*recall, LoadMode::Spillover));
  CHECK(rig.engine.Tempo().nsPerQuarter == 600000000u);
  CHECK(rig.engine.TempoCounts().jumps >= 2u);
  // Under a clock the clock wins: the stored tempo is ignored.
  std::vector<Ev> ticks;
  const int64_t   f0 = rig.engine.SampleCounter();
  for (int64_t k = 0; k < 40; ++k) ticks.push_back(Tick(f0 + 100 + k * 1000, static_cast<uint32_t>(k)));
  Render(rig.engine, Plucks(48000), ticks);
  REQUIRE(rig.engine.Tempo().source == static_cast<uint8_t>(tempo::ClockSource::ClockFree));
  const uint32_t followed = rig.engine.Tempo().nsPerQuarter;
  REQUIRE(rig.engine.LoadPreset(*start, LoadMode::Spillover));
  CHECK(rig.engine.Tempo().nsPerQuarter == followed);
  // The device setting is stored and kept by loads; rows 83 and 84 act only through events.
  CHECK(rig.engine.GetParam(ParamId::TempoRecall) == 1.0f);
  REQUIRE(rig.engine.LoadPreset(*start, LoadMode::Exact));
  CHECK(rig.engine.GetParam(ParamId::TempoRecall) == 1.0f);
  rig.engine.SetParam(ParamId::PerfSubdiv, 5.0f);
  rig.engine.SetParam(ParamId::PerfTimeMode, 2.0f);
  Render(rig.engine, in, {});
  CHECK(rig.engine.Tempo().subdiv == 0u);
  CHECK(rig.engine.Tempo().timeMode == 0u);
}

TEST_CASE("Init's tempo state, and events before Init applying nothing", "[clock]") {
  Engine unready;
  CHECK(unready.Tempo().nsPerQuarter == 0u);  // no core before Init
  Rig rig;
  const TempoInfo t = rig.engine.Tempo();
  CHECK(t.nsPerQuarter == 500000000u);  // 120 BPM
  CHECK(t.timeMode == 0u);
  CHECK(t.subdiv == 0u);
  CHECK(t.source == 0u);
  CHECK(t.position == -1);  // boundary 0 at frame 0: nothing rendered yet
  CHECK(rig.engine.TempoCounts().taps == 0u);
}

TEST_CASE("CLOCK: one hit per frame, a catch-up and a boundary at one frame (§6.3)", "[clock]") {
  // ×8 (G = 3) at 120 BPM (1,000 frames a tick). A MIDI Start, then ten ticks bunched on frame
  // 1,000, as a computer master's held tick releases them (§4.5): with no slope yet each tick's own
  // frame is its boundary (§3.3 step 4), so the phasor ends at tick 8 with boundary 9 exactly at
  // 1,000. The catch-up fires 6 there and step 2 fires 9 there too; the second is born at 1,001.
  // The grid then runs on at P: 12 at 4,000. Frame 0's hit is Restart's boundary 0.
  auto preset = Preset(kPlain, kClockOnly, 500000, 5);
  Rig  rig;
  REQUIRE(rig.engine.LoadPreset(*preset, LoadMode::Exact));
  std::vector<Ev> ev = {Transport(100, TransportKind::Start, 0, true, 0)};
  for (uint32_t k = 0; k < 10; ++k) ev.push_back(Tick(1000, 1 + k));
  std::vector<int64_t> clock;
  Render(rig.engine, Plucks(4500), ev, {1}, nullptr, &clock);
  CHECK(clock == std::vector<int64_t>{0, 1000, 1001, 4000});
  CHECK(rig.engine.Tempo().source == static_cast<uint8_t>(tempo::ClockSource::ClockRunning));
}

TEST_CASE("CLOCK: a jittered hit still waiting is dropped by a load to a mode without clock "
          "(§6.3)",
          "[clock]") {
  // ×8 at 120 BPM (3,000 frames a grid point) with jitter 1: a hit waits up to 1,500 frames.
  // Grid point 30 (frame 30,000) is born after 30,001 when nothing intervenes; a Spillover load to
  // a mode without `clock` at 30,001 drops it, as a trigger whose source the mode leaves out.
  const Params jit   = With(kPlain, {{ParamId::Jitter, 1.0f}});
  auto         clock = Preset(jit, kClockOnly, 500000, 5);
  auto         none  = Preset(jit, kSourceFootswitch | kSourceMidiNote, 500000, 5);
  const Stereo in    = Plucks(40000);
  std::vector<int64_t> control;
  {
    Rig rig;
    REQUIRE(rig.engine.LoadPreset(*clock, LoadMode::Exact));
    Render(rig.engine, in, {}, {1}, nullptr, &control);
  }
  const bool waiting = std::any_of(control.begin(), control.end(),
                                   [](int64_t f) { return f > 30001 && f <= 31500; });
  REQUIRE(waiting);
  std::vector<int64_t> born;
  Rig                  rig;
  REQUIRE(rig.engine.LoadPreset(*clock, LoadMode::Exact));
  Render(rig.engine, in, {Load(30001, none.get())}, {1}, nullptr, &born);
  REQUIRE_FALSE(born.empty());
  CHECK(born.back() < 30001);
}

TEST_CASE("CLOCK: the gap rule runs before an event of any type (§3.5)", "[clock]") {
  // Thirty ticks (ClockFree after 24), 1.5 s of silence, and then only a SetParam: the gap and the
  // loss apply at the SetParam's frame.
  auto preset = Preset(kPlain, kClockOnly, 500000, 0);
  Rig  rig;
  REQUIRE(rig.engine.LoadPreset(*preset, LoadMode::Exact));
  std::vector<Ev> ev;
  for (uint32_t k = 0; k < 30; ++k) ev.push_back(Tick(1000 + 1000 * k, k));
  ev.push_back(Set(30000 + 72000, ParamId::Mix, 0.9f, 30));
  Render(rig.engine, Plucks(110000), ev);
  CHECK(rig.engine.TempoCounts().gaps == 1u);
  CHECK(rig.engine.TempoCounts().losses == 1u);
}

TEST_CASE("CLOCK: after an Exact load under a running master nothing fires before its tick "
          "(§2.5, note 26)",
          "[clock]") {
  // The producer's re-asserts at frame 0 of the new timeline (§2.5): the committed tempo (128
  // BPM), Locate to the next tick's position (408, a beat) and Continue. The first CLOCK birth is
  // the master's beat at its tick, 1,700; Restart's boundary 0 at frame 0, off that beat, fires
  // nothing.
  auto preset = Preset(kPlain, kClockOnly, 500000, 0);
  Rig  rig;
  REQUIRE(rig.engine.LoadPreset(*preset, LoadMode::Exact));
  std::vector<Ev> ev = {TempoNs(0, 468750000u, 0), Transport(0, TransportKind::Locate, 408, true, 1),
                        Transport(0, TransportKind::Continue, 0, true, 2)};
  for (uint32_t k = 0; k < 30; ++k) ev.push_back(Tick(1700 + (k * 1875) / 2, 3 + k));
  std::vector<int64_t> clock;
  Render(rig.engine, Plucks(30000), ev, {1}, nullptr, &clock);
  REQUIRE_FALSE(clock.empty());
  CHECK(clock.front() == 1700);
  CHECK(rig.engine.Tempo().source == static_cast<uint8_t>(tempo::ClockSource::ClockRunning));
}
