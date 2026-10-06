// The engine state API (docs/design/determinism-profile.md §5.8-§5.12): Restart, the
// random-number epoch, LoadPreset, frame-stamped events and their transport, and the
// toolchain ID.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>

#include "FpEnvTestUtil.h"
#include "brainscape/Engine.h"
#include "brainscape/EventQueue.h"
#include "brainscape/HostArenas.h"
#include "brainscape/SoundRevision.h"
#include "catch.hpp"

using namespace brainscape;
using testing::HostileFpScope;

namespace {

using Params = std::vector<std::pair<ParamId, float>>;
using Ev     = Engine::Event;
using EvType = Engine::EventType;

struct Stereo {
  std::vector<float> l, r;
};

bool Same(const Stereo& a, const Stereo& b) {
  return a.l.size() == b.l.size() &&
         std::memcmp(a.l.data(), b.l.data(), a.l.size() * sizeof(float)) == 0 &&
         std::memcmp(a.r.data(), b.r.data(), a.r.size() * sizeof(float)) == 0;
}

bool SameFrame(const Stereo& a, const Stereo& b, size_t i) {
  return std::memcmp(&a.l[i], &b.l[i], sizeof(float)) == 0 &&
         std::memcmp(&a.r[i], &b.r[i], sizeof(float)) == 0;
}

// The first and last frames where a and b differ, or -1.
int64_t FirstDiff(const Stereo& a, const Stereo& b) {
  for (size_t i = 0; i < a.l.size(); ++i) {
    if (!SameFrame(a, b, i)) return static_cast<int64_t>(i);
  }
  return -1;
}
int64_t LastDiff(const Stereo& a, const Stereo& b) {
  for (size_t i = a.l.size(); i-- > 0;) {
    if (!SameFrame(a, b, i)) return static_cast<int64_t>(i);
  }
  return -1;
}

Stereo Slice(const Stereo& s, size_t from, size_t to) {
  return {std::vector<float>(s.l.begin() + from, s.l.begin() + to),
          std::vector<float>(s.r.begin() + from, s.r.begin() + to)};
}

// Plucks every 100 ms over a quiet noise floor, different per channel and per seed, so
// the onset detector fires and marks are recorded.
Stereo Plucks(size_t frames, uint32_t seed) {
  uint32_t x    = seed | 1u;
  auto     next = [&x] {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return static_cast<float>(x & 0xFFFFFFu) / 8388608.0f - 1.0f;
  };
  Stereo s;
  s.l.resize(frames);
  s.r.resize(frames);
  for (size_t i = 0; i < frames; ++i) {
    s.l[i] = 0.001f * next();
    s.r[i] = 0.001f * next();
  }
  for (size_t at = 2000 + seed % 1000; at + 2400 < frames; at += 4800) {
    for (size_t i = 0; i < 2400; ++i) {
      const float v = 0.6f * next() * static_cast<float>(std::exp(-static_cast<double>(i) / 480.0));
      s.l[at + i] += v;
      s.r[at + i] += 0.8f * v;
    }
  }
  return s;
}

EngineConfig SmallConfig(uint32_t ringLog2 = 15) {
  EngineConfig cfg;
  cfg.historyFrames = 1u << ringLog2;
  return cfg;
}

struct Rig {
  host::HeapArenas arenas;
  Engine           engine;
  explicit Rig(const EngineConfig& cfg) : arenas(PlanMemory(cfg)) {
    REQUIRE(arenas.ok());
    REQUIRE(engine.Init(cfg, arenas.get()));
  }
};

// A complete preset (companion §6.1): every leaf, `params` over the defaults.
PresetState Complete(const Params& params) {
  PresetState s;
  for (size_t i = 0; i < kNumParams; ++i) {
    s.leaves[i] = {static_cast<uint32_t>(kParamTable[i].id), kParamTable[i].def};
  }
  s.leafCount = static_cast<uint32_t>(kNumParams);
  for (const auto& p : params) s.leaves[static_cast<uint32_t>(p.first) - 1u].value = p.second;
  return s;
}

// The start state the golden harness used before LoadPreset: Init, then every value
// set, then Reset drains them and snaps the smoothers.
void InitParams(Engine& e, const Params& params) {
  for (const auto& p : params) e.SetParam(p.first, p.second);
  e.Reset();
}

Ev Param(int64_t frame, uint32_t seq, ParamId id, float value) {
  Ev e;
  e.frame = frame;
  e.seq   = seq;
  e.type  = EvType::SetParam;
  e.id    = static_cast<uint32_t>(id);
  e.value = value;
  return e;
}
Ev Freeze(int64_t frame, uint32_t seq, bool on) {
  Ev e;
  e.frame = frame;
  e.seq   = seq;
  e.type  = EvType::Freeze;
  e.value = on ? 1.f : 0.f;
  return e;
}
Ev Trig(int64_t frame, uint32_t seq) {
  Ev e;
  e.frame = frame;
  e.seq   = seq;
  e.type  = EvType::Trigger;
  e.id    = static_cast<uint32_t>(Engine::TriggerSource::Footswitch);
  e.value = 1.f;
  return e;
}
Ev Spill(int64_t frame, uint32_t seq, const PresetState* preset) {
  Ev e;
  e.frame  = frame;
  e.seq    = seq;
  e.type   = EvType::SpilloverLoad;
  e.preset = preset;
  return e;
}

Engine::BlockEvent ToBlock(const Ev& e, int64_t blockStart) {
  Engine::BlockEvent b;
  b.offset = static_cast<uint32_t>(e.frame - blockStart);
  b.seq    = e.seq;
  b.type   = e.type;
  b.id     = e.id;
  b.value  = e.value;
  b.preset = e.preset;
  return b;
}

void ProcessBlock(Engine& e, const Stereo& in, Stereo* out, size_t pos, size_t n,
                  const std::vector<Engine::BlockEvent>& events) {
  const float* ins[2]  = {in.l.data() + pos, in.r.data() + pos};
  float*       outs[2] = {out->l.data() + pos, out->r.data() + pos};
  Engine::ProcessContext ctx;
  ctx.in        = ins;
  ctx.out       = outs;
  ctx.numFrames = static_cast<uint32_t>(n);
  ctx.events    = events.data();
  ctx.numEvents = static_cast<uint32_t>(events.size());
  e.Process(ctx);
}

// Renders `in` from the engine's current frame in blocks of `pattern` (repeated), with
// `events` (absolute frames, sorted) delivered as stamped events in ProcessContext.
Stereo RenderStamped(Engine& e, const Stereo& in, const std::vector<Ev>& events,
                     const std::vector<uint32_t>& pattern) {
  const int64_t start = e.SampleCounter();
  Stereo        out{std::vector<float>(in.l.size()), std::vector<float>(in.l.size())};
  std::vector<Engine::BlockEvent> block;
  size_t next = 0, bi = 0, pos = 0;
  while (pos < in.l.size()) {
    const size_t  n  = std::min<size_t>(pattern[bi++ % pattern.size()], in.l.size() - pos);
    const int64_t f0 = start + static_cast<int64_t>(pos);
    block.clear();
    while (next < events.size() && events[next].frame < f0 + static_cast<int64_t>(n)) {
      block.push_back(ToBlock(events[next++], f0));
    }
    ProcessBlock(e, in, &out, pos, n, block);
    pos += n;
  }
  return out;
}

// What a wrapper does without engine-side events: split every block at the event frames
// and apply the events between the parts through SetParam, SetFreeze, Trigger and
// LoadPreset(..., Spillover), which take effect at the next part's first frame.
void ApplyUnstamped(Engine& e, const Ev& ev) {
  switch (ev.type) {
    case EvType::SetParam: e.SetParam(static_cast<ParamId>(ev.id), ev.value); break;
    case EvType::Freeze: e.SetFreeze(ev.value != 0.f); break;
    case EvType::Trigger: e.Trigger(static_cast<Engine::TriggerSource>(ev.id), ev.value); break;
    case EvType::SpilloverLoad: e.LoadPreset(*ev.preset, LoadMode::Spillover); break;
  }
}

Stereo RenderSplit(Engine& e, const Stereo& in, const std::vector<Ev>& events,
                   const std::vector<uint32_t>& pattern) {
  const int64_t start = e.SampleCounter();
  Stereo        out{std::vector<float>(in.l.size()), std::vector<float>(in.l.size())};
  size_t next = 0, bi = 0, pos = 0;
  while (pos < in.l.size()) {
    const int64_t f0 = start + static_cast<int64_t>(pos);
    while (next < events.size() && events[next].frame == f0) ApplyUnstamped(e, events[next++]);
    size_t n = std::min<size_t>(pattern[bi++ % pattern.size()], in.l.size() - pos);
    if (next < events.size() && events[next].frame < f0 + static_cast<int64_t>(n)) {
      n = static_cast<size_t>(events[next].frame - f0);
    }
    ProcessBlock(e, in, &out, pos, n, {});
    pos += n;
  }
  return out;
}

const std::vector<std::vector<uint32_t>> kPatterns = {
    {1}, {7}, {48}, {127}, {512}, {48, 1, 127, 32}, {300, 512, 5, 64}};

// The contract-#1 freeze set (profile §5.7), feedback and every post stage engaged.
const Params kBusy = {
    {ParamId::DelayMs, 100.0f},       {ParamId::Mix, 1.0f},
    {ParamId::Feedback, 0.6f},        {ParamId::GrainSizeMs, 60.0f},
    {ParamId::Overlap, 0.55f},        {ParamId::SprayMs, 50.0f},
    {ParamId::PitchSt, 7.0f},         {ParamId::SpreadCents, 20.0f},
    {ParamId::ReverseProb, 0.3f},     {ParamId::Jitter, 1.0f},
    {ParamId::TriggerSens, 0.8f},     {ParamId::OnsetTrigger, 1.0f},
    {ParamId::PositionSource, 1.0f},  {ParamId::ModDepth, 0.4f},
    {ParamId::ModRateHz, 3.0f},       {ParamId::DelayMix, 0.4f},
    {ParamId::DelayFb, 0.6f},         {ParamId::DelayTimeMs, 120.0f},
    {ParamId::ReverbMix, 0.5f},       {ParamId::ReverbTime, 0.8f},
    {ParamId::FilterCutoffHz, 4000.0f}, {ParamId::FilterRes, 0.4f},
    {ParamId::FilterMorph, 0.7f}};

// Events at odd frames, none on any block grid: parameters, freeze on and off, triggers
// (two at one frame), and same-frame changes whose order matters.
std::vector<Ev> OddScript() {
  return {Param(1001, 0, ParamId::Mix, 0.7f),
          Param(2003, 1, ParamId::PitchSt, -5.0f),
          Freeze(3001, 2, true),
          Param(3001, 3, ParamId::Feedback, 0.9f),
          Trig(4097, 4),
          Trig(4097, 5),
          Param(5555, 6, ParamId::OutTrimDb, -6.0f),
          Param(5555, 7, ParamId::OutTrimDb, 3.0f),
          Param(6007, 8, ParamId::FilterCutoffHz, 20000.0f),  // the filter's exact bypass
          Freeze(9001, 9, false),
          Param(10007, 10, ParamId::PositionSource, 0.0f),
          Param(12011, 11, ParamId::FilterCutoffHz, 900.0f),
          Param(12011, 12, ParamId::FilterMorph, 1.5f),
          Trig(15001, 13),
          Freeze(16001, 14, true),
          Param(17777, 15, ParamId::DelayTimeMs, 333.0f),
          Param(19999, 16, ParamId::ReverseProb, 1.0f)};
}

}  // namespace

