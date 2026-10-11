#include "bench/Configs.h"

#include <algorithm>
#include <cstring>

#include "Render.h"
#include "Sha256.h"
#include "bench/FactoryPackages.h"

namespace brainscape::fw::bench {

using golden::CompletePreset;
using golden::VectorCase;

namespace {

// The structure of the pessimistic configuration: onset-triggered grains positioned at marks,
// the mode of a corpus package (dsp/tests/golden/presets) with the default macros and CTRL,
// which the live image's `onset on` and `marks on` load too (firmware/live/LivePresets.h).
constexpr const char* kOnsetMarks = "strum_marks";

// 0 -> 1 -> 0 over `period` blocks.
float Tri(uint32_t b, uint32_t period) {
  const uint32_t phase = b % period;
  const uint32_t half  = period / 2u;
  return phase < half ? static_cast<float>(phase) / static_cast<float>(half)
                      : static_cast<float>(period - phase) / static_cast<float>(half);
}

void PackFrames(const float* l, const float* r, uint32_t n, uint8_t* bytes) {
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t u[2];
    std::memcpy(&u[0], &l[i], 4);
    std::memcpy(&u[1], &r[i], 4);
    for (int c = 0; c < 2; ++c) {
      for (int b = 0; b < 4; ++b) bytes[8u * i + 4u * c + b] = static_cast<uint8_t>(u[c] >> (8 * b));
    }
  }
}

// The fuzz sets' generator: a 32-bit LCG (Numerical Recipes'), the same on every build.
uint32_t Next(uint32_t* s) {
  *s = *s * 1664525u + 1013904223u;
  return *s;
}
// A uniform choice of n, from the generator's high bits.
uint32_t Pick(uint32_t* s, uint32_t n) { return static_cast<uint32_t>((static_cast<uint64_t>(Next(s) >> 8) * n) >> 24); }
// A position of [0, 1] in 1/1000 steps (exact decimal-free arithmetic: k / 1000 in binary32).
float Unit(uint32_t* s) { return static_cast<float>(Pick(s, 1001u)) / 1000.0f; }

// The fuzz sets' structures: the default mode, then corpus packages' modes and CTRLs.
constexpr const char* kFuzzStructures[] = {nullptr,          "strum_marks",     "burst_spaced",
                                           "repeat_loops",   "pitchset_random", "pitchset_cycle",
                                           "clock_cloud",    "sources_midi",    "voice_limit",
                                           "macro_sweep",    "burst_marks",     "repeat_mark_aging"};
constexpr uint32_t    kNumFuzzStructures = sizeof kFuzzStructures / sizeof kFuzzStructures[0];
constexpr const char* kFuzzVectors[]     = {"strums_16s", "plucks_12s", "onset_bursts_6s"};

// A random value of every Leaf row, canonicalized, from seed s.
ParamList FuzzLeaves(uint32_t* s) {
  ParamList out;
  for (size_t i = 0; i < kNumLeafParams; ++i) {
    const ParamId          id = LeafId(i);
    const ParamDescriptor* d  = FindParam(id);
    out.push_back({id, Canonicalize(id, d->min + (d->max - d->min) * Unit(s))});
  }
  return out;
}

}  // namespace

ParamList Nominal() {
  // grain-engine.md §8's nominal row: 64 voices at unity rate, 20 ms grains, every post stage
  // in moderate use, the tamer in the loop.
  return {{P::Overlap, 1.0f},  {P::GrainSizeMs, 20.0f}, {P::Feedback, 0.5f},
          {P::ModDepth, 0.3f}, {P::DelayMix, 0.3f},     {P::ReverbMix, 0.3f},
          {P::FilterCutoffHz, 5000.0f}};
}

ParamList Pessimistic(float grainMs) {
  // 64 voices at +24 st (r = 4) with full cents spread, reverse, spray and jitter, feedback above
  // unity, every post stage on (modulation at its maximum rate and depth), and in the kOnsetMarks
  // structure onset-triggered grains positioned at marks.
  return {{P::Overlap, 1.0f},        {P::GrainSizeMs, grainMs}, {P::TransposeSt, 24.0f},
          {P::SpreadCents, 100.0f},  {P::ReverseProb, 0.5f},    {P::SprayMs, 200.0f},
          {P::Jitter, 1.0f},         {P::PanSpread, 1.0f},      {P::Feedback, 1.05f},
          {P::ModDepth, 1.0f},       {P::ModRateHz, 10.0f},     {P::DelayMix, 0.5f},
          {P::DelayFb, 0.6f},        {P::DelayTimeMs, 350.0f},  {P::ReverbMix, 0.5f},
          {P::ReverbTime, 0.9f},     {P::FilterCutoffHz, 2000.0f}, {P::FilterRes, 0.5f},
          {P::FilterMorph, 1.5f},    {P::TriggerSens, 0.8f}};
}

