#include <algorithm>
#include <cstring>
#include <utility>

#include "Document.h"
#include "Migrate.h"
#include "Number.h"
#include "Text.h"

// Schema 1 of the preset document (docs/design/mode-compiler.md §2), as one table in code:
// Visit() and VisitLayer() name every key in canonical order (§6.4) with its type, range,
// default and the feature or wave it needs, and two visitors walk them, the Reader (JSON to
// Document, validating, §2.7) and the Writer (Document to canonical JSON). What the build plays
// comes from dsp/: a leaf is playable when its row is a Leaf row (Params.h), structure when its
// feature bit is in kSupportedModeFeatures (Mode.h), so a wave's pull request widens the schema
// without touching this table.
//
// Integer-only (§1.4 principle 3): numbers are read by the exact binary32 reader (Number.h) and
// compared as the integers their bit patterns order like.
namespace bsc {

using namespace brainscape;

namespace {

// ── Bit patterns of the schema's float limits (compiler_unit checks each) ──────────────
constexpr uint32_t kB0       = 0x00000000u;  // 0
constexpr uint32_t kBHalf    = 0x3F000000u;  // 0.5
constexpr uint32_t kB1       = 0x3F800000u;  // 1
constexpr uint32_t kBMinus1  = 0xBF800000u;  // -1
constexpr uint32_t kB16th    = 0x3D800000u;  // 1/16
constexpr uint32_t kB16      = 0x41800000u;  // 16
constexpr uint32_t kB24      = 0x41C00000u;  // 24
constexpr uint32_t kBMinus24 = 0xC1C00000u;  // -24
constexpr uint32_t kB10      = 0x41200000u;  // 10
constexpr uint32_t kB20000   = 0x469C4000u;  // 20000
constexpr uint32_t kB1000    = 0x447A0000u;  // 1000
constexpr uint32_t kB5000    = 0x459C4000u;  // 5000
constexpr uint32_t kBTenth   = 0x3DCCCCCDu;  // 0.1
constexpr uint32_t kB500     = 0x43FA0000u;  // 500
constexpr uint32_t kB5       = 0x40A00000u;  // 5
constexpr uint32_t kB80      = 0x42A00000u;  // 80

// Not mode features: the stored performance state (§2.6), whose fields arrive in two parts
// (docs/design/clock.md §11.1): the time mode, the subdivision and the tempo with the tempo
// core, which brings CLOCK, so a build that plays `clock` plays them; and global reverse later
// in W2 (ReadOptions::globalReverse), so `reverse: true` stays E6 until then.
constexpr uint32_t kNeedPerformanceTempo   = 1u << 31;
constexpr uint32_t kNeedPerformanceReverse = 1u << 30;

constexpr uint32_t kFirstMacro = static_cast<uint32_t>(ParamId::MacroActivity);

// ── Field specifications ─────────────────────────────────────────────────────────────────
struct EnumSpec {
  const char* const* names;
  uint8_t            count;
  uint8_t            def;
  const uint32_t*    features;  // per value; or null, and `feature` for any value but `def`
  uint32_t           feature;
  // E5's hint for a value that is not a name, when it says something the list does not; or null.
  const char* (*hint)(std::string_view) = nullptr;
};

struct FloatSpec {
  uint32_t    lo, hi;
  bool        loOpen;  // (lo, hi]
  uint32_t    def;
  bool        required;
  uint32_t    feature;  // a value other than def needs it
  const char* code;     // the range error's code
};

struct IntSpec {
  int64_t     lo, hi, def;
  bool        required;
  uint32_t    feature;
  const char* code;
};

struct RecordSpec {
  uint32_t    min, max;
  bool        core;
  const char* what;  // "pitch entries", for E7
};

const char* const kFamilyNames[]    = {"none", "recall", "reverie", "misfire", "echoic"};
const char* const kSourceNames[]    = {"periodic", "clock", "onset", "footswitch", "midi_note"};
// The Subdiv control's six positions as rate multipliers, in code order (clock.md §5.1, D7):
// code 0, TAP, is the default; never note values, so the Microcosm's printed labels are E5.
const char* const kSubdivNames[]    = {"tap", "x1/4", "x1/2", "x2", "x4", "x8"};
// The note values of synced fields, by code (clock.md §5.2): 0 off, then sixteen by duration,
// `t` a triplet and `d` dotted.
const char* const kDivisionNames[]  = {"off",  "1/32", "1/16t", "1/16", "1/8t", "1/16d",
                                       "1/8",  "1/4t", "1/8d",  "1/4",  "1/2t", "1/4d",
                                       "1/2",  "1/1t", "1/2d",  "1/1",  "2/1"};
static_assert(sizeof(kDivisionNames) / sizeof(kDivisionNames[0]) == kMaxSyncDivision + 1u,
              "a name per division code");
const char* const kStepOrderNames[] = {"fixed", "shuffle", "random"};
const char* const kPositionNames[]  = {"live", "mark", "pin", "grid"};
const char* const kSprayLawNames[]  = {"uniform", "exp"};
const char* const kMarkWalkNames[]  = {"none", "cascade", "random"};
const char* const kRearmNames[]     = {"off", "time", "onset", "manual"};
const char* const kSelectNames[]    = {"cycle", "random"};
const char* const kQuantizeNames[]  = {"off", "scale"};
const char* const kBandNames[]      = {"lp", "bp", "hp", "notch"};
const char* const kCutoffSrcNames[] = {"fixed", "random", "lfo", "env"};
const char* const kShapeNames[]     = {"sine", "triangle", "saw", "square", "random"};
const char* const kRouteFromNames[] = {"modulator0", "modulator1"};
const char* const kRouteToNames[]   = {"size", "cutoff", "ratio", "position", "layer_mix", "pan"};
const char* const kDrawNames[]      = {"grain.pitch", "grain.pan",     "grain.position",
                                       "grain.size",  "grain.reverse", "grain.gain"};
const char* const kTimeModeNames[]  = {"free", "subdiv", "tempo"};
const char* const kStageNames[]     = {"mod", "delay", "reverb", "filter"};

const uint32_t kPositionFeatures[] = {0, kModeFeatureMarkPosition, kModeFeaturePinPosition,
                                      kModeFeatureGridPosition};
const uint32_t kSprayLawFeatures[] = {0, kModeFeatureExpSpray};

const EnumSpec kFamilySpec{kFamilyNames, 5, 0, nullptr, 0};
const EnumSpec kStepOrderSpec{kStepOrderNames, kStepOrderCount, 0, nullptr, kModeFeatureSteps};
const EnumSpec kPositionSpec{kPositionNames, kPositionSourceCount, 0, kPositionFeatures, 0};
const EnumSpec kSprayLawSpec{kSprayLawNames, kSprayLawCount, 0, kSprayLawFeatures, 0};
const EnumSpec kMarkWalkSpec{kMarkWalkNames, kMarkWalkCount, 0, nullptr, kModeFeatureMarkWalk};
const EnumSpec kRearmSpec{kRearmNames, kPinRearmCount, 0, nullptr, kModeFeaturePinPosition};
const EnumSpec kSelectSpec{kSelectNames, kPitchSelectCount, 0, nullptr, kModeFeaturePitchSet};
const EnumSpec kQuantizeSpec{kQuantizeNames, kQuantizeModeCount, 0, nullptr, kModeFeatureQuantize};
const EnumSpec kBandSpec{kBandNames, kSvfBandCount, 0, nullptr, 0};
const EnumSpec kCutoffSrcSpec{kCutoffSrcNames, kCutoffSourceCount, 0, nullptr, 0};
const EnumSpec kShapeSpec{kShapeNames, kModulatorShapeCount, 0, nullptr, 0};
const EnumSpec kTimeModeSpec{kTimeModeNames, kTimeModeCount, 0, nullptr, kNeedPerformanceTempo};
// The Microcosm's printed labels name the same positions, but "1/4" and "1/2" read as the note
// values row 63 takes beside them, so no spelling of them is read (clock.md §5.1, record P6).
const char* SubdivHint(std::string_view text) {
  for (const char* printed : {"1/4", "1/2", "TAP", "2x", "4x", "8x"}) {
    if (text == printed) {
      return "Subdiv's positions are rates, written x1/4, x1/2, tap, x2, x4 and x8; the "
             "Microcosm's printed labels are not read, as \"1/4\" and \"1/2\" would read as "
             "note values";
    }
  }
  return nullptr;
}
const EnumSpec kPerfSubdivSpec{kSubdivNames, kSubdivisionCount, 0, nullptr, kNeedPerformanceTempo,
                               SubdivHint};

const FloatSpec kSlotShareSpec{kB0, kB1, true, kB1, false, kModeFeatureTwoLayers, "E4"};
const FloatSpec kMarkJitterSpec{kB0, kB1, false, kB0, false, kModeFeatureMarkWalk, "E4"};
const FloatSpec kRearmMsSpec{kB10, kB20000, false, kB1000, false, kModeFeaturePinPosition, "E4"};
const FloatSpec kGlideSpec{kBMinus24, kB24, false, kB0, false, kModeFeatureGlide, "E4"};
const FloatSpec kEntryStSpec{kBMinus24, kB24, false, kB0, true, 0, "E4"};
const FloatSpec kPosSelSpec{kB0, kB5000, false, kB0, false, 0, "E4"};
const FloatSpec kUnitDefOneSpec{kB0, kB1, false, kB1, false, 0, "E4"};
const FloatSpec kAttackSpec{kB0, kB5000, false, kB0, false, 0, "E4"};
const FloatSpec kReleaseSpec{kB0, kB20000, false, kB0, false, 0, "E4"};
const FloatSpec kAmountSpec{kBMinus1, kB1, false, kB0, true, 0, "E4"};
const FloatSpec kDuckAttackSpec{kBTenth, kB500, false, kB5, false, kModeFeatureDryDuck, "E4"};
const FloatSpec kDuckReleaseSpec{kB1, kB5000, false, kB80, false, kModeFeatureDryDuck, "E4"};

const IntSpec kSlotSpec{0, 15, 0, false, 0, "E4"};
const IntSpec kRatioIdxSpec{0, 7, 0, false, 0, "E4"};
const IntSpec kFlagsSpec{0, 1, 0, false, 0, "E4"};
const IntSpec kWeightSpec{1, 16, 1, false, 0, "E4"};
const IntSpec kMarkIndexSpec{0, kMaxMarkIndex, 0, false, kModeFeatureMarkWalk, "E8"};
const IntSpec kRootSpec{0, 11, 0, false, kModeFeatureQuantize, "E4"};
const IntSpec kRouteLayerSpec{0, kMaxModeLayers - 1, 0, false, 0, "E4"};
const IntSpec kTempoSpec{kMinUsPerQuarter, kMaxUsPerQuarter, 500000, false, kNeedPerformanceTempo,
                        "E4"};

const RecordSpec kPitchRecords{1, kMaxPitchEntries, true, "pitch entries"};
const RecordSpec kStepRecords{0, kMaxSteps, false, "steps"};
const RecordSpec kModulatorRecords{0, kMaxModulators, false, "modulators"};
const RecordSpec kRouteRecords{0, kMaxRoutes, false, "routes"};
const RecordSpec kLinkRecords{0, kMaxLinks, false, "links"};

// Hints for keys schema 1 does not have (§2.3's removals, the renames of §4.2).
const char* RemovedHint(std::string_view key) {
  struct Hint {
    const char* key;
    const char* hint;
  };
  static const Hint kHints[] = {
      {"bypass", "removed in schema 1: each post stage is exactly transparent at its neutral leaf"},
      {"hp_hz", "removed in schema 1: the feedback taming chain is engine-owned"},
      {"lp_hz", "removed in schema 1: the feedback taming chain is engine-owned"},
      {"diffusion", "removed in schema 1: the feedback taming chain is engine-owned"},
      {"size_law", "removed in schema 1"},
      {"enabled", "removed in schema 1: dry_duck.depth 0 is off"},
      {"out_trim_db", "renamed wet_trim_db"},
      {"st", "the pitch leaf is transpose_st; pitch.set holds the entries"},
      {"category", "the preset's family is `family`"},
      {"ratio_gen", "editor-only data lives under editor.ratio_gen"},
      {"onset_trigger", "schema 1 lists onset in scheduler.sources"},
      {"time", "the post delay's time is time_ms (milliseconds)"},
      {"subdiv", "withdrawn from the scheduler: the subdivision is performance.subdiv"},
      {"tempo_source", "withdrawn: the tempo source is a device setting, not part of a preset"},
  };
  for (const Hint& h : kHints) {
    if (key == h.key) return h.hint;
  }
  return nullptr;
}

std::string Quoted(std::string_view s) { return "`" + std::string(s) + "`"; }

std::string Join(const char* const* names, size_t count, uint32_t mask = 0xFFFFFFFFu) {
  std::string out;
  for (size_t i = 0; i < count; ++i) {
    if ((mask & (1u << i)) == 0u) continue;
    if (!out.empty()) out += ", ";
    out += names[i];
  }
  return out;
}

const char* TypeName(json::Type t) {
  switch (t) {
    case json::Type::Null:
      return "null";
    case json::Type::Bool:
      return "a boolean";
    case json::Type::Number:
      return "a number";
    case json::Type::String:
      return "a string";
    case json::Type::Array:
      return "an array";
    case json::Type::Object:
      return "an object";
  }
  return "?";
}

// What each feature is called in E6 and the wave that brings it (§1.2, §7.6).
struct FeatureInfo {
  uint32_t    bit;
  const char* name;
  const char* wave;
};
const FeatureInfo kFeatureInfo[] = {
    {kModeFeatureOnset, "the onset source", "sound revision 2 (3a)"},
    {kModeFeatureMarkPosition, "mark positioning", "sound revision 2 (3a)"},
    {kModeFeatureSources, "source selection", "W1"},
    {kModeFeaturePitchSet, "pitch sets", "W1"},
    {kModeFeatureClock, "CLOCK", "W2"},
    {kModeFeatureSteps, "step tables", "W2"},
    {kModeFeatureMarkWalk, "mark index and walk", "W2"},
    {kModeFeatureTempoSync, "tempo-synced times", "W2"},
    {kModeFeatureTwoLayers, "a second layer", "W3"},
    {kModeFeaturePinPosition, "pin positioning", "W3"},
    {kModeFeatureGridPosition, "grid positioning", "W3"},
    {kModeFeatureExpSpray, "the exp spray law", "W3"},
    {kModeFeatureQuantize, "scale quantization", "W3"},
    {kModeFeatureGlide, "glide", "W3"},
    {kModeFeatureSvf, "the per-voice SVF", "W3"},
    {kModeFeatureCrush, "the per-voice bit-crush", "W3"},
    {kModeFeatureModulators, "modulators", "W3"},
    {kModeFeatureRoutes, "modulation routes", "W3"},
    {kModeFeatureLinks, "links", "W3"},
    {kModeFeatureDryDuck, "dry-duck envelope times", "W3"},
    {kNeedPerformanceTempo, "the stored tempo, time mode and subdivision", "W2 (the tempo core)"},
    {kNeedPerformanceReverse, "global reverse", "W2 (global reverse)"},
};

const FeatureInfo* InfoFor(uint32_t bit) {
  for (const FeatureInfo& f : kFeatureInfo) {
    if (f.bit == bit) return &f;
  }
  return nullptr;
}

bool SamePitchSet(const PitchSet& a, const PitchSet& b) {
  if (a.count != b.count) return false;
  for (uint32_t i = 0; i < kMaxPitchEntries; ++i) {
    if (BitsOf(a.entries[i].st) != BitsOf(b.entries[i].st) ||
        a.entries[i].weight != b.entries[i].weight) {
      return false;
    }
  }
  return true;
}

void SetBits(float* f, uint32_t bits) { std::memcpy(f, &bits, sizeof bits); }

// Strict text for META (§5.3): no control character (C0, DEL, C1). JSON already made it UTF-8.
bool PrintableText(std::string_view s) {
  for (size_t i = 0; i < s.size(); ++i) {
    const auto c = static_cast<uint8_t>(s[i]);
    if (c < 0x20u || c == 0x7Fu) return false;
    if (c == 0xC2u && i + 1 < s.size()) {
      const auto n = static_cast<uint8_t>(s[i + 1]);
      if (n >= 0x80u && n <= 0x9Fu) return false;  // U+0080-U+009F
    }
  }
  return true;
}

bool ValidIdText(std::string_view s) {
  for (const char c : s) {
    const bool ok =
        (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
}

struct TextSpec {
  uint32_t minBytes, maxBytes;
  bool     id;  // [a-z0-9._-]
  bool     required;
};
const TextSpec kIdSpec{1, kMaxIdBytes, true, true};
const TextSpec kNameSpec{1, kMaxNameBytes, false, true};
const TextSpec kAuthorSpec{0, kMaxAuthorBytes, false, false};
const TextSpec kDescriptionSpec{0, kMaxDescriptionBytes, false, false};
const TextSpec kDisplaySpec{1, kMaxDisplayNameBytes, false, false};

const ParamDescriptor& Row(ParamId id) { return *FindParam(id); }

// The unit of a row for messages (" ms"), or "".
std::string UnitSuffix(const ParamDescriptor& d) {
  return d.unit != nullptr && d.unit[0] != '\0' ? std::string(" ") + d.unit : std::string();
}

// ═════════════════════════════════════════════════════════════════════════════════════════
// The schema (one table): keys in canonical order (§6.4) with their specifications.
// ═════════════════════════════════════════════════════════════════════════════════════════

template <class V>
void VisitLayer(V& v, Document& d, uint32_t n) {
  ModeBlob&  m  = d.state->mode;
  ModeLayer& L  = m.layers[n];
  PitchSet&  P  = m.pitch[n];
  const bool l1 = n == 1u;
  const auto id = [&](ParamId first, ParamId second) { return l1 ? second : first; };
  v.Float("slot_share", L.slotShare, kSlotShareSpec);
  v.Leaf("voice_count", id(ParamId::VoiceCount, ParamId::L1VoiceCount));
  v.Leaf("level_db", id(ParamId::LevelDb, ParamId::L1LevelDb));
  v.Object("position", true, [&] {
    v.Enum("source", L.source, kPositionSpec, true, "source:" + Dec(n));
    v.Leaf("base_ms", id(ParamId::DelayMs, ParamId::L1DelayMs));
    v.Leaf("spray_ms", id(ParamId::SprayMs, ParamId::L1SprayMs));
    v.Division("base_sync", L.baseSync, kModeFeatureTempoSync, "base_sync:" + Dec(n));
    v.Enum("spray_law", L.sprayLaw, kSprayLawSpec);
    v.Leaf("repeat", id(ParamId::Repeat, ParamId::L1Repeat));
    v.Object("mark", false, [&] {
      v.Int("index", L.markIndex, kMarkIndexSpec);
      v.Enum("walk", L.markWalk, kMarkWalkSpec);
      v.Float("jitter", L.markJitter, kMarkJitterSpec);
    });
    v.Object("pin", false, [&] {
      v.Enum("rearm", L.pinRearm, kRearmSpec);
      v.Float("rearm_ms", L.pinRearmMs, kRearmMsSpec);
    });
  });
  v.Leaf("size_ms", id(ParamId::GrainSizeMs, ParamId::L1GrainSizeMs));
  v.Leaf("decay_ms", id(ParamId::DecayMs, ParamId::L1DecayMs));
  v.Object("window", false, [&] {
    v.Leaf("sustain", id(ParamId::WindowSustain, ParamId::L1WindowSustain));
    v.Leaf("skew", id(ParamId::WindowSkew, ParamId::L1WindowSkew));
    v.Leaf("smoothness", id(ParamId::WindowSmooth, ParamId::L1WindowSmooth));
  });
  v.Object("pitch", true, [&] {
    v.Records("set", P.count, kPitchRecords, [&](uint32_t i) {
      PitchEntry& e = P.entries[i];
      if constexpr (V::kReading) e = PitchEntry{0.0f, 1, 0};
      v.Float("st", e.st, kEntryStSpec);
      v.Int("weight", e.weight, kWeightSpec);
    });
    v.NeedIf(!SamePitchSet(P, kDefaultPitchSet), kModeFeaturePitchSet, "set",
             "a pitch set other than [{\"st\": 0, \"weight\": 1}]");
    v.Enum("select", L.pitchSelect, kSelectSpec, true);
    v.Leaf("transpose_st", id(ParamId::TransposeSt, ParamId::L1TransposeSt));
    v.Leaf("spread_cents", id(ParamId::SpreadCents, ParamId::L1SpreadCents));
    v.Leaf("reverse_prob", id(ParamId::ReverseProb, ParamId::L1ReverseProb));
    v.Object("quantize", false, [&] {
      v.Enum("mode", L.quantize, kQuantizeSpec);
      v.Int("root", L.quantizeRoot, kRootSpec);
      v.Scale("scale", L.scaleMask);
    });
    v.Object("glide", false, [&] {
      v.Float("st_start", L.glideStStart, kGlideSpec);
      v.Float("st_end", L.glideStEnd, kGlideSpec);
      v.Leaf("curve", id(ParamId::GlideCurve, ParamId::L1GlideCurve));
    });
  });
  v.Leaf("pan_spread", id(ParamId::PanSpread, ParamId::L1PanSpread));
  v.Modifiers(L);
  v.Object("svf", false, [&] {
    v.Leaf("cutoff_hz", id(ParamId::SvfCutoffHz, ParamId::L1SvfCutoffHz));
    v.Leaf("res", id(ParamId::SvfRes, ParamId::L1SvfRes));
  });
  v.Object("crush", false, [&] {
    v.Leaf("bits", id(ParamId::CrushBits, ParamId::L1CrushBits));
    v.Leaf("downsample", id(ParamId::CrushDownsample, ParamId::L1CrushDownsample));
  });
}

template <class V>
void Visit(V& v, Document& d) {
  ModeBlob& m = d.state->mode;
  v.Identity(d);  // schema_version, id, name, family, sound_rev, sound_hash, meta
  v.Object("global", false, [&] { v.Leaf("mix", ParamId::Mix); });
  v.Object("scheduler", false, [&] {
    v.Sources(m.schedule.sources);
    v.Leaf("overlap", ParamId::Overlap);
    v.Leaf("jitter", ParamId::Jitter);
    v.Leaf("intermittency", ParamId::Intermittency);
    v.Object("burst", false, [&] {
      v.Leaf("count", ParamId::BurstCount);
      v.Leaf("spacing_ms", ParamId::BurstSpacingMs);
    });
    v.Object("steps", false, [&] {
      v.Leaf("count", ParamId::StepCount);
      v.Enum("order", m.schedule.stepOrder, kStepOrderSpec);
      v.Records("entries", m.steps.countMax, kStepRecords, [&](uint32_t i) {
        StepEntry& e = m.steps.entries[i];
        if constexpr (V::kReading) e = StepEntry{0, 0, 0, 0, 0.0f, 1.0f, 1.0f};
        v.Int("slot", e.slot, kSlotSpec);
        v.Float("pos_sel", e.posSel, kPosSelSpec);
        v.Int("ratio_idx", e.ratioIdx, kRatioIdxSpec);
        v.Float("gain", e.gain, kUnitDefOneSpec);
        v.Float("prob", e.prob, kUnitDefOneSpec);
        v.Int("flags", e.flags, kFlagsSpec);
      });
      v.NeedIf(m.steps.countMax > 0u, kModeFeatureSteps, "entries", "a step table");
    });
  });
  v.Layers(m, [&](uint32_t n) { VisitLayer(v, d, n); });
  v.Leaf("layer_mix", ParamId::LayerMix);
  v.Object("feedback", false, [&] { v.Leaf("amount", ParamId::Feedback); });
  v.Object("post", false, [&] {
    v.Object("mod", false, [&] {
      v.Leaf("rate_hz", ParamId::ModRateHz);
      v.Leaf("depth", ParamId::ModDepth);
    });
    v.Object("delay", false, [&] {
      v.Leaf("time_ms", ParamId::DelayTimeMs);
      v.Leaf("fb", ParamId::DelayFb);
      v.Leaf("mix", ParamId::DelayMix);
      v.Leaf("sync", ParamId::DelaySync);
    });
    v.Object("reverb", false, [&] {
      v.Leaf("time", ParamId::ReverbTime);
      v.Leaf("mix", ParamId::ReverbMix);
      v.Leaf("mode", ParamId::ReverbMode);
    });
    v.Object("filter", false, [&] {
      v.Leaf("cutoff_hz", ParamId::FilterCutoffHz);
      v.Leaf("res", ParamId::FilterRes);
      v.Leaf("morph", ParamId::FilterMorph);
    });
    v.PostOrder("order");
  });
  v.Object("dry_duck", false, [&] {
    v.Leaf("depth", ParamId::DryDuckDepth);
    v.Float("attack_ms", m.dryDuck.attackMs, kDuckAttackSpec);
    v.Float("release_ms", m.dryDuck.releaseMs, kDuckReleaseSpec);
  });
  v.Leaf("wet_trim_db", ParamId::WetTrimDb);
  v.Object("trigger", false, [&] { v.Leaf("sensitivity", ParamId::TriggerSens); });
  uint8_t modulators = 0;
  while (modulators < kMaxModulators && m.modulators[modulators].type != ModulatorType::None) {
    ++modulators;
  }
  v.Records("modulators", modulators, kModulatorRecords, [&](uint32_t i) {
    Modulator& e = m.modulators[i];
    if constexpr (V::kReading) e = Modulator{ModulatorType::Lfo, 0, 0, 0, 0.0f, 0.0f};
    v.ModType("type", e.type);
    v.Enum("shape", e.shape, kShapeSpec);
    v.Division("sync", e.sync, kModeFeatureModulators);
    v.Float("attack_ms", e.attackMs, kAttackSpec);
    v.Float("release_ms", e.releaseMs, kReleaseSpec);
    v.Leaf("rate_hz", i == 0 ? ParamId::Modulator0RateHz : ParamId::Modulator1RateHz);
    v.Leaf("depth", i == 0 ? ParamId::Modulator0Depth : ParamId::Modulator1Depth);
  });
  v.NeedIf(modulators > 0u, kModeFeatureModulators, "modulators", "modulators");
  v.Records("routes", m.routes.count, kRouteRecords, [&](uint32_t i) {
    Route& e = m.routes.entries[i];
    if constexpr (V::kReading) e = Route{};
    v.Name("from", e.from, kRouteFromNames, kRouteSourceCount);
    v.Name("to", e.to, kRouteToNames, kRouteDestinationCount);
    v.Int("layer", e.layer, kRouteLayerSpec);
    v.Float("amount", e.amount, kAmountSpec);
  });
  v.NeedIf(m.routes.count > 0u, kModeFeatureRoutes, "routes", "modulation routes");
  v.Records("links", m.links.count, kLinkRecords, [&](uint32_t i) {
    Route& e = m.links.entries[i];
    if constexpr (V::kReading) e = Route{};
    v.Name("from", e.from, kDrawNames, kLinkDrawCount);
    v.Name("to", e.to, kDrawNames, kLinkDrawCount);
    v.Int("layer", e.layer, kRouteLayerSpec);
    v.Float("amount", e.amount, kAmountSpec);
  });
  v.NeedIf(m.links.count > 0u, kModeFeatureLinks, "links", "links");
  v.Macros(d);
  v.Controls(d);
  v.Object("performance", false, [&] {
    PerformanceState& p = d.state->performance;
    v.Bool("reverse", p.reverse, kNeedPerformanceReverse);
    v.Enum("time_mode", p.timeMode, kTimeModeSpec);
    v.Enum("subdiv", p.subdiv, kPerfSubdivSpec);
    v.Int("tempo_us_per_quarter", p.usPerQuarter, kTempoSpec);
  });
  v.Object("editor", false, [&] {
    v.RatioGen(d);
    v.Detached(d);
  });
}

// ═════════════════════════════════════════════════════════════════════════════════════════
// The reader
// ═════════════════════════════════════════════════════════════════════════════════════════

class Reader {
 public:
  static constexpr bool kReading = true;

  Reader(Document& d, const json::Value& root, const ReadOptions& options,
         std::vector<Finding>& findings)
      : d_(d), options_(options), out_(findings) {
    for (uint32_t id = 1; id <= kNumParams; ++id)
      leafBits_[id] = BitsOf(Row(static_cast<ParamId>(id)).def);
    stack_.push_back(Ctx{&root, "", std::vector<bool>(root.members.size(), false)});
  }

  void Run() {
    if (stack_.back().obj->type != json::Type::Object) {
      Error("E3", At(*stack_.back().obj, ""), "a preset document is an object");
      return;
    }
    Visit(*this, d_);
    CloseUnused();
    Unsupported();
    if (!HasErrors(out_)) Finish();
  }

  // ── Primitives ──────────────────────────────────────────────────────────────────────
  template <class F>
  void Object(const char* key, bool /*core*/, F&& body) {
    const json::Member* m = Take(key);
    if (m == nullptr) return;
    if (m->value.type != json::Type::Object) {
      TypeError(*m, "an object");
      return;
    }
    Push(m->value, Pointer(key));
    body();
    Pop();
  }

  void Leaf(const char* key, ParamId leafId) {
    const json::Member* m = Take(key);
    if (m == nullptr) return;
    const auto             raw = static_cast<uint32_t>(leafId);
    const ParamDescriptor& row = Row(leafId);
    uint32_t               bits;
    if (!ReadFloat(*m, &bits)) return;
    const Location loc = At(m->value, Pointer(key));
    if (LessBits(bits, BitsOf(row.min)) || LessBits(BitsOf(row.max), bits)) {
      Error("E4", loc,
            NumberText(bits) + " is outside " + Quoted(row.name) + "'s range [" +
                NumberText(BitsOf(row.min)) + ", " + NumberText(BitsOf(row.max)) + "]" +
                UnitSuffix(row));
      return;
    }
    if (row.kind == ParamKind::Reserved && bits != BitsOf(row.def)) {
      Error("E6", loc,
            Quoted(row.name) + " other than its default (" + NumberText(BitsOf(row.def)) +
                ") needs " + LeafWave(raw) + "; this build plays the default only");
      return;
    }
    leafBits_[raw]               = bits;
    leafWritten_[raw]            = true;
    d_.where["leaf:" + Dec(raw)] = loc;
  }

  template <class E>
  void Enum(const char* key, E& value, const EnumSpec& spec, bool /*core*/ = false,
            const std::string& whereKey = std::string()) {
    const json::Member* m = Take(key);
    if (m == nullptr) return;
    if (m->value.type != json::Type::String) {
      TypeError(*m, "a string");
      return;
    }
    const Location loc = At(m->value, Pointer(key));
    for (uint8_t i = 0; i < spec.count; ++i) {
      if (m->value.text == spec.names[i]) {
        value = static_cast<E>(i);
        if (!whereKey.empty()) d_.where[whereKey] = loc;
        if (i != spec.def) {
          const uint32_t f = spec.features != nullptr ? spec.features[i] : spec.feature;
          Need(f, loc, Quoted(m->value.text));
        }
        return;
      }
    }
    const char* hint = spec.hint != nullptr ? spec.hint(m->value.text) : nullptr;
    Error("E5", loc,
          Quoted(m->value.text) + " is not one of " + Join(spec.names, spec.count) +
              (hint != nullptr ? std::string(" (") + hint + ")" : std::string()));
  }

  void Float(const char* key, float& value, const FloatSpec& spec) {
    const json::Member* m = Take(key);
    if (m == nullptr) {
      if (spec.required) Missing(key);
      return;
    }
    uint32_t bits;
    if (!ReadFloat(*m, &bits)) return;
    const Location loc   = At(m->value, Pointer(key));
    const bool     below = spec.loOpen ? !LessBits(spec.lo, bits) : LessBits(bits, spec.lo);
    if (below || LessBits(spec.hi, bits)) {
      Error(spec.code, loc,
            NumberText(bits) + " is outside " + (spec.loOpen ? "(" : "[") + NumberText(spec.lo) +
                ", " + NumberText(spec.hi) + "]");
      return;
    }
    SetBits(&value, bits);
    if (bits != spec.def) Need(spec.feature, loc, Quoted(key) + " " + NumberText(bits));
  }

  template <class I>
  void Int(const char* key, I& value, const IntSpec& spec) {
    const json::Member* m = Take(key);
    if (m == nullptr) {
      if (spec.required) Missing(key);
      return;
    }
    int64_t x = 0;
    if (!ReadInt(*m, &x)) return;
    const Location loc = At(m->value, Pointer(key));
    if (x < spec.lo || x > spec.hi) {
      Error(spec.code, loc,
            DecSigned(x) + " is outside [" + DecSigned(spec.lo) + ", " + DecSigned(spec.hi) + "]");
      return;
    }
    value = static_cast<I>(x);
    if (x != spec.def) Need(spec.feature, loc, Quoted(key) + " " + DecSigned(x));
  }

  void Bool(const char* key, uint8_t& value, uint32_t feature) {
    const json::Member* m = Take(key);
    if (m == nullptr) return;
    if (m->value.type != json::Type::Bool) {
      TypeError(*m, "a boolean");
      return;
    }
    value = m->value.boolean ? 1u : 0u;
    if (value != 0u) Need(feature, At(m->value, Pointer(key)), Quoted(key) + " true");
  }

  // An index into a vocabulary, written as its name (route and link endpoints).
  void Name(const char* key, uint8_t& value, const char* const* names, uint8_t count) {
    const json::Member* m = Take(key);
    if (m == nullptr) {
      Missing(key);
      return;
    }
    if (m->value.type != json::Type::String) {
      TypeError(*m, "a string");
      return;
    }
    for (uint8_t i = 0; i < count; ++i) {
      if (m->value.text == names[i]) {
        value = i;
        return;
      }
    }
    Error("E5", At(m->value, Pointer(key)),
          Quoted(m->value.text) + " is not one of " + Join(names, count));
  }

  void ModType(const char* key, brainscape::ModulatorType& value) {
    const json::Member* m = Take(key);
    if (m == nullptr) return;  // lfo
    if (m->value.type != json::Type::String) {
      TypeError(*m, "a string");
      return;
    }
    if (m->value.text == "lfo" || m->value.text == "env") {
      value = m->value.text == "lfo" ? brainscape::ModulatorType::Lfo
                                     : brainscape::ModulatorType::Envelope;
      return;
    }
    Error("E5", At(m->value, Pointer(key)), Quoted(m->value.text) + " is not one of lfo, env");
  }

  // A tempo division (§2.2): "off", or a note value of clock.md §5.2 by name, which needs
  // `feature` (a synced base delay: tempo-synced times). `whereKey` records where it was read,
  // for the lints (L13, L14).
  void Division(const char* key, uint8_t& value, uint32_t feature,
                const std::string& whereKey = std::string()) {
    const json::Member* m = Take(key);
    if (m == nullptr) return;
    if (m->value.type != json::Type::String) {
      TypeError(*m, "a string");
      return;
    }
    const Location loc = At(m->value, Pointer(key));
    if (!whereKey.empty()) d_.where[whereKey] = loc;
    for (uint8_t i = 0; i <= kMaxSyncDivision; ++i) {
      if (m->value.text == kDivisionNames[i]) {
        value = i;
        if (i != 0u) Need(feature, loc, Quoted(key) + " " + Quoted(m->value.text));
        return;
      }
    }
    Error("E5", loc,
          Quoted(m->value.text) + " is not one of " +
              Join(kDivisionNames, sizeof kDivisionNames / sizeof kDivisionNames[0]));
  }

  template <class C, class F>
  void Records(const char* key, C& count, const RecordSpec& spec, F&& element) {
    const json::Member* m = Take(key);
    if (m == nullptr) return;
    if (m->value.type != json::Type::Array) {
      TypeError(*m, "an array");
      return;
    }
    const std::string ptr = Pointer(key);
    size_t            n   = m->value.items.size();
    if (n > spec.max) {
      Error("E7", At(m->value, ptr), "at most " + Dec(spec.max) + " " + spec.what);
      n = spec.max;
    }
    if (n < spec.min) {
      Error("E4", At(m->value, ptr), "at least " + Dec(spec.min) + " " + spec.what);
      return;
    }
    count = static_cast<C>(n);
    for (size_t i = 0; i < n; ++i) {
      const json::Value& e  = m->value.items[i];
      const std::string  ep = ptr + "/" + Dec(i);
      if (e.type != json::Type::Object) {
        Error("E3", At(e, ep), std::string("expected an object, found ") + TypeName(e.type));
        continue;
      }
      Push(e, ep);
      element(static_cast<uint32_t>(i));
      Pop();
    }
  }

  void NeedIf(bool condition, uint32_t feature, const char* key, const std::string& what) {
    if (!condition) return;
    const json::Member* m = Peek(key);
    Need(feature, m != nullptr ? At(m->value, Pointer(key)) : Here(), what);
  }

  // ── Special sections ────────────────────────────────────────────────────────────────
  void Identity(Document& d) {
    const json::Member* sv = Take("schema_version");
    if (sv == nullptr) {
      Missing("schema_version");
    } else {
      int64_t x = 0;
      if (ReadInt(*sv, &x)) {
        const Location loc = At(sv->value, Pointer("schema_version"));
        if (x > static_cast<int64_t>(kSchemaVersion)) {
          Error("E2", loc,
                "schema_version " + DecSigned(x) +
                    " is newer than this compiler, which reads "
                    "schema " +
                    Dec(kSchemaVersion));
        } else if (x < 1) {
          Error("E4", loc, "schema_version starts at 1");
        } else {
          d.schemaVersion = static_cast<uint32_t>(x);
        }
      }
    }
    Text("id", d.id, kIdSpec, "id");
    Text("name", d.name, kNameSpec, "name");
    Enum("family", d.family, kFamilySpec);
    // The stamp (§2.2): written by bspc stamp, kept by fmt, ignored by compile.
    const json::Member* rev  = Take("sound_rev");
    const json::Member* hash = Take("sound_hash");
    if ((rev == nullptr) != (hash == nullptr)) {
      Error("E3", Here(), "sound_rev and sound_hash are written together, by bspc stamp");
    } else if (rev != nullptr) {
      int64_t x  = 0;
      bool    ok = ReadInt(*rev, &x);
      if (ok && (x < 1 || x > 0xFFFFFFFFll)) {
        Error("E4", At(rev->value, Pointer("sound_rev")), "sound_rev is a revision from 1");
        ok = false;
      }
      if (hash->value.type != json::Type::String) {
        TypeError(*hash, "a string");
        ok = false;
      } else if (!ParseHex(hash->value.text, d.soundHash.bytes, 32)) {
        Error("E4", At(hash->value, Pointer("sound_hash")),
              "sound_hash is 64 lowercase hex digits");
        ok = false;
      }
      if (ok) {
        d.stamped  = true;
        d.soundRev = static_cast<uint32_t>(x);
      }
    }
    const json::Member* meta = Take("meta");
    if (meta != nullptr) {
      if (meta->value.type != json::Type::Object) {
        TypeError(*meta, "an object");
      } else {
        Push(meta->value, Pointer("meta"));
        Text("author", d.author, kAuthorSpec, "author");
        Text("description", d.description, kDescriptionSpec, "description");
        Tags(d);
        Pop();
      }
    }
  }

  void Sources(uint8_t& sources) {
    const json::Member* m = Take("sources");
    if (m == nullptr) return;
    if (m->value.type != json::Type::Array) {
      TypeError(*m, "an array");
      return;
    }
    const std::string ptr = Pointer("sources");
    d_.where["sources"]   = At(m->value, ptr);
    uint8_t bits          = 0;
    for (size_t i = 0; i < m->value.items.size(); ++i) {
      const json::Value& e  = m->value.items[i];
      const Location     at = At(e, ptr + "/" + Dec(i));
      if (e.type != json::Type::String) {
        Error("E3", at, std::string("expected a string, found ") + TypeName(e.type));
        continue;
      }
      uint8_t bit = 0;
      for (uint8_t k = 0; k < 5; ++k) {
        if (e.text == kSourceNames[k]) bit = static_cast<uint8_t>(1u << k);
      }
      if (bit == 0) {
        Error("E5", at,
              Quoted(e.text) + " is not one of " + Join(kSourceNames, 5) +
                  (e.text == "stochastic" ? " (stochastic is periodic with jitter 1)" : ""));
        continue;
      }
      if ((bits & bit) != 0u) {
        Error("E5", at, Quoted(e.text) + " is listed twice");
        continue;
      }
      bits = static_cast<uint8_t>(bits | bit);
    }
    sources = bits;
    // What this build plays, for the messages (§2.7 E6's example).
    uint32_t playable = kDefaultSources;
    if ((options_.supportedFeatures & kModeFeatureOnset) != 0u) playable |= kSourceOnset;
    if ((options_.supportedFeatures & kModeFeatureClock) != 0u) playable |= kSourceClock;
    const std::string supports = "; this build supports " + Join(kSourceNames, 5, playable);
    const Location    loc      = At(m->value, ptr);
    for (uint8_t k = 0; k < 5; ++k) {
      const auto bit = static_cast<uint8_t>(1u << k);
      if ((bits & bit) != 0u && bit == kSourceOnset) {
        Need(kModeFeatureOnset, loc, Quoted("onset") + " needs sound revision 2 (3a)" + supports,
             true);
      }
      if ((bits & bit) != 0u && bit == kSourceClock) {
        Need(kModeFeatureClock, loc, Quoted("clock") + " needs W2 (CLOCK)" + supports, true);
      }
      if ((bits & bit) == 0u && (kDefaultSources & bit) != 0u) {
        Need(kModeFeatureSources, loc,
             "leaving out " + Quoted(kSourceNames[k]) +
                 " needs W1 (source selection); this "
                 "build plays periodic, footswitch and midi_note always",
             true);
      }
    }
  }

  template <class F>
  void Layers(ModeBlob& m, F&& visitLayer) {
    const json::Member* lm = Take("layers");
    if (lm == nullptr) return;
    if (lm->value.type != json::Type::Array) {
      TypeError(*lm, "an array");
      return;
    }
    const std::string ptr = Pointer("layers");
    size_t            n   = lm->value.items.size();
    if (n > kMaxModeLayers) {
      Error("E7", At(lm->value, ptr), "at most 2 layers");
      n = kMaxModeLayers;
    }
    if (n < 1u) {
      Error("E4", At(lm->value, ptr), "at least 1 layer");
      return;
    }
    m.schedule.layerCount = static_cast<uint8_t>(n);
    if (n == 2u) {
      m.layers[1] = ModeLayer{};
      m.pitch[1]  = kDefaultPitchSet;
      Need(kModeFeatureTwoLayers, At(lm->value.items[1], ptr + "/1"), "a second layer");
    }
    for (size_t i = 0; i < n; ++i) {
      const json::Value& e  = lm->value.items[i];
      const std::string  ep = ptr + "/" + Dec(i);
      if (e.type != json::Type::Object) {
        Error("E3", At(e, ep), std::string("expected an object, found ") + TypeName(e.type));
        continue;
      }
      Push(e, ep);
      visitLayer(static_cast<uint32_t>(i));
      Pop();
    }
  }

  void Scale(const char* key, uint16_t& mask) {
    const json::Member* m = Take(key);
    if (m == nullptr) return;
    if (m->value.type != json::Type::Array) {
      TypeError(*m, "an array");
      return;
    }
    const std::string ptr  = Pointer(key);
    uint16_t          bits = 0;
    for (size_t i = 0; i < m->value.items.size(); ++i) {
      const json::Value& e  = m->value.items[i];
      const Location     at = At(e, ptr + "/" + Dec(i));
      int64_t            pc = 0;
      if (!ReadIntValue(e, at, &pc)) continue;
      if (pc < 0 || pc > 11) {
        Error("E4", at, "a pitch class is 0-11");
        continue;
      }
      if ((bits & (1u << pc)) != 0u) {
        Error("E5", at, "pitch class " + DecSigned(pc) + " is listed twice");
        continue;
      }
      bits = static_cast<uint16_t>(bits | (1u << pc));
    }
    mask = bits;
    if (bits != 0u) Need(kModeFeatureQuantize, At(m->value, ptr), "a scale");
  }

  void Modifiers(ModeLayer& L) {
    const json::Member* m = Take("modifiers");
    if (m == nullptr) return;
    if (m->value.type != json::Type::Array) {
      TypeError(*m, "an array");
      return;
    }
    const std::string ptr = Pointer("modifiers");
    size_t            n   = m->value.items.size();
    if (n > 2u) {
      Error("E7", At(m->value, ptr), "at most 2 modifiers, of distinct ops");
      n = 2;
    }
    for (size_t i = 0; i < n; ++i) {
      const json::Value& e  = m->value.items[i];
      const std::string  ep = ptr + "/" + Dec(i);
      if (e.type != json::Type::Object) {
        Error("E3", At(e, ep), std::string("expected an object, found ") + TypeName(e.type));
        continue;
      }
      Push(e, ep);
      const json::Member* op    = Take("op");
      ModifierOp          value = ModifierOp::None;
      if (op == nullptr) {
        Missing("op");
      } else if (op->value.type != json::Type::String) {
        TypeError(*op, "a string");
      } else if (op->value.text == "svf" || op->value.text == "crush") {
        value = op->value.text == "svf" ? ModifierOp::Svf : ModifierOp::Crush;
      } else {
        Error("E5", At(op->value, Pointer("op")),
              Quoted(op->value.text) + " is not one of svf, crush");
      }
      if (value != ModifierOp::None && (L.modifier[0] == value || L.modifier[1] == value)) {
        Error("E5", At(op->value, Pointer("op")), "a layer's modifiers are distinct ops");
        value = ModifierOp::None;
      }
      if (value == ModifierOp::Svf) {
        Enum("band", L.svfBand, kBandSpec);
        Enum("cutoff_src", L.svfCutoffSource, kCutoffSrcSpec);
        Need(kModeFeatureSvf, At(e, ep), "an svf modifier");
      } else if (value == ModifierOp::Crush) {
        Need(kModeFeatureCrush, At(e, ep), "a crush modifier");
      }
      if (value != ModifierOp::None) {
        L.modifier[L.modifier[0] == ModifierOp::None ? 0 : 1] = value;
      }
      Pop();
    }
  }

  void PostOrder(const char* key) {
    const json::Member* m = Take(key);
    if (m == nullptr) return;
    if (m->value.type != json::Type::Array) {
      TypeError(*m, "an array");
      return;
    }
    const std::string ptr     = Pointer(key);
    bool              inOrder = m->value.items.size() == 4u;
    uint32_t          seen    = 0;
    for (size_t i = 0; i < m->value.items.size(); ++i) {
      const json::Value& e  = m->value.items[i];
      const Location     at = At(e, ptr + "/" + Dec(i));
      if (e.type != json::Type::String) {
        Error("E3", at, std::string("expected a string, found ") + TypeName(e.type));
        return;
      }
      uint32_t k = 4;
      for (uint32_t s = 0; s < 4; ++s) {
        if (e.text == kStageNames[s]) k = s;
      }
      if (k == 4) {
        Error("E5", at, Quoted(e.text) + " is not one of " + Join(kStageNames, 4));
        return;
      }
      if ((seen & (1u << k)) != 0u) {
        Error("E5", at, Quoted(e.text) + " is listed twice");
        return;
      }
      seen |= 1u << k;
      inOrder = inOrder && k == i;
    }
    if (!inOrder) {
      Error("E6", At(m->value, ptr),
            "post-chain reordering is deferred (after v1); this build plays mod, delay, reverb, "
            "filter");
    }
  }

  void Macros(Document& d) {
    struct Pending {
      bool                     defined = false;
      std::vector<MacroTarget> targets;
    };
    Pending             p[kMaxMacros];
    const json::Member* mm = Take("macros");
    if (mm != nullptr) {
      const std::string ptr = Pointer("macros");
      if (mm->value.type != json::Type::Array) {
        TypeError(*mm, "an array");
      } else {
        if (mm->value.items.size() > kMaxMacros) {
          Error("E7", At(mm->value, ptr), "at most 8 macros");
        }
        for (size_t i = 0; i < mm->value.items.size(); ++i) {
          const json::Value& e  = mm->value.items[i];
          const std::string  ep = ptr + "/" + Dec(i);
          if (e.type != json::Type::Object) {
            Error("E3", At(e, ep), std::string("expected an object, found ") + TypeName(e.type));
            continue;
          }
          Push(e, ep);
          uint32_t            macroId = 0;
          const json::Member* idm     = Take("id");
          if (idm == nullptr) {
            Missing("id");
          } else if (idm->value.type != json::Type::String) {
            TypeError(*idm, "a string");
          } else {
            macroId = MacroIdByName(idm->value.text);
            if (macroId == 0) {
              Error("E5", At(idm->value, Pointer("id")),
                    Quoted(idm->value.text) +
                        " is not a macro: activity, repeats, shape, time, "
                        "space, filter, aux1, aux2");
            } else if (p[macroId - kFirstMacro].defined) {
              Error("E8", At(idm->value, Pointer("id")),
                    "macro " + Quoted(idm->value.text) + " is defined twice");
              macroId = 0;
            }
          }
          std::string display;
          Text("display_name", display, kDisplaySpec,
               macroId != 0 ? "display:" + Dec(macroId) : std::string());
          std::vector<MacroTarget> targets;
          const bool               written = ReadTargets(macroId, &targets);
          Pop();
          if (macroId != 0) {
            // A key left out takes its schema default (§2.2): a performance macro's targets
            // are §3.2's; written as [], it has none.
            if (!written) targets = DefaultTargets(macroId);
            Pending& slot                        = p[macroId - kFirstMacro];
            slot.defined                         = true;
            slot.targets                         = std::move(targets);
            d.displayName[macroId - kFirstMacro] = display;
            d_.where["macro:" + Dec(macroId)]    = At(e, ep);
          }
        }
      }
    }
    // An omitted performance macro takes its schema default (§3.2).
    const MacroTable& def = kDefaultMacroTable;
    for (uint32_t k = 0; k < def.macroCount; ++k) {
      Pending& slot = p[def.macros[k].id - kFirstMacro];
      if (slot.defined) continue;
      slot.defined = true;
      slot.targets = DefaultTargets(def.macros[k].id);
    }
    uint32_t total = 0;
    for (const Pending& slot : p) total += static_cast<uint32_t>(slot.targets.size());
    if (total > kMaxTargets) {
      Error("E7", mm != nullptr ? At(mm->value, Pointer("macros")) : Here(),
            "at most 32 macro targets in all, the defaults of omitted macros included (" +
                Dec(total) + ")");
      return;
    }
    MacroTable t{};
    for (uint32_t k = 0; k < kMaxMacros; ++k) {
      if (!p[k].defined) continue;
      MacroDef& md = t.macros[t.macroCount++];
      md.id        = kFirstMacro + k;
      md.first     = t.targetCount;
      md.count     = static_cast<uint8_t>(p[k].targets.size());
      for (const MacroTarget& target : p[k].targets) t.targets[t.targetCount++] = target;
    }
    d.state->mode.macros = t;
  }

  void Controls(Document& d) {
    float               position[kMaxMacros];
    bool                given[kMaxMacros] = {};
    const json::Member* cm                = Take("controls");
    const json::Member* exprMember        = nullptr;
    std::string         exprPtr;
    if (cm != nullptr && cm->value.type != json::Type::Object) {
      TypeError(*cm, "an object");
      cm = nullptr;
    }
    const MacroTable& t       = d.state->mode.macros;
    const auto        defined = [&](uint32_t macroId) {
      for (uint32_t k = 0; k < t.macroCount; ++k) {
        if (t.macros[k].id == macroId) return true;
      }
      return false;
    };
    if (cm != nullptr) {
      Push(cm->value, Pointer("controls"));
      const json::Member* pm = Take("macro_positions");
      if (pm != nullptr && pm->value.type != json::Type::Object) {
        TypeError(*pm, "an object");
      } else if (pm != nullptr) {
        const std::string ptr = Pointer("macro_positions");
        for (const json::Member& entry : pm->value.members) {
          const std::string ep      = ptr + "/" + json::PointerToken(entry.key);
          const uint32_t    macroId = MacroIdByName(entry.key);
          if (macroId == 0 || !defined(macroId)) {
            Error("E8", Location{ep, entry.line, entry.column},
                  "no macro " + Quoted(entry.key) + " is defined");
            continue;
          }
          uint32_t bits = 0;
          if (!ReadFloatValue(entry.value, At(entry.value, ep), &bits)) continue;
          if (LessBits(bits, kB0) || LessBits(kB1, bits)) {
            Error("E4", At(entry.value, ep), "a macro position is in [0, 1]");
            continue;
          }
          SetBits(&position[macroId - kFirstMacro], bits);
          given[macroId - kFirstMacro]         = true;
          d_.where["position:" + Dec(macroId)] = At(entry.value, ep);
        }
      }
      exprMember = Take("expression");
      exprPtr    = Pointer("expression");
      ReadExpression(d, exprMember, exprPtr);
      Pop();
    }
    // One position per defined macro, by ascending id; an omitted one compiles as 0.5 (§3.5).
    ControlState c{};
    for (MacroPosition& p : c.positions) p = MacroPosition{0, 0.0f};
    c.present    = 1;
    c.macroCount = t.macroCount;
    for (uint32_t k = 0; k < t.macroCount; ++k) {
      const uint32_t macroId = t.macros[k].id;
      c.positions[k].macroId = macroId;
      if (given[macroId - kFirstMacro]) {
        c.positions[k].position = position[macroId - kFirstMacro];
      } else {
        SetBits(&c.positions[k].position, kBHalf);
        d.omittedPositions.push_back(macroId);
      }
    }
    c.exprCount = d.state->control.exprCount;
    for (uint32_t i = 0; i < kMaxExpressions; ++i)
      c.expressions[i] = d.state->control.expressions[i];
    d.state->control = c;
  }

  void RatioGen(Document& d) {
    const json::Member* m = Take("ratio_gen");
    if (m == nullptr) return;
    if (m->value.type != json::Type::Object) {
      TypeError(*m, "an object");
      return;
    }
    d.hasRatioGen = true;
    d.ratioGen    = EditorValue(m->value, Pointer("ratio_gen"));
  }

  void Detached(Document& d) {
    const json::Member* m = Take("detached");
    if (m == nullptr) return;
    if (m->value.type != json::Type::Array) {
      TypeError(*m, "an array");
      return;
    }
    const std::string ptr = Pointer("detached");
    for (size_t i = 0; i < m->value.items.size(); ++i) {
      const json::Value& e  = m->value.items[i];
      const Location     at = At(e, ptr + "/" + Dec(i));
      if (e.type != json::Type::String) {
        Error("E3", at, std::string("expected a string, found ") + TypeName(e.type));
        continue;
      }
      const ParamDescriptor* row = SchemaLeaf(e.text);
      if (row == nullptr) {
        Error("E8", at, Quoted(e.text) + " is not a leaf of schema 1");
        continue;
      }
      const auto leafId = static_cast<uint32_t>(row->id);
      if (std::find(d.detached.begin(), d.detached.end(), leafId) != d.detached.end()) {
        Error("E5", at, Quoted(e.text) + " is listed twice");
        continue;
      }
      d.detached.push_back(leafId);
      d_.where["detached:" + Dec(leafId)] = at;
    }
    std::sort(d.detached.begin(), d.detached.end());
  }

 private:
  struct Ctx {
    const json::Value* obj;
    std::string        pointer;
    std::vector<bool>  used;
  };

  // ── Context ─────────────────────────────────────────────────────────────────────────
  const json::Member* Peek(const char* key) const {
    const Ctx& c = stack_.back();
    for (const json::Member& m : c.obj->members) {
      if (m.key == key) return &m;
    }
    return nullptr;
  }

  const json::Member* Take(const char* key) {
    Ctx& c = stack_.back();
    for (size_t i = 0; i < c.obj->members.size(); ++i) {
      if (c.obj->members[i].key == key) {
        c.used[i] = true;
        return &c.obj->members[i];
      }
    }
    return nullptr;
  }

  void Push(const json::Value& obj, std::string pointer) {
    stack_.push_back(Ctx{&obj, std::move(pointer), std::vector<bool>(obj.members.size(), false)});
  }

  void Pop() {
    CloseUnused();
    stack_.pop_back();
  }

  // E2 for every key the schema did not take at this level.
  void CloseUnused() {
    const Ctx& c = stack_.back();
    for (size_t i = 0; i < c.obj->members.size(); ++i) {
      if (c.used[i]) continue;
      const json::Member& m    = c.obj->members[i];
      const char*         hint = RemovedHint(m.key);
      Error(
          "E2", Location{c.pointer + "/" + json::PointerToken(m.key), m.line, m.column},
          "unknown key " + Quoted(m.key) + (hint != nullptr ? std::string(" (") + hint + ")" : ""));
    }
  }

  std::string Pointer(const char* key) const {
    return stack_.back().pointer + "/" + json::PointerToken(key);
  }

  Location Here() const {
    const Ctx& c = stack_.back();
    return Location{c.pointer, c.obj->line, c.obj->column};
  }

  static Location At(const json::Value& v, std::string pointer) {
    return Location{std::move(pointer), v.line, v.column};
  }

  // ── Findings ────────────────────────────────────────────────────────────────────────
  void Error(const char* code, Location at, std::string message) {
    out_.push_back(Finding{code, true, std::move(at), std::move(message)});
  }

  void TypeError(const json::Member& m, const char* expected) {
    Error("E3", At(m.value, Pointer(m.key.c_str())),
          std::string("expected ") + expected + ", found " + TypeName(m.value.type));
  }

  void Missing(const char* key) { Error("E3", Here(), "missing required key " + Quoted(key)); }

  // A feature (or the performance state) the content needs. The first place that needs each
  // one is where E6 points if this build lacks it.
  void Need(uint32_t features, const Location& at, const std::string& what, bool whole = false) {
    for (uint32_t bit = 1; bit != 0u; bit <<= 1) {
      if ((features & bit) == 0u || (needs_ & bit) != 0u) continue;
      needs_ |= bit;
      const FeatureInfo* info    = InfoFor(bit);
      std::string        message = whole ? what
                                         : what + " needs " +
                                        (info != nullptr ? info->wave : "a later wave") + " (" +
                                        (info != nullptr ? info->name : "?") + ")";
      if (!whole) message += "; this build plays the default structure";
      needAt_.push_back(NeedRecord{bit, at, std::move(message)});
    }
  }

  // ── Values ──────────────────────────────────────────────────────────────────────────
  bool ReadFloat(const json::Member& m, uint32_t* bits) {
    return ReadFloatValue(m.value, At(m.value, Pointer(m.key.c_str())), bits);
  }

  // A number read as the correctly rounded binary32 and canonicalized (§2.2): -0 and
  // subnormals become +0, the latter with finding L1; overflow is an error.
  bool ReadFloatValue(const json::Value& v, const Location& at, uint32_t* bits,
                      const char* subnormalFate = "it compiles to +0") {
    if (v.type != json::Type::Number) {
      Error("E3", at, std::string("expected a number, found ") + TypeName(v.type));
      return false;
    }
    float              f = 0.0f;
    const NumberStatus s = ParseJsonNumber(v.text.data(), v.text.size(), &f);
    if (s == NumberStatus::Range) {
      Error("E4", at, v.text + " overflows binary32");
      return false;
    }
    if (s != NumberStatus::Ok) {
      Error("E3", at, v.text + " is not a JSON number");
      return false;
    }
    uint32_t b = BitsOf(f);
    if ((b & 0x7F800000u) == 0u) {
      if ((b & 0x7FFFFFFFu) != 0u) {
        d_.notes.push_back(
            Finding{"L1", false, at, v.text + " is subnormal in binary32; " + subnormalFate});
      }
      b = 0u;
    }
    *bits = b;
    return true;
  }

  bool ReadInt(const json::Member& m, int64_t* out) {
    return ReadIntValue(m.value, At(m.value, Pointer(m.key.c_str())), out);
  }

  bool ReadIntValue(const json::Value& v, const Location& at, int64_t* out) {
    if (v.type != json::Type::Number) {
      Error("E3", at, std::string("expected an integer, found ") + TypeName(v.type));
      return false;
    }
    const json::IntStatus s = json::ParseInt(v.text, out);
    if (s == json::IntStatus::NotInteger) {
      Error("E3", at, v.text + " is not an integer (no fraction or exponent)");
      return false;
    }
    if (s == json::IntStatus::Range) {
      Error("E4", at, v.text + " is outside the 64-bit integers");
      return false;
    }
    return true;
  }

  void Text(const char* key, std::string& value, const TextSpec& spec,
            const std::string& whereKey) {
    const json::Member* m = Take(key);
    if (m == nullptr) {
      if (spec.required) Missing(key);
      return;
    }
    if (m->value.type != json::Type::String) {
      TypeError(*m, "a string");
      return;
    }
    const Location     at = At(m->value, Pointer(key));
    const std::string& s  = m->value.text;
    if (s.size() < spec.minBytes || s.size() > spec.maxBytes) {
      Error("E4", at,
            Quoted(key) + " is " + Dec(s.size()) + " bytes; it takes " + Dec(spec.minBytes) + "-" +
                Dec(spec.maxBytes));
      return;
    }
    if (spec.id ? !ValidIdText(s) : !PrintableText(s)) {
      Error("E4", at,
            spec.id ? Quoted(key) + " takes [a-z0-9._-] only"
                    : Quoted(key) + " cannot hold a control character");
      return;
    }
    value = s;
    if (!whereKey.empty()) d_.where[whereKey] = at;
  }

  void Tags(Document& d) {
    const json::Member* m = Take("tags");
    if (m == nullptr) return;
    if (m->value.type != json::Type::Array) {
      TypeError(*m, "an array");
      return;
    }
    const std::string ptr = Pointer("tags");
    if (m->value.items.size() > kMaxTags) Error("E7", At(m->value, ptr), "at most 8 tags");
    for (size_t i = 0; i < m->value.items.size() && i < kMaxTags; ++i) {
      const json::Value& e  = m->value.items[i];
      const Location     at = At(e, ptr + "/" + Dec(i));
      if (e.type != json::Type::String) {
        Error("E3", at, std::string("expected a string, found ") + TypeName(e.type));
        continue;
      }
      if (e.text.empty() || e.text.size() > kMaxTagBytes || !PrintableText(e.text)) {
        Error("E4", at, "a tag is 1-32 bytes of text without control characters");
        continue;
      }
      if (std::find(d.tags.begin(), d.tags.end(), e.text) != d.tags.end()) {
        Error("E5", at, "tag " + Quoted(e.text) + " is listed twice");
        continue;
      }
      d.tags.push_back(e.text);
      d_.where["tag:" + Dec(d.tags.size() - 1)] = at;
    }
  }

  // Two numbers: a range or an in_range.
  bool ReadPair(const json::Member& m, uint32_t out[2]) {
    const std::string ptr = Pointer(m.key.c_str());
    if (m.value.type != json::Type::Array || m.value.items.size() != 2u) {
      Error("E3", At(m.value, ptr), "expected an array of two numbers");
      return false;
    }
    return ReadFloatValue(m.value.items[0], At(m.value.items[0], ptr + "/0"), &out[0]) &&
           ReadFloatValue(m.value.items[1], At(m.value.items[1], ptr + "/1"), &out[1]);
  }

  // §3.2's default targets of a macro (none for aux1 and aux2).
  static std::vector<MacroTarget> DefaultTargets(uint32_t macroId) {
    std::vector<MacroTarget> out;
    const MacroTable&        def = kDefaultMacroTable;
    for (uint32_t k = 0; k < def.macroCount; ++k) {
      if (def.macros[k].id != macroId) continue;
      for (uint32_t i = 0; i < def.macros[k].count; ++i) {
        out.push_back(def.targets[def.macros[k].first + i]);
      }
    }
    return out;
  }

  // A macro's targets (§3.2): {param, range [lo, hi], in_range [a, b], curve}. Returns whether
  // the key is written.
  bool ReadTargets(uint32_t macroId, std::vector<MacroTarget>* targets) {
    const json::Member* tm = Take("targets");
    if (tm == nullptr) return false;
    if (tm->value.type != json::Type::Array) {
      TypeError(*tm, "an array");
      return true;
    }
    const std::string ptr = Pointer("targets");
    if (tm->value.items.size() > kMaxMacroTargets) {
      Error("E7", At(tm->value, ptr), "at most 8 targets per macro");
    }
    for (size_t j = 0; j < tm->value.items.size() && j < kMaxMacroTargets; ++j) {
      const json::Value& e  = tm->value.items[j];
      const std::string  ep = ptr + "/" + Dec(j);
      if (e.type != json::Type::Object) {
        Error("E3", At(e, ep), std::string("expected an object, found ") + TypeName(e.type));
        continue;
      }
      Push(e, ep);
      MacroTarget            target{0, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f};
      bool                   ok  = true;
      const ParamDescriptor* row = nullptr;
      const json::Member*    pm  = Take("param");
      if (pm == nullptr) {
        Missing("param");
        ok = false;
      } else if (pm->value.type != json::Type::String) {
        TypeError(*pm, "a string");
        ok = false;
      } else {
        row = TargetRow(pm->value.text, At(pm->value, Pointer("param")), false);
        ok  = row != nullptr;
        if (ok) {
          target.param = static_cast<uint32_t>(row->id);
          for (const MacroTarget& other : *targets) {
            if (other.param == target.param) {
              Error("E8", At(pm->value, Pointer("param")),
                    Quoted(row->name) + " is targeted twice by one macro");
              ok = false;
            }
          }
        }
      }
      uint32_t            range[2] = {0, 0};
      const json::Member* rm       = Take("range");
      if (rm == nullptr) {
        Missing("range");
        ok = false;
      } else if (!ReadPair(*rm, range)) {
        ok = false;
      } else if (row != nullptr) {
        for (int k = 0; k < 2; ++k) {
          if (LessBits(range[k], BitsOf(row->min)) || LessBits(BitsOf(row->max), range[k])) {
            Error("E9",
                  At(rm->value.items[static_cast<size_t>(k)], Pointer("range") + "/" + Dec(k)),
                  NumberText(range[k]) + " is outside " + Quoted(row->name) + "'s range [" +
                      NumberText(BitsOf(row->min)) + ", " + NumberText(BitsOf(row->max)) + "]");
            ok = false;
          }
        }
      }
      uint32_t            in[2] = {kB0, kB1};
      const json::Member* im    = Take("in_range");
      if (im != nullptr) {
        if (!ReadPair(*im, in)) {
          ok = false;
        } else if (LessBits(in[0], kB0) || LessBits(kB1, in[1]) || !LessBits(in[0], in[1])) {
          Error("E9", At(im->value, Pointer("in_range")), "in_range [a, b] needs 0 <= a < b <= 1");
          ok = false;
        }
      }
      uint32_t            curve = kB1;
      const json::Member* cv    = Take("curve");
      if (cv != nullptr) {
        if (!ReadFloat(*cv, &curve)) {
          ok = false;
        } else if (LessBits(curve, kB16th) || LessBits(kB16, curve)) {
          Error("E9", At(cv->value, Pointer("curve")), "curve is in [1/16, 16]");
          ok = false;
        }
      }
      if (ok && macroId != 0) {
        SetBits(&target.lo, range[0]);
        SetBits(&target.hi, range[1]);
        SetBits(&target.inLo, in[0]);
        SetBits(&target.inHi, in[1]);
        SetBits(&target.curve, curve);
        d_.where["target:" + Dec(macroId) + ":" + Dec(targets->size())] = At(e, ep);
        targets->push_back(target);
      }
      Pop();
    }
    return true;
  }

  // A macro or expression target named `name`: a leaf of schema 1, or (expression) a macro.
  const ParamDescriptor* TargetRow(const std::string& name, const Location& at, bool expression) {
    if (expression) {
      for (uint32_t k = 0; k < kMaxMacros; ++k) {
        const ParamDescriptor& row = Row(static_cast<ParamId>(kFirstMacro + k));
        if (name == row.name) return &row;
      }
    }
    const ParamDescriptor* row = SchemaLeaf(name);
    if (row == nullptr) {
      std::string why = " is not a leaf of schema 1";
      for (const ParamDescriptor& d : kParamTable) {
        if (d.name == nullptr || name != d.name) continue;
        if (d.kind == ParamKind::Macro) {
          why = " is a macro; a macro targets leaves";
        } else {
          why = " is not a leaf of a preset";
        }
      }
      Error("E8", at, Quoted(name) + why);
      return nullptr;
    }
    if (!expression && row->id == ParamId::Mix) {
      Error("E8", at, "global.mix is no macro's target (§3.1): the Mix knob sets it");
      return nullptr;
    }
    if (row->kind == ParamKind::Reserved) {
      Error("E6", at,
            Quoted(name) + " is a " + LeafWave(static_cast<uint32_t>(row->id)) +
                " leaf; targets on it need " + LeafWave(static_cast<uint32_t>(row->id)));
      return nullptr;
    }
    return row;
  }

  void ReadExpression(Document& d, const json::Member* m, const std::string& ptr) {
    if (m == nullptr) return;
    if (m->value.type != json::Type::Array) {
      TypeError(*m, "an array");
      return;
    }
    if (m->value.items.size() > kMaxExpressions) {
      Error("E7", At(m->value, ptr), "at most 4 expression assignments");
    }
    ControlState& c = d.state->control;
    c.exprCount     = 0;
    for (size_t i = 0; i < m->value.items.size() && i < kMaxExpressions; ++i) {
      const json::Value& e  = m->value.items[i];
      const std::string  ep = ptr + "/" + Dec(i);
      if (e.type != json::Type::Object) {
        Error("E3", At(e, ep), std::string("expected an object, found ") + TypeName(e.type));
        continue;
      }
      Push(e, ep);
      const ParamDescriptor* row = nullptr;
      const json::Member*    tm  = Take("target");
      if (tm == nullptr) {
        Missing("target");
      } else if (tm->value.type != json::Type::String) {
        TypeError(*tm, "a string");
      } else {
        row = TargetRow(tm->value.text, At(tm->value, Pointer("target")), true);
      }
      ExpressionAssignment a{};
      bool                 ok = row != nullptr;
      if (row != nullptr) {
        a.target = static_cast<uint32_t>(row->id);
        a.lo     = row->min;
        a.hi     = row->max;
        a.curve  = 1.0f;
      }
      for (int k = 0; k < 2; ++k) {
        const char*         key = k == 0 ? "lo" : "hi";
        const json::Member* em  = Take(key);
        if (em == nullptr) continue;
        uint32_t bits = 0;
        if (!ReadFloat(*em, &bits)) {
          ok = false;
          continue;
        }
        if (row != nullptr &&
            (LessBits(bits, BitsOf(row->min)) || LessBits(BitsOf(row->max), bits))) {
          Error("E9", At(em->value, Pointer(key)),
                NumberText(bits) + " is outside " + Quoted(row->name) + "'s range [" +
                    NumberText(BitsOf(row->min)) + ", " + NumberText(BitsOf(row->max)) + "]");
          ok = false;
          continue;
        }
        SetBits(k == 0 ? &a.lo : &a.hi, bits);
      }
      const json::Member* cv = Take("curve");
      if (cv != nullptr) {
        uint32_t bits = 0;
        if (!ReadFloat(*cv, &bits)) {
          ok = false;
        } else if (LessBits(bits, kB16th) || LessBits(kB16, bits)) {
          Error("E9", At(cv->value, Pointer("curve")), "curve is in [1/16, 16]");
          ok = false;
        } else {
          SetBits(&a.curve, bits);
        }
      }
      Pop();
      if (ok) {
        d_.where["expression:" + Dec(c.exprCount)] = At(e, ep);
        c.expressions[c.exprCount++]               = a;
      }
    }
  }

  // Editor data whose shape schema 1 leaves open (editor.ratio_gen, §2.2), validated and in
  // canonical form like the rest of the document (§6.4): numbers read as binary32 (overflow is
  // E4; -0 and subnormals, the latter with L1, become +0) and written as their canonical text,
  // object members sorted by key (bytewise), arrays in order, positions cleared so editor data
  // compares by content. Two spellings of the same data format, and hash, the same.
  json::Value EditorValue(const json::Value& v, const std::string& pointer) {
    json::Value out;
    out.type    = v.type;
    out.boolean = v.boolean;
    out.text    = v.text;
    if (v.type == json::Type::Number) {
      uint32_t bits = 0;
      if (ReadFloatValue(v, At(v, pointer), &bits, "it is kept as 0")) out.text = NumberText(bits);
    }
    for (size_t i = 0; i < v.items.size(); ++i)
      out.items.push_back(EditorValue(v.items[i], pointer + "/" + Dec(i)));
    for (const json::Member& m : v.members) {
      json::Member member;
      member.key   = m.key;
      member.value = EditorValue(m.value, pointer + "/" + json::PointerToken(m.key));
      out.members.push_back(std::move(member));
    }
    std::sort(out.members.begin(), out.members.end(),
              [](const json::Member& x, const json::Member& y) { return x.key < y.key; });
    return out;
  }

  // ── After the walk: rules that need the whole document (§2.7) ───────────────────────
  void Finish() {
    PresetState& s = *d_.state;
    ModeBlob&    m = s.mode;
    // Leaves of absent elements are not written, or hold their defaults (§2.2).
    for (uint32_t leafId = 1; leafId <= kNumParams; ++leafId) {
      if (!leafWritten_[leafId] || ElementPresent(m, leafId)) continue;
      const ParamDescriptor& row = Row(static_cast<ParamId>(leafId));
      if (leafBits_[leafId] != BitsOf(row.def)) {
        Error("E8", *d_.Where("leaf:" + Dec(leafId)),
              Quoted(row.name) +
                  " belongs to an element this preset does not have, so it "
                  "keeps its default");
      }
    }
    // E8: macro and expression targets name leaves of present elements; references hold.
    const MacroTable& t = m.macros;
    for (uint32_t k = 0; k < t.macroCount; ++k) {
      for (uint32_t i = 0; i < t.macros[k].count; ++i) {
        const MacroTarget& target = t.targets[t.macros[k].first + i];
        if (ElementPresent(m, target.param)) continue;
        const Location* at = d_.Where("target:" + Dec(t.macros[k].id) + ":" + Dec(i));
        Error("E8", at != nullptr ? *at : Here(),
              Quoted(Row(static_cast<ParamId>(target.param)).name) +
                  " belongs to an element this preset does not have");
      }
      // E10: a written shape macro has a target.
      if (t.macros[k].id == static_cast<uint32_t>(ParamId::MacroShape) && t.macros[k].count == 0u) {
        const Location* at = d_.Where("macro:" + Dec(t.macros[k].id));
        Error("E10", at != nullptr ? *at : Here(),
              "the shape macro has no target (\"Shape does nothing\" is a mapping defect)");
      }
    }
    for (uint32_t i = 0; i < s.control.exprCount; ++i) {
      const uint32_t target = s.control.expressions[i].target;
      if (IsSchemaLeaf(target) && !ElementPresent(m, target)) {
        Error("E8", *d_.Where("expression:" + Dec(i)),
              Quoted(Row(static_cast<ParamId>(target)).name) +
                  " belongs to an element this preset does not have");
      }
    }
    for (uint32_t i = 0; i < m.steps.countMax; ++i) {
      if (m.steps.entries[i].ratioIdx >= m.pitch[0].count) {
        Error("E8", Location{"/scheduler/steps/entries/" + Dec(i) + "/ratio_idx", 0, 0},
              "ratio_idx names no entry of layer 0's pitch set");
      }
    }
    uint32_t modulators = 0;
    while (modulators < kMaxModulators && m.modulators[modulators].type != ModulatorType::None) {
      ++modulators;
    }
    for (uint32_t i = 0; i < m.routes.count; ++i) {
      if (m.routes.entries[i].from >= modulators ||
          m.routes.entries[i].layer >= m.schedule.layerCount) {
        Error("E8", Location{"/routes/" + Dec(i), 0, 0},
              "a route's modulator and layer must exist");
      }
    }
    for (uint32_t i = 0; i < m.links.count; ++i) {
      if (m.links.entries[i].from == m.links.entries[i].to ||
          m.links.entries[i].layer >= m.schedule.layerCount) {
        Error("E8", Location{"/links/" + Dec(i), 0, 0},
              "a link joins two different draws of an existing layer");
      }
    }
    for (uint32_t l = 0; l < m.schedule.layerCount; ++l) {
      const ModeLayer& L = m.layers[l];
      if (L.quantize == QuantizeMode::Off && (L.quantizeRoot != 0u || L.scaleMask != 0u)) {
        Error("E8", Location{"/layers/" + Dec(l) + "/pitch/quantize", 0, 0},
              "a root and scale need quantize mode \"scale\"");
      }
      if (L.quantize == QuantizeMode::Scale && L.scaleMask == 0u) {
        Error("E4", Location{"/layers/" + Dec(l) + "/pitch/quantize", 0, 0},
              "a scale needs at least one pitch class");
      }
    }
    // The leaves: every Leaf row of a present element, ascending (§6.2 STAT).
    s.leafCount = 0;
    for (const ParamDescriptor& row : kParamTable) {
      const auto leafId = static_cast<uint32_t>(row.id);
      if (row.kind != ParamKind::Leaf || !ElementPresent(m, leafId)) continue;
      PresetLeaf& leaf = s.leaves[s.leafCount++];
      leaf.id          = leafId;
      SetBits(&leaf.value, leafBits_[leafId]);
    }
    // E11: two layers share the voices and the slots.
    if (m.schedule.layerCount == 2u) LayerBudget(s);
    if (HasErrors(out_)) return;
    // The features the content requires: what the reader recorded, by construction.
    m.features = RequiredModeFeatures(m);
    if (m.features != (needs_ & kModeFeatureAll)) {
      Error("X1", Here(),
            "internal: the schema recorded features " + Dec(needs_ & kModeFeatureAll) +
                " where RequiredModeFeatures gives " + Dec(m.features));
    }
  }

  // E6: what this build cannot play, named with its wave, where it is first written.
  void Unsupported() {
    uint32_t supported = options_.supportedFeatures & kModeFeatureAll;
    if ((options_.supportedFeatures & kModeFeatureClock) != 0u) supported |= kNeedPerformanceTempo;
    if (options_.globalReverse) supported |= kNeedPerformanceReverse;
    for (const NeedRecord& n : needAt_) {
      if ((supported & n.bit) == 0u) Error("E6", n.at, n.message);
    }
  }

  // Exact a + b <= 1 for the slot shares, and the voice_count maxima within 64 (§2.7 E11).
  void LayerBudget(const PresetState& s) {
    const auto share = [](uint32_t b, uint64_t out[3]) {
      // b as an integer scaled by 2^149 (b canonical, in (0, 1]).
      const uint32_t exponent = (b >> 23) & 0xFFu;
      uint64_t       part[3]  = {(b & 0x7FFFFFu) | (exponent != 0u ? 0x800000u : 0u), 0, 0};
      for (uint32_t sft = exponent > 0u ? exponent - 1u : 0u; sft > 0;) {
        const uint32_t k = sft > 63u ? 63u : sft;
        part[2]          = (part[2] << k) | (part[1] >> (64u - k));
        part[1]          = (part[1] << k) | (part[0] >> (64u - k));
        part[0] <<= k;
        sft -= k;
      }
      out[0] = part[0];
      out[1] = part[1];
      out[2] = part[2];
    };
    uint64_t a[3], b[3], sum[3];
    share(BitsOf(s.mode.layers[0].slotShare), a);
    share(BitsOf(s.mode.layers[1].slotShare), b);
    uint64_t carry = 0;
    for (int i = 0; i < 3; ++i) {
      const uint64_t x = a[i] + b[i];
      const uint64_t c = x < a[i] ? 1u : 0u;
      sum[i]           = x + carry;
      carry            = c | (sum[i] < x ? 1u : 0u);
    }
    const uint64_t one2 = uint64_t{1} << 21;  // 2^149 = word 2, bit 21
    if (!(sum[2] < one2 || (sum[2] == one2 && sum[1] == 0u && sum[0] == 0u))) {
      Error("E11", Location{"/layers", 0, 0}, "the two layers' slot shares sum past 1");
    }
    const auto rounded = [](uint32_t bits) -> uint32_t {
      if ((bits & 0x80000000u) != 0u || OrderKey(bits) < OrderKey(kB1)) return 1;
      const uint32_t exponent = (bits >> 23) & 0xFFu;
      if (exponent >= 127u + 6u) return 64;
      const uint32_t e  = exponent - 127u;
      const uint32_t mm = (bits & 0x7FFFFFu) | 0x800000u;
      return (mm >> (23u - e)) + ((mm >> (22u - e)) & 1u);
    };
    const auto maxVoices = [&](uint32_t leafId) {
      uint32_t          best = rounded(leafBits_[leafId]);
      const MacroTable& t    = s.mode.macros;
      for (uint32_t i = 0; i < t.targetCount; ++i) {
        if (t.targets[i].param != leafId) continue;
        best = std::max({best, rounded(BitsOf(t.targets[i].lo)), rounded(BitsOf(t.targets[i].hi))});
      }
      for (uint32_t i = 0; i < s.control.exprCount; ++i) {
        const ExpressionAssignment& e = s.control.expressions[i];
        if (e.target != leafId) continue;
        best = std::max({best, rounded(BitsOf(e.lo)), rounded(BitsOf(e.hi))});
      }
      return best;
    };
    if (maxVoices(static_cast<uint32_t>(ParamId::VoiceCount)) +
            maxVoices(static_cast<uint32_t>(ParamId::L1VoiceCount)) >
        64u) {
      Error("E11", Location{"/layers", 0, 0},
            "the two layers' voice_count maxima (leaves, macro and expression ends) sum past 64");
    }
  }

  struct NeedRecord {
    uint32_t    bit;
    Location    at;
    std::string message;
  };

  Document&               d_;
  const ReadOptions&      options_;
  std::vector<Finding>&   out_;
  std::vector<Ctx>        stack_;
  uint32_t                leafBits_[kNumParams + 1]    = {};
  bool                    leafWritten_[kNumParams + 1] = {};
  uint32_t                needs_                       = 0;
  std::vector<NeedRecord> needAt_;
};

// ═════════════════════════════════════════════════════════════════════════════════════════
// The writer
// ═════════════════════════════════════════════════════════════════════════════════════════

class Writer {
 public:
  static constexpr bool kReading = false;

  Writer(const Document& d, bool withEditor) : d_(d), withEditor_(withEditor) {
    stack_.push_back(json::Value::Object());
  }

  json::Value Take() { return std::move(stack_.back()); }

  template <class F>
  void Object(const char* key, bool core, F&& body) {
    if (!withEditor_ && std::string_view(key) == "editor" && stack_.size() == 1u) return;
    stack_.push_back(json::Value::Object());
    body();
    json::Value v = std::move(stack_.back());
    stack_.pop_back();
    if (core || !v.members.empty()) Top().Add(key, std::move(v));
  }

  void Leaf(const char* key, ParamId leafId) {
    const auto             raw = static_cast<uint32_t>(leafId);
    const ParamDescriptor& row = Row(leafId);
    if (row.kind != ParamKind::Leaf || !ElementPresent(d_.state->mode, raw)) return;
    Top().Add(key, json::Value::Number(NumberText(d_.LeafBits(raw))));
  }

  template <class E>
  void Enum(const char* key, const E& value, const EnumSpec& spec, bool core = false,
            const std::string& /*whereKey*/ = std::string()) {
    const auto v = static_cast<uint8_t>(value);
    if (v == spec.def && !core && records_ == 0) return;
    Top().Add(key, json::Value::String(v < spec.count ? spec.names[v] : "?"));
  }

  void Float(const char* key, const float& value, const FloatSpec& spec) {
    const uint32_t bits = BitsOf(value);
    if (bits == spec.def && records_ == 0) return;
    Top().Add(key, json::Value::Number(NumberText(bits)));
  }

  template <class I>
  void Int(const char* key, const I& value, const IntSpec& spec) {
    const auto x = static_cast<int64_t>(value);
    if (x == spec.def && records_ == 0) return;
    Top().Add(key, json::Value::Number(DecSigned(x)));
  }

  void Bool(const char* key, const uint8_t& value, uint32_t /*feature*/) {
    if (value == 0u && records_ == 0) return;
    Top().Add(key, json::Value::Bool(value != 0u));
  }

  void Name(const char* key, const uint8_t& value, const char* const* names, uint8_t count) {
    Top().Add(key, json::Value::String(value < count ? names[value] : "?"));
  }

  void ModType(const char* key, const brainscape::ModulatorType& value) {
    Top().Add(key,
              json::Value::String(value == brainscape::ModulatorType::Envelope ? "env" : "lfo"));
  }

  void Division(const char* key, const uint8_t& value, uint32_t /*feature*/,
                const std::string& /*whereKey*/ = std::string()) {
    if (value == 0u && records_ == 0) return;
    Top().Add(key, json::Value::String(value <= kMaxSyncDivision ? kDivisionNames[value] : "?"));
  }

  template <class C, class F>
  void Records(const char* key, const C& count, const RecordSpec& spec, F&& element) {
    if (count == 0u && !spec.core) return;
    json::Value array = json::Value::Array();
    ++records_;
    for (uint32_t i = 0; i < static_cast<uint32_t>(count); ++i) {
      stack_.push_back(json::Value::Object());
      element(i);
      array.Push(std::move(stack_.back()));
      stack_.pop_back();
    }
    --records_;
    Top().Add(key, std::move(array));
  }

  void NeedIf(bool, uint32_t, const char*, const std::string&) {}

  void Identity(const Document& d) {
    json::Value& o = Top();
    o.Add("schema_version", json::Value::Number(Dec(d.schemaVersion)));
    o.Add("id", json::Value::String(d.id));
    o.Add("name", json::Value::String(d.name));
    o.Add("family", json::Value::String(kFamilyNames[static_cast<uint8_t>(d.family) % 5u]));
    if (d.stamped) {
      o.Add("sound_rev", json::Value::Number(Dec(d.soundRev)));
      o.Add("sound_hash", json::Value::String(Hex(d.soundHash.bytes, 32)));
    }
    json::Value meta = json::Value::Object();
    meta.Add("author", json::Value::String(d.author));
    meta.Add("description", json::Value::String(d.description));
    json::Value tags = json::Value::Array();
    for (const std::string& tag : d.tags) tags.Push(json::Value::String(tag));
    meta.Add("tags", std::move(tags));
    o.Add("meta", std::move(meta));
  }

  void Sources(const uint8_t& sources) {
    json::Value a = json::Value::Array();
    for (uint8_t k = 0; k < 5; ++k) {
      if ((sources & (1u << k)) != 0u) a.Push(json::Value::String(kSourceNames[k]));
    }
    Top().Add("sources", std::move(a));
  }

  template <class F>
  void Layers(const ModeBlob& m, F&& visitLayer) {
    json::Value array = json::Value::Array();
    for (uint32_t n = 0; n < m.schedule.layerCount && n < kMaxModeLayers; ++n) {
      stack_.push_back(json::Value::Object());
      visitLayer(n);
      array.Push(std::move(stack_.back()));
      stack_.pop_back();
    }
    Top().Add("layers", std::move(array));
  }

  void Scale(const char* key, const uint16_t& mask) {
    if (mask == 0u) return;
    json::Value a = json::Value::Array();
    for (uint32_t pc = 0; pc < 12; ++pc) {
      if ((mask & (1u << pc)) != 0u) a.Push(json::Value::Number(Dec(pc)));
    }
    Top().Add(key, std::move(a));
  }

  void Modifiers(const ModeLayer& L) {
    json::Value a = json::Value::Array();
    for (const ModifierOp op : L.modifier) {
      if (op == ModifierOp::None) continue;
      json::Value o = json::Value::Object();
      o.Add("op", json::Value::String(op == ModifierOp::Svf ? "svf" : "crush"));
      if (op == ModifierOp::Svf) {
        o.Add("band", json::Value::String(kBandNames[static_cast<uint8_t>(L.svfBand) % 4u]));
        o.Add("cutoff_src",
              json::Value::String(kCutoffSrcNames[static_cast<uint8_t>(L.svfCutoffSource) % 4u]));
      }
      a.Push(std::move(o));
    }
    if (!a.items.empty()) Top().Add("modifiers", std::move(a));
  }

  void PostOrder(const char*) {}

  void Macros(const Document& d) {
    const MacroTable& t = d.state->mode.macros;
    json::Value       a = json::Value::Array();
    for (uint32_t k = 0; k < t.macroCount && k < kMaxMacros; ++k) {
      const MacroDef& md = t.macros[k];
      json::Value     o  = json::Value::Object();
      o.Add("id", json::Value::String(MacroName(md.id) != nullptr ? MacroName(md.id) : "?"));
      const std::string& display = md.id >= kFirstMacro && md.id < kFirstMacro + kMaxMacros
                                       ? d.displayName[md.id - kFirstMacro]
                                       : std::string();
      if (!display.empty()) o.Add("display_name", json::Value::String(display));
      json::Value targets = json::Value::Array();
      for (uint32_t i = 0; i < md.count; ++i) {
        const MacroTarget&     target = t.targets[md.first + i];
        const ParamDescriptor* row    = FindParam(static_cast<ParamId>(target.param));
        json::Value            to     = json::Value::Object();
        to.Add("param",
               json::Value::String(row != nullptr && row->name != nullptr ? row->name : "?"));
        to.Add("range", Pair(BitsOf(target.lo), BitsOf(target.hi)));
        to.Add("in_range", Pair(BitsOf(target.inLo), BitsOf(target.inHi)));
        to.Add("curve", json::Value::Number(NumberText(BitsOf(target.curve))));
        targets.Push(std::move(to));
      }
      o.Add("targets", std::move(targets));
      a.Push(std::move(o));
    }
    Top().Add("macros", std::move(a));
  }

  void Controls(const Document& d) {
    const ControlState& c         = d.state->control;
    json::Value         controls  = json::Value::Object();
    json::Value         positions = json::Value::Object();
    for (uint32_t k = 0; k < c.macroCount && k < kMaxMacros; ++k) {
      const char* name = MacroName(c.positions[k].macroId);
      positions.Add(name != nullptr ? name : "?",
                    json::Value::Number(NumberText(BitsOf(c.positions[k].position))));
    }
    controls.Add("macro_positions", std::move(positions));
    if (c.exprCount > 0u) {
      json::Value expression = json::Value::Array();
      for (uint32_t i = 0; i < c.exprCount && i < kMaxExpressions; ++i) {
        const ExpressionAssignment& e   = c.expressions[i];
        const ParamDescriptor*      row = FindParam(static_cast<ParamId>(e.target));
        json::Value                 o   = json::Value::Object();
        o.Add("target",
              json::Value::String(row != nullptr && row->name != nullptr ? row->name : "?"));
        o.Add("lo", json::Value::Number(NumberText(BitsOf(e.lo))));
        o.Add("hi", json::Value::Number(NumberText(BitsOf(e.hi))));
        o.Add("curve", json::Value::Number(NumberText(BitsOf(e.curve))));
        expression.Push(std::move(o));
      }
      controls.Add("expression", std::move(expression));
    }
    Top().Add("controls", std::move(controls));
  }

  void RatioGen(const Document& d) {
    if (d.hasRatioGen) Top().Add("ratio_gen", d.ratioGen);
  }

  void Detached(const Document& d) {
    if (d.detached.empty()) return;
    json::Value a = json::Value::Array();
    for (const uint32_t leafId : d.detached) {
      const ParamDescriptor* row = FindParam(static_cast<ParamId>(leafId));
      a.Push(json::Value::String(row != nullptr && row->name != nullptr ? row->name : "?"));
    }
    Top().Add("detached", std::move(a));
  }

 private:
  json::Value& Top() { return stack_.back(); }

  static json::Value Pair(uint32_t a, uint32_t b) {
    json::Value p = json::Value::Array();
    p.Push(json::Value::Number(NumberText(a)));
    p.Push(json::Value::Number(NumberText(b)));
    return p;
  }

  const Document&          d_;
  bool                     withEditor_;
  std::vector<json::Value> stack_;
  int                      records_ = 0;  // inside records every field is written
};

}  // namespace

// ═════════════════════════════════════════════════════════════════════════════════════════

bool ReadDocument(const json::Value& root, const ReadOptions& options, Document* out,
                  std::vector<Finding>* findings) {
  *out = Document{};
  std::vector<Finding>  local;
  std::vector<Finding>& f      = findings != nullptr ? *findings : local;
  const size_t          before = f.size();
  // Step 2's migration by name (Migrate.h): a document of an older schema is renamed into this
  // one's keys first. Schema 1 is the first, so none is older yet.
  const json::Value* version =
      root.type == json::Type::Object ? root.Find("schema_version") : nullptr;
  int64_t v = 0;
  if (version != nullptr && version->type == json::Type::Number &&
      json::ParseInt(version->text, &v) == json::IntStatus::Ok && v >= 1 &&
      v < static_cast<int64_t>(kSchemaVersion)) {
    json::Value migrated = root;
    MigrateByName(&migrated, static_cast<uint32_t>(v));
    Reader reader(*out, migrated, options, f);
    reader.Run();
  } else {
    Reader reader(*out, root, options, f);
    reader.Run();
  }
  // In document order (the walk reports some keys when it leaves their object).
  std::stable_sort(f.begin() + static_cast<std::ptrdiff_t>(before), f.end(),
                   [](const Finding& x, const Finding& y) {
                     const auto key = [](const Finding& g) {
                       return (static_cast<uint64_t>(g.at.line == 0 ? UINT32_MAX : g.at.line)
                               << 32) |
                              g.at.column;
                     };
                     return key(x) < key(y);
                   });
  for (size_t i = before; i < f.size(); ++i) {
    if (f[i].error) return false;
  }
  return true;
}

bool ReadDocumentText(std::string_view text, const ReadOptions& options, Document* out,
                      std::vector<Finding>* findings) {
  if (text.size() > kMaxDocumentBytes) {
    if (findings != nullptr) {
      findings->push_back(Finding{"E12", true, Location{},
                                  "the document is " + Dec(text.size()) + " bytes; at most " +
                                      Dec(kMaxDocumentBytes) + " are read (a package is at most " +
                                      Dec(kMaxPackageBytes) + ")"});
    }
    return false;
  }
  json::Value      root;
  json::ParseError error;
  if (!json::Parse(text, &root, &error)) {
    if (findings != nullptr) {
      findings->push_back(
          Finding{"E1", true, Location{error.pointer, error.line, error.column}, error.message});
    }
    return false;
  }
  return ReadDocument(root, options, out, findings);
}

json::Value WriteDocument(const Document& doc, bool withEditor) {
  Writer w(doc, withEditor);
  // The visit functions take a mutable document for the reader; the writer only reads it.
  Visit(w, const_cast<Document&>(doc));
  return w.Take();
}

std::string FormatDocument(const Document& doc, bool withEditor) {
  return json::Serialize(WriteDocument(doc, withEditor));
}

}  // namespace bsc