// ── Frame-stamped events (§5.11) ──────────────────────────────────────────────────────

TEST_CASE("events at odd frames equal a wrapper-side split, at every block size") {
  const Stereo          input  = Plucks(24000, 0xC0FFEEu);
  const std::vector<Ev> script = OddScript();
  Stereo                ref;
  uint32_t              onsets = 0;
  {
    Rig rig(SmallConfig());
    InitParams(rig.engine, kBusy);
    ref    = RenderSplit(rig.engine, input, script, {48});
    onsets = rig.engine.ConsumeOnsetCount();
  }
  REQUIRE(onsets >= 3);  // marks and onset-triggered grains are in play
  for (const auto& pattern : kPatterns) {
    INFO("block pattern starting " << pattern[0]);
    Rig rig(SmallConfig());
    InitParams(rig.engine, kBusy);
    REQUIRE(Same(RenderStamped(rig.engine, input, script, pattern), ref));
  }
  Rig split(SmallConfig());
  InitParams(split.engine, kBusy);
  REQUIRE(Same(RenderSplit(split.engine, input, script, {512}), ref));
}

TEST_CASE("an event applies from its frame; same-frame events apply in sequence order") {
  const Stereo input = Plucks(8000, 0xBEEFu);
  auto render = [&](const std::vector<Ev>& events) {
    Rig rig(SmallConfig());
    InitParams(rig.engine, kBusy);
    return RenderStamped(rig.engine, input, events, {512});
  };
  const Stereo none = render({});
  const Stereo one  = render({Param(777, 0, ParamId::Mix, 0.2f)});
  REQUIRE(FirstDiff(none, one) == 777);  // frames before the stamp use the old state

  // Later sequence numbers win at one frame, whatever the values.
  REQUIRE(Same(render({Param(777, 0, ParamId::Mix, 0.9f), Param(777, 1, ParamId::Mix, 0.2f)}),
               one));
  const Stereo high = render({Param(777, 0, ParamId::Mix, 0.9f)});
  REQUIRE(Same(render({Param(777, 0, ParamId::Mix, 0.2f), Param(777, 1, ParamId::Mix, 0.9f)}),
               high));

  // Freeze and a parameter at one frame: both apply before the frame renders.
  const Stereo a = render({Freeze(1501, 0, true), Param(1501, 1, ParamId::PitchSt, 12.0f)});
  const Stereo b = render({Param(1501, 0, ParamId::PitchSt, 12.0f), Freeze(1501, 1, true)});
  REQUIRE(Same(a, b));
  REQUIRE(FirstDiff(none, a) >= 1501);

  // Events carry canonical values; the engine canonicalizes anyway (NaN -> minimum).
  float nan;
  const uint32_t nanBits = 0x7FC00000u;
  std::memcpy(&nan, &nanBits, sizeof nan);
  REQUIRE(Same(render({Param(777, 0, ParamId::Mix, nan)}),
               render({Param(777, 0, ParamId::Mix, 0.0f)})));
}