ParamList With(ParamList base, const ParamList& changes) {
  for (const auto& c : changes) {
    bool found = false;
    for (auto& kv : base) {
      if (kv.first == c.first) {
        kv.second = c.second;
        found     = true;
      }
    }
    if (!found) base.push_back(c);
  }
  return base;
}

// ---- Setup ---------------------------------------------------------------------------------

namespace {

std::unique_ptr<PresetState> BuildPreset(const PresetSource& preset, const char* structure) {
  std::unique_ptr<PresetState> mode;
  if (structure != nullptr) {
    mode = CompletePreset(PresetSource{structure, {}});
    if (mode == nullptr) return nullptr;
  }
  return CompletePreset(preset, 0, mode.get());
}

// The nominal preset with macro.activity driving eight targets, each on a power curve (2 or 1/2):
// the worst MacroMove the cost governor's event reserve must cover (about 538 cycles a curved
// target, compiler §3.3). The other macros keep §3.2's defaults.
std::unique_ptr<PresetState> EightTargetMacro() {
  std::unique_ptr<PresetState> p = CompletePreset(Nominal());
  if (p == nullptr) return nullptr;
  MacroTable&       t   = p->mode.macros;
  const MacroTable& def = kDefaultMacroTable;
  // activity's own two targets go; eight take their place, then the other macros' targets.
  const MacroTarget eight[kMaxMacroTargets] = {
      {6, 0.25f, 0.85f, 0.0f, 1.0f, 2.0f},     {7, 0.0f, 200.0f, 0.0f, 1.0f, 0.5f},
      {3, 0.0f, 0.9f, 0.0f, 1.0f, 2.0f},       {12, 0.9f, 0.1f, 0.0f, 1.0f, 0.5f},
      {13, 0.3f, 0.7f, 0.0f, 1.0f, 2.0f},      {1, 20.0f, 2000.0f, 0.0f, 1.0f, 0.5f},
      {20, 0.0f, 0.5f, 0.0f, 1.0f, 2.0f},      {22, 0.0f, 1.0f, 0.0f, 1.0f, 0.5f}};
  MacroTable out{};
  out.macroCount  = def.macroCount;
  uint32_t n      = 0;
  for (uint32_t m = 0; m < def.macroCount; ++m) {
    out.macros[m]       = def.macros[m];
    out.macros[m].first = static_cast<uint8_t>(n);
    if (def.macros[m].id == static_cast<uint32_t>(ParamId::MacroActivity)) {
      out.macros[m].count = kMaxMacroTargets;
      for (const MacroTarget& x : eight) out.targets[n++] = x;
    } else {
      for (uint32_t k = 0; k < def.macros[m].count; ++k) out.targets[n++] = def.targets[def.macros[m].first + k];
    }
  }
  out.targetCount = static_cast<uint8_t>(n);
  t               = out;
  return p;
}

}  // namespace

bool Bench::Setup(Engine* engine, const char** why) {
  engine_ = engine;
  corpus_ = golden::BuildCorpus();
  loadA_  = BuildPreset(PresetSource{nullptr, Pessimistic(20.0f)}, kOnsetMarks);
  loadB_  = BuildPreset(PresetSource{nullptr, Nominal()}, nullptr);
  macro8_ = EightTargetMacro();
  if (loadA_ == nullptr || loadB_ == nullptr || !CheckPreset(*loadA_) || !CheckPreset(*loadB_)) {
    *why = "the events configuration's presets do not build (a package missing from the image?)";
    return false;
  }
  if (macro8_ == nullptr || !CheckPreset(*macro8_)) {
    *why = "the eight-target macro preset does not validate";
    return false;
  }
  murmuration_ = std::make_unique<PresetState>();
  PresetDiagnostic diag;
  if (!DecodePreset(kFactoryMurmuration, kFactoryMurmurationSize, murmuration_.get(), &diag) ||
      !CheckPreset(*murmuration_)) {
    *why = "the factory preset murmuration does not decode";
    return false;
  }
  // The fuzz streams' loads: four random presets in random structures, fixed seeds.
  for (uint32_t i = 0; i < 4u; ++i) {
    uint32_t s = 0x6C078965u * (i + 1u);
    const ParamList leaves = FuzzLeaves(&s);
    fuzzLoads_.push_back(BuildPreset(PresetSource{nullptr, leaves}, kFuzzStructures[Pick(&s, kNumFuzzStructures)]));
    if (fuzzLoads_.back() == nullptr || !CheckPreset(*fuzzLoads_.back())) {
      *why = "a fuzz load does not build";
      return false;
    }
  }
  return true;
}

