// Sound revision 2's mode runtime (docs/design/mode-compiler.md §7, §10.4): LoadPreset's steps
// with the mode and CTRL (validation, the per-kind rules, sinceRev, performance state, modes
// compared by content), the macro and expression events on the mode and CTRL a load installs,
// the wet gain and the cutoff kill, and Trails and FastCut mode switches, with their block-split
// invariance (contract #1) and a hostile caller's FP environment (determinism profile §4.1);
// and sound revision 3's Mix law (§7.1, R3b).
#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "FpEnvTestUtil.h"
#include "brainscape/Engine.h"
#include "brainscape/HostArenas.h"
#include "brainscape/Mode.h"
#include "brainscape/ModeEval.h"
#include "brainscape/Preset.h"
#include "brainscape/SoundRevision.h"
#include "catch.hpp"
#include "detail/MixLaw.h"

using namespace brainscape;

namespace {

using Ev     = Engine::Event;
using EvType = Engine::EventType;
using Params = std::vector<std::pair<ParamId, float>>;

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

// Plucks every 100 ms over a quiet noise floor, so onsets fire and marks are recorded.
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

Stereo Dc(size_t frames, float value) {
  return {std::vector<float>(frames, value), std::vector<float>(frames, value)};
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

// A complete preset, `params` over the defaults, of the default mode; on the heap (§5.1).
std::unique_ptr<PresetState> Complete(const Params& params) {
  auto s = std::make_unique<PresetState>();
  for (size_t i = 0; i < kNumLeafParams; ++i) {
    s->leaves[i] = {static_cast<uint32_t>(LeafId(i)), FindParam(LeafId(i))->def};
  }
  s->leafCount = static_cast<uint32_t>(kNumLeafParams);
  for (const auto& p : params) {
    REQUIRE(IsLeaf(p.first));
    s->leaves[LeafIndex(p.first)].value = p.second;
  }
  return s;
}

// The onset source on and mark positioning, as structure (the retired rows 27 and 28).
void Structure(PresetState* s, bool onset, bool mark) {
  if (onset) s->mode.schedule.sources = static_cast<uint8_t>(s->mode.schedule.sources | kSourceOnset);
  s->mode.layers[0].source = mark ? PositionSource::Mark : PositionSource::Live;
  s->mode.features         = RequiredModeFeatures(s->mode);
  REQUIRE(ComputeModeHash(s->mode, &s->mode.modeHash));
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
Ev Macro(int64_t frame, uint32_t seq, ParamId macro, float position) {
  Ev e = Set(frame, seq, macro, position);
  e.type = EvType::MacroMove;
  return e;
}
Ev Expr(int64_t frame, uint32_t seq, float position) {
  Ev e;
  e.frame = frame;
  e.seq   = seq;
  e.type  = EvType::Expression;
  e.value = position;
  return e;
}
Ev Load(int64_t frame, uint32_t seq, const PresetState* preset,
        SwitchStyle style = SwitchStyle::Trails) {
  Ev e;
  e.frame  = frame;
  e.seq    = seq;
  e.type   = EvType::SpilloverLoad;
  e.id     = static_cast<uint32_t>(style);
  e.preset = preset;
  return e;
}

// Renders `in` from the engine's current frame in blocks of `pattern` (repeated), with
// `events` (sorted by frame, then sequence) stamped into each block. With `word`, that control
// word is the caller's around every Process call, as a careless host would leave it; each call
// must hand it back.
Stereo Render(Engine& e, const Stereo& in, const std::vector<Ev>& events,
              const std::vector<uint32_t>& pattern = {48},
              const detail::FpWord* word = nullptr) {
  const int64_t start = e.SampleCounter();
  Stereo        out{std::vector<float>(in.l.size()), std::vector<float>(in.l.size())};
  std::vector<Engine::BlockEvent> block;
  size_t next = 0, bi = 0, wordsLost = 0;
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
    if (word == nullptr) {
      e.Process(ctx);
    } else {
      const testing::HostileFpScope scope(*word);
      e.Process(ctx);
      wordsLost += detail::ReadFpControl() != *word ? 1u : 0u;
    }
    pos += n;
  }
  REQUIRE(wordsLost == 0u);
  return out;
}

// From the exact-restart state with `preset`, the render of `in` with `events`.
Stereo RenderFrom(const PresetState& preset, const Stereo& in, const std::vector<Ev>& events,
                  const std::vector<uint32_t>& pattern = {48}) {
  Rig rig;
  REQUIRE(rig.engine.LoadPreset(preset, LoadMode::Exact));
  return Render(rig.engine, in, events, pattern);
}

// A mode whose macros include aux1 with a curve, a reversed range and an in_range window, and
// whose CTRL assigns the pedal to macro.time and to post.reverb.mix.
std::unique_ptr<PresetState> CustomMode(const Params& params) {
  auto        s = Complete(params);
  MacroTable& t = s->mode.macros;
  t.macros[t.macroCount++] =
      MacroDef{static_cast<uint32_t>(ParamId::MacroAux1), t.targetCount, 2, 0};
  t.targets[t.targetCount++] =
      MacroTarget{static_cast<uint32_t>(ParamId::DelayTimeMs), 1500.0f, 40.0f, 0.2f, 0.8f, 2.0f};
  t.targets[t.targetCount++] =
      MacroTarget{static_cast<uint32_t>(ParamId::TransposeSt), 0.0f, 12.0f, 0.0f, 1.0f, 0.5f};
  REQUIRE(ComputeModeHash(s->mode, &s->mode.modeHash));
  ControlState& c          = s->control;
  c.positions[c.macroCount++] = MacroPosition{static_cast<uint32_t>(ParamId::MacroAux1), 0.5f};
  c.exprCount      = 2;
  c.expressions[0] = ExpressionAssignment{static_cast<uint32_t>(ParamId::MacroTime), 0.1f, 0.9f, 2.0f};
  c.expressions[1] = ExpressionAssignment{static_cast<uint32_t>(ParamId::ReverbMix), 0.0f, 0.8f, 1.0f};
  PresetDiagnostic d;
  INFO(PresetErrorName(d.error));
  REQUIRE(ValidateMode(*s, &d));
  return s;
}

const Params kBusy = {{ParamId::Mix, 0.8f},        {ParamId::Feedback, 0.4f},
                      {ParamId::GrainSizeMs, 60.0f}, {ParamId::DelayMs, 120.0f},
                      {ParamId::DelayMix, 0.3f},     {ParamId::DelayTimeMs, 90.0f},
                      {ParamId::ReverbMix, 0.3f},    {ParamId::FilterCutoffHz, 3000.0f}};

}  // namespace

// ── LoadPreset with the mode and CTRL (design §7.3) ────────────────────────────────────────

TEST_CASE("an invalid mode or CTRL applies nothing, by any kind of load", "[modes]") {
  const Stereo input = Plucks(9600);
  const auto   good  = Complete(kBusy);
  // A mode this build cannot play (a mark walk, W2; until sound revision 5 this case was a
  // pitch set), and a CTRL that does not match MACR.
  auto unsupported = Complete({{ParamId::Mix, 0.1f}});
  unsupported->mode.layers[0].markWalk = MarkWalk::Cascade;
  unsupported->mode.features           = RequiredModeFeatures(unsupported->mode);
  REQUIRE((unsupported->mode.features & ~kSupportedModeFeatures) == kModeFeatureMarkWalk);
  auto mismatched                       = Complete({{ParamId::Mix, 0.1f}});
  mismatched->control.macroCount        = 5;
  mismatched->control.positions[5]      = MacroPosition{};
  for (const PresetState* bad : {unsupported.get(), mismatched.get()}) {
    LoadReport checked;
    CHECK_FALSE(CheckPreset(*bad, &checked));
    CHECK(checked.invalidMode);
    for (const LoadMode mode : {LoadMode::Exact, LoadMode::Spillover}) {
      Rig rig;
      REQUIRE(rig.engine.LoadPreset(*good, LoadMode::Exact));
      LoadReport report;
      CHECK_FALSE(rig.engine.LoadPreset(*bad, mode, &report));
      CHECK_FALSE(report.applied);
      CHECK(report.invalidMode);
      CHECK_FALSE(report.exact);
      CHECK(rig.engine.GetParam(ParamId::Mix) == 0.8f);  // the leaves stay
      CHECK(rig.engine.ModeSwitches() == 0u);
    }
    // As a SpilloverLoad event: the render is the one without it.
    CHECK(Same(RenderFrom(*good, input, {Load(4801, 0, bad, SwitchStyle::FastCut)}),
               RenderFrom(*good, input, {})));
  }
}

TEST_CASE("a load's missing and unknown leaves, sinceRev and performance state", "[modes]") {
  // A leafless state is never exact: soundRev 0 (not from a package) and a revision above this
  // build's both count as this build's, at which every Leaf row exists (§7.3 step 2).
  // A revision counts the rows that existed at it: a revision-1 package lacks wave 1's leaves
  // without missing them.
  for (const uint32_t rev : {0u, 1u, kSoundRevision, 1000u}) {
    auto empty      = std::make_unique<PresetState>();
    empty->soundRev = rev;
    const uint32_t at = rev == 0u || rev > kSoundRevision ? kSoundRevision : rev;
    uint32_t       existed = 0;
    for (size_t i = 0; i < kNumLeafParams; ++i) existed += FindParam(LeafId(i))->sinceRev <= at ? 1u : 0u;
    LoadReport report;
    CHECK_FALSE(CheckPreset(*empty, &report));
    CHECK(report.missingIds == existed);
    CHECK_FALSE(report.invalidMode);
  }
  {
    uint32_t r1 = 0;
    for (size_t i = 0; i < kNumLeafParams; ++i) r1 += FindParam(LeafId(i))->sinceRev == 1u ? 1u : 0u;
    CHECK(r1 == 26u);  // sound revision 1's rows but the retired 27 and 28
  }
  for (size_t i = 0; i < kNumLeafParams; ++i) {
    CHECK(FindParam(LeafId(i))->sinceRev >= 1u);
    CHECK(FindParam(LeafId(i))->sinceRev <= kSoundRevision);
  }
  // Stored performance state this build cannot play yet (W2): applied, but inexact.
  auto preset                  = Complete({{ParamId::Mix, 0.25f}});
  preset->performance.reverse  = 1;
  preset->performance.timeMode = TimeMode::Tempo;
  Rig        rig;
  LoadReport report;
  CHECK_FALSE(rig.engine.LoadPreset(*preset, LoadMode::Exact, &report));
  CHECK(report.applied);
  CHECK(report.unsupported == 2u);
  CHECK(rig.engine.GetParam(ParamId::Mix) == 0.25f);
  preset->performance = PerformanceState{};
  CHECK(rig.engine.LoadPreset(*preset, LoadMode::Exact, &report));
  CHECK(report.unsupported == 0u);
}

TEST_CASE("the engine compares modes by content, never by modeHash", "[modes]") {
  const Stereo input = Plucks(14400);
  auto         live  = Complete(kBusy);
  auto         marks = Complete(kBusy);
  Structure(marks.get(), true, true);
  // The marks mode with live's modeHash: stale, and not trusted.
  auto stale      = std::make_unique<PresetState>(*marks);
  stale->mode.modeHash = live->mode.modeHash;
  // Live's content with a garbage modeHash: the same mode.
  auto garbage = std::make_unique<PresetState>(*live);
  garbage->mode.modeHash.bytes[0] ^= 0xFFu;
  Rig rig;
  REQUIRE(rig.engine.LoadPreset(*live, LoadMode::Exact));
  const uint32_t before = rig.engine.ModeSwitches();
  REQUIRE(rig.engine.LoadPreset(*garbage, LoadMode::Spillover));
  CHECK(rig.engine.ModeSwitches() == before);  // same content
  REQUIRE(rig.engine.LoadPreset(*stale, LoadMode::Spillover));
  CHECK(rig.engine.ModeSwitches() == before + 1u);  // different content, the same hash
  REQUIRE(rig.engine.LoadPreset(*marks, LoadMode::Exact));
  CHECK(rig.engine.ModeSwitches() == before + 1u);
  REQUIRE(rig.engine.LoadPreset(*live, LoadMode::Exact));
  CHECK(rig.engine.ModeSwitches() == before + 2u);
  // What plays is the content: the stale-hash load renders as the true one.
  CHECK(Same(RenderFrom(*live, input, {Load(4801, 0, stale.get())}),
             RenderFrom(*live, input, {Load(4801, 0, marks.get())})));
  CHECK_FALSE(Same(RenderFrom(*live, input, {Load(4801, 0, stale.get())}),
                   RenderFrom(*live, input, {Load(4801, 0, garbage.get())})));
}

TEST_CASE("onset and mark positioning come from the mode", "[modes]") {
  const Stereo input = Plucks(19200);
  auto         plain = Complete(kBusy);
  auto         onset = Complete(kBusy);
  auto         marks = Complete(kBusy);
  Structure(onset.get(), true, false);
  Structure(marks.get(), true, true);
  const Stereo a = RenderFrom(*plain, input, {});
  const Stereo b = RenderFrom(*onset, input, {});
  const Stereo c = RenderFrom(*marks, input, {});
  CHECK_FALSE(Same(a, b));
  CHECK_FALSE(Same(b, c));
  // The retired rows are no-ops: setting them changes nothing.
  CHECK(Same(RenderFrom(*plain, input,
                        {Set(1001, 0, ParamId::OnsetTrigger, 1.0f),
                         Set(1001, 1, ParamId::PositionSource, 1.0f)}),
             a));
}

// ── The wet gain (design §7.2, R3) ─────────────────────────────────────────────────────────

TEST_CASE("the trim and the effect volume scale the wet signal only, as one gain", "[modes]") {
  const Stereo input = Plucks(14400);
  // At mix 0 the output is the dry input, bit for bit, whatever the trim and effect volume (the
  // plucks hold no −0; the Mix law's hostile-input case below pins the sign of a zero).
  for (const float trim : {-24.0f, 0.0f, 24.0f}) {
    Rig rig;
    rig.engine.SetParam(ParamId::EffectVolumeDb, -9.0f);
    REQUIRE(rig.engine.LoadPreset(*Complete({{ParamId::Mix, 0.0f}, {ParamId::WetTrimDb, trim},
                                             {ParamId::Feedback, 0.5f}}),
                                  LoadMode::Exact));
    const Stereo out = Render(rig.engine, input, {});
    CHECK(Same(out, input));
  }
  // The trim and the effect volume add in dB before the one conversion (WetGainTarget).
  auto render = [&](float trim, float volume) {
    Rig rig;
    rig.engine.SetParam(ParamId::EffectVolumeDb, volume);
    REQUIRE(rig.engine.LoadPreset(*Complete({{ParamId::Mix, 0.6f}, {ParamId::WetTrimDb, trim},
                                             {ParamId::ReverbMix, 0.3f}}),
                                  LoadMode::Exact));
    return Render(rig.engine, input, {});
  };
  const Stereo trimmed = render(-6.0f, 0.0f);
  CHECK(Same(trimmed, render(0.0f, -6.0f)));
  CHECK(Same(trimmed, render(-3.0f, -3.0f)));
  CHECK_FALSE(Same(trimmed, render(0.0f, 0.0f)));
}

TEST_CASE("the cutoff minimum kills the wet; a trim change while killed stays muted", "[modes]") {
  const Stereo input = Plucks(5 * 48000);
  Rig          rig;
  REQUIRE(rig.engine.LoadPreset(*Complete({{ParamId::Mix, 1.0f}, {ParamId::FilterMorph, 2.0f},
                                           {ParamId::FilterCutoffHz, 800.0f},
                                           {ParamId::Feedback, 0.5f}}),
                                LoadMode::Exact));
  // Into the kill by SetParam at 1 s, a trim change at 2 s, out at 3 s; then the Filter macro
  // (the default mode's: 40 Hz-20 kHz) at 0 kills at 4 s.
  const Stereo out =
      Render(rig.engine, input,
             {Set(48001, 0, ParamId::FilterCutoffHz, 40.0f), Set(96001, 0, ParamId::WetTrimDb, 12.0f),
              Set(144001, 0, ParamId::FilterCutoffHz, 41.0f), Macro(192001, 0, ParamId::MacroFilter, 0.0f)});
  auto silent = [&](size_t from, size_t to) {
    for (size_t i = from; i < to; ++i) {
      if (Bits(out.l[i]) << 1 != 0u || Bits(out.r[i]) << 1 != 0u) return false;  // ±0 only
    }
    return true;
  };
  auto active = [&](size_t from, size_t to) { return !silent(from, to); };
  CHECK(active(24000, 48001));
  CHECK(silent(48001 + 24000, 144001));  // the smoother reaches exactly 0, the trim stays muted
  CHECK(active(144001 + 4800, 192001));   // 41 Hz is not the kill
  CHECK(silent(192001 + 24000, out.l.size()));
  CHECK(rig.engine.GetParam(ParamId::FilterCutoffHz) == 40.0f);
}

// ── The Mix law (design §7.1, R3b; sound revision 3) ───────────────────────────────────────

namespace {

// The law as the design states it, written apart from the engine's (detail/MixLaw.h): dry at
// unity up to the middle and wet at unity from it, each falling linearly to 0 at its far end.
float LawDry(float m) { return m <= 0.5f ? 1.0f : 2.0f * (1.0f - m); }
float LawWet(float m) { return m >= 0.5f ? 1.0f : 2.0f * m; }

Params WithMix(Params params, float mix, float cutoffHz = 3000.0f) {
  for (auto& p : params) {
    if (p.first == ParamId::Mix) p.second = mix;
    if (p.first == ParamId::FilterCutoffHz) p.second = cutoffHz;
  }
  return params;
}

}  // namespace

TEST_CASE("the Mix law's gains are exact, monotonic and at unity across the middle", "[modes]") {
  using detail::MixLaw;
  CHECK(Bits(MixLaw(0.0f).dry) == Bits(1.0f));
  CHECK(Bits(MixLaw(0.0f).wet) == Bits(0.0f));
  CHECK(Bits(MixLaw(0.5f).dry) == Bits(1.0f));
  CHECK(Bits(MixLaw(0.5f).wet) == Bits(1.0f));
  CHECK(Bits(MixLaw(1.0f).dry) == Bits(0.0f));
  CHECK(Bits(MixLaw(1.0f).wet) == Bits(1.0f));
  CHECK(MixLaw(0.25f).wet == 0.5f);
  CHECK(MixLaw(0.75f).dry == 0.5f);
  // Every binary32 Mix within 2^14 ulps of 0, 0.5 and 1, and one in 4,093 elsewhere in [0, 1], in
  // ascending order: each gain is the law's, the reals' 2m and 2(1 - m) exactly (checked in
  // binary64), within [0, 1]; dry never rises, wet never falls, and they never sum below 1, so
  // no Mix plays less than unity in all.
  const uint32_t half = Bits(0.5f), one = Bits(1.0f), window = 1u << 14;
  size_t         checked = 0, wrong = 0;
  float          lastDry = 1.0f, lastWet = 0.0f;
  for (uint32_t u = 0;;) {
    float m;
    std::memcpy(&m, &u, sizeof m);
    const detail::MixGains g  = MixLaw(m);
    const double           dm = static_cast<double>(m);
    const bool ok = Bits(g.dry) == Bits(LawDry(m)) && Bits(g.wet) == Bits(LawWet(m)) &&
                    (m <= 0.5f || static_cast<double>(g.dry) == 2.0 - 2.0 * dm) &&
                    (m >= 0.5f || static_cast<double>(g.wet) == 2.0 * dm) && g.dry >= 0.0f &&
                    g.dry <= 1.0f && g.wet >= 0.0f && g.wet <= 1.0f && g.dry <= lastDry &&
                    g.wet >= lastWet && static_cast<double>(g.dry) + g.wet >= 1.0;
    wrong += ok ? 0u : 1u;
    ++checked;
    lastDry = g.dry;
    lastWet = g.wet;
    if (u == one) break;
    const bool dense = u < window || (u + window > half && u < half + window) || u + window > one;
    u                = dense ? u + 1u : std::min(u + 4093u, one);
  }
  CHECK(checked > 300000u);
  CHECK(wrong == 0u);
}

TEST_CASE("Mix 0 plays the dry input and Mix 1 the wet only; the middle plays both at unity",
          "[modes]") {
  const Stereo input = Plucks(14400);
  // The wet path never reads Mix, so the render at Mix 1 is the wet signal itself when the law
  // holds; every Mix then plays dry·LawDry(m) + wet·LawWet(m), bit for bit. A dry share left at
  // Mix 1, or a wet below unity above the middle, would break the cases above 0.5; a dry below
  // unity up to the middle, those below it.
  const Stereo wet = RenderFrom(*Complete(WithMix(kBusy, 1.0f)), input, {});
  CHECK(Same(RenderFrom(*Complete(WithMix(kBusy, 0.0f)), input, {}), input));
  for (const float m : {0.1f, 0.25f, 0.35f, 0.45f, 0.5f, 0.55f, 0.75f, 0.9f}) {
    INFO("mix " << m);
    Stereo expected = input;
    for (size_t i = 0; i < input.l.size(); ++i) {
      expected.l[i] = input.l[i] * LawDry(m) + wet.l[i] * LawWet(m);
      expected.r[i] = input.r[i] * LawDry(m) + wet.r[i] * LawWet(m);
    }
    CHECK(Same(RenderFrom(*Complete(WithMix(kBusy, m)), input, {}), expected));
  }
  // With the wet killed from the load's frame (the cutoff minimum), the dry alone: the input
  // itself up to the middle, half of it at 0.75, and silence at Mix 1 while the input sounds.
  for (const float m : {0.0f, 0.3f, 0.5f, 0.75f}) {
    INFO("killed, mix " << m);
    Stereo expected = input;
    for (size_t i = 0; i < input.l.size(); ++i) {
      expected.l[i] = input.l[i] * LawDry(m);
      expected.r[i] = input.r[i] * LawDry(m);
    }
    CHECK(Same(RenderFrom(*Complete(WithMix(kBusy, m, 40.0f)), input, {}), expected));
  }
  const Stereo muted = RenderFrom(*Complete(WithMix(kBusy, 1.0f, 40.0f)), input, {});
  size_t       sounding = 0;
  for (size_t i = 0; i < input.l.size(); ++i) {
    sounding += (Bits(muted.l[i]) << 1 | Bits(muted.r[i]) << 1) != 0u ? 1u : 0u;  // not ±0
  }
  CHECK(sounding == 0u);
}

TEST_CASE("Mix 0 passes hostile input through bit for bit, up to the sign of a zero", "[modes]") {
  // Every 7th frame of the plucks replaced, per channel, by −0, +0, a subnormal or a value near
  // ±FLT_MAX. At Mix 0 the engine plays dry·1 + wet·0 (detail/MixLaw.h): every sample is the
  // input's bits except a −0 input, which comes out −0 + (wet·0), a zero with the wet sample's
  // sign (−0 + +0 is +0). The wet sample there is the Mix 1 render's, −0·0 + wet, the wet's bits.
  Stereo   input = Plucks(14400);
  uint32_t x     = 0x2468ACE1u;
  auto     next  = [&x] {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
  };
  auto fromBits = [](uint32_t u) {
    float v;
    std::memcpy(&v, &u, sizeof v);
    return v;
  };
  size_t injected[4] = {};
  for (size_t i = 0; i < input.l.size(); i += 7) {
    for (float* s : {&input.l[i], &input.r[i]}) {
      const uint32_t kind = next() % 4u, r = next(), sign = next() & 0x80000000u;
      ++injected[kind];
      *s = kind == 0u   ? fromBits(0x80000000u)
           : kind == 1u ? 0.0f
           : kind == 2u ? fromBits(sign | ((r & 0x007FFFFFu) | 1u))
                        : fromBits(sign | 0x7F000000u | (r & 0x007FFFFFu));  // 1.7e38 to FLT_MAX
    }
  }
  for (const size_t n : injected) CHECK(n > 400u);
  const Stereo wet = RenderFrom(*Complete(WithMix(kBusy, 1.0f)), input, {});
  const Stereo out = RenderFrom(*Complete(WithMix(kBusy, 0.0f)), input, {});
  size_t       wrong = 0, keptNegative = 0, cleared = 0;
  for (size_t i = 0; i < input.l.size(); ++i) {
    for (int c = 0; c < 2; ++c) {
      const uint32_t in = Bits(c == 0 ? input.l[i] : input.r[i]);
      const uint32_t o  = Bits(c == 0 ? out.l[i] : out.r[i]);
      if (in != 0x80000000u) {
        wrong += o != in ? 1u : 0u;
        continue;
      }
      const uint32_t w = Bits(c == 0 ? wet.l[i] : wet.r[i]);
      wrong += o != (w & 0x80000000u) ? 1u : 0u;  // a zero with the wet sample's sign
      (o == 0u ? cleared : keptNegative) += 1u;
    }
  }
  CHECK(wrong == 0u);
  // Both outcomes occur: the sign of a zero at Mix 0 follows the wet, so "bit for bit" holds up
  // to it and no further.
  CHECK(cleared > 0u);
  CHECK(keptNegative > 0u);
}

TEST_CASE("a Mix move lands on the endpoints' bits once smoothed", "[modes]") {
  const Stereo input = Plucks(48000);
  // From 0.2 to 1 and from 0.6 to 0 at 0.1 s: once the smoother lands on its target (within
  // 0.5 s), the output is the static Mix 1 and Mix 0 renders' bits.
  const Stereo wet  = RenderFrom(*Complete(WithMix(kBusy, 1.0f)), input, {});
  const Stereo up   = RenderFrom(*Complete(WithMix(kBusy, 0.2f)), input,
                                 {Set(4801, 0, ParamId::Mix, 1.0f)});
  const Stereo down = RenderFrom(*Complete(WithMix(kBusy, 0.6f)), input,
                                 {Set(4801, 0, ParamId::Mix, 0.0f)});
  const size_t from = 4801 + 24000;
  auto         tail = [&](const Stereo& a, const Stereo& b) {
    const size_t bytes = (a.l.size() - from) * sizeof(float);
    return std::memcmp(a.l.data() + from, b.l.data() + from, bytes) == 0 &&
           std::memcmp(a.r.data() + from, b.r.data() + from, bytes) == 0;
  };
  CHECK(tail(up, wet));
  CHECK(tail(down, input));
  CHECK_FALSE(Same(up, wet));  // before the move, Mix 0.2 plays
}

// ── Macro and expression events (design §3.4) ──────────────────────────────────────────────

TEST_CASE("a MacroMove applies its targets as SetParam events would, in list order", "[modes]") {
  const Stereo input  = Plucks(14400);
  const auto   preset = CustomMode(kBusy);
  for (const ParamId macro : {ParamId::MacroActivity, ParamId::MacroShape, ParamId::MacroTime,
                              ParamId::MacroFilter, ParamId::MacroAux1}) {
    for (const float position : {0.0f, 0.3f, 1.0f}) {
      INFO("macro " << static_cast<uint32_t>(macro) << " at " << position);
      PresetLeaf   out[kMaxMacroTargets];
      const size_t n = EvalMacro(preset->mode, macro, position, out, kMaxMacroTargets);
      REQUIRE(n > 0u);
      std::vector<Ev> sets;
      for (size_t i = 0; i < n; ++i) {
        sets.push_back(Set(4801, static_cast<uint32_t>(i), static_cast<ParamId>(out[i].id), out[i].value));
      }
      Rig rig;
      REQUIRE(rig.engine.LoadPreset(*preset, LoadMode::Exact));
      const Stereo moved = Render(rig.engine, input, {Macro(4801, 0, macro, position)});
      for (size_t i = 0; i < n; ++i) {
        CHECK(Bits(rig.engine.GetParam(static_cast<ParamId>(out[i].id))) == Bits(out[i].value));
      }
      CHECK(Same(moved, RenderFrom(*preset, input, sets)));
    }
  }
  // A macro the mode leaves undefined, and an id that is not a macro, do nothing.
  const auto   plain = Complete(kBusy);
  const Stereo none  = RenderFrom(*plain, input, {});
  CHECK(Same(RenderFrom(*plain, input, {Macro(4801, 0, ParamId::MacroAux1, 0.9f)}), none));
  CHECK(Same(RenderFrom(*plain, input, {Macro(4801, 0, ParamId::DelayMs, 0.9f)}), none));
  // SetParam on a macro row is a no-op; a later move at one frame wins over a SetParam.
  CHECK(Same(RenderFrom(*plain, input, {Set(4801, 0, ParamId::MacroRepeats, 1.0f)}), none));
  CHECK(Same(RenderFrom(*plain, input,
                        {Set(4801, 0, ParamId::Feedback, 0.9f), Macro(4801, 1, ParamId::MacroRepeats, 0.0f)}),
             RenderFrom(*plain, input, {Set(4801, 0, ParamId::Feedback, 0.0f)})));
}

TEST_CASE("an Expression event applies CTRL's assignments as SetParam events would", "[modes]") {
  const Stereo input  = Plucks(14400);
  const auto   preset = CustomMode(kBusy);
  for (const float position : {0.0f, 0.4f, 1.0f}) {
    INFO("pedal at " << position);
    PresetLeaf   out[kMaxExpressions * kMaxMacroTargets];
    const size_t n = EvalExpression(preset->mode, preset->control, position, out, 32);
    REQUIRE(n == 2u);  // macro.time's one target, then post.reverb.mix
    REQUIRE(out[0].id == static_cast<uint32_t>(ParamId::DelayMs));
    std::vector<Ev> sets;
    for (size_t i = 0; i < n; ++i) {
      sets.push_back(Set(7001, static_cast<uint32_t>(i), static_cast<ParamId>(out[i].id), out[i].value));
    }
    CHECK(Same(RenderFrom(*preset, input, {Expr(7001, 0, position)}), RenderFrom(*preset, input, sets)));
  }
  // Without assignments the pedal does nothing (§3.4).
  const auto plain = Complete(kBusy);
  CHECK(Same(RenderFrom(*plain, input, {Expr(7001, 0, 0.7f)}), RenderFrom(*plain, input, {})));
}

// A load installs its mode's macro table and its CTRL (design §3.4, §7.3 step 3): the moves
// after it, one at the load's own frame included, evaluate on the loaded preset, never on the one
// before (review finding: a Spillover load that kept either passed every test).
TEST_CASE("moves after a Spillover load evaluate on the loaded mode and CTRL", "[modes]") {
  const Stereo input  = Plucks(14400);
  const auto   plain  = Complete(kBusy);    // the default mode: aux1 undefined, no assignments
  const auto   custom = CustomMode(kBusy);  // aux1 defined; the pedal on macro.time, reverb mix
  PresetLeaf   m[kMaxMacroTargets];
  const size_t nm = EvalMacro(custom->mode, ParamId::MacroAux1, 0.9f, m, kMaxMacroTargets);
  PresetLeaf   x[kMaxExpressions * kMaxMacroTargets];
  const size_t nx =
      EvalExpression(custom->mode, custom->control, 0.8f, x, kMaxExpressions * kMaxMacroTargets);
  REQUIRE(nm == 2u);
  REQUIRE(nx == 2u);
  const Stereo loadOnly = RenderFrom(*plain, input, {Load(3001, 0, custom.get())});
  for (const int64_t at : {int64_t{3001}, int64_t{4801}}) {  // at the load's frame, then after it
    INFO("the macro move at frame " << at);
    std::vector<Ev> sets = {Load(3001, 0, custom.get())};
    for (size_t i = 0; i < nm; ++i) {
      sets.push_back(Set(at, static_cast<uint32_t>(1 + i), static_cast<ParamId>(m[i].id), m[i].value));
    }
    for (size_t i = 0; i < nx; ++i) {
      sets.push_back(Set(6001, static_cast<uint32_t>(i), static_cast<ParamId>(x[i].id), x[i].value));
    }
    const Stereo viaMoves = RenderFrom(*plain, input,
        {Load(3001, 0, custom.get()), Macro(at, 1, ParamId::MacroAux1, 0.9f), Expr(6001, 0, 0.8f)});
    CHECK_FALSE(Same(viaMoves, loadOnly));
    CHECK(Same(viaMoves, RenderFrom(*plain, input, sets)));
  }
}

TEST_CASE("a load of the same mode with another CTRL installs that CTRL", "[modes]") {
  const Stereo input  = Plucks(14400);
  const auto   first  = CustomMode(kBusy);
  auto         second = CustomMode(kBusy);  // the same ModeBlob, the pedal on the post delay mix
  second->control.exprCount      = 1;
  second->control.expressions[0] =
      ExpressionAssignment{static_cast<uint32_t>(ParamId::DelayMix), 0.0f, 1.0f, 1.0f};
  second->control.expressions[1] = ExpressionAssignment{};
  PresetDiagnostic d;
  REQUIRE(ValidateMode(*second, &d));
  REQUIRE(std::memcmp(&first->mode, &second->mode, sizeof(ModeBlob)) == 0);
  {
    Rig rig;  // not a mode switch: the content is the same
    REQUIRE(rig.engine.LoadPreset(*first, LoadMode::Exact));
    const uint32_t before = rig.engine.ModeSwitches();
    REQUIRE(rig.engine.LoadPreset(*second, LoadMode::Spillover));
    CHECK(rig.engine.ModeSwitches() == before);
  }
  PresetLeaf   x[kMaxExpressions * kMaxMacroTargets];
  const size_t nx =
      EvalExpression(second->mode, second->control, 0.8f, x, kMaxExpressions * kMaxMacroTargets);
  REQUIRE(nx == 1u);
  const Stereo viaMove = RenderFrom(*first, input, {Load(3001, 0, second.get()), Expr(6001, 0, 0.8f)});
  const Stereo viaSet  = RenderFrom(*first, input,
      {Load(3001, 0, second.get()), Set(6001, 0, static_cast<ParamId>(x[0].id), x[0].value)});
  CHECK(Same(viaMove, viaSet));
  CHECK_FALSE(Same(viaMove, RenderFrom(*first, input, {Load(3001, 0, second.get())})));
}

// ── Trails and FastCut (design §7.3, R7) ───────────────────────────────────────────────────

TEST_CASE("a FastCut load fades the sounding grains over 128 frames; Trails keeps them", "[modes]") {
  // One unity voice at a time over DC (the degenerate clean delay): 100 ms abutting grains,
  // no randomness, no feedback, mix 1, so the output is the grain's sample.
  const Params clean = {{ParamId::DelayMs, 50.0f},   {ParamId::Mix, 1.0f},
                        {ParamId::GrainSizeMs, 100.0f}, {ParamId::Overlap, 0.25f},
                        {ParamId::SprayMs, 0.0f},    {ParamId::Jitter, 0.0f},
                        {ParamId::WindowSustain, 1.0f}, {ParamId::WindowSmooth, 0.0f},
                        {ParamId::PanSpread, 0.0f}};
  EngineConfig cfg    = Config();
  cfg.ditherRingWrite = false;
  const auto   preset = Complete(clean);
  const Stereo input  = Dc(24000, 0.5f);
  const int64_t cut = 9600 + 2407;  // mid-grain: grains are born at 0, 4800, 9600, 14400, ...
  auto render = [&](const std::vector<Ev>& events) {
    Rig rig(cfg);
    REQUIRE(rig.engine.LoadPreset(*preset, LoadMode::Exact));
    return Render(rig.engine, input, events, {1});
  };
  const Stereo trails = render({Load(cut, 0, preset.get(), SwitchStyle::Trails)});
  const Stereo fast   = render({Load(cut, 0, preset.get(), SwitchStyle::FastCut)});
  const float  x      = trails.l[static_cast<size_t>(cut) - 1];
  REQUIRE(x > 0.4f);
  REQUIRE(std::memcmp(trails.l.data(), fast.l.data(), static_cast<size_t>(cut) * sizeof(float)) == 0);
  for (uint32_t k = 0; k < kFastCutFrames; ++k) {
    INFO("frame " << k << " of the fade");
    const float fade = static_cast<float>(kFastCutFrames - k) * (1.0f / 128.0f);
    CHECK(Bits(fast.l[static_cast<size_t>(cut) + k]) == Bits(x * fade));
    CHECK(Bits(trails.l[static_cast<size_t>(cut) + k]) == Bits(x));
  }
  // The faded grain's voice is free: silence until the scheduler's next birth, at 14400 (its
  // phase carries over a Spillover load).
  for (size_t i = static_cast<size_t>(cut) + kFastCutFrames; i < 14400; ++i) {
    REQUIRE(Bits(fast.l[i]) == 0u);
  }
  CHECK(Bits(fast.l[14400]) == Bits(x));
  // A second FastCut 50 frames into the fade fades its own grains only: the first keeps its
  // ramp, it does not start again.
  const Stereo twice = render({Load(cut, 0, preset.get(), SwitchStyle::FastCut),
                               Load(cut + 50, 0, preset.get(), SwitchStyle::FastCut)});
  CHECK(Same(twice, fast));
}

TEST_CASE("mode switches, macro and expression moves are block-split invariant", "[modes]") {
  const Stereo input  = Plucks(24000);
  const auto   preset = CustomMode(kBusy);
  auto         marks  = CustomMode(kBusy);
  Structure(marks.get(), true, true);
  const std::vector<Ev> script = {
      Macro(1001, 0, ParamId::MacroActivity, 0.8f), Expr(2003, 0, 0.3f),
      Load(3007, 0, marks.get(), SwitchStyle::FastCut), Macro(3007, 1, ParamId::MacroAux1, 0.7f),
      Load(3050, 0, preset.get(), SwitchStyle::FastCut), Expr(9001, 0, 0.9f),
      Load(12011, 0, marks.get(), SwitchStyle::Trails), Macro(15013, 0, ParamId::MacroFilter, 0.0f),
      Set(17777, 0, ParamId::WetTrimDb, -6.0f), Macro(19001, 0, ParamId::MacroFilter, 1.0f)};
  const Stereo ref = RenderFrom(*preset, input, script, {1});
  for (const auto& pattern : std::vector<std::vector<uint32_t>>{
           {7}, {48}, {127}, {512}, {48, 1, 127, 32}, {300, 512, 5, 64}}) {
    INFO("block pattern starting " << pattern[0]);
    CHECK(Same(RenderFrom(*preset, input, script, pattern), ref));
  }
}

// Determinism profile §4.1, §6.4: macro, expression and load events run inside Process's guard,
// and CheckPreset and LoadPreset own the control word too, so a hostile caller (FTZ|DAZ or FZ|DN
// with round-toward-zero) gets the clean render and its own word back after every call; on x64 so
// does a caller with every exception unmasked, where FP work outside a guard would trap.
TEST_CASE("macro, expression and load events ignore the caller's FP environment", "[modes]") {
  const Stereo input  = Plucks(24000);
  const auto   preset = CustomMode(kBusy);
  auto         marks  = CustomMode(kBusy);
  Structure(marks.get(), true, true);
  const std::vector<Ev> script = {
      Macro(1001, 0, ParamId::MacroActivity, 0.8f), Expr(2003, 0, 0.3f),
      Load(3007, 0, marks.get(), SwitchStyle::FastCut), Macro(3007, 1, ParamId::MacroAux1, 0.7f),
      Expr(3007, 2, 0.6f), Load(3050, 0, preset.get(), SwitchStyle::FastCut), Expr(9001, 0, 0.9f),
      Load(12011, 0, marks.get(), SwitchStyle::Trails), Macro(15013, 0, ParamId::MacroFilter, 0.0f),
      Set(17777, 0, ParamId::WetTrimDb, -6.0f), Macro(19001, 0, ParamId::MacroFilter, 1.0f)};
  const Stereo clean = RenderFrom(*preset, input, script);
  detail::FpWord words[] = {testing::kHostileFpWord,
#if defined(BRAINSCAPE_FPENV_X64)
                            testing::kTrapAllFpWord,
#endif
  };
  for (const detail::FpWord word : words) {
    INFO("caller's word " << word);
    const EngineConfig cfg = Config();
    MemoryPlan         plan;
    bool               ok = false, exact = false;
    size_t             wordsLost = 0;
    auto call = [&](auto&& fn) {
      const testing::HostileFpScope scope(word);
      fn();
      wordsLost += detail::ReadFpControl() != word ? 1u : 0u;
    };
    call([&] { plan = PlanMemory(cfg); });
    host::HeapArenas arenas(plan);
    REQUIRE(arenas.ok());
    Engine engine;
    call([&] { ok = engine.Init(cfg, arenas.get()); });
    REQUIRE(ok);
    call([&] { exact = CheckPreset(*marks); });
    REQUIRE(exact);
    call([&] { ok = engine.LoadPreset(*preset, LoadMode::Exact); });
    REQUIRE(ok);
    CHECK(Same(Render(engine, input, script, {48}, &word), clean));
    REQUIRE(wordsLost == 0u);
  }
}