// The unstamped calls keep their meaning: they apply at the first frame of the next
// Process call, before the events stamped there ("SetParam, then Process", §5.11).
TEST_CASE("SetParam, SetFreeze and Trigger apply at the block start, before its events") {
  const Stereo input = Plucks(6000, 0x5EEDu);
  auto render = [&](float preMix, bool stamped) {
    Rig rig(SmallConfig());
    InitParams(rig.engine, kBusy);
    rig.engine.SetParam(ParamId::Mix, preMix);
    rig.engine.SetFreeze(true);
    rig.engine.Trigger();
    const std::vector<Ev> events =
        stamped ? std::vector<Ev>{Param(0, 0, ParamId::Mix, 0.25f)} : std::vector<Ev>{};
    return RenderStamped(rig.engine, input, events, {256});
  };
  REQUIRE(Same(render(0.9f, true), render(0.25f, false)));
  REQUIRE_FALSE(Same(render(0.9f, false), render(0.25f, false)));
}

TEST_CASE("events past the block or out of order are applied, never dropped") {
#if defined(NDEBUG)
  // Caller errors that Debug builds assert on; Release applies such an event late.
  const Stereo input = Plucks(4096, 0xABCu);
  auto render = [&](uint32_t badOffset) {
    Rig rig(SmallConfig());
    InitParams(rig.engine, kBusy);
    Stereo out{std::vector<float>(input.l.size()), std::vector<float>(input.l.size())};
    for (size_t pos = 0; pos < input.l.size(); pos += 512) {
      std::vector<Engine::BlockEvent> events;
      if (pos == 1024) {
        events.push_back(ToBlock(Param(1024 + 300, 0, ParamId::Mix, 0.1f), 1024));
        events.push_back(ToBlock(Param(1024 + badOffset, 1, ParamId::Mix, 0.6f), 1024));
      }
      ProcessBlock(rig.engine, input, &out, pos, 512, events);
    }
    return out;
  };
  auto stamped = [&](const std::vector<Ev>& events) {
    Rig rig(SmallConfig());
    InitParams(rig.engine, kBusy);
    return RenderStamped(rig.engine, input, events, {512});
  };
  // Offset 100 after offset 300: applied where it is reached, at 300.
  REQUIRE(Same(render(100), stamped({Param(1324, 0, ParamId::Mix, 0.1f),
                                     Param(1324, 1, ParamId::Mix, 0.6f)})));
  // Offset 900 in a 512-frame block: applied after the block, at the next block's start.
  REQUIRE(Same(render(900), stamped({Param(1324, 0, ParamId::Mix, 0.1f),
                                     Param(1536, 1, ParamId::Mix, 0.6f)})));
#else
  SUCCEED("asserted in Debug builds");
#endif
}