const VectorCase* Bench::FindVector(const char* name) const {
  for (const VectorCase& v : corpus_) {
    if (std::strcmp(v.name, name) == 0) return &v;
  }
  return nullptr;
}

PresetSource Bench::Corpus(const char* vector, const char* preset) const {
  const VectorCase* v = FindVector(vector);
  if (v != nullptr) {
    for (const golden::PresetCase& p : v->presets) {
      if (std::strcmp(p.name, preset) == 0) return PresetSource{p.package, p.params};
    }
  }
  return PresetSource{};
}

Spec Bench::Make(const std::string& config, const char* vector, PresetSource preset, uint32_t seconds,
                 const char* structure) const {
  Spec s;
  s.config    = config;
  s.input     = vector;
  s.preset    = std::move(preset);
  s.structure = structure;
  const VectorCase* v = FindVector(vector);
  if (v != nullptr) {
    s.notes  = golden::VectorNotes(*v);
    s.frames = std::min(seconds * 48000u, v->frames);
  }
  return s;
}

Spec Bench::MakePessimistic(const std::string& config, const char* vector, float grainMs, uint32_t seconds,
                            const ParamList& changes, const char* structure) const {
  return Make(config, vector, PresetSource{nullptr, With(Pessimistic(grainMs), changes)}, seconds, structure);
}

std::unique_ptr<PresetState> Bench::Build(const Spec& s) const {
  if (s.state != nullptr) return std::make_unique<PresetState>(*s.state);
  return BuildPreset(s.preset, s.structure);
}

// ---- Session 1's suites --------------------------------------------------------------------

std::vector<Spec> Bench::Blocks(bool quick) const {
  const uint32_t    sec = quick ? 4u : 10u;
  std::vector<Spec> specs;
  specs.push_back(Make("default", "plucks_12s", PresetSource{}, sec));
  specs.push_back(Make("nominal", "strums_16s", PresetSource{nullptr, Nominal()}, sec));
  specs.push_back(MakePessimistic("pess_render", "onset_bursts_6s", 20.0f, 6));
  specs.push_back(MakePessimistic("pess_births", "onset_bursts_6s", 1.0f, 6));
  Spec events = MakePessimistic("pess_events: sweeps every block, triggers, freeze, loads", "strums_16s", 20.0f, sec);
  events.events.kind = Events::Player;
  specs.push_back(events);
  specs.push_back(Make("corpus:tail_post_fb", "strums_16s", Corpus("strums_tail_123s", "tail_post_fb"), sec));
  specs.push_back(Make("corpus:pitch_reverse_spray", "plucks_12s", Corpus("plucks_12s", "pitch_reverse_spray"), sec));
  specs.push_back(Make("corpus:max_delay_spray_rev_up24", "plucks_12s",
                       Corpus("plucks_12s", "max_delay_spray_rev_up24"), sec));
  specs.push_back(Make("corpus:dense_1ms", "onset_bursts_6s", Corpus("onset_bursts_6s", "dense_1ms"), 6));
  return specs;
}

std::vector<Spec> Bench::Stages(bool quick) const {
  const uint32_t sec = quick ? 3u : 6u;
  struct Variant {
    const char* name;
    ParamList   changes;
    bool        onsetMarks = true;  // false: the default mode (no onset grains, no marks)
  };
  const Variant variants[] = {
      {"all stages", {}},
      {"-modulation", {{P::ModDepth, 0.0f}}},
      {"-post delay", {{P::DelayMix, 0.0f}}},
      {"-reverb", {{P::ReverbMix, 0.0f}}},
      {"-filter", {{P::FilterCutoffHz, 20000.0f}}},
      {"-feedback", {{P::Feedback, 0.0f}}},
      {"-onset grains and marks", {}, false},
      {"-pitch (r = 1)", {{P::TransposeSt, 0.0f}, {P::SpreadCents, 0.0f}}},
      {"-grains (1 voice)", {{P::Overlap, 0.0f}}},
  };
  std::vector<Spec> specs;
  for (const Variant& v : variants) {
    Spec s = MakePessimistic(std::string("stages:") + v.name, "onset_bursts_6s", 20.0f, sec, v.changes);
    if (!v.onsetMarks) s.structure = nullptr;
    specs.push_back(s);
  }
  return specs;
}

