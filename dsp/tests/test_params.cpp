// The permanent parameter-ID table (docs/design/mode-compiler.md §4): its rows, their kinds
// and per-kind rules (§4.1), the Leaf-row helpers every consumer iterates, and the domain
// bitmask that routes a change to its rebuild (§7.2, R1 since sound revision 2), with the
// lone-change regression tests.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "brainscape/Engine.h"
#include "brainscape/HostArenas.h"
#include "brainscape/Mode.h"
#include "brainscape/ParamDisplay.h"
#include "brainscape/SoundRevision.h"
#include "catch.hpp"

using namespace brainscape;

namespace {

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

uint32_t Bits(float v) {
  uint32_t u = 0;
  std::memcpy(&u, &v, sizeof u);
  return u;
}

// Plucks every 100 ms over a quiet noise floor, so the onset detector fires and marks are
// recorded (as test_state.cpp's input).
Stereo Plucks(size_t frames) {
  uint32_t x    = 0x2468ACE1u;
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
  for (size_t at = 1200; at + 2400 < frames; at += 4800) {
    for (size_t i = 0; i < 2400; ++i) {
      const float v = 0.6f * next() * static_cast<float>(std::exp(-static_cast<double>(i) / 480.0));
      s.l[at + i] += v;
      s.r[at + i] += 0.8f * v;
    }
  }
  return s;
}

struct Rig {
  host::HeapArenas arenas;
  Engine           engine;
  explicit Rig(const EngineConfig& cfg) : arenas(PlanMemory(cfg)) {
    REQUIRE(arenas.ok());
    REQUIRE(engine.Init(cfg, arenas.get()));
  }
};

EngineConfig Config() {
  EngineConfig cfg;
  cfg.historyFrames = 1u << 16;
  return cfg;
}

using Params = std::vector<std::pair<ParamId, float>>;

// A complete preset: every Leaf row by ordinal, `params` over the defaults.
PresetState Complete(const Params& params) {
  PresetState s;
  for (size_t i = 0; i < kNumLeafParams; ++i) {
    s.leaves[i] = {static_cast<uint32_t>(LeafId(i)), FindParam(LeafId(i))->def};
  }
  s.leafCount = static_cast<uint32_t>(kNumLeafParams);
  for (const auto& p : params) {
    REQUIRE(IsLeaf(p.first));
    s.leaves[LeafIndex(p.first)].value = p.second;
  }
  return s;
}

Ev Set(int64_t frame, uint32_t seq, ParamId id, float value) {
  Ev e;
  e.frame = frame;
  e.seq   = seq;
  e.type  = EvType::SetParam;
  e.id    = static_cast<uint32_t>(id);
  e.value = value;
  return e;
}

// Renders `in` in 48-frame blocks from the engine's current frame, with `events` (sorted by
// frame, then sequence) stamped into each block.
Stereo Render(Engine& e, const Stereo& in, const std::vector<Ev>& events) {
  REQUIRE(std::is_sorted(events.begin(), events.end(), [](const Ev& a, const Ev& b) {
    return a.frame != b.frame ? a.frame < b.frame : a.seq < b.seq;
  }));
  const int64_t start = e.SampleCounter();
  REQUIRE((events.empty() || events.front().frame >= start));
  Stereo        out{std::vector<float>(in.l.size()), std::vector<float>(in.l.size())};
  std::vector<Engine::BlockEvent> block;
  size_t next = 0;
  for (size_t pos = 0; pos < in.l.size();) {
    const size_t  n  = std::min<size_t>(48, in.l.size() - pos);
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

// From the exact-restart state with `preset`, the render of `in` with `events`. The load
// must be exact unless `inexact`.
Stereo RenderFrom(const PresetState& preset, const Stereo& in, const std::vector<Ev>& events,
                  bool inexact = false) {
  Rig rig(Config());
  REQUIRE(rig.engine.LoadPreset(preset, LoadMode::Exact) != inexact);
  return Render(rig.engine, in, events);
}

ParamKind KindOf(uint32_t id) { return FindParam(static_cast<ParamId>(id))->kind; }

}  // namespace

// ── The table (design §4.2) ────────────────────────────────────────────────────────────

TEST_CASE("the ID table runs 1-82 with the design's kinds for this build") {
  REQUIRE(kNumParams == 82u);
  for (uint32_t id = 1; id <= kNumParams; ++id) {
    const ParamDescriptor* d = FindParam(static_cast<ParamId>(id));
    REQUIRE(d != nullptr);
    REQUIRE(static_cast<uint32_t>(d->id) == id);
    INFO("id " << id << " " << (d->name != nullptr ? d->name : "(retired)"));
    ParamKind want = ParamKind::Reserved;  // 29-68, 79 (W2), 80 (looper), 81 (phase D)
    if (id <= 26) want = ParamKind::Leaf;
    if (id == 27 || id == 28) want = ParamKind::Retired;  // into structure at r2 (§7.6 item 4)
    if (id >= 69 && id <= 76) want = ParamKind::Macro;
    if (id == 77 || id == 78) want = ParamKind::Performance;
    if (id == 82) want = ParamKind::Global;
    CHECK(d->kind == want);
    CHECK(d->sinceRev == (want == ParamKind::Leaf ? 1u : 0u));
  }
  CHECK(FindParam(static_cast<ParamId>(0)) == nullptr);
  CHECK(FindParam(static_cast<ParamId>(83)) == nullptr);
}

TEST_CASE("renamed and new rows carry the design's names, ranges and domains") {
  struct Row {
    ParamId     id;
    const char* name;
    float       min, max, def;
    uint8_t     domain;
  };
  const Row rows[] = {
      {ParamId::WetTrimDb, "wet_trim_db", -24.0f, 24.0f, 0.0f, kDomainWet},
      {ParamId::TransposeSt, "layer0.pitch.transpose_st", -24.0f, 24.0f, 0.0f, kDomainGranular},
      {ParamId::FilterCutoffHz, "post.filter.cutoff_hz", 40.0f, 20000.0f, 20000.0f,
       kDomainPost | kDomainWet},
      {ParamId::TriggerSens, "trigger.sensitivity", 0.0f, 1.0f, 0.5f, kDomainDetector},
      {ParamId::Mix, "global.mix", 0.0f, 1.0f, 0.5f, kDomainMix},
      {ParamId::Feedback, "feedback.amount", 0.0f, 1.1f, 0.0f, kDomainFeedback},
      {ParamId::Repeat, "layer0.position.repeat", 1.0f, 16.0f, 1.0f, kDomainGranular},
      {ParamId::DecayMs, "layer0.decay_ms", 0.0f, 20000.0f, 0.0f, kDomainGranular},
      {ParamId::VoiceCount, "layer0.voice_count", 1.0f, 64.0f, 64.0f, kDomainGranular},
      {ParamId::LevelDb, "layer0.level_db", -24.0f, 6.0f, 0.0f, kDomainGranular},
      {ParamId::GlideCurve, "layer0.pitch.glide.curve", -1.0f, 1.0f, 0.0f, kDomainGranular},
      {ParamId::SvfCutoffHz, "layer0.svf.cutoff_hz", 20.0f, 20000.0f, 20000.0f, kDomainGranular},
      {ParamId::SvfRes, "layer0.svf.res", 0.0f, 1.0f, 0.1f, kDomainGranular},
      {ParamId::CrushBits, "layer0.crush.bits", 1.0f, 16.0f, 16.0f, kDomainGranular},
      {ParamId::CrushDownsample, "layer0.crush.downsample", 1.0f, 32.0f, 1.0f, kDomainGranular},
      {ParamId::Intermittency, "scheduler.intermittency", 0.0f, 1.0f, 0.0f, kDomainGranular},
      {ParamId::BurstCount, "scheduler.burst.count", 1.0f, 16.0f, 1.0f, kDomainGranular},
      {ParamId::BurstSpacingMs, "scheduler.burst.spacing_ms", 0.0f, 500.0f, 0.0f, kDomainGranular},
      {ParamId::StepCount, "scheduler.steps.count", 1.0f, 16.0f, 16.0f, kDomainGranular},
      {ParamId::LayerMix, "layer_mix", 0.0f, 1.0f, 0.5f, kDomainGranular},
      {ParamId::DryDuckDepth, "dry_duck.depth", 0.0f, 1.0f, 0.0f, kDomainMix},
      {ParamId::DelaySync, "post.delay.sync", 0.0f, 16.0f, 0.0f, kDomainPost},
      {ParamId::ReverbMode, "post.reverb.mode", 0.0f, 3.0f, 0.0f, kDomainPost},
      {ParamId::Modulator1Depth, "modulator1.depth", 0.0f, 1.0f, 0.0f, kDomainGranular},
      {ParamId::MacroActivity, "macro.activity", 0.0f, 1.0f, 0.5f, kDomainNone},
      {ParamId::MacroAux2, "macro.aux2", 0.0f, 1.0f, 0.5f, kDomainNone},
      {ParamId::PerfFreeze, "perf.freeze", 0.0f, 1.0f, 0.0f, kDomainNone},
      {ParamId::PerfExpression, "perf.expression", 0.0f, 1.0f, 0.0f, kDomainNone},
      {ParamId::PerfReverse, "perf.reverse", 0.0f, 1.0f, 0.0f, kDomainNone},
      {ParamId::PerfLoopLevel, "perf.loop_level", 0.0f, 1.0f, 1.0f, kDomainNone},
      {ParamId::TriggerOffset, "global.trigger_offset", -1.0f, 1.0f, 0.0f, kDomainDetector},
      {ParamId::EffectVolumeDb, "global.effect_volume_db", -24.0f, 12.0f, 0.0f, kDomainWet},
  };
  for (const Row& r : rows) {
    const ParamDescriptor& d = *FindParam(r.id);
    INFO(r.name);
    CHECK(std::string(d.name) == r.name);
    CHECK(Bits(d.min) == Bits(r.min));
    CHECK(Bits(d.max) == Bits(r.max));
    CHECK(Bits(d.def) == Bits(r.def));
    CHECK(d.domain == r.domain);
  }
  // 27 and 28 are tombstones: their ids, never reused, and nothing else (§4.1).
  for (const ParamId retired : {ParamId::OnsetTrigger, ParamId::PositionSource}) {
    const ParamDescriptor& d = *FindParam(retired);
    CHECK(d.kind == ParamKind::Retired);
    CHECK(d.name == nullptr);
    CHECK(d.domain == kDomainNone);
    CHECK(d.sinceRev == 0u);
  }
  // Layer 1 (38-56) repeats layer 0's leaves under layer1., ranges and defaults included.
  const std::pair<ParamId, ParamId> layers[] = {
      {ParamId::DelayMs, ParamId::L1DelayMs},         {ParamId::SprayMs, ParamId::L1SprayMs},
      {ParamId::Repeat, ParamId::L1Repeat},           {ParamId::GrainSizeMs, ParamId::L1GrainSizeMs},
      {ParamId::DecayMs, ParamId::L1DecayMs},         {ParamId::VoiceCount, ParamId::L1VoiceCount},
      {ParamId::LevelDb, ParamId::L1LevelDb},         {ParamId::PanSpread, ParamId::L1PanSpread},
      {ParamId::WindowSustain, ParamId::L1WindowSustain}, {ParamId::WindowSkew, ParamId::L1WindowSkew},
      {ParamId::WindowSmooth, ParamId::L1WindowSmooth}, {ParamId::TransposeSt, ParamId::L1TransposeSt},
      {ParamId::SpreadCents, ParamId::L1SpreadCents}, {ParamId::ReverseProb, ParamId::L1ReverseProb},
      {ParamId::GlideCurve, ParamId::L1GlideCurve},   {ParamId::SvfCutoffHz, ParamId::L1SvfCutoffHz},
      {ParamId::SvfRes, ParamId::L1SvfRes},           {ParamId::CrushBits, ParamId::L1CrushBits},
      {ParamId::CrushDownsample, ParamId::L1CrushDownsample}};
  REQUIRE(sizeof layers / sizeof layers[0] == 19u);
  for (const auto& p : layers) {
    const ParamDescriptor& a = *FindParam(p.first);
    const ParamDescriptor& b = *FindParam(p.second);
    INFO(a.name << " / " << b.name);
    CHECK(std::string(a.name).rfind("layer0.", 0) == 0);
    CHECK(std::string(b.name) == "layer1." + std::string(a.name).substr(7));
    CHECK(Bits(a.min) == Bits(b.min));
    CHECK(Bits(a.max) == Bits(b.max));
    CHECK(Bits(a.def) == Bits(b.def));
    CHECK(b.domain == kDomainGranular);
  }
  const char* macros[] = {"activity", "repeats", "shape", "time", "space", "filter", "aux1", "aux2"};
  for (uint32_t k = 0; k < 8; ++k) {
    CHECK(std::string(FindParam(static_cast<ParamId>(69 + k))->name) == std::string("macro.") + macros[k]);
  }
}

TEST_CASE("the Leaf rows are the presets' leaves, in ascending id order") {
  REQUIRE(kNumLeafParams == 26u);  // sound revision 1's rows but the retired 27 and 28
  uint32_t prev = 0;
  for (size_t i = 0; i < kNumLeafParams; ++i) {
    const auto id = static_cast<uint32_t>(LeafId(i));
    CHECK(id > prev);
    prev = id;
    CHECK(KindOf(id) == ParamKind::Leaf);
    CHECK(LeafIndex(LeafId(i)) == i);
    CHECK(IsLeaf(id));
  }
  size_t leaves = 0;
  for (const ParamDescriptor& d : kParamTable) {
    if (d.kind == ParamKind::Leaf) {
      ++leaves;
    } else {
      CHECK(LeafIndex(d.id) == kNumLeafParams);
      CHECK_FALSE(IsLeaf(d.id));
    }
  }
  CHECK(leaves == kNumLeafParams);
  CHECK(LeafIndex(0u) == kNumLeafParams);
  CHECK(LeafIndex(83u) == kNumLeafParams);
  CHECK(LeafIndex(0xFFFFFFFFu) == kNumLeafParams);
  // Constant-evaluable, so firmware and plugin tables can be sized from them.
  static_assert(LeafIndex(ParamId::DelayMs) == 0u, "");
  static_assert(LeafIndex(ParamId::TriggerSens) == 25u, "");
  static_assert(LeafIndex(ParamId::PositionSource) == kNumLeafParams, "");
  static_assert(!IsLeaf(ParamId::MacroFilter), "");
}

// ── Per-kind rules (design §4.1, §10.4) ────────────────────────────────────────────────

TEST_CASE("SetParam stores Leaf and Global rows only") {
  Rig rig(Config());
  Engine& e = rig.engine;
  for (const ParamDescriptor& d : kParamTable) {
    INFO((d.name != nullptr ? d.name : "(retired)"));
    const bool stored = d.kind == ParamKind::Leaf || d.kind == ParamKind::Global;
    e.SetParam(d.id, d.max);
    CHECK(e.GetParam(d.id) == (stored ? d.max : 0.0f));
  }
  e.SetParam(ParamId::EffectVolumeDb, 99.0f);  // canonicalized like any stored row
  CHECK(e.GetParam(ParamId::EffectVolumeDb) == 12.0f);
  e.SetParam(ParamId::MacroActivity, 0.25f);  // a macro moves only by MacroMove (r2)
  CHECK(e.GetParam(ParamId::MacroActivity) == 0.0f);
}

TEST_CASE("changes to rows the engine does not play leave the output unchanged") {
  const Stereo      input  = Plucks(24000);
  const PresetState preset = Complete({{ParamId::Mix, 0.8f}, {ParamId::Feedback, 0.5f},
                                       {ParamId::ReverbMix, 0.3f}});
  const Stereo      plain  = RenderFrom(preset, input, {});
  std::vector<Ev>   events;  // in (frame, sequence) order
  uint32_t          seq = 0;
  for (const int64_t frame : {int64_t{1001}, int64_t{9001}}) {
    for (const ParamDescriptor& d : kParamTable) {
      if (d.kind == ParamKind::Leaf || d.kind == ParamKind::Global) continue;
      events.push_back(Set(frame, seq++, d.id, frame == 1001 ? d.max : d.min));
    }
  }
  // SetParam on a Macro, Performance, Reserved or Retired row is a no-op (design §4.1): a
  // macro moves only by MacroMove, and 27 and 28 are structure since r2.
  CHECK(Same(RenderFrom(preset, input, events), plain));
}

TEST_CASE("the effect volume, a Global row, scales the wet signal from its frame") {
  const Stereo      input  = Plucks(24000);
  const PresetState preset = Complete({{ParamId::Mix, 0.8f}, {ParamId::ReverbMix, 0.3f}});
  const Stereo      plain  = RenderFrom(preset, input, {});
  const Stereo      lower  = RenderFrom(preset, input, {Set(9001, 0, ParamId::EffectVolumeDb, -12.0f)});
  CHECK_FALSE(Same(lower, plain));
  // Nothing before its frame changes.
  CHECK(std::memcmp(lower.l.data(), plain.l.data(), 9001 * sizeof(float)) == 0);
  CHECK(std::memcmp(lower.r.data(), plain.r.data(), 9001 * sizeof(float)) == 0);
}

TEST_CASE("a preset naming a row that is not a Leaf loads inexact and changes nothing") {
  Rig     rig(Config());
  Engine& e = rig.engine;
  e.SetParam(ParamId::EffectVolumeDb, -6.0f);
  const PresetState base = Complete({{ParamId::DelayMs, 300.0f}});
  // IDs 27 and 28 are Retired, 31 and 81 Reserved, 69 a Macro, 77 Performance, 82 Global: none
  // is stored in a preset (§10.4).
  PresetState p = base;
  for (const uint32_t id : {27u, 28u, 31u, 69u, 77u, 81u, 82u}) p.leaves[p.leafCount++] = {id, 1.0f};
  std::sort(p.leaves, p.leaves + p.leafCount,
            [](const PresetLeaf& a, const PresetLeaf& b) { return a.id < b.id; });
  for (const LoadMode mode : {LoadMode::Exact, LoadMode::Spillover}) {
    LoadReport report;
    REQUIRE_FALSE(e.LoadPreset(p, mode, &report));
    CHECK(report.applied);
    CHECK(report.unknownIds == 7u);
    CHECK(report.missingIds == 0u);
    CHECK(report.duplicateIds == 0u);
    CHECK(report.changedValues == 0u);
    CHECK(e.GetParam(ParamId::DelayMs) == 300.0f);
    CHECK(e.GetParam(ParamId::EffectVolumeDb) == -6.0f);  // the stored 1.0 is ignored
    CHECK(e.GetParam(ParamId::VoiceCount) == 0.0f);
  }
  LoadReport checked;
  CHECK_FALSE(CheckPreset(p, &checked));
  CHECK(checked.unknownIds == 7u);
  CHECK(CheckPreset(base));
  // The output equals that of the same preset without them.
  const Stereo input = Plucks(12000);
  CHECK(Same(RenderFrom(p, input, {}, true), RenderFrom(base, input, {})));
}

TEST_CASE("Global rows survive every load and Restart; Init resets them") {
  Rig               rig(Config());
  Engine&           e      = rig.engine;
  const PresetState preset = Complete({{ParamId::Mix, 0.2f}});
  const float       vol    = -7.5f;
  e.SetParam(ParamId::EffectVolumeDb, vol);
  REQUIRE(e.LoadPreset(preset, LoadMode::Exact));
  CHECK(e.GetParam(ParamId::EffectVolumeDb) == vol);
  CHECK(e.GetParam(ParamId::Mix) == 0.2f);
  REQUIRE(e.LoadPreset(preset, LoadMode::Spillover));
  CHECK(e.GetParam(ParamId::EffectVolumeDb) == vol);
  e.Restart();
  CHECK(e.GetParam(ParamId::EffectVolumeDb) == vol);
  e.Reset();
  CHECK(e.GetParam(ParamId::EffectVolumeDb) == vol);
  // The SpilloverLoad event, with a SetParam event on the Global row before it at its frame.
  const Stereo input = Plucks(4800);
  Ev           load;
  load.frame  = e.SampleCounter() + 2401;
  load.seq    = 1;
  load.type   = EvType::SpilloverLoad;
  load.preset = &preset;
  Render(e, input, {Set(load.frame, 0, ParamId::EffectVolumeDb, 3.0f), load});
  CHECK(e.GetParam(ParamId::EffectVolumeDb) == 3.0f);
  EngineConfig cfg = Config();
  host::HeapArenas arenas(PlanMemory(cfg));
  REQUIRE(e.Init(cfg, arenas.get()));
  CHECK(e.GetParam(ParamId::EffectVolumeDb) == 0.0f);
}

// ── Domains: every lone change reaches its rebuild (design §7.2) ─────────────────────────

namespace {

// A busy start state in which a change to any Leaf row is audible within the render: every
// post stage engaged, onsets triggering grains (the mode's onset source) among about eight
// periodic voices.
const Params kBusy = {
    {ParamId::DelayMs, 120.0f},     {ParamId::Mix, 0.9f},          {ParamId::Feedback, 0.4f},
    {ParamId::GrainSizeMs, 60.0f},  {ParamId::Overlap, 0.5f},      {ParamId::SprayMs, 10.0f},
    {ParamId::TransposeSt, 0.0f},   {ParamId::SpreadCents, 10.0f}, {ParamId::ReverseProb, 0.2f},
    {ParamId::Jitter, 0.3f},        {ParamId::ModRateHz, 1.3f},    {ParamId::ModDepth, 0.3f},
    {ParamId::DelayTimeMs, 90.0f},  {ParamId::DelayFb, 0.4f},      {ParamId::DelayMix, 0.3f},
    {ParamId::ReverbTime, 0.6f},    {ParamId::ReverbMix, 0.3f},    {ParamId::FilterCutoffHz, 3000.0f},
    {ParamId::FilterRes, 0.3f},     {ParamId::FilterMorph, 0.5f},  {ParamId::TriggerSens, 0.6f}};

// `preset` with the onset source on, and with mark positioning when `mark` (unit tests set the
// structure directly; the golden corpus takes it only from compiled packages, §10.3).
PresetState WithStructure(PresetState preset, bool onset, bool mark) {
  if (onset) preset.mode.schedule.sources = static_cast<uint8_t>(preset.mode.schedule.sources | kSourceOnset);
  if (mark) preset.mode.layers[0].source = PositionSource::Mark;
  preset.mode.features = RequiredModeFeatures(preset.mode);
  return preset;
}
PresetState Busy() { return WithStructure(Complete(kBusy), true, false); }

// The value each Leaf row changes to. A new Leaf row must be added here (the test requires
// an entry for every Leaf row).
const std::map<ParamId, float> kChangeTo = {
    {ParamId::DelayMs, 400.0f},       {ParamId::Mix, 0.3f},           {ParamId::Feedback, 0.9f},
    {ParamId::WetTrimDb, -9.0f},      {ParamId::GrainSizeMs, 25.0f},  {ParamId::Overlap, 0.8f},
    {ParamId::SprayMs, 150.0f},       {ParamId::TransposeSt, 7.0f},   {ParamId::SpreadCents, 80.0f},
    {ParamId::ReverseProb, 1.0f},     {ParamId::Jitter, 1.0f},        {ParamId::WindowSustain, 1.0f},
    {ParamId::WindowSkew, 0.0f},      {ParamId::WindowSmooth, 0.0f},  {ParamId::PanSpread, 1.0f},
    {ParamId::ModRateHz, 7.0f},       {ParamId::ModDepth, 1.0f},      {ParamId::DelayTimeMs, 400.0f},
    {ParamId::DelayFb, 0.9f},         {ParamId::DelayMix, 1.0f},      {ParamId::ReverbTime, 1.0f},
    {ParamId::ReverbMix, 1.0f},       {ParamId::FilterCutoffHz, 400.0f}, {ParamId::FilterRes, 1.0f},
    {ParamId::FilterMorph, 2.0f},     {ParamId::TriggerSens, 1.0f},   {ParamId::EffectVolumeDb, -9.0f}};

constexpr int64_t kChangeFrame = 2401;  // off the 48-frame grid, before the second pluck

// A Leaf row of exactly `domain` other than `except`, and a value other than the busy state's.
std::pair<ParamId, float> Other(uint8_t domain, ParamId except) {
  for (size_t i = 0; i < kNumLeafParams; ++i) {
    const ParamDescriptor& d = *FindParam(LeafId(i));
    if (d.id != except && d.domain == domain) return {d.id, d.min == 0.0f ? 0.123f : d.min};
  }
  FAIL("no other leaf of the domain");
  return {except, 0.0f};
}

// The renders of the busy state with `id` changed alone at kChangeFrame, and with the same
// change among edits that rebuild the granular and post domains without changing any other
// value: a granular and a post leaf each set to another value and back at the same frame. A
// row the engine routes to the rebuild that reads it (its domain bits, R1) renders the same both
// ways; its change must also be audible.
struct LoneChange {
  Stereo lone, among;
};
const Stereo& BusyInput() {
  static const Stereo input = Plucks(36000);
  return input;
}
const Stereo& BusyUnchanged() {
  static const Stereo out = RenderFrom(Busy(), BusyInput(), {});
  return out;
}
LoneChange RenderLoneChange(ParamId id) {
  const Stereo&     input  = BusyInput();
  const PresetState preset = Busy();
  const float       to     = kChangeTo.at(id);
  const auto        g      = Other(kDomainGranular, id);
  const auto        p      = Other(kDomainPost, id);
  const float       gWas   = preset.leaves[LeafIndex(g.first)].value;
  const float       pWas   = preset.leaves[LeafIndex(p.first)].value;
  const float       was    = IsLeaf(id) ? preset.leaves[LeafIndex(id)].value : FindParam(id)->def;
  REQUIRE(Bits(was) != Bits(to));
  REQUIRE(Bits(gWas) != Bits(g.second));
  REQUIRE(Bits(pWas) != Bits(p.second));
  LoneChange r;
  r.lone  = RenderFrom(preset, input, {Set(kChangeFrame, 0, id, to)});
  r.among = RenderFrom(preset, input,
                       {Set(kChangeFrame, 0, id, to), Set(kChangeFrame, 1, g.first, g.second),
                        Set(kChangeFrame, 2, g.first, gWas), Set(kChangeFrame, 3, p.first, p.second),
                        Set(kChangeFrame, 4, p.first, pWas)});
  return r;
}

}  // namespace

TEST_CASE("a lone change to any Leaf row, or the effect volume, takes effect at its frame") {
  std::vector<ParamId> ids;
  for (size_t i = 0; i < kNumLeafParams; ++i) ids.push_back(LeafId(i));
  ids.push_back(ParamId::EffectVolumeDb);  // the Global row (§3.8), Wet like the trim
  for (const ParamId id : ids) {
    REQUIRE(kChangeTo.count(id) == 1u);  // one lone change per Leaf row (design §7.2)
    INFO(FindParam(id)->name);
    const LoneChange r = RenderLoneChange(id);
    // The change is audible among the other edits, so comparing with it means something.
    CHECK_FALSE(Same(r.among, BusyUnchanged()));
    CHECK(Same(r.lone, r.among));
  }
}

// The routing bug of design §7.2 (record §2.1): sound revision 1 sent a change to 27 or 28 to
// the post rebuild, which did not read them, so alone it never reached the grains. Since r2 they
// are structure, which a load changes as a whole (§7.3 step 3: every domain rebuilds), so a load
// that changes only the mode takes effect at its frame, and renders as the same load among
// other edits does.
TEST_CASE("a Spillover load that changes only the mode reaches the grains at its frame") {
  const Stereo&     input = BusyInput();
  for (const bool mark : {false, true}) {
    INFO((mark ? "mark positioning on" : "the onset source off"));
    const PresetState to = WithStructure(Complete(kBusy), !mark, mark);
    Ev load;
    load.frame  = kChangeFrame;
    load.seq    = 0;
    load.type   = EvType::SpilloverLoad;
    load.preset = &to;
    const auto   g    = Other(kDomainGranular, ParamId::Mix);
    const float  gWas = to.leaves[LeafIndex(g.first)].value;
    const Stereo lone = RenderFrom(Busy(), input, {load});
    const Stereo among =
        RenderFrom(Busy(), input,
                   {load, Set(kChangeFrame, 1, g.first, g.second), Set(kChangeFrame, 2, g.first, gWas)});
    CHECK_FALSE(Same(lone, BusyUnchanged()));
    CHECK(Same(lone, among));
  }
}