// ── Restart (§5.8) ────────────────────────────────────────────────────────────────────

// Init + parameters, then a render, against: Init + parameters, a render that leaves
// every kind of state behind (feedback in the FIFO and diffusers, ring and post-chain
// history, voices, marks, an engaged freeze, queued triggers, pending parameters, a
// Spillover epoch), Restart, and the same render. Restart keeps the parameter values it
// finds, here changed by events and a pending SetParam, so the reference starts from those.
TEST_CASE("Restart returns a running engine to the post-Init state, keeping parameters") {
  struct Case {
    const char* name;
    Params      params;
    float       feedbackAfter;
  };
  const Case cases[] = {
      {"feedback, marks, freeze and every post stage", kBusy, 0.95f},
      {"self-oscillating feedback through the tamer",
       {{ParamId::Feedback, 1.1f}, {ParamId::ReverseProb, 1.0f}, {ParamId::DelayMs, 120.0f},
        {ParamId::GrainSizeMs, 100.0f}, {ParamId::Overlap, 0.25f}, {ParamId::SprayMs, 0.0f},
        {ParamId::Jitter, 0.0f}, {ParamId::WindowSustain, 1.0f}, {ParamId::WindowSmooth, 0.0f},
        {ParamId::PanSpread, 0.0f}},
       1.1f},
      {"engine defaults (dither, jitter, spray)", {}, 0.5f},
  };
  const Stereo prefix = Plucks(30011, 0x1111u);  // an odd length: the counter ends off-grid
  const Stereo input  = Plucks(24000, 0x2222u);
  // The render after the restart: freeze across the re-anchor, triggers, a parameter.
  const std::vector<Ev> script = {Freeze(4001, 0, true), Trig(5003, 1),
                                  Param(7001, 2, ParamId::Mix, 0.8f), Freeze(15001, 3, false)};
  for (const Case& c : cases) {
    INFO(c.name);
    Params changed = c.params;
    changed.push_back({ParamId::Feedback, c.feedbackAfter});
    changed.push_back({ParamId::PitchSt, -3.0f});
    changed.push_back({ParamId::ReverbMix, 0.45f});

    Stereo ref;
    {
      Rig rig(SmallConfig());
      InitParams(rig.engine, changed);
      ref = RenderStamped(rig.engine, input, script, {48});
    }

    Rig  rig(SmallConfig());
    auto spill = Complete(c.params);
    InitParams(rig.engine, c.params);
    const std::vector<Ev> before = {
        Freeze(2001, 0, true), Trig(2500, 1), Spill(12007, 2, &spill),
        Param(12007, 3, ParamId::Feedback, c.feedbackAfter),
        Param(15001, 4, ParamId::PitchSt, -3.0f), Freeze(20001, 5, true)};
    RenderStamped(rig.engine, prefix, before, {127});
    rig.engine.SetParam(ParamId::ReverbMix, 0.45f);  // pending: Restart drains it
    for (int i = 0; i < 3; ++i) rig.engine.Trigger();  // queued: Restart drops them
    REQUIRE(rig.engine.GetFreeze());
    REQUIRE(rig.engine.EpochStart() == 12007);

    rig.engine.Restart();
    REQUIRE(rig.engine.SampleCounter() == 0);
    REQUIRE(rig.engine.EpochStart() == 0);
    REQUIRE_FALSE(rig.engine.GetFreeze());
    REQUIRE(rig.engine.ConsumeOnsetCount() == 0);
    REQUIRE(rig.engine.GetParam(ParamId::PitchSt) == -3.0f);
    REQUIRE(Same(RenderStamped(rig.engine, input, script, {48}), ref));
  }
}

// The test above fails if Restart does only what the real-time calls do: the prefix
// leaves state that Reset keeps, and Reset + ClearHistory keeps the counter.
TEST_CASE("Reset and ClearHistory do not reach the exact-restart state") {
  const Stereo prefix = Plucks(30011, 0x1111u);
  const Stereo input  = Plucks(12000, 0x2222u);
  Stereo       ref;
  {
    Rig rig(SmallConfig());
    InitParams(rig.engine, kBusy);
    ref = RenderStamped(rig.engine, input, {}, {48});
  }
  for (int variant = 0; variant < 3; ++variant) {
    Rig rig(SmallConfig());
    InitParams(rig.engine, kBusy);
    RenderStamped(rig.engine, prefix, {}, {48});
    rig.engine.Reset();
    if (variant >= 1) rig.engine.ClearHistory();
    if (variant == 2) rig.engine.Restart();
    INFO("variant " << variant);
    REQUIRE(Same(RenderStamped(rig.engine, input, {}, {48}), ref) == (variant == 2));
  }
}