std::vector<Spec> Bench::Births(bool quick) const {
  // Scheduler births at equal voice counts (target = 64 * overlap^3; overlap values whose
  // binary32 target is >= 48 and 16): 1 ms grains are born every 48/T frames and live 48,
  // 20 ms grains every 960/T frames and live 960, so the voices rendered per block match and
  // the births per block differ by (T - T/20). Per-birth DetMath paths on: pitch with cents
  // spread, reverse, spray, pan; jitter 0 for an exact birth count; post stages off. The
  // difference also carries the short grains' ring-read locality (a new spray position per
  // birth), so it bounds ScheduleGrain's cost from above rather than isolating it.
  const uint32_t sec = quick ? 4u : 10u;
  struct Birth {
    float    overlap;
    uint32_t voices;
  };
  std::vector<Spec> specs;
  for (const Birth bt : {Birth{0.90856034f, 48u}, Birth{0.62996054f, 16u}}) {
    for (const float ms : {1.0f, 20.0f}) {
      const ParamList params = {{P::Overlap, bt.overlap}, {P::GrainSizeMs, ms}, {P::Jitter, 0.0f},
                                {P::SprayMs, 50.0f},      {P::TransposeSt, 7.0f}, {P::SpreadCents, 50.0f},
                                {P::ReverseProb, 0.5f},   {P::PanSpread, 1.0f}, {P::Mix, 1.0f}};
      Spec s = Make(std::string("births:") + std::to_string(bt.voices) + " voices, " +
                        (ms == 1.0f ? "1 ms" : "20 ms"),
                    "plucks_12s", PresetSource{nullptr, params}, sec);
      s.voices      = bt.voices;
      s.grainFrames = ms == 1.0f ? 48u : 960u;
      specs.push_back(s);
    }
  }
  return specs;
}

std::vector<Spec> Bench::Tail() const {
  // The golden tail vector exactly as the corpus renders it (one Exact load, 48-frame blocks,
  // no events): its hash must equal golden.json's strums_tail_123s/tail_post_fb, at FZ = 0 and
  // at FZ = 1 (profile §6.4's forced-flush control). The silent tail starts after the vector's
  // active frames (3 s of strums).
  std::vector<Spec> specs;
  const VectorCase* tailVec = FindVector("strums_tail_123s");
  Spec              goldenTail;
  goldenTail.config = "golden:strums_tail_123s/tail_post_fb";
  goldenTail.input  = "strums_tail_123s";
  goldenTail.preset = Corpus("strums_tail_123s", "tail_post_fb");
  if (tailVec != nullptr) {
    goldenTail.notes          = golden::VectorNotes(*tailVec);
    goldenTail.frames         = tailVec->frames;
    goldenTail.tailStartFrame = tailVec->activeFrames;
  }
  specs.push_back(goldenTail);
  // 2 s of noise, then 120 s of silence through feedback and every post stage (§4.2 b).
  Spec noise;
  noise.config    = "noise-tail:pessimistic, feedback 0.95";
  noise.input     = "2 s noise + 120 s silence";
  noise.preset    = PresetSource{nullptr, With(Pessimistic(20.0f), {{P::Feedback, 0.95f}})};
  noise.structure = kOnsetMarks;
  testsignal::Note n;
  n.kind    = testsignal::Kind::Noise;
  n.start   = 0;
  n.length  = 2u * 48000u;
  n.level   = 1 << 22;  // -6 dBFS
  n.seed    = 0x7A11u;
  n.attack  = 48;
  n.release = 480;  // ends at start + length
  noise.notes.push_back(n);
  noise.frames         = 122u * 48000u;
  noise.tailStartFrame = n.start + n.length;
  specs.push_back(noise);
  return specs;
}

Spec Bench::RestartRender() const {
  return MakePessimistic("restart:pess_render, 2 s", "onset_bursts_6s", 20.0f, 2);
}

// ---- §8.2: corners, wave 1's stressors, CLOCK, fuzz ---------------------------------------

