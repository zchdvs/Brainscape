// Wave 1 of the mode runtime (docs/design/mode-compiler.md §7.5, §10.3, §10.4), one feature and
// one sound revision at a time: trigger sources, bursts and intermittency (R9, sound revision 4);
// pitch sets (R10, 5).
// Each feature's cases run block-split invariance (contract #1) and the level contract (#3).
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "brainscape/Engine.h"
#include "brainscape/HostArenas.h"
#include "brainscape/Mode.h"
#include "brainscape/Preset.h"
#include "catch.hpp"
#include "detail/GrainMath.h"

using namespace brainscape;

namespace {

using Ev     = Engine::Event;
using EvType = Engine::EventType;
using Params = std::vector<std::pair<ParamId, float>>;
using TS     = Engine::TriggerSource;

struct Stereo {
  std::vector<float> l, r;
};

bool Same(const Stereo& a, const Stereo& b) {
  return a.l.size() == b.l.size() &&
         std::memcmp(a.l.data(), b.l.data(), a.l.size() * sizeof(float)) == 0 &&
         std::memcmp(a.r.data(), b.r.data(), a.r.size() * sizeof(float)) == 0;
}

// Plucks every 100 ms over a quiet noise floor, so onsets fire and marks are recorded.
Stereo Plucks(size_t frames) {
  uint32_t x    = 0x2468ACE1u;
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

// A steady sine: no onsets, a constant level.
Stereo Sine(size_t frames, float amplitude = 0.25f) {
  Stereo s{std::vector<float>(frames), std::vector<float>(frames)};
  for (size_t i = 0; i < frames; ++i) {
    const float v = amplitude * static_cast<float>(std::sin(0.0287979 * static_cast<double>(i)));
    s.l[i]        = v;
    s.r[i]        = v;
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

// A complete preset, `params` over the defaults, of the default mode, with `sources`.
std::unique_ptr<PresetState> Preset(const Params& params, uint8_t sources = kDefaultSources) {
  auto s = std::make_unique<PresetState>();
  for (size_t i = 0; i < kNumLeafParams; ++i) {
    s->leaves[i] = {static_cast<uint32_t>(LeafId(i)), FindParam(LeafId(i))->def};
  }
  s->leafCount = static_cast<uint32_t>(kNumLeafParams);
  for (const auto& p : params) {
    REQUIRE(IsLeaf(p.first));
    s->leaves[LeafIndex(p.first)].value = p.second;
  }
  s->mode.schedule.sources = sources;
  s->mode.features         = RequiredModeFeatures(s->mode);
  REQUIRE(ComputeModeHash(s->mode, &s->mode.modeHash));
  PresetDiagnostic d;
  REQUIRE(ValidateMode(*s, &d));
  return s;
}

// `s` with layer 0 playing `set` by `select` (its mode re-hashed and validated).
struct SetEntry {
  float    st;
  uint16_t weight;
};
std::unique_ptr<PresetState> WithSet(std::unique_ptr<PresetState> s, const std::vector<SetEntry>& set,
                                     PitchSelect select = PitchSelect::Cycle) {
  REQUIRE(!set.empty());
  REQUIRE(set.size() <= kMaxPitchEntries);
  s->mode.pitch[0]       = PitchSet{};
  s->mode.pitch[0].count = static_cast<uint8_t>(set.size());
  for (size_t i = 0; i < set.size(); ++i) {
    s->mode.pitch[0].entries[i] = PitchEntry{set[i].st, set[i].weight, 0};
  }
  s->mode.layers[0].pitchSelect = select;
  s->mode.features              = RequiredModeFeatures(s->mode);
  REQUIRE(ComputeModeHash(s->mode, &s->mode.modeHash));
  PresetDiagnostic d;
  REQUIRE(ValidateMode(*s, &d));
  return s;
}

Ev Event(int64_t frame, uint32_t seq, EvType type, uint32_t id, float value = 0.f) {
  Ev e;
  e.frame = frame;
  e.seq   = seq;
  e.type  = type;
  e.id    = id;
  e.value = value;
  return e;
}
Ev Trig(int64_t frame, uint32_t seq, TS src) {
  return Event(frame, seq, EvType::Trigger, static_cast<uint32_t>(src), 1.f);
}
Ev Set(int64_t frame, uint32_t seq, ParamId id, float value) {
  return Event(frame, seq, EvType::SetParam, static_cast<uint32_t>(id), value);
}
Ev Load(int64_t frame, uint32_t seq, const PresetState* preset) {
  Ev e     = Event(frame, seq, EvType::SpilloverLoad, 0);
  e.preset = preset;
  return e;
}

// Renders `in` from the engine's current frame in blocks of `pattern` (repeated), with `events`
// (sorted by frame, then sequence) stamped into each block.
Stereo Render(Engine& e, const Stereo& in, const std::vector<Ev>& events,
              const std::vector<uint32_t>& pattern = {48}) {
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
    e.Process(ctx);
    pos += n;
  }
  return out;
}

Stereo RenderFrom(const PresetState& preset, const Stereo& in, const std::vector<Ev>& events,
                  const std::vector<uint32_t>& pattern = {48}) {
  Rig rig;
  REQUIRE(rig.engine.LoadPreset(preset, LoadMode::Exact));
  return Render(rig.engine, in, events, pattern);
}

// The frames at which the scheduler did each thing, once per count (Engine::Stats): rendered
// one frame per block from the exact-restart state with `preset`.
struct Timeline {
  std::vector<int64_t> births, bursts, skips;
};
Timeline Track(const PresetState& preset, const Stereo& in, const std::vector<Ev>& events) {
  Rig rig;
  REQUIRE(rig.engine.LoadPreset(preset, LoadMode::Exact));
  Timeline           t;
  Engine::GrainStats was = rig.engine.Stats();
  size_t             next = 0;
  std::vector<Engine::BlockEvent> block;
  float              l = 0.f, r = 0.f;
  for (size_t f = 0; f < in.l.size(); ++f) {
    block.clear();
    while (next < events.size() && events[next].frame == static_cast<int64_t>(f)) {
      const Ev&          ev = events[next++];
      Engine::BlockEvent b;
      b.seq    = ev.seq;
      b.type   = ev.type;
      b.id     = ev.id;
      b.value  = ev.value;
      b.preset = ev.preset;
      block.push_back(b);
    }
    const float* ins[2]  = {in.l.data() + f, in.r.data() + f};
    float*       outs[2] = {&l, &r};
    Engine::ProcessContext ctx;
    ctx.in        = ins;
    ctx.out       = outs;
    ctx.numFrames = 1;
    ctx.events    = block.data();
    ctx.numEvents = static_cast<uint32_t>(block.size());
    rig.engine.Process(ctx);
    const Engine::GrainStats now = rig.engine.Stats();
    for (uint64_t k = was.births; k < now.births; ++k) t.births.push_back(static_cast<int64_t>(f));
    for (uint64_t k = was.burstBirths; k < now.burstBirths; ++k) t.bursts.push_back(static_cast<int64_t>(f));
    for (uint64_t k = was.skips; k < now.skips; ++k) t.skips.push_back(static_cast<int64_t>(f));
    was = now;
  }
  return t;
}

using Frames = std::vector<int64_t>;

double Rms(const std::vector<float>& x, size_t from, size_t to) {
  double sum = 0.0;
  for (size_t i = from; i < to; ++i) sum += static_cast<double>(x[i]) * x[i];
  return std::sqrt(sum / static_cast<double>(to - from));
}

}  // namespace

// ── Trigger sources, bursts and intermittency (R9, sound revision 4) ──────────────────────────

TEST_CASE("R9: footswitch and MIDI triggers fire only when the mode lists their source", "[wave1]") {
  const Stereo in = Sine(9600);
  const std::vector<Ev> events = {Trig(1001, 0, TS::Footswitch), Trig(2002, 0, TS::MidiNote),
                                  Trig(3003, 0, TS::Sidechain),
                                  Event(4004, 0, EvType::Trigger, 7u, 1.f)};  // an unknown id
  struct Case {
    uint8_t sources;
    Frames  births;
  };
  const Case cases[] = {
      {kSourceFootswitch, {1001, 3003, 4004}},  // Sidechain and unknown ids count as Footswitch
      {kSourceMidiNote, {2002}},
      {kSourceFootswitch | kSourceMidiNote, {1001, 2002, 3003, 4004}},
      {0, {}},
  };
  for (const Case& c : cases) {
    INFO("sources " << static_cast<int>(c.sources));
    const auto preset = Preset({}, c.sources);
    CHECK(Track(*preset, in, events).births == c.births);
    // The unstamped Trigger() of each source, called before the block at its frame, the same.
    Rig rig;
    REQUIRE(rig.engine.LoadPreset(*preset, LoadMode::Exact));
    const Engine::GrainStats before = rig.engine.Stats();
    Stereo                   part{std::vector<float>(1), std::vector<float>(1)};
    for (size_t f = 0; f < in.l.size(); ++f) {
      if (f == 1001) rig.engine.Trigger(TS::Footswitch);
      if (f == 2002) rig.engine.Trigger(TS::MidiNote);
      if (f == 3003) rig.engine.Trigger(TS::Sidechain);
      if (f == 4004) rig.engine.Trigger(static_cast<TS>(7));
      part.l[0] = in.l[f];
      part.r[0] = in.r[f];
      Render(rig.engine, part, {}, {1});
    }
    CHECK(rig.engine.Stats().births - before.births == c.births.size());
  }
}

TEST_CASE("R9: a trigger due when the mode leaves its source out is dropped", "[wave1]") {
  // Three footswitch triggers at one frame fire on three frames; a load at the second frame of a
  // mode without `footswitch` drops the two still due, in either delivery.
  const Stereo in      = Sine(4800);
  const auto   both    = Preset({}, kSourceFootswitch | kSourceMidiNote);
  const auto   midi    = Preset({}, kSourceMidiNote);
  const std::vector<Ev> events = {Trig(1000, 0, TS::Footswitch), Trig(1000, 1, TS::Footswitch),
                                  Trig(1000, 2, TS::Footswitch), Load(1001, 0, midi.get())};
  CHECK(Track(*both, in, events).births == Frames{1000});
  // The load before the triggers at their frame: none fires.
  CHECK(Track(*both, in, {Load(1000, 0, midi.get()), Trig(1000, 1, TS::Footswitch)}).births.empty());
  // After them at their frame: the triggers are decided as they fall due, after the frame's
  // events, so none fires either.
  CHECK(Track(*both, in, {Trig(1000, 0, TS::Footswitch), Load(1000, 1, midi.get())}).births.empty());
}

TEST_CASE("R9: without `periodic` there are no free-running births", "[wave1]") {
  const Stereo in = Sine(24000);
  for (const uint8_t sources : {uint8_t{0}, kSourceOnset, uint8_t(kSourceOnset | kSourceFootswitch)}) {
    INFO("sources " << static_cast<int>(sources));
    // The least sensitive detector, so the sine's start fires no onset.
    const auto preset = Preset({{ParamId::Overlap, 1.0f}, {ParamId::TriggerSens, 0.0f}}, sources);
    const Timeline t      = Track(*preset, in, {});
    CHECK(t.births.empty());  // no floor of one voice either
    // Nothing plays but the dry signal: the Mix law's dry at unity, the wet exactly zero.
    const Stereo out = RenderFrom(*preset, in, {});
    size_t       differ = 0;
    for (size_t i = 0; i < in.l.size(); ++i) differ += (out.l[i] != in.l[i] || out.r[i] != in.r[i]) ? 1u : 0u;
    CHECK(differ == 0u);
  }
  // The default sources run the scheduler, at least one voice.
  CHECK_FALSE(Track(*Preset({{ParamId::Overlap, 0.0f}}), in, {}).births.empty());
}

TEST_CASE("R9: a trigger births burst.count grains, max(1, round(spacing_ms * 48)) apart", "[wave1]") {
  const Stereo in = Sine(9600);
  struct Case {
    float  count, spacingMs;
    Frames births;
  };
  const Case cases[] = {
      {1.0f, 0.0f, {1000}},
      {4.0f, 0.0f, {1000, 1001, 1002, 1003}},             // spacing 0: consecutive frames
      {4.0f, 2.5f, {1000, 1120, 1240, 1360}},             // 120 frames
      {3.0f, 0.01f, {1000, 1001, 1002}},                  // 0.48 rounds to 0: at least a frame
      {3.0f, 0.03125f, {1000, 1002, 1004}},               // 1.5 rounds half away: 2 frames
      {2.6f, 1.0f, {1000, 1048, 1096}},                   // the count read as RoundHalfAway: 3
      {16.0f, 500.0f, {1000}},                            // the rest past the render
  };
  for (const Case& c : cases) {
    INFO("count " << c.count << " spacing " << c.spacingMs);
    const auto preset = Preset({{ParamId::BurstCount, c.count}, {ParamId::BurstSpacingMs, c.spacingMs}},
                               kSourceFootswitch);
    const Timeline t = Track(*preset, in, {Trig(1000, 0, TS::Footswitch)});
    CHECK(t.births == c.births);
    CHECK(t.bursts.size() == c.births.size() - 1u);
  }
}

TEST_CASE("R9: bursts of triggers at one frame take one frame each", "[wave1]") {
  const Stereo in = Sine(4800);
  // Spacing 0: two triggers' bursts of 3 give 6 births on consecutive frames, never two at one.
  const auto dense = Preset({{ParamId::BurstCount, 3.0f}}, kSourceFootswitch);
  CHECK(Track(*dense, in, {Trig(1000, 0, TS::Footswitch), Trig(1000, 1, TS::Footswitch)}).births ==
        Frames{1000, 1001, 1002, 1003, 1004, 1005});
  // Spaced: each trigger's own spacing from its own first grain.
  const auto spaced = Preset({{ParamId::BurstCount, 3.0f}, {ParamId::BurstSpacingMs, 2.5f}},
                             kSourceFootswitch);
  CHECK(Track(*spaced, in, {Trig(1000, 0, TS::Footswitch), Trig(1000, 1, TS::Footswitch)}).births ==
        Frames{1000, 1001, 1120, 1121, 1240, 1241});
}

TEST_CASE("R9: onsets fire bursts; intermittency skips whole triggers", "[wave1]") {
  const Stereo in = Plucks(48000);
  const auto   one = Preset({{ParamId::TriggerSens, 0.6f}}, kSourceOnset);
  const auto   four =
      Preset({{ParamId::TriggerSens, 0.6f}, {ParamId::BurstCount, 4.0f}, {ParamId::BurstSpacingMs, 5.0f}},
             kSourceOnset);
  const Timeline a = Track(*one, in, {});
  const Timeline b = Track(*four, in, {});
  REQUIRE(a.births.size() >= 5u);
  CHECK(b.births.size() == 4u * a.births.size());
  CHECK(b.bursts.size() == 3u * a.births.size());
  // Every trigger skipped: no births, one skip per onset.
  const auto never = Preset(
      {{ParamId::TriggerSens, 0.6f}, {ParamId::BurstCount, 4.0f}, {ParamId::Intermittency, 1.0f}},
      kSourceOnset);
  const Timeline c = Track(*never, in, {});
  CHECK(c.births.empty());
  CHECK(c.skips == a.births);
  // Half skipped: an accepted trigger's whole burst, a skipped one's nothing.
  const auto half = Preset(
      {{ParamId::TriggerSens, 0.6f}, {ParamId::BurstCount, 4.0f}, {ParamId::Intermittency, 0.5f}},
      kSourceOnset | kSourceFootswitch);
  std::vector<Ev> triggers;
  for (uint32_t k = 0; k < 64; ++k) triggers.push_back(Trig(200 + 731 * k, 0, TS::Footswitch));
  const Timeline d = Track(*half, in, triggers);
  const size_t   fired = a.births.size() + triggers.size() - d.skips.size();
  CHECK(d.births.size() == 4u * fired);
  CHECK(d.skips.size() > 10u);
  CHECK(d.skips.size() < 74u);
}

TEST_CASE("R9: a skipped periodic birth still consumes its interval", "[wave1]") {
  // Jitter 0, eight voices of 480 frames, one born every 60: a skipped birth keeps the schedule,
  // so with every birth skipped the skips land where the births would have.
  const Stereo in   = Sine(9600);
  const Params base = {{ParamId::Overlap, 0.5f}, {ParamId::Jitter, 0.0f}, {ParamId::GrainSizeMs, 10.0f},
                       {ParamId::SprayMs, 0.0f}};
  Params skipAll = base;
  skipAll.emplace_back(ParamId::Intermittency, 1.0f);
  const Timeline born    = Track(*Preset(base), in, {});
  const Timeline skipped = Track(*Preset(skipAll), in, {});
  REQUIRE(born.births.size() > 100u);
  CHECK(skipped.births.empty());
  CHECK(skipped.skips == born.births);
}

TEST_CASE("R9: a load of another mode resets bursts in progress, of the same mode keeps them",
          "[wave1]") {
  const Stereo in = Sine(4800);
  const auto   a  = Preset({{ParamId::BurstCount, 4.0f}, {ParamId::BurstSpacingMs, 2.5f}},
                           kSourceFootswitch);
  const auto   b  = Preset({{ParamId::BurstCount, 4.0f}, {ParamId::BurstSpacingMs, 2.5f}},
                           kSourceFootswitch | kSourceMidiNote);
  const Ev     trigger = Trig(1000, 0, TS::Footswitch);
  CHECK(Track(*a, in, {trigger, Load(1130, 0, a.get())}).births == Frames{1000, 1120, 1240, 1360});
  CHECK(Track(*a, in, {trigger, Load(1130, 0, b.get())}).births == Frames{1000, 1120});
  // An Exact load restarts: a burst still due when it lands never fires.
  Rig rig;
  REQUIRE(rig.engine.LoadPreset(*a, LoadMode::Exact));
  Render(rig.engine, in, {Event(4700, 0, EvType::Trigger, 0, 1.f)});  // due again from 4820
  const Engine::GrainStats s = rig.engine.Stats();
  REQUIRE(rig.engine.LoadPreset(*a, LoadMode::Exact));
  Render(rig.engine, in, {});
  CHECK(rig.engine.Stats().births == s.births);
}

TEST_CASE("R9: sources, bursts and intermittency are block-split invariant (contract #1)",
          "[wave1]") {
  const Stereo in = Plucks(28800);
  const auto   a  = Preset({{ParamId::TriggerSens, 0.6f},
                            {ParamId::BurstCount, 3.0f},
                            {ParamId::BurstSpacingMs, 7.0f},
                            {ParamId::Intermittency, 0.4f},
                            {ParamId::Overlap, 0.3f},
                            {ParamId::SprayMs, 30.0f},
                            {ParamId::Mix, 0.8f}},
                           kSourcePeriodic | kSourceOnset | kSourceFootswitch | kSourceMidiNote);
  const auto   b  = Preset({{ParamId::TriggerSens, 0.6f},
                            {ParamId::BurstCount, 5.0f},
                            {ParamId::Intermittency, 0.2f},
                            {ParamId::Mix, 0.8f}},
                           kSourceOnset | kSourceMidiNote);
  const std::vector<Ev> events = {
      Trig(777, 0, TS::Footswitch),       Trig(777, 1, TS::MidiNote),
      Set(5003, 0, ParamId::BurstCount, 6.0f), Trig(6001, 0, TS::Footswitch),
      Load(9001, 0, b.get()),             Trig(9001, 1, TS::Footswitch),
      Trig(12345, 0, TS::MidiNote),       Set(15001, 0, ParamId::Intermittency, 0.9f),
      Load(20011, 0, a.get()),            Trig(20011, 1, TS::Footswitch)};
  const Stereo ref = RenderFrom(*a, in, events, {48});
  for (const std::vector<uint32_t>& pattern :
       {std::vector<uint32_t>{1}, {512}, {37, 5, 300, 1}, {64, 3}}) {
    INFO("pattern of " << pattern.size() << " starting " << pattern[0]);
    CHECK(Same(RenderFrom(*a, in, events, pattern), ref));
  }
  // The triggers mattered: without them the render differs.
  std::vector<Ev> noTriggers;
  for (const Ev& e : events) {
    if (e.type != EvType::Trigger) noTriggers.push_back(e);
  }
  CHECK_FALSE(Same(RenderFrom(*a, in, noTriggers, {48}), ref));
}

TEST_CASE("R9: a burst keeps the level of one grain (contract #3)", "[wave1]") {
  // Coherent grains (unity rate, no spray, a rectangular window, centre pan) born on consecutive
  // frames from the footswitch: the normalization's N is burst.count without a free-running
  // source, so 1 to 16 grains play at one grain's level.
  const Stereo in = Sine(48000);
  std::vector<Ev> triggers;
  for (uint32_t k = 0; k < 8; ++k) triggers.push_back(Trig(4800 + 4800 * k, 0, TS::Footswitch));
  const Params base = {{ParamId::Mix, 1.0f},          {ParamId::GrainSizeMs, 50.0f},
                       {ParamId::SprayMs, 0.0f},      {ParamId::DelayMs, 20.0f},
                       {ParamId::WindowSustain, 1.0f}, {ParamId::WindowSmooth, 0.0f},
                       {ParamId::PanSpread, 0.0f}};
  double one = 0.0;
  for (const float count : {1.0f, 2.0f, 4.0f, 8.0f, 16.0f}) {
    Params p = base;
    p.emplace_back(ParamId::BurstCount, count);
    // Each grain sounds 2,400 frames from its trigger: measure inside the first grain's span.
    const Stereo out = RenderFrom(*Preset(p, kSourceFootswitch), in, triggers);
    double       sum = 0.0;
    for (uint32_t k = 0; k < 8; ++k) {
      const size_t from = 4800 + 4800 * k + 600, to = from + 1200;
      sum += Rms(out.l, from, to);
    }
    const double level = sum / 8.0;
    if (count == 1.0f) one = level;
    INFO("burst " << count << ": " << 20.0 * std::log10(level / one) << " dB");
    REQUIRE(one > 0.05);
    CHECK(std::fabs(20.0 * std::log10(level / one)) < 0.5);
  }
}

// ── Pitch sets (R10, sound revision 5) ──────────────────────────────────────────────────────

TEST_CASE("R10: `cycle` plays the entries in order, each weight times", "[wave1]") {
  using grainmath::NextCycleEntry;
  using grainmath::PitchCycle;
  const uint16_t        w[3] = {2, 1, 3};
  PitchCycle            c;
  std::vector<uint32_t> got;
  for (int i = 0; i < 13; ++i) got.push_back(NextCycleEntry(&c, w, 3));
  CHECK(got == std::vector<uint32_t>{0, 0, 1, 2, 2, 2, 0, 0, 1, 2, 2, 2, 0});
  // One entry of weight 1, the default set: always entry 0, and the state never moves.
  const uint16_t one[1] = {1};
  PitchCycle     d;
  for (int i = 0; i < 5; ++i) CHECK(NextCycleEntry(&d, one, 1) == 0u);
  CHECK(d.entry == 0u);
  CHECK(d.played == 0u);
  // A state past a smaller set is reduced modulo it, from that entry's first play.
  PitchCycle e{5, 1};
  CHECK(NextCycleEntry(&e, w, 3) == 2u);  // 5 mod 3
  CHECK(NextCycleEntry(&e, w, 3) == 2u);
  CHECK(NextCycleEntry(&e, w, 3) == 2u);
  CHECK(NextCycleEntry(&e, w, 3) == 0u);
}

TEST_CASE("R10: `random` picks entries in proportion to their weights, in integers", "[wave1]") {
  using grainmath::RandomEntry;
  // The entry whose cumulative weights hold floor(u24 * sum / 2^24): exact at the boundaries.
  const uint16_t w2[2] = {1, 3};
  CHECK(RandomEntry(0u, w2, 2, 4) == 0u);
  CHECK(RandomEntry((1u << 22) - 1u, w2, 2, 4) == 0u);
  CHECK(RandomEntry(1u << 22, w2, 2, 4) == 1u);
  CHECK(RandomEntry((1u << 24) - 1u, w2, 2, 4) == 1u);
  // Over every 24-bit value, entry i takes its share of 2^24 to within one value.
  const uint16_t w[4]     = {3, 1, 16, 7};
  const uint32_t sum      = 27;
  uint32_t       count[4] = {};
  for (uint32_t u = 0; u < (1u << 24); ++u) ++count[RandomEntry(u, w, 4, sum)];
  for (int i = 0; i < 4; ++i) {
    const double share = static_cast<double>(w[i]) * 16777216.0 / sum;
    INFO("entry " << i << ": " << count[i] << " of 2^24, share " << share);
    CHECK(std::fabs(static_cast<double>(count[i]) - share) <= 1.0);
  }
  // One entry: always it.
  const uint16_t one[1] = {9};
  CHECK(RandomEntry((1u << 24) - 1u, one, 1, 9) == 0u);
  // The engine's draw is purpose 8's extended key (R8), never a purpose-0-7 key of its frame.
  for (int64_t abs : {int64_t{0}, int64_t{1}, int64_t{48000}, int64_t{1} << 33}) {
    for (uint32_t p = 0; p < 8; ++p) {
      CHECK(grainmath::DrawKey(abs, grainmath::Draw::PitchSelect) !=
            grainmath::DrawKey(abs, static_cast<grainmath::Draw>(p)));
    }
  }
}

TEST_CASE("R10: one-entry sets and the default set play the transpose bit for bit", "[wave1]") {
  // entry + transpose is the pitch: {7} at 0, {3} at +4 and {0} at +7 all give 7 st exactly,
  // under either selection, and a set of equal entries is that one pitch.
  const Stereo in   = Plucks(28800);
  const Params base = {{ParamId::SprayMs, 25.0f},    {ParamId::SpreadCents, 15.0f},
                       {ParamId::Jitter, 0.5f},      {ParamId::ReverseProb, 0.3f},
                       {ParamId::TriggerSens, 0.6f}, {ParamId::BurstCount, 2.0f}};
  auto with = [&](float transpose) {
    Params p = base;
    p.emplace_back(ParamId::TransposeSt, transpose);
    return p;
  };
  const uint8_t         sources = kDefaultSources | kSourceOnset;
  const std::vector<Ev> events  = {Trig(5001, 0, TS::Footswitch), Trig(5001, 1, TS::MidiNote)};
  const Stereo          ref     = RenderFrom(*Preset(with(7.0f), sources), in, events);
  CHECK(Same(RenderFrom(*WithSet(Preset(with(0.0f), sources), {{7.0f, 1}}), in, events), ref));
  CHECK(Same(RenderFrom(*WithSet(Preset(with(4.0f), sources), {{3.0f, 4}}), in, events), ref));
  CHECK(Same(RenderFrom(*WithSet(Preset(with(0.0f), sources), {{7.0f, 2}, {7.0f, 5}, {7.0f, 1}}),
                        in, events),
             ref));
  CHECK(Same(RenderFrom(*WithSet(Preset(with(0.0f), sources), {{7.0f, 2}, {7.0f, 5}},
                                 PitchSelect::Random),
                        in, events),
             ref));
  // A set of two pitches plays something else.
  CHECK_FALSE(Same(
      RenderFrom(*WithSet(Preset(with(0.0f), sources), {{7.0f, 1}, {0.0f, 1}}), in, events), ref));
}

TEST_CASE("R10: the set, its order, its weights and the selection each change the sound",
          "[wave1]") {
  const Stereo in    = Plucks(19200);
  const Params base  = {{ParamId::SprayMs, 10.0f}, {ParamId::Jitter, 0.3f}, {ParamId::Overlap, 0.6f}};
  const Stereo zero  = RenderFrom(*Preset(base), in, {});
  const Stereo up    = RenderFrom(*WithSet(Preset(base), {{12.0f, 1}}), in, {});
  const Stereo both  = RenderFrom(*WithSet(Preset(base), {{0.0f, 1}, {12.0f, 1}}), in, {});
  const Stereo flip  = RenderFrom(*WithSet(Preset(base), {{12.0f, 1}, {0.0f, 1}}), in, {});
  const Stereo heavy = RenderFrom(*WithSet(Preset(base), {{0.0f, 3}, {12.0f, 1}}), in, {});
  const Stereo rnd =
      RenderFrom(*WithSet(Preset(base), {{0.0f, 1}, {12.0f, 1}}, PitchSelect::Random), in, {});
  const Stereo* all[] = {&zero, &up, &both, &flip, &heavy, &rnd};
  for (size_t i = 0; i < 6; ++i) {
    for (size_t j = 0; j < i; ++j) {
      INFO("renders " << j << " and " << i);
      CHECK_FALSE(Same(*all[i], *all[j]));
    }
  }
}

TEST_CASE("R10: a load of another mode restarts the cycle, of the same mode keeps it",
          "[wave1]") {
  // One periodic voice at a time (overlap 0), 100 ms grains abutting: births at 0, 4800 and
  // 9600, so a load at 2400 lands after one birth, with the cycle's +12 next. Reverse grains
  // (reverse_prob 1) make every set's normalization the same, so only the pitch differs.
  const Stereo in   = Sine(14400);
  const Params base = {{ParamId::Overlap, 0.0f},     {ParamId::Jitter, 0.0f},
                       {ParamId::SprayMs, 0.0f},     {ParamId::GrainSizeMs, 100.0f},
                       {ParamId::ReverseProb, 1.0f}, {ParamId::DelayMs, 20.0f},
                       {ParamId::Mix, 1.0f}};
  const std::vector<SetEntry> set = {{0.0f, 1}, {12.0f, 1}};
  const auto a = WithSet(Preset(base), set);
  // Other modes: the same set, or the set {0}, with a source no event uses left out.
  const uint8_t others = kSourcePeriodic | kSourceFootswitch;
  const auto    b      = WithSet(Preset(base, others), set);
  const auto    zero   = Preset(base, others);
  CHECK(Track(*a, in, {}).births == Frames{0, 4800, 9600});
  const Stereo keep  = RenderFrom(*a, in, {Load(2400, 0, a.get())});
  const Stereo reset = RenderFrom(*a, in, {Load(2400, 0, b.get())});
  const Stereo flat  = RenderFrom(*a, in, {Load(2400, 0, zero.get())});
  // Reset, the birth at 4800 plays the cycle's first entry, 0 st, as the set {0} does; the
  // kept cycle plays +12 there. From 9600 the reset cycle plays +12 and the set {0} 0 st.
  auto sameFrom = [](const Stereo& x, const Stereo& y, size_t from, size_t to) {
    for (size_t i = from; i < to; ++i) {
      if (x.l[i] != y.l[i] || x.r[i] != y.r[i]) return false;
    }
    return true;
  };
  CHECK(sameFrom(keep, reset, 0, 4800));
  CHECK(sameFrom(reset, flat, 0, 9600));
  CHECK_FALSE(sameFrom(keep, flat, 4800, 9600));
  CHECK_FALSE(sameFrom(reset, flat, 9600, 14400));
  // An Exact load restarts the cycle too: from the load on, the render is a fresh one's.
  Rig rig;
  REQUIRE(rig.engine.LoadPreset(*a, LoadMode::Exact));
  Render(rig.engine, Sine(2400), {});  // one birth: +12 next
  REQUIRE(rig.engine.LoadPreset(*a, LoadMode::Exact));
  CHECK(Same(Render(rig.engine, in, {}), RenderFrom(*a, in, {})));
}

TEST_CASE("R10: pitch sets are block-split invariant (contract #1)", "[wave1]") {
  const Stereo in     = Plucks(28800);
  const Params params = {{ParamId::TriggerSens, 0.6f}, {ParamId::BurstCount, 3.0f},
                         {ParamId::SprayMs, 30.0f},    {ParamId::Jitter, 0.6f},
                         {ParamId::Overlap, 0.5f},     {ParamId::Mix, 0.8f}};
  const uint8_t sources = kDefaultSources | kSourceOnset;
  const auto    cyc = WithSet(Preset(params, sources), {{0.0f, 2}, {12.0f, 1}, {-7.0f, 3}});
  const auto    rnd = WithSet(Preset(params, sources), {{0.0f, 5}, {-12.0f, 2}, {19.0f, 1}},
                              PitchSelect::Random);
  const std::vector<Ev> events = {
      Trig(777, 0, TS::Footswitch),          Set(3001, 0, ParamId::TransposeSt, -5.0f),
      Load(6007, 0, rnd.get()),              Trig(6007, 1, TS::MidiNote),
      Set(9999, 0, ParamId::TransposeSt, 9.0f), Load(14001, 0, cyc.get()),
      Load(17003, 0, cyc.get()),             Trig(20011, 0, TS::Footswitch)};
  const Stereo ref = RenderFrom(*cyc, in, events, {48});
  for (const std::vector<uint32_t>& pattern :
       {std::vector<uint32_t>{1}, {512}, {37, 5, 300, 1}, {64, 3}}) {
    INFO("pattern of " << pattern.size() << " starting " << pattern[0]);
    CHECK(Same(RenderFrom(*cyc, in, events, pattern), ref));
  }
}

TEST_CASE("R10: a pitch set keeps the level across the overlap sweep (contract #3)", "[wave1]") {
  // Two steady tones, spray 30 ms (decorrelated positions): the set {0, +12, -12} plays within
  // 1.5 dB of the set {0} at every overlap, and within 1.5 dB of itself across the sweep.
  Stereo in = Sine(48000, 0.3f);
  for (size_t i = 0; i < in.l.size(); ++i) {
    const float v = 0.3f * static_cast<float>(std::sin(0.0431969 * static_cast<double>(i)));
    in.l[i] += v;
    in.r[i] += v;
  }
  auto level = [&](const std::vector<SetEntry>& set, float overlap) {
    const Params p = {{ParamId::Overlap, overlap}, {ParamId::SprayMs, 30.0f},
                      {ParamId::GrainSizeMs, 60.0f}, {ParamId::Mix, 1.0f},
                      {ParamId::PanSpread, 0.0f}};
    const Stereo out = RenderFrom(*WithSet(Preset(p), set), in, {});
    return 20.0 * std::log10(Rms(out.l, 24000, 48000));
  };
  const std::vector<SetEntry> three = {{0.0f, 1}, {12.0f, 1}, {-12.0f, 1}};
  double lo = 1e9, hi = -1e9;
  for (const float overlap : {0.3f, 0.5f, 0.75f, 1.0f}) {
    const double set = level(three, overlap), unison = level({{0.0f, 1}}, overlap);
    INFO("overlap " << overlap << ": set " << set << " dB, {0} " << unison << " dB");
    CHECK(std::fabs(set - unison) < 1.5);
    lo = std::min(lo, set);
    hi = std::max(hi, set);
  }
  INFO("the set across the sweep: " << lo << " to " << hi << " dB");
  CHECK(hi - lo < 1.5);
}