// ── LoadPreset (§5.10) ────────────────────────────────────────────────────────────────

TEST_CASE("an Exact load reaches the exact-restart state from any engine") {
  const Stereo prefix = Plucks(20000, 0x3333u);
  const Stereo input  = Plucks(20000, 0x4444u);
  const std::vector<Ev> script = {Freeze(3001, 0, true), Trig(4003, 1), Freeze(9001, 2, false)};
  const PresetState preset = Complete(kBusy);

  Stereo ref;
  {
    Rig rig(SmallConfig());
    InitParams(rig.engine, kBusy);
    ref = RenderStamped(rig.engine, input, script, {48});
  }
  {
    Rig        rig(SmallConfig());
    LoadReport report;
    REQUIRE(rig.engine.LoadPreset(preset, LoadMode::Exact, &report));
    REQUIRE((report.applied && report.exact));
    REQUIRE(Same(RenderStamped(rig.engine, input, script, {48}), ref));
  }
  {
    // A used engine on another preset, frozen, with a Spillover epoch.
    Rig rig(SmallConfig());
    const PresetState other = Complete({{ParamId::DelayMs, 1500.0f}, {ParamId::Feedback, 1.05f},
                                        {ParamId::ReverbMix, 1.0f}, {ParamId::Jitter, 0.0f}});
    REQUIRE(rig.engine.LoadPreset(other, LoadMode::Exact));
    RenderStamped(rig.engine, prefix, {Freeze(5001, 0, true), Spill(7001, 1, &other)}, {64});
    rig.engine.SetFreeze(true);
    REQUIRE(rig.engine.LoadPreset(preset, LoadMode::Exact));
    REQUIRE_FALSE(rig.engine.GetFreeze());
    REQUIRE(rig.engine.SampleCounter() == 0);
    REQUIRE(rig.engine.EpochStart() == 0);
    REQUIRE(Same(RenderStamped(rig.engine, input, script, {48}), ref));
  }
}

TEST_CASE("LoadPreset applies defaults, then canonical leaves, and reports inexact loads") {
  Rig rig(SmallConfig());

  SECTION("a complete, canonical preset is exact") {
    LoadReport report;
    REQUIRE(rig.engine.LoadPreset(Complete({{ParamId::Mix, 0.3f}}), LoadMode::Exact, &report));
    REQUIRE(report.applied);
    REQUIRE(report.exact);
    REQUIRE(report.unknownIds + report.missingIds + report.duplicateIds + report.changedValues ==
            0);
    REQUIRE(rig.engine.GetParam(ParamId::Mix) == 0.3f);
  }
  SECTION("a missing leaf loads its default, not the value before the load") {
    for (const LoadMode mode : {LoadMode::Exact, LoadMode::Spillover}) {
      rig.engine.SetParam(ParamId::DelayMs, 1000.0f);
      PresetState p = Complete({});
      p.leaves[0]   = p.leaves[--p.leafCount];  // drop DelayMs; leaves now unsorted
      LoadReport report;
      REQUIRE_FALSE(rig.engine.LoadPreset(p, mode, &report));
      REQUIRE(report.applied);
      REQUIRE_FALSE(report.exact);
      REQUIRE(report.missingIds == 1);
      REQUIRE(rig.engine.GetParam(ParamId::DelayMs) == FindParam(ParamId::DelayMs)->def);
    }
  }
  SECTION("unknown and duplicate ids, and values canonicalization changes") {
    PresetState p = Complete({});
    float       nan, subnormal, negZero;
    const uint32_t bits[3] = {0x7FC00000u, 0x00000001u, 0x80000000u};
    std::memcpy(&nan, &bits[0], sizeof nan);
    std::memcpy(&subnormal, &bits[1], sizeof subnormal);
    std::memcpy(&negZero, &bits[2], sizeof negZero);
    p.leaves[static_cast<uint32_t>(ParamId::Mix) - 1u].value      = nan;
    p.leaves[static_cast<uint32_t>(ParamId::SprayMs) - 1u].value  = subnormal;
    p.leaves[static_cast<uint32_t>(ParamId::Feedback) - 1u].value = negZero;
    p.leaves[static_cast<uint32_t>(ParamId::DelayMs) - 1u].value  = 9000.0f;
    p.leaves[p.leafCount++] = {999u, 1.0f};
    p.leaves[p.leafCount++] = {0u, 1.0f};
    p.leaves[p.leafCount++] = {static_cast<uint32_t>(ParamId::PitchSt), 12.0f};  // a repeat

    LoadReport checked;
    REQUIRE_FALSE(CheckPreset(p, &checked));
    REQUIRE_FALSE(checked.applied);
    LoadReport report;
    REQUIRE_FALSE(rig.engine.LoadPreset(p, LoadMode::Exact, &report));
    REQUIRE(report.applied);
    REQUIRE(report.unknownIds == 2);
    REQUIRE(report.duplicateIds == 1);
    REQUIRE(report.changedValues == 4);  // NaN, the subnormal, -0 and 9000 ms
    REQUIRE(report.missingIds == 0);
    REQUIRE(checked.unknownIds == report.unknownIds);
    REQUIRE(checked.duplicateIds == report.duplicateIds);
    REQUIRE(checked.changedValues == report.changedValues);
    REQUIRE(rig.engine.GetParam(ParamId::Mix) == FindParam(ParamId::Mix)->min);
    REQUIRE(rig.engine.GetParam(ParamId::SprayMs) == 0.0f);
    REQUIRE(rig.engine.GetParam(ParamId::DelayMs) == FindParam(ParamId::DelayMs)->max);
    REQUIRE(rig.engine.GetParam(ParamId::PitchSt) == 0.0f);  // the first leaf counts
    const float fb = rig.engine.GetParam(ParamId::Feedback);
    uint32_t    fbBits;
    std::memcpy(&fbBits, &fb, sizeof fbBits);
    REQUIRE(fbBits == 0u);  // +0, not -0
  }
  SECTION("leaves past kMaxLeaves cannot be read") {
    PresetState p = Complete({});
    p.leafCount   = PresetState::kMaxLeaves + 5u;
    for (uint32_t i = static_cast<uint32_t>(kNumParams); i < PresetState::kMaxLeaves; ++i) {
      p.leaves[i] = {1000u + i, 0.0f};
    }
    LoadReport report;
    REQUIRE_FALSE(CheckPreset(p, &report));
    REQUIRE(report.unknownIds == PresetState::kMaxLeaves - kNumParams + 5u);
  }
  SECTION("an engine that is not initialized applies nothing") {
    Engine     idle;
    LoadReport report;
    REQUIRE_FALSE(idle.LoadPreset(Complete({}), LoadMode::Exact, &report));
    REQUIRE_FALSE(report.applied);
    REQUIRE(report.exact);
  }
}