std::vector<Spec> Bench::Stress(bool quick) const {
  const uint32_t    sec = quick ? 4u : 10u;
  std::vector<Spec> specs;
  const ParamList   allPost = {{P::Feedback, 1.05f}, {P::ModDepth, 1.0f}, {P::ModRateHz, 10.0f},
                               {P::DelayMix, 0.5f},  {P::DelayFb, 0.6f},  {P::DelayTimeMs, 350.0f},
                               {P::ReverbMix, 0.5f}, {P::ReverbTime, 0.9f}, {P::FilterCutoffHz, 2000.0f},
                               {P::FilterRes, 0.5f}, {P::FilterMorph, 1.5f}};
  // Corners: 64 voices of 1.33 ms grains at jitter 0 and +24 st with every post stage (the legal
  // corner of §1, about 196 % before the speed pack), with and without onset marks, and with a
  // manual trigger due on every frame; 5 ms grains; the default at overlap 1 and three pitches;
  // the factory Murmuration at Activity max.
  const ParamList corner = {{P::Jitter, 0.0f}, {P::SpreadCents, 0.0f}};
  specs.push_back(MakePessimistic("corner:64v 1.33ms jitter0 +24st all post, marks", "onset_bursts_6s", 1.33f, 6, corner));
  specs.push_back(
      MakePessimistic("corner:64v 1.33ms jitter0 +24st all post, no marks", "onset_bursts_6s", 1.33f, 6, corner, nullptr));
  {
    Spec s = MakePessimistic("corner:same + manual trigger every frame", "onset_bursts_6s", 1.33f, 6, corner);
    s.events.kind = Events::TriggerFlood;
    specs.push_back(s);
  }
  specs.push_back(Make("corner:64v 5ms jitter0 +24st all post", "onset_bursts_6s",
                       PresetSource{nullptr, With(allPost, {{P::Overlap, 1.0f}, {P::GrainSizeMs, 5.0f},
                                                            {P::Jitter, 0.0f}, {P::TransposeSt, 24.0f}})},
                       6));
  for (const float st : {0.0f, 12.0f, 24.0f}) {
    specs.push_back(Make("corner:default overlap 1 +" + std::to_string(static_cast<int>(st)) + "st", "plucks_12s",
                         PresetSource{nullptr, {{P::Overlap, 1.0f}, {P::TransposeSt, st}}}, sec));
  }
  {
    Spec s        = Make("corner:Murmuration at Activity max", "strums_16s", PresetSource{}, sec);
    s.state       = murmuration_.get();
    s.events.kind = Events::ActivityMax;
    specs.push_back(s);
  }
  // Wave 1's stressors, in the corpus's structures, and CLOCK.
  {
    Spec s = MakePessimistic("w1:burst 16 at spacing 0, triggered every 50 ms", "strums_16s", 20.0f, sec,
                             {{P::BurstCount, 16.0f}, {P::BurstSpacingMs, 0.0f}}, "burst_spaced");
    s.events.kind = Events::Every50ms;
    specs.push_back(s);
  }
  specs.push_back(MakePessimistic("w1:repeat 16, 16-frame mark-capped passes, decay, 64 voices", "onset_bursts_6s",
                                  1.0f, 6, {{P::Repeat, 16.0f}, {P::DecayMs, 30.0f}}, "repeat_mark_aging"));
  specs.push_back(MakePessimistic("w1:8-entry pitch set with spread", "plucks_12s", 20.0f, sec,
                                  {{P::SpreadCents, 30.0f}}, "pitchset_random"));
  {
    Spec s = MakePessimistic("w1:MIDI flood of 48 per block, 2 s on, 2 s off", "strums_16s", 20.0f, sec, {},
                             "sources_midi");
    s.events.kind = Events::Midi48;
    specs.push_back(s);
  }
  {
    Spec s = MakePessimistic("w1:voice_count 32 with triggers", "strums_16s", 20.0f, sec,
                             {{P::VoiceCount, 32.0f}}, "sources_midi");
    s.events.kind = Events::Every50ms;
    specs.push_back(s);
  }
  specs.push_back(Make("clock:clock_hits", "strums_16s", PresetSource{"clock_hits", {}}, sec));
  specs.push_back(Make("clock:clock_cloud", "strums_16s", PresetSource{"clock_cloud", {}}, sec));
  {
    Spec s        = Make("macro:macro_sweep with macro moves", "strums_16s", PresetSource{"macro_sweep", {}}, sec);
    s.events.kind = Events::MacroSweep;
    specs.push_back(s);
  }
  // Fuzz: random leaves in a random structure, random events, 2,000 blocks each.
  const uint32_t sets = quick ? 10u : 50u, blocks = quick ? 500u : 2000u;
  for (uint32_t i = 0; i < sets; ++i) {
    uint32_t        seed   = 0x9E3779B9u * (i + 1u);
    const ParamList leaves = FuzzLeaves(&seed);
    const char*     shape  = kFuzzStructures[Pick(&seed, kNumFuzzStructures)];
    const char*     vector = kFuzzVectors[Pick(&seed, 3u)];
    Spec s = Make("fuzz:" + std::to_string(i), vector, PresetSource{nullptr, leaves}, 60u, shape);
    s.frames         = std::min(s.frames, blocks * kBlock);
    s.events.kind    = Events::Fuzz;
    s.events.seed    = seed;
    specs.push_back(s);
  }
  return specs;
}