// ── Spillover loads and the random-number epoch (§5.9, §5.10, §2.4) ─────────────────

TEST_CASE("a Spillover load event at an odd frame equals the wrapper-side load") {
  const Stereo      input = Plucks(24000, 0x6666u);
  const PresetState q     = Complete({{ParamId::DelayMs, 333.0f}, {ParamId::PitchSt, -12.0f},
                                      {ParamId::Feedback, 0.3f}, {ParamId::ReverbMix, 0.7f},
                                      {ParamId::Jitter, 0.5f}});
  const std::vector<Ev> script = {Freeze(3001, 0, true), Param(7777, 1, ParamId::Mix, 0.9f),
                                  Spill(7777, 2, &q), Param(7777, 3, ParamId::OutTrimDb, -2.0f),
                                  Trig(9001, 4)};
  Stereo ref;
  {
    Rig rig(SmallConfig());
    InitParams(rig.engine, kBusy);
    ref = RenderSplit(rig.engine, input, script, {48});
    REQUIRE(rig.engine.EpochStart() == 7777);
    REQUIRE_FALSE(rig.engine.GetFreeze());  // every load turns freeze off
    REQUIRE(rig.engine.GetParam(ParamId::Mix) == FindParam(ParamId::Mix)->def);  // the load
    REQUIRE(rig.engine.GetParam(ParamId::OutTrimDb) == -2.0f);  // then the later event
  }
  for (const auto& pattern : kPatterns) {
    INFO("block pattern starting " << pattern[0]);
    Rig rig(SmallConfig());
    InitParams(rig.engine, kBusy);
    REQUIRE(Same(RenderStamped(rig.engine, input, script, pattern), ref));
    REQUIRE(rig.engine.EpochStart() == 7777);
  }
}

// A Spillover load of the running preset changes only the epoch: trails, grains and the
// scheduler phase carry over, so a preset without randomness renders on unchanged, and
// one with randomness changes from the load frame on.
TEST_CASE("a Spillover load keeps trails and restarts the random-number epoch") {
  const Stereo input = Plucks(24000, 0x7777u);
  auto render = [&](const Params& params, bool dither, bool load) {
    EngineConfig cfg    = SmallConfig();
    cfg.ditherRingWrite = dither;
    Rig rig(cfg);
    InitParams(rig.engine, params);
    const PresetState     same = Complete(params);
    const std::vector<Ev> events =
        load ? std::vector<Ev>{Spill(10007, 0, &same)} : std::vector<Ev>{};
    return RenderStamped(rig.engine, input, events, {48});
  };
  const Params fixed = {{ParamId::DelayMs, 120.0f}, {ParamId::Feedback, 0.5f},
                        {ParamId::GrainSizeMs, 50.0f}, {ParamId::Overlap, 0.25f},
                        {ParamId::SprayMs, 0.0f}, {ParamId::Jitter, 0.0f},
                        {ParamId::WindowSustain, 1.0f}, {ParamId::WindowSmooth, 0.0f},
                        {ParamId::PanSpread, 0.0f}, {ParamId::ReverbMix, 0.3f}};
  REQUIRE(Same(render(fixed, false, true), render(fixed, false, false)));
  // With the ring dither, or with jittered, sprayed grains, the draws after the load
  // change: the epoch moved, the counter did not.
  REQUIRE(FirstDiff(render(fixed, true, true), render(fixed, true, false)) >= 10007);
  REQUIRE(FirstDiff(render(kBusy, false, true), render(kBusy, false, false)) >= 10007);
  REQUIRE(LastDiff(render(kBusy, false, true), render(kBusy, false, false)) > 10007);
}