// ---- Costs by difference -------------------------------------------------------------------

std::vector<Spec> Bench::Post(bool quick) const {
  // One grain voice (overlap 0), 20 ms, no feedback; every stage off but the one measured. The
  // configurations' differences from "post:none" are the stages' costs: settled (its mix at a
  // constant target), moving (the mix's target changing every block, so its smoother never
  // settles), the delay's head gliding to a new time every block, and the filter's
  // coefficients recomputed every block.
  const uint32_t  sec  = quick ? 2u : 4u;
  const ParamList base = {{P::Overlap, 0.0f}, {P::GrainSizeMs, 20.0f}, {P::Feedback, 0.0f},
                          {P::ModDepth, 0.0f}, {P::DelayMix, 0.0f},   {P::ReverbMix, 0.0f},
                          {P::FilterCutoffHz, 20000.0f}};
  struct V {
    const char* name;
    ParamList   changes;
    EventPlan   plan;
  };
  EventPlan none;
  const auto sweep = [](ParamId id, float lo, float hi, uint32_t period) {
    EventPlan e;
    e.kind   = Events::Sweep;
    e.param  = id;
    e.lo     = lo;
    e.hi     = hi;
    e.period = period;
    return e;
  };
  const V variants[] = {
      {"none", {}, none},
      {"modulation settled", {{P::ModDepth, 1.0f}, {P::ModRateHz, 10.0f}}, none},
      {"modulation moving", {{P::ModDepth, 1.0f}, {P::ModRateHz, 10.0f}}, sweep(P::ModDepth, 0.3f, 1.0f, 64)},
      {"delay settled", {{P::DelayMix, 0.5f}, {P::DelayFb, 0.6f}, {P::DelayTimeMs, 350.0f}}, none},
      {"delay moving", {{P::DelayMix, 0.5f}, {P::DelayFb, 0.6f}, {P::DelayTimeMs, 350.0f}},
       sweep(P::DelayMix, 0.2f, 0.8f, 64)},
      {"delay head gliding", {{P::DelayMix, 0.5f}, {P::DelayFb, 0.6f}, {P::DelayTimeMs, 350.0f}},
       sweep(P::DelayTimeMs, 100.0f, 400.0f, 400)},
      {"reverb settled", {{P::ReverbMix, 0.5f}, {P::ReverbTime, 0.9f}}, none},
      {"reverb moving", {{P::ReverbMix, 0.5f}, {P::ReverbTime, 0.9f}}, sweep(P::ReverbMix, 0.2f, 0.8f, 64)},
      {"filter settled", {{P::FilterCutoffHz, 2000.0f}, {P::FilterRes, 0.5f}, {P::FilterMorph, 1.5f}}, none},
      {"filter moving", {{P::FilterCutoffHz, 2000.0f}, {P::FilterRes, 0.5f}, {P::FilterMorph, 1.5f}},
       sweep(P::FilterCutoffHz, 500.0f, 8000.0f, 200)},
  };
  std::vector<Spec> specs;
  for (const V& v : variants) {
    Spec s   = Make(std::string("post:") + v.name, "plucks_12s", PresetSource{nullptr, With(base, v.changes)}, sec);
    s.events = v.plan;
    specs.push_back(s);
  }
  return specs;
}

std::vector<Spec> Bench::EventCosts(bool quick) const {
  // The nominal preset (64 voices, every post stage moderate) with one event at block 16k + 8:
  // the event block's cycles against the block before it are the event's cost, its rebuilds and
  // what it starts in that block included.
  const uint32_t sec = quick ? 4u : 10u;
  struct V {
    const char*       name;
    Engine::EventType type;
    uint32_t          id;
    float             lo, value;
    bool              macro8;
  };
  using T = Engine::EventType;
  const V variants[] = {
      {"SetParam layer0.size_ms", T::SetParam, static_cast<uint32_t>(P::GrainSizeMs), 20.0f, 25.0f, false},
      {"SetParam post.filter.cutoff_hz", T::SetParam, static_cast<uint32_t>(P::FilterCutoffHz), 5000.0f, 3000.0f,
       false},
      {"MacroMove, eight curved targets", T::MacroMove, static_cast<uint32_t>(P::MacroActivity), 0.2f, 0.8f, true},
      {"SpilloverLoad FastCut", T::SpilloverLoad, static_cast<uint32_t>(SwitchStyle::FastCut), 0.f, 0.f, false},
      {"SpilloverLoad Trails", T::SpilloverLoad, static_cast<uint32_t>(SwitchStyle::Trails), 0.f, 0.f, false},
      {"Trigger", T::Trigger, static_cast<uint32_t>(Engine::TriggerSource::Footswitch), 0.f, 0.f, false},
  };
  std::vector<Spec> specs;
  for (const V& v : variants) {
    Spec s = Make(std::string("event:") + v.name, "strums_16s", PresetSource{nullptr, Nominal()}, sec);
    if (v.macro8) s.state = macro8_.get();
    s.events.kind  = Events::EveryN;
    s.events.type  = v.type;
    s.events.id    = v.id;
    s.events.lo    = v.lo;
    s.events.value = v.value;
    specs.push_back(s);
  }
  return specs;
}