// Profile §2.4: a Spillover load as specified does not reconverge with the Exact render
// of the same preset, even without feedback, because grains and the scheduler phase
// carry over. The record's probe measured the contrast: a load followed by Reset, which
// kills the voices and re-arms the scheduler, reconverges within about 0.26 s. That
// half also proves the draws are keyed on the epoch: with absolute keys it never would.
TEST_CASE("a Spillover load never reconverges, as the profile states; with Reset it does") {
  const EngineConfig cfg       = SmallConfig(17);
  const size_t       loadFrame = 98304;  // a multiple of the 256-frame onset hop (§8.3 Q4)
  const Stereo       prior     = Plucks(loadFrame, 0xABCDEFu);
  const Stereo       input     = Plucks(4 * 48000, 0x1234567u);
  const Params       priorParams = {{ParamId::DelayMs, 400.0f}, {ParamId::GrainSizeMs, 60.0f},
                                  {ParamId::Jitter, 0.6f}, {ParamId::Overlap, 0.8f},
                                  {ParamId::Mix, 1.0f}};
  const struct {
    const char* name;
    Params      params;
  } presets[] = {
      {"default (jitter 0.2, spray 20 ms)", {{ParamId::Mix, 1.0f}}},
      {"periodic", {{ParamId::Mix, 1.0f}, {ParamId::Jitter, 0.0f}, {ParamId::SprayMs, 0.0f}}},
  };
  for (const auto& p : presets) {
    INFO(p.name);
    const PresetState preset = Complete(p.params);
    Stereo            exact;
    {
      Rig rig(cfg);
      REQUIRE(rig.engine.LoadPreset(preset, LoadMode::Exact));
      exact = RenderStamped(rig.engine, input, {}, {48});
    }
    for (const bool reset : {false, true}) {
      Rig rig(cfg);
      REQUIRE(rig.engine.LoadPreset(Complete(priorParams), LoadMode::Exact));
      RenderStamped(rig.engine, prior, {}, {48});
      REQUIRE(rig.engine.LoadPreset(preset, LoadMode::Spillover));
      REQUIRE(rig.engine.EpochStart() == static_cast<int64_t>(loadFrame));
      if (reset) rig.engine.Reset();
      const Stereo  spill = RenderStamped(rig.engine, input, {}, {48});
      const int64_t last  = LastDiff(spill, exact);
      INFO("reset " << reset << ": last differing frame " << last);
      if (reset) {
        REQUIRE(last >= 0);
        REQUIRE(last < 24000);  // identical from half a second on
      } else {
        REQUIRE(last == static_cast<int64_t>(input.l.size()) - 1);  // still differs at the end
      }
    }
  }
}

// ── The transport (§5.11) ─────────────────────────────────────────────────────────────

TEST_CASE("EventQueue hands each block its events as offsets and counts overflows") {
  EventQueue q;
  Engine::BlockEvent out[EventQueue::kCapacity];

  SECTION("offsets, late stamps and blocks") {
    REQUIRE(q.Push(Param(10, 0, ParamId::Mix, 0.1f)));
    REQUIRE(q.Push(Param(10, 1, ParamId::Mix, 0.2f)));
    REQUIRE(q.Push(Param(60, 2, ParamId::Mix, 0.3f)));
    REQUIRE(q.Push(Param(100, 3, ParamId::Mix, 0.4f)));
    REQUIRE(q.Push(Param(130, 4, ParamId::Mix, 0.5f)));
    REQUIRE(q.PopBlock(0, 48, out, EventQueue::kCapacity) == 2);
    REQUIRE((out[0].offset == 10 && out[1].offset == 10 && out[0].seq == 0 && out[1].seq == 1));
    REQUIRE(q.PopBlock(48, 48, out, EventQueue::kCapacity) == 1);
    REQUIRE((out[0].offset == 12 && out[0].value == 0.3f));
    REQUIRE(q.PopBlock(96, 48, out, 1) == 1);  // the rest waits for room
    REQUIRE(out[0].offset == 4);
    REQUIRE(q.Push(Param(5, 5, ParamId::Mix, 0.6f)));  // late, or "now"
    REQUIRE(q.PopBlock(144, 48, out, EventQueue::kCapacity) == 2);
    REQUIRE((out[0].offset == 0 && out[0].value == 0.5f));  // 130, carried: applies late
    REQUIRE((out[1].offset == 0 && out[1].value == 0.6f));
    REQUIRE(q.PopBlock(192, 48, out, EventQueue::kCapacity) == 0);
    REQUIRE(q.Overflows() == 0);
  }
  SECTION("a full queue refuses and counts, and never coalesces") {
    for (uint32_t i = 0; i < EventQueue::kCapacity; ++i) {
      REQUIRE(q.Push(Param(i, i, ParamId::Mix, 0.5f)));  // one parameter, many values
    }
    REQUIRE_FALSE(q.Push(Param(999, 999, ParamId::Mix, 0.1f)));
    REQUIRE_FALSE(q.Push(Param(999, 1000, ParamId::Mix, 0.2f)));
    REQUIRE(q.Overflows() == 2);
    REQUIRE(q.PopBlock(0, 512, out, EventQueue::kCapacity) == EventQueue::kCapacity);
    for (uint32_t i = 0; i < EventQueue::kCapacity; ++i) REQUIRE(out[i].seq == i);
    REQUIRE(q.Push(Param(600, 0, ParamId::Mix, 0.1f)));
    REQUIRE(q.Overflows() == 2);
  }
  SECTION("one producer thread and one consumer keep every event in order") {
    static constexpr uint32_t kEvents = 50000;
    std::thread        producer([&q] {
      for (uint32_t i = 0; i < kEvents; ++i) {
        while (!q.Push(Param(i / 4, i, ParamId::Mix, 0.5f))) std::this_thread::yield();
      }
    });
    uint32_t expect = 0;
    int64_t  block  = 0;
    while (expect < kEvents) {
      const uint32_t n = q.PopBlock(block, 48, out, EventQueue::kCapacity);
      for (uint32_t i = 0; i < n; ++i) {
        REQUIRE(out[i].seq == expect);
        ++expect;
      }
      if (n == 0) std::this_thread::yield();
      block += 48;
    }
    producer.join();
  }
}