std::vector<Spec> Bench::LedgerSet(bool quick) const {
  std::vector<Spec> specs = Blocks(quick);
  for (Spec& s : Stress(quick)) {
    if (s.events.kind != Events::Fuzz) specs.push_back(s);
  }
  return specs;
}

// ---- Rendering -----------------------------------------------------------------------------

void Bench::Push(uint32_t b, const EventPlan& plan, uint32_t* seq, Rendered* r) {
  const int64_t f0   = static_cast<int64_t>(b) * kBlock;
  const auto    push = [&](int64_t frame, Engine::EventType type, uint32_t id, float value,
                        const PresetState* preset) {
    Engine::Event e;
    e.frame  = frame;
    e.seq    = (*seq)++;
    e.type   = type;
    e.id     = id;
    e.value  = value;
    e.preset = preset;
    ++r->eventsPushed;
    if (!queue_.Push(e)) ++r->eventsRefused;
  };
  const auto set = [&](int64_t frame, ParamId id, float v) {
    push(frame, Engine::EventType::SetParam, static_cast<uint32_t>(id), Canonicalize(id, v), nullptr);
  };
  const auto trigger = [&](int64_t frame, Engine::TriggerSource src) {
    push(frame, Engine::EventType::Trigger, static_cast<uint32_t>(src), 1.f, nullptr);
  };
  switch (plan.kind) {
    case Events::None: break;
    case Events::Player:
      if (b % 1000u == 500u) {
        const bool nominal = (b / 1000u) % 2u == 0u;
        push(f0, Engine::EventType::SpilloverLoad,
             static_cast<uint32_t>(nominal ? SwitchStyle::FastCut : SwitchStyle::Trails), 0.f,
             nominal ? loadB_.get() : loadA_.get());
      }
      if (b % 1500u == 750u) push(f0, Engine::EventType::Freeze, 0, (b / 1500u) % 2u == 0u ? 1.f : 0.f, nullptr);
      set(f0, P::FilterCutoffHz, 500.f + 7500.f * Tri(b, 2000u));
      set(f0, P::TransposeSt, -12.f + 36.f * Tri(b, 3000u));
      set(f0, P::GrainSizeMs, 5.f + 195.f * Tri(b, 4000u));
      if (b % 250u == 0u) {
        for (uint32_t k = 0; k < 4u; ++k) trigger(f0 + 12 * static_cast<int64_t>(k), Engine::TriggerSource::Footswitch);
      }
      break;
    case Events::TriggerFlood:
      for (uint32_t k = 0; k < kBlock; ++k) trigger(f0, Engine::TriggerSource::Footswitch);
      break;
    case Events::Every50ms:
      if ((b * kBlock) % 2400u < kBlock) trigger(f0, Engine::TriggerSource::Footswitch);
      break;
    case Events::Midi48:
      if ((b / 2000u) % 2u == 0u) {
        for (uint32_t k = 0; k < kBlock; ++k) trigger(f0, Engine::TriggerSource::MidiNote);
      }
      break;
    case Events::MacroSweep:
      if (b % 300u == 7u) trigger(f0, Engine::TriggerSource::Footswitch);
      for (uint32_t m = 0; m < 8u; ++m) {
        const ParamId id = static_cast<ParamId>(static_cast<uint32_t>(P::MacroActivity) + m);
        push(f0, Engine::EventType::MacroMove, static_cast<uint32_t>(id),
             Canonicalize(id, Tri(b + 137u * m, 900u + 50u * m)), nullptr);
      }
      break;
    case Events::ActivityMax:
      if (b == 0u) push(0, Engine::EventType::MacroMove, static_cast<uint32_t>(P::MacroActivity), 1.0f, nullptr);
      break;
    case Events::Sweep:
      set(f0, plan.param, plan.lo + (plan.hi - plan.lo) * Tri(b, plan.period));
      break;
    case Events::EveryN: {
      if (b % 16u != 8u) break;
      const bool second = (b / 16u) % 2u == 1u;
      switch (plan.type) {
        case Engine::EventType::SetParam:
          set(f0, static_cast<ParamId>(plan.id), second ? plan.value : plan.lo);
          break;
        case Engine::EventType::MacroMove:
          push(f0, plan.type, plan.id, second ? plan.value : plan.lo, nullptr);
          break;
        case Engine::EventType::SpilloverLoad:
          push(f0, plan.type, plan.id, 0.f, second ? loadA_.get() : loadB_.get());
          break;
        default: push(f0, plan.type, plan.id, 1.f, nullptr); break;
      }
      break;
    }
    case Events::Fuzz: {
      uint32_t* s = &fuzzState_;
      if (Pick(s, 100u) < 20u) {
        const ParamId          id = LeafId(Pick(s, static_cast<uint32_t>(kNumLeafParams)));
        const ParamDescriptor* d  = FindParam(id);
        set(f0, id, d->min + (d->max - d->min) * Unit(s));
      }
      if (Pick(s, 100u) < 5u) {
        const ParamId id = static_cast<ParamId>(static_cast<uint32_t>(P::MacroActivity) + Pick(s, 8u));
        push(f0, Engine::EventType::MacroMove, static_cast<uint32_t>(id), Unit(s), nullptr);
      }
      if (Pick(s, 100u) < 3u) trigger(f0, Engine::TriggerSource::Footswitch);
      if (Pick(s, 100u) < 2u) {
        const uint32_t n = 1u + Pick(s, 16u);
        for (uint32_t k = 0; k < n; ++k) trigger(f0, Engine::TriggerSource::MidiNote);
      }
      if (Pick(s, 1000u) < 5u) {
        const float on = Pick(s, 2u) != 0u ? 1.f : 0.f;
        push(f0, Engine::EventType::Freeze, 0, on, nullptr);
      }
      if (Pick(s, 1000u) < 2u) {
        const PresetState* load = fuzzLoads_[Pick(s, static_cast<uint32_t>(fuzzLoads_.size()))].get();
        push(f0, Engine::EventType::SpilloverLoad,
             static_cast<uint32_t>(Pick(s, 2u) != 0u ? SwitchStyle::FastCut : SwitchStyle::Trails), 0.f, load);
      }
      break;
    }
  }
}

Bench::Rendered Bench::Render(const Spec& s, BlockFn fn, void* user) {
  Rendered r;
  {
    const std::unique_ptr<PresetState> preset = Build(s);
    r.loaded = preset != nullptr && engine_->LoadPreset(*preset, LoadMode::Exact);
  }
  queue_.Clear();  // the Exact load started a new engine timeline at frame 0
  fuzzState_ = s.events.seed;
  auto gen   = std::make_unique<testsignal::Generator>();
  gen->Start(s.notes.data(), static_cast<uint32_t>(s.notes.size()));
  golden::Sha256 sha;
  int32_t        qL[kBlock], qR[kBlock];
  float          inL[kBlock], inR[kBlock], outL[kBlock], outR[kBlock];
  uint8_t        bytes[kBlock * 8];
  const float*   ins[2]  = {inL, inR};
  float*         outs[2] = {outL, outR};
  uint32_t       seq     = 0;
  r.blocks               = std::min(s.frames / kBlock, kMaxBlocks);
  for (uint32_t b = 0; b < r.blocks; ++b) {
    gen->RenderQ23(qL, qR, kBlock);
    for (uint32_t i = 0; i < kBlock; ++i) {
      inL[i] = testsignal::Q23ToFloat(qL[i]);
      inR[i] = testsignal::Q23ToFloat(qR[i]);
    }
    Engine::ProcessContext ctx;
    ctx.in        = ins;
    ctx.out       = outs;
    ctx.numFrames = kBlock;
    if (s.events.kind != Events::None) {
      Push(b, s.events, &seq, &r);
      ctx.events    = blockEvents_;
      ctx.numEvents = queue_.PopBlock(static_cast<int64_t>(b) * kBlock, kBlock, blockEvents_, EventQueue::kCapacity);
    }
    fn(user, b, ctx);
    PackFrames(outL, outR, kBlock, bytes);
    sha.Update(bytes, sizeof bytes);
  }
  queue_.Clear();
  r.hash = sha.Hex();
  return r;
}

}  // namespace brainscape::fw::bench