TEST_CASE("events through the queue render as events in ProcessContext") {
  const Stereo          input  = Plucks(24000, 0xC0FFEEu);
  const std::vector<Ev> script = OddScript();
  Rig                   ref(SmallConfig());
  InitParams(ref.engine, kBusy);
  const Stereo expected = RenderStamped(ref.engine, input, script, {48});

  Rig        rig(SmallConfig());
  EventQueue q;
  InitParams(rig.engine, kBusy);
  for (const Ev& e : script) REQUIRE(q.Push(e));
  Stereo out{std::vector<float>(input.l.size()), std::vector<float>(input.l.size())};
  Engine::BlockEvent buf[EventQueue::kCapacity];
  for (size_t pos = 0; pos < input.l.size(); pos += 48) {
    const uint32_t n = q.PopBlock(rig.engine.SampleCounter(), 48, buf, EventQueue::kCapacity);
    ProcessBlock(rig.engine, input, &out, pos, 48, std::vector<Engine::BlockEvent>(buf, buf + n));
  }
  REQUIRE(Same(out, expected));
  REQUIRE(q.Overflows() == 0);
}

// ── The FP environment at the new entry points (§4.1) ─────────────────────────────────

TEST_CASE("Restart, LoadPreset and CheckPreset own the FP control word") {
  const Stereo      input   = Plucks(9600, 0x9999u);
  const PresetState preset  = Complete(kBusy);
  const PresetState pitched = Complete({{ParamId::PitchSt, 5.0f}});
  auto render = [&](detail::FpWord host) {
    Rig    rig(SmallConfig());
    size_t wordsLost = 0;
    auto   call      = [&](auto&& fn) {
      const HostileFpScope scope(host);
      fn();
      if (detail::ReadFpControl() != host) ++wordsLost;
    };
    LoadReport report;
    call([&] { CheckPreset(preset, &report); });
    call([&] { rig.engine.LoadPreset(preset, LoadMode::Exact, &report); });
    Stereo a = RenderStamped(rig.engine, Slice(input, 0, 4800), {Freeze(1001, 0, true)}, {48});
    call([&] { rig.engine.LoadPreset(pitched, LoadMode::Spillover); });
    Stereo b = RenderStamped(rig.engine, Slice(input, 4800, 9600), {}, {48});
    call([&] { rig.engine.Restart(); });
    Stereo c = RenderStamped(rig.engine, Slice(input, 0, 4800), {}, {48});
    REQUIRE(wordsLost == 0);
    a.l.insert(a.l.end(), b.l.begin(), b.l.end());
    a.r.insert(a.r.end(), b.r.begin(), b.r.end());
    a.l.insert(a.l.end(), c.l.begin(), c.l.end());
    a.r.insert(a.r.end(), c.r.begin(), c.r.end());
    return a;
  };
  const Stereo clean = render(detail::kFpProfileWord);
  REQUIRE(Same(render(testing::kHostileFpWord), clean));
#if defined(BRAINSCAPE_FPENV_X64)
  REQUIRE(Same(render(testing::kTrapAllFpWord), clean));
#endif
}

// ── The toolchain ID (§5.12) ──────────────────────────────────────────────────────────

TEST_CASE("BuildToolchain names the compiler, target and FP flags that built the engine") {
  const ToolchainId& id = BuildToolchain();
  // The tests are compiled with the engine's compiler.
#if defined(__clang__) && defined(__apple_build_version__)
  REQUIRE(std::strcmp(id.compiler, "appleclang") == 0);
#elif defined(__clang__)
  REQUIRE(std::strcmp(id.compiler, "clang") == 0);
#elif defined(__GNUC__)
  REQUIRE(std::strcmp(id.compiler, "gcc") == 0);
#elif defined(_MSC_VER)
  REQUIRE(std::strcmp(id.compiler, "msvc") == 0);
#endif
  REQUIRE(std::strlen(id.version) > 0);
#if defined(__x86_64__) || defined(_M_X64)
  REQUIRE(std::strncmp(id.target, "x86_64-", 7) == 0);
#endif
#if defined(_MSC_VER) && !defined(__clang__)
  REQUIRE(std::strstr(id.fpFlags, "/fp:precise") != nullptr);
#else
  REQUIRE(std::strstr(id.fpFlags, "-ffp-contract=off") != nullptr);
#endif
  REQUIRE(std::strlen(id.fpFlagsHash) == 16);
  for (const char* c = id.fpFlagsHash; *c != '\0'; ++c) {
    REQUIRE(((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'f')));
  }
}
