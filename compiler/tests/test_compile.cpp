// The compiler (docs/design/mode-compiler.md §2, §6, §8, §10.1): schema 1 read and validated
// (E1-E12), the canonical form, the package, decompiling and the round-trip contract.
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "Compile.h"
#include "Document.h"
#include "Migrate.h"
#include "Text.h"
#include "TestUtil.h"
#include "brainscape/SoundRevision.h"
#include "catch.hpp"

using namespace bsc;
using namespace bsctest;
using brainscape::ParamDescriptor;
using brainscape::ParamId;
using brainscape::ParamKind;

namespace {

// Compiles `text`; on failure the findings show in the test's output.
CompileResult Ok(const std::string& text, const CompileOptions& options = {}) {
  CompileResult r = Compile(text, options);
  INFO(All(r.findings));
  REQUIRE(r.ok);
  return r;
}

// The findings of a document that must not compile, with `code` among them at `pointer`.
std::vector<Finding> Refused(const std::string& text, const char* code, const char* pointer,
                             const CompileOptions& options = {}) {
  const CompileResult r = Compile(text, options);
  INFO(text);
  INFO(All(r.findings));
  REQUIRE_FALSE(r.ok);
  bool found = false;
  for (const Finding& f : r.findings) {
    found = found || (f.code == code && f.error && (pointer == nullptr || f.at.pointer == pointer));
  }
  REQUIRE(found);
  return r.findings;
}

std::string With(const std::string& path, json::Value value, const std::string& base = Minimal()) {
  json::Value root = Parse(base);
  Set(root, path, std::move(value));
  return json::Serialize(root);
}

std::string Fmt(const std::string& text, const ReadOptions& o = {}) {
  std::string          out;
  std::vector<Finding> f;
  INFO(All(f));
  REQUIRE(FormatText(text, &out, &f, o));
  return out;
}

const ParamDescriptor& Row(uint32_t id) { return *brainscape::FindParam(static_cast<ParamId>(id)); }

// The value at a JSON pointer ("/layers/0/size_ms"), or null.
const json::Value* Lookup(const json::Value& root, const std::string& pointer) {
  const json::Value* at = &root;
  size_t             i  = 1;
  while (at != nullptr && i <= pointer.size()) {
    const size_t      end = pointer.find('/', i);
    const std::string token =
        pointer.substr(i, end == std::string::npos ? std::string::npos : end - i);
    if (at->type == json::Type::Array) {
      const size_t k = static_cast<size_t>(std::stoul(token));
      at             = k < at->items.size() ? &at->items[k] : nullptr;
    } else {
      at = at->Find(token);
    }
    if (end == std::string::npos) break;
    i = end + 1;
  }
  return at;
}

// The round-trip contract (§10.1) for one document: Decompile(Compile(J)) is the JSON section,
// Fmt(J) stamped; rebuilt from the package it is the same minus editor data; the JSON section
// recompiles to the same bytes; Fmt is idempotent.
void RoundTrip(const std::string& text, const CompileOptions& o = {}) {
  const CompileResult r   = Ok(text, o);
  const std::string   fmt = Fmt(text, o.read);
  REQUIRE(Fmt(fmt, o.read) == fmt);
  Document stamped  = r.doc;
  stamped.stamped   = true;
  stamped.soundRev  = brainscape::kSoundRevision;
  stamped.soundHash = r.soundHash;
  REQUIRE(r.json == FormatDocument(stamped));
  const DecompileResult d = Decompile(r.package.data(), r.package.size(), false, o);
  REQUIRE(d.ok);
  REQUIRE_FALSE(d.rebuilt);
  REQUIRE(d.json == r.json);
  const DecompileResult rb = Decompile(r.package.data(), r.package.size(), true, o);
  INFO(All(rb.findings));
  REQUIRE(rb.ok);
  REQUIRE(rb.json == FormatDocument(stamped, false));
  const CompileResult again = Ok(r.json, o);
  REQUIRE(again.package == r.package);
  REQUIRE(Ok(r.json, o).package == r.package);  // deterministic
  REQUIRE(Fmt(r.json, o.read) == r.json);
  REQUIRE(Verify(r.package.data(), r.package.size(), o).empty());
}

// One layer with every element and the vocabulary of every wave (needs AllFeatures()).
const char* const kFullStructure = R"({
  "schema_version": 1, "id": "test.full", "name": "Full",
  "scheduler": {"sources": ["onset", "clock", "midi_note"], "subdiv": "tap",
    "steps": {"order": "shuffle", "entries": [
      {"slot": 0, "pos_sel": 10, "ratio_idx": 1, "gain": 0.5, "prob": 0.75, "flags": 1},
      {"slot": 3}]}},
  "layers": [{"slot_share": 0.5,
    "position": {"source": "mark", "spray_law": "exp",
                 "mark": {"index": 2, "walk": "cascade", "jitter": 0.25},
                 "pin": {"rearm": "time", "rearm_ms": 2500}},
    "pitch": {"set": [{"st": 0, "weight": 2}, {"st": 12}, {"st": -7.5, "weight": 16}],
              "select": "random",
              "quantize": {"mode": "scale", "root": 2, "scale": [11, 0, 2, 4, 5, 7, 9]},
              "glide": {"st_start": -12, "st_end": 12}},
    "modifiers": [{"op": "svf", "band": "bp", "cutoff_src": "lfo"}, {"op": "crush"}]}],
  "dry_duck": {"attack_ms": 10, "release_ms": 200},
  "modulators": [{"type": "lfo", "shape": "triangle", "attack_ms": 5, "release_ms": 50},
                 {"type": "env"}],
  "routes": [{"from": "modulator0", "to": "cutoff", "amount": 0.5},
             {"from": "modulator1", "to": "pan", "amount": -0.25}],
  "links": [{"from": "grain.pitch", "to": "grain.pan", "amount": 0.4}],
  "performance": {"reverse": true, "time_mode": "tempo", "subdiv": "2x",
                  "tempo_source": "midi", "tempo_us_per_quarter": 400000}
})";

}  // namespace

TEST_CASE("compile: the empty document is the default mode", "[compile]") {
  const CompileResult r = Ok(Minimal());
  // §5.1: the default ModeBlob's hash is the compiler's encoding of an empty document.
  const DecodedPackage p = DecodePackage(r.package.data(), r.package.size());
  REQUIRE(p.ok);
  REQUIRE(std::equal(std::begin(p.state->mode.modeHash.bytes),
                     std::end(p.state->mode.modeHash.bytes),
                     std::begin(brainscape::kDefaultModeHash.bytes)));
  REQUIRE(p.state->mode.features == 0u);
  REQUIRE(p.info.soundRev == brainscape::kSoundRevision);
  REQUIRE(p.info.schemaVersion == 1u);
  REQUIRE(p.info.flags == 0u);
  REQUIRE(p.info.meta.length > 0u);
  REQUIRE(p.hasJson);
  REQUIRE(p.json == r.json);
  // Every Leaf row of this build at its default, ascending (rows 27 and 28 from the structure).
  REQUIRE(p.state->leafCount == brainscape::kNumLeafParams);
  for (uint32_t i = 0; i < p.state->leafCount; ++i) {
    REQUIRE(p.state->leaves[i].id == static_cast<uint32_t>(brainscape::LeafId(i)));
    REQUIRE(Bits(p.state->leaves[i].value) == Bits(Row(p.state->leaves[i].id).def));
  }
  // CTRL: the six default macros at 0.5.
  REQUIRE(p.state->control.present == 1u);
  REQUIRE(p.state->control.macroCount == 6u);
  for (uint32_t k = 0; k < 6; ++k) REQUIRE(p.state->control.positions[k].position == 0.5f);
  // The header's FACTORY flag follows the id (§6.1).
  const CompileResult f = Ok(Minimal("factory.minimal"));
  REQUIRE(DecodePackage(f.package.data(), f.package.size()).info.flags ==
          brainscape::kPackageFlagFactory);
  // META and flags are outside sound_hash.
  REQUIRE(std::equal(std::begin(f.soundHash.bytes), std::end(f.soundHash.bytes),
                     std::begin(r.soundHash.bytes)));
  RoundTrip(Minimal());
}

TEST_CASE("compile: every leaf is addressed by its path; later waves' leaves at defaults only",
          "[compile]") {
  for (const ParamDescriptor& row : brainscape::kParamTable) {
    const auto id = static_cast<uint32_t>(row.id);
    if (row.kind != ParamKind::Leaf && row.kind != ParamKind::Reserved) continue;
    INFO(row.name);
    if (id >= static_cast<uint32_t>(ParamId::MacroActivity)) {
      // Later performance and device rows: never a document's leaf.
      REQUIRE(SchemaLeaf(row.name) == nullptr);
      REQUIRE_FALSE(Compile(With(LeafPath(row.name), Num("0"))).ok);
      continue;
    }
    REQUIRE(SchemaLeaf(row.name) == &row);
    const uint32_t    def   = Bits(row.def);
    const uint32_t    value = def != Bits(row.max) ? Bits(row.max) : Bits(row.min);
    const std::string text  = With(LeafPath(row.name), Num(NumberText(value).c_str()));
    if (row.kind == ParamKind::Leaf) {
      const CompileResult r = Ok(text);
      REQUIRE(r.doc.LeafBits(id) == value);
      REQUIRE(r.doc.Where("leaf:" + Dec(id))->pointer == LeafPointer(row.name));
      // The canonical form writes it where it was read.
      const json::Value  canonical = Parse(r.json);
      const json::Value* written   = Lookup(canonical, LeafPointer(row.name));
      REQUIRE(written != nullptr);
      REQUIRE(written->text == NumberText(value));
    } else {
      const std::vector<Finding> f     = Refused(text, "E6", LeafPointer(row.name).c_str());
      bool                       named = false;
      for (const Finding& x : f) named = named || x.message.find(LeafWave(id)) != std::string::npos;
      REQUIRE(named);
      // Its default is accepted, and not written: the row is not a Leaf of this build. (A
      // modulator's leaf makes a modulator, W3's; a layer-1 leaf a second layer, which with
      // voice_count's default of 64 per layer exceeds the voices, E11.)
      if (std::string(row.name).compare(0, 7, "layer1.") != 0) {
        const CompileResult r =
            Ok(With(LeafPath(row.name), Num(NumberText(def).c_str())), AllFeatures());
        REQUIRE(Lookup(Parse(r.json), LeafPointer(row.name)) == nullptr);
      }
    }
  }
}

TEST_CASE("compile: the schema's ElementPresent is the validator's", "[compile]") {
  for (const std::string& text : {Minimal(), std::string(kFullStructure)}) {
    Document             d;
    std::vector<Finding> f;
    REQUIRE(ReadDocumentText(text, AllFeatures().read, &d, &f));
    for (uint32_t id = 0; id <= brainscape::kNumParams + 2; ++id) {
      REQUIRE(ElementPresent(d.state->mode, id) ==
              brainscape::blob::ElementPresent(d.state->mode, id));
    }
  }
  brainscape::ModeBlob m;
  m.schedule.layerCount   = 2;
  m.layers[1]             = brainscape::ModeLayer{};
  m.layers[1].modifier[0] = brainscape::ModifierOp::Svf;
  m.modulators[0].type    = brainscape::ModulatorType::Lfo;
  m.steps.countMax        = 3;
  for (uint32_t id = 0; id <= brainscape::kNumParams + 2; ++id) {
    REQUIRE(ElementPresent(m, id) == brainscape::blob::ElementPresent(m, id));
  }
}

TEST_CASE("compile: the retired rows 27 and 28 are structure, which this build plays",
          "[compile]") {
  using namespace brainscape;
  // Sound revision 2 retired them (§4.2): no name, no leaf; schema 1 writes them as structure.
  REQUIRE(FindParam(ParamId::OnsetTrigger)->kind == ParamKind::Retired);
  REQUIRE(FindParam(ParamId::PositionSource)->kind == ParamKind::Retired);
  REQUIRE(SchemaLeaf("scheduler.onset_trigger") == nullptr);
  REQUIRE(SchemaLeaf("layer0.position.source") == nullptr);
  Refused(With("scheduler.onset_trigger", Num("1")), "E2", "/scheduler/onset_trigger");
  Refused(With("layers[0].position.source", Num("1")), "E3", "/layers/0/position/source");
  // The structure they became compiles here (kSupportedModeFeatures), and no STAT leaf names
  // them.
  struct Case {
    const char* path;
    const char* json;
    uint32_t    feature;
  };
  const Case cases[] = {
      {"scheduler.sources", R"(["periodic", "onset", "footswitch", "midi_note"])",
       kModeFeatureOnset},
      {"layers[0].position.source", R"("mark")", kModeFeatureMarkPosition},
  };
  for (const Case& c : cases) {
    INFO(c.path << " = " << c.json);
    const std::string    text = With(c.path, Parse(c.json));
    const CompileResult  r    = Ok(text);
    const DecodedPackage p    = DecodePackage(r.package.data(), r.package.size());
    REQUIRE(p.ok);
    REQUIRE(p.state->mode.features == c.feature);
    for (uint32_t i = 0; i < p.state->leafCount; ++i) {
      REQUIRE(p.state->leaves[i].id != static_cast<uint32_t>(ParamId::OnsetTrigger));
      REQUIRE(p.state->leaves[i].id != static_cast<uint32_t>(ParamId::PositionSource));
    }
    RoundTrip(text);
  }
}

TEST_CASE("compile: each later-wave feature is E6 here and compiles where supported", "[compile]") {
  using namespace brainscape;
  struct Case {
    const char* path;
    const char* json;
    uint32_t    feature;
    const char* wave;
  };
  const Case cases[] = {
      {"scheduler.sources", R"(["periodic", "clock", "footswitch", "midi_note"])",
       kModeFeatureClock, "W2"},
      {"scheduler.subdiv", R"("2x")", kModeFeatureClock, "W2"},
      {"scheduler.steps.entries", R"([{"slot": 1}])", kModeFeatureSteps, "W2"},
      {"scheduler.steps.order", R"("random")", kModeFeatureSteps, "W2"},
      {"layers[0].position.mark.walk", R"("cascade")", kModeFeatureMarkWalk, "W2"},
      {"layers[0].position.mark.index", "3", kModeFeatureMarkWalk, "W2"},
      {"layers[0].slot_share", "0.5", kModeFeatureTwoLayers, "W3"},
      {"layers[0].position.source", R"("pin")", kModeFeaturePinPosition, "W3"},
      {"layers[0].position.pin.rearm_ms", "300", kModeFeaturePinPosition, "W3"},
      {"layers[0].position.source", R"("grid")", kModeFeatureGridPosition, "W3"},
      {"layers[0].position.spray_law", R"("exp")", kModeFeatureExpSpray, "W3"},
      {"layers[0].pitch.quantize", R"({"mode": "scale", "scale": [0, 7]})", kModeFeatureQuantize,
       "W3"},
      {"layers[0].pitch.glide.st_end", "12", kModeFeatureGlide, "W3"},
      {"layers[0].modifiers", R"([{"op": "svf"}])", kModeFeatureSvf, "W3"},
      {"layers[0].modifiers", R"([{"op": "crush"}])", kModeFeatureCrush, "W3"},
      {"modulators", R"([{"type": "env"}])", kModeFeatureModulators, "W3"},
      {"links", R"([{"from": "grain.size", "to": "grain.gain", "amount": 0.5}])", kModeFeatureLinks,
       "W3"},
      {"dry_duck.release_ms", "100", kModeFeatureDryDuck, "W3"},
  };
  for (const Case& c : cases) {
    INFO(c.path << " = " << c.json);
    const std::string          text      = With(c.path, Parse(c.json));
    const std::vector<Finding> f         = Refused(text, "E6", nullptr);
    bool                       waveNamed = false;
    for (const Finding& x : f) waveNamed = waveNamed || x.message.find(c.wave) != std::string::npos;
    REQUIRE(waveNamed);
    const CompileResult  r = Ok(text, AllFeatures());
    const DecodedPackage p = DecodePackage(r.package.data(), r.package.size(), AllFeatures());
    REQUIRE(p.ok);
    REQUIRE(p.state->mode.features == c.feature);
    RoundTrip(text, AllFeatures());
  }
  // Routes need a modulator; the two features together.
  const std::string routes =
      With("routes", Parse(R"([{"from": "modulator0", "to": "size", "amount": 0.5}])"),
           With("modulators", Parse(R"([{}])")));
  REQUIRE(DecodePackage(Ok(routes, AllFeatures()).package.data(),
                        Ok(routes, AllFeatures()).package.size(), AllFeatures())
              .state->mode.features == (kModeFeatureModulators | kModeFeatureRoutes));
  // The whole vocabulary at once.
  const CompileResult full = Ok(kFullStructure, AllFeatures());
  RoundTrip(kFullStructure, AllFeatures());
  const DecodedPackage p = DecodePackage(full.package.data(), full.package.size(), AllFeatures());
  REQUIRE(p.state->mode.layers[0].scaleMask == 0x0AB5u);
  REQUIRE(p.state->mode.pitch[0].count == 3u);
  REQUIRE(p.state->mode.pitch[0].entries[1].weight == 1u);
  REQUIRE(p.state->mode.steps.entries[1].gain == 1.0f);
  REQUIRE(p.state->performance.usPerQuarter == 400000u);
  // Performance state, a W2 field, is E6 until CLOCK.
  Refused(With("performance.reverse", json::Value::Bool(true)), "E6", "/performance/reverse");
  // Tempo divisions are W2's vocabulary, so E6 even where features are widened.
  Refused(With("layers[0].position.base_sync", Str("1/4")), "E6", "/layers/0/position/base_sync",
          AllFeatures());
  Ok(With("layers[0].position.base_sync", Str("off")));
}

TEST_CASE("compile: wave 1 compiles here as each feature lands", "[compile]") {
  using namespace brainscape;
  // Source selection (sound revision 4, §7.5 R9): any subset of the sources this build plays,
  // the empty set included (lint L5 says it is silent until triggered).
  struct Case {
    const char* json;
    uint8_t     sources;
    uint32_t    feature;
  };
  const Case cases[] = {
      {R"(["onset"])", kSourceOnset, kModeFeatureOnset | kModeFeatureSources},
      {R"(["periodic", "footswitch"])", kSourcePeriodic | kSourceFootswitch, kModeFeatureSources},
      {R"(["midi_note", "periodic"])", kSourcePeriodic | kSourceMidiNote, kModeFeatureSources},
      {R"([])", 0, kModeFeatureSources},
  };
  for (const Case& c : cases) {
    INFO("scheduler.sources = " << c.json);
    const std::string    text = With("scheduler.sources", Parse(c.json));
    const CompileResult  r    = Ok(text);
    const DecodedPackage p    = DecodePackage(r.package.data(), r.package.size());
    REQUIRE(p.ok);
    REQUIRE(p.state->mode.schedule.sources == c.sources);
    REQUIRE(p.state->mode.features == c.feature);
    RoundTrip(text);
  }
  // The burst and intermittency leaves (57-59) take any value in range, and STAT holds them.
  const std::string leaves =
      With("scheduler.intermittency", Num("0.25"),
           With("scheduler.burst.count", Num("6"), With("scheduler.burst.spacing_ms", Num("12.5"))));
  const CompileResult  r = Ok(leaves);
  const DecodedPackage p = DecodePackage(r.package.data(), r.package.size());
  REQUIRE(p.ok);
  auto leaf = [&](ParamId id) {
    for (uint32_t i = 0; i < p.state->leafCount; ++i) {
      if (p.state->leaves[i].id == static_cast<uint32_t>(id)) return p.state->leaves[i].value;
    }
    return -1.0f;
  };
  REQUIRE(leaf(ParamId::Intermittency) == 0.25f);
  REQUIRE(leaf(ParamId::BurstCount) == 6.0f);
  REQUIRE(leaf(ParamId::BurstSpacingMs) == 12.5f);
  RoundTrip(leaves);
  Refused(With("scheduler.burst.count", Num("17")), "E4", "/scheduler/burst/count");
  Refused(With("scheduler.burst.spacing_ms", Num("501")), "E4", "/scheduler/burst/spacing_ms");

  // Pitch sets (sound revision 5, §7.5 R10): 1-8 entries of st -24..24 and weight 1-16 (default
  // 1), `cycle` or `random`, in the authored order; the default set alone compiles to no PSET.
  struct SetCase {
    const char* set;
    const char* select;
    uint8_t     count;
    float       st[3];
    uint16_t    weight[3];
    uint32_t    feature;
  };
  const SetCase sets[] = {
      {R"([{"st": 0}, {"st": 7, "weight": 3}])", "cycle", 2, {0.0f, 7.0f}, {1, 3},
       kModeFeaturePitchSet},
      {R"([{"st": 12, "weight": 2}, {"st": 0}, {"st": -12, "weight": 16}])", "random", 3,
       {12.0f, 0.0f, -12.0f}, {2, 1, 16}, kModeFeaturePitchSet},
      {R"([{"st": 0, "weight": 1}])", "random", 1, {0.0f}, {1}, kModeFeaturePitchSet},
      {R"([{"st": -24}])", "cycle", 1, {-24.0f}, {1}, kModeFeaturePitchSet},
      {R"([{"st": 0}])", "cycle", 1, {0.0f}, {1}, 0},
  };
  for (const SetCase& c : sets) {
    INFO("layers[0].pitch.set = " << c.set << ", select " << c.select);
    const std::string    text = With("layers[0].pitch.set", Parse(c.set),
                                     With("layers[0].pitch.select", Str(c.select)));
    const CompileResult  rs   = Ok(text);
    const DecodedPackage ps   = DecodePackage(rs.package.data(), rs.package.size());
    REQUIRE(ps.ok);
    const PitchSet& set = ps.state->mode.pitch[0];
    REQUIRE(set.count == c.count);
    for (uint32_t i = 0; i < c.count; ++i) {
      REQUIRE(set.entries[i].st == c.st[i]);
      REQUIRE(set.entries[i].weight == c.weight[i]);
    }
    REQUIRE((ps.state->mode.layers[0].pitchSelect == PitchSelect::Random) ==
            (std::string(c.select) == "random"));
    REQUIRE(ps.state->mode.features == c.feature);
    RoundTrip(text);
  }
  Refused(With("layers[0].pitch.set", Parse(R"([{"st": 24.5}])")), "E4", "/layers/0/pitch/set/0/st");
  Refused(With("layers[0].pitch.set", Parse(R"([{"st": 0, "weight": 17}])")), "E4",
          "/layers/0/pitch/set/0/weight");
  Refused(With("layers[0].pitch.set", Parse(R"([{"st": 0, "weight": 1.5}])")), "E3",
          "/layers/0/pitch/set/0/weight");
  Refused(With("layers[0].pitch.set",
               Parse(R"([{"st": 0}, {"st": 1}, {"st": 2}, {"st": 3}, {"st": 4}, {"st": 5},
                        {"st": 6}, {"st": 7}, {"st": 8}])")),
          "E7", "/layers/0/pitch/set");
}

TEST_CASE("compile: errors E1-E12 name the rule and the place", "[compile]") {
  // E1: strict JSON.
  Refused("{\"schema_version\": 1, \"id\": \"a\", \"name\": \"A\",}", "E1", nullptr);
  Refused("{\"schema_version\": 1, \"schema_version\": 1}", "E1", "/schema_version");
  // E2: unknown keys and newer schemas.
  Refused(With("layers[0].size_law", Str("exp")), "E2", "/layers/0/size_law");
  Refused(With("post.bypass", Parse("[\"delay\"]")), "E2", "/post/bypass");
  Refused(With("out_trim_db", Num("1")), "E2", "/out_trim_db");
  Refused(With("schema_version", Num("2")), "E2", "/schema_version");
  Refused(With("global.trigger_offset", Num("0")), "E2", "/global/trigger_offset");
  // E3: types, integers and required keys.
  Refused(With("global.mix", Str("0.5")), "E3", "/global/mix");
  Refused(With("layers[0].pitch.set", Parse(R"([{"st": 0, "weight": 1.5}])")), "E3",
          "/layers/0/pitch/set/0/weight");
  Refused(With("layers[0].pitch.set", Parse(R"([{"st": 0, "weight": 1e0}])")), "E3",
          "/layers/0/pitch/set/0/weight");
  Refused("{\"schema_version\": 1, \"name\": \"A\"}", "E3", "");
  Refused("[1]", "E3", "");
  Refused(With("macros", Parse(R"([{"id": "time", "targets": [{"range": [0, 1]}]}])")), "E3",
          "/macros/0/targets/0");
  Refused(With("sound_rev", Num("1")), "E3", "");
  // E4: ranges, overflow, strings.
  Refused(With("layers[0].size_ms", Num("501")), "E4", "/layers/0/size_ms");
  Refused(With("feedback.amount", Num("1.1000001")), "E4", "/feedback/amount");
  Refused(With("layers[0].position.base_ms", Num("4e38")), "E4", "/layers/0/position/base_ms");
  Refused(With("id", Str("Not.Lowercase")), "E4", "/id");
  Refused(With("name", Str(std::string(33, 'n'))), "E4", "/name");
  Refused(With("name", Str("tab\there")), "E4", "/name");
  Refused(With("meta.description", Str(std::string(513, 'd'))), "E4", "/meta/description");
  Refused(With("controls.macro_positions.time", Num("1.5")), "E4",
          "/controls/macro_positions/time");
  Refused(With("layers", Parse("[]")), "E4", "/layers");
  // E5: vocabularies and sets.
  Refused(With("family", Str("glitch")), "E5", "/family");
  Refused(With("scheduler.sources", Parse(R"(["periodic", "stochastic"])")), "E5",
          "/scheduler/sources/1");
  Refused(With("scheduler.sources", Parse(R"(["periodic", "periodic"])")), "E5",
          "/scheduler/sources/1");
  Refused(With("meta.tags", Parse(R"(["a", "a"])")), "E5", "/meta/tags/1");
  Refused(With("macros", Parse(R"([{"id": "volume"}])")), "E5", "/macros/0/id");
  Refused(With("layers[0].modifiers", Parse(R"([{"op": "svf"}, {"op": "svf"}])")), "E5",
          "/layers/0/modifiers/1/op", AllFeatures());
  // E6: §2.7's example message.
  const std::vector<Finding> clock = Refused(
      With("scheduler.sources", Parse(R"(["clock", "periodic", "footswitch", "midi_note"])")), "E6",
      "/scheduler/sources");
  REQUIRE(clock[0].message ==
          "`clock` needs W2 (CLOCK); this build supports periodic, onset, footswitch, midi_note");
  Refused(With("layers[0].voice_count", Num("8")), "E6", "/layers/0/voice_count");
  Refused(
      With(
          "macros",
          Parse(
              R"([{"id": "aux1", "targets": [{"param": "layer0.decay_ms", "range": [0, 1000]}]}])")),
      "E6", "/macros/0/targets/0/param");
  Refused(With("post.order", Parse(R"(["mod", "reverb", "delay", "filter"])")), "E6",
          "/post/order");
  Ok(With("post.order", Parse(R"(["mod", "delay", "reverb", "filter"])")));
  // E7: caps.
  Refused(With("layers", Parse("[{}, {}, {}]")), "E7", "/layers", AllFeatures());
  Refused(
      With(
          "layers[0].pitch.set",
          Parse(
              R"([{"st":0},{"st":1},{"st":2},{"st":3},{"st":4},{"st":5},{"st":6},{"st":7},{"st":8}])")),
      "E7", "/layers/0/pitch/set", AllFeatures());
  Refused(With("meta.tags", Parse(R"(["1","2","3","4","5","6","7","8","9"])")), "E7", "/meta/tags");
  Refused(
      With(
          "controls.expression",
          Parse(
              R"([{"target":"global.mix"},{"target":"global.mix"},{"target":"global.mix"},{"target":"global.mix"},{"target":"global.mix"}])")),
      "E7", "/controls/expression");
  {
    std::string many = "[";
    for (int i = 0; i < 9; ++i)
      many += std::string(i ? "," : "") + R"({"param": "layer0.size_ms", "range": [1, 2]})";
    Refused(With("macros", Parse(R"([{"id": "aux1", "targets": )" + many + "]}]")), "E7",
            "/macros/0/targets");
    // 32 targets in all, the defaults of omitted macros included (9 of them here).
    std::string eight    = "[";
    const char* leaves[] = {"scheduler.overlap",         "scheduler.jitter",
                            "layer0.pan_spread",         "layer0.window.skew",
                            "layer0.window.sustain",     "layer0.window.smoothness",
                            "layer0.pitch.reverse_prob", "post.delay.mix"};
    for (int i = 0; i < 8; ++i) {
      eight +=
          std::string(i ? "," : "") + "{\"param\": \"" + leaves[i] + "\", \"range\": [0.5, 1]}";
    }
    eight += "]";
    const std::string three = R"([{"id": "aux1", "targets": )" + eight +
                              R"(}, {"id": "aux2", "targets": )" + eight +
                              R"(}, {"id": "time", "targets": )" + eight + "}]";
    // 8 + 8 + 8 + the five other defaults' 8 targets = 32: allowed.
    Ok(With("macros", Parse(three)));
    const std::string four = R"([{"id": "aux1", "targets": )" + eight +
                             R"(}, {"id": "aux2", "targets": )" + eight +
                             R"(}, {"id": "time", "targets": )" + eight +
                             R"(}, {"id": "space", "targets": )" + eight + "}]";
    Refused(With("macros", Parse(four)), "E7", "/macros");
  }
  // E8: references.
  Refused(With("macros", Parse(R"([{"id": "time"}, {"id": "time"}])")), "E8", "/macros/1/id");
  Refused(With("macros",
               Parse(R"([{"id": "time", "targets": [{"param": "global.mix", "range": [0, 1]}]}])")),
          "E8", "/macros/0/targets/0/param");
  Refused(
      With("macros",
           Parse(R"([{"id": "time", "targets": [{"param": "layer0.nope", "range": [0, 1]}]}])")),
      "E8", "/macros/0/targets/0/param");
  Refused(
      With("macros",
           Parse(R"([{"id": "time", "targets": [{"param": "macro.space", "range": [0, 1]}]}])")),
      "E8", "/macros/0/targets/0/param");
  Refused(
      With(
          "macros",
          Parse(
              R"([{"id": "time", "targets": [{"param": "scheduler.onset_trigger", "range": [0, 1]}]}])")),
      "E8", "/macros/0/targets/0/param");
  Refused(
      With(
          "macros",
          Parse(
              R"([{"id": "time", "targets": [{"param": "layer0.size_ms", "range": [1, 2]}, {"param": "layer0.size_ms", "range": [3, 4]}]}])")),
      "E8", "/macros/0/targets/1/param");
  Refused(With("controls.macro_positions.aux1", Num("0.5")), "E8",
          "/controls/macro_positions/aux1");
  Refused(With("controls.expression", Parse(R"([{"target": "global.effect_volume_db"}])")), "E8",
          "/controls/expression/0/target");
  Refused(With("editor.detached", Parse(R"(["post.nope"])")), "E8", "/editor/detached/0");
  Refused(With("layers[0].position.mark.index", Num("16")), "E8", "/layers/0/position/mark/index",
          AllFeatures());
  Refused(With("scheduler.steps.entries", Parse(R"([{"ratio_idx": 1}])")), "E8", nullptr,
          AllFeatures());
  Refused(With("routes", Parse(R"([{"from": "modulator0", "to": "size", "amount": 0.5}])")), "E8",
          "/routes/0", AllFeatures());
  Refused(With("links", Parse(R"([{"from": "grain.pan", "to": "grain.pan", "amount": 0.5}])")),
          "E8", "/links/0", AllFeatures());
  // E9: macro targets' curve, in_range and range; expression ends.
  Refused(
      With(
          "macros",
          Parse(
              R"([{"id": "time", "targets": [{"param": "layer0.size_ms", "range": [1, 2], "curve": 0.05}]}])")),
      "E9", "/macros/0/targets/0/curve");
  Refused(
      With(
          "macros",
          Parse(
              R"([{"id": "time", "targets": [{"param": "layer0.size_ms", "range": [1, 2], "in_range": [0.5, 0.5]}]}])")),
      "E9", "/macros/0/targets/0/in_range");
  Refused(
      With("macros",
           Parse(R"([{"id": "time", "targets": [{"param": "layer0.size_ms", "range": [0, 2]}]}])")),
      "E9", "/macros/0/targets/0/range/0");
  Refused(With("controls.expression", Parse(R"([{"target": "global.mix", "hi": 2}])")), "E9",
          "/controls/expression/0/hi");
  Refused(With("controls.expression", Parse(R"([{"target": "macro.time", "curve": 17}])")), "E9",
          "/controls/expression/0/curve");
  Ok(With(
      "macros",
      Parse(
          R"([{"id": "time", "targets": [{"param": "layer0.size_ms", "range": [500, 1], "curve": 16, "in_range": [0, 0.25]}]}])")));
  // E10: a written shape macro does something.
  Refused(With("macros", Parse(R"([{"id": "shape", "targets": []}])")), "E10", "/macros/0");
  Ok(With("macros", Parse(R"([{"id": "activity", "targets": []}])")));
  // A macro written without `targets` keeps its default targets (§2.2's per-key default),
  // renamed here; aux1 has none.
  {
    const CompileResult r =
        Ok(With("macros", Parse(R"([{"id": "shape", "display_name": "Contour"}, {"id": "aux1"}])")));
    const brainscape::MacroTable& t = r.doc.state->mode.macros;
    REQUIRE(t.macroCount == 7u);
    REQUIRE(t.targetCount == 9u);
    REQUIRE(t.macros[2].count == 2u);
    REQUIRE(t.targets[t.macros[2].first].param == static_cast<uint32_t>(ParamId::WindowSustain));
    REQUIRE(t.macros[6].id == static_cast<uint32_t>(ParamId::MacroAux1));
    REQUIRE(t.macros[6].count == 0u);
    REQUIRE(r.doc.displayName[2] == "Contour");
  }
  // E11: two layers share the slots and the voices (voice_count is W1's, so its default 64 per
  // layer already sums past 64).
  Refused(With("layers", Parse(R"([{"slot_share": 0.5}, {"slot_share": 0.5}])")), "E11", "/layers",
          AllFeatures());
  Refused(With("layers", Parse(R"([{"slot_share": 0.75}, {"slot_share": 0.5}])")), "E11", "/layers",
          AllFeatures());
  // E12: the package within 16 KiB (editor data is carried in the JSON section).
  Refused(With("editor.ratio_gen", Parse("{\"note\": \"" + std::string(16000, 'x') + "\"}")), "E12",
          "");
  // ...and a document's text within 1 MiB, refused before it is parsed (whitespace counts).
  std::string big = Minimal();
  big.insert(1, kMaxDocumentBytes + 1u - big.size(), ' ');
  REQUIRE(big.size() == kMaxDocumentBytes + 1u);
  const std::vector<Finding> f = Refused(big, "E12", "");
  REQUIRE(f.size() == 1u);
  big.erase(1, 1);
  Ok(big);
}

TEST_CASE("compile: the canonical form (§6.4)", "[compile]") {
  const std::string messy = R"({"name": "Messy", "id": "test.messy", "schema_version": 1,
    "scheduler": {"jitter": 0.250, "sources": ["midi_note", "periodic", "footswitch"], "subdiv": "1/4"},
    "layers": [{"size_ms": 1.0e2, "slot_share": 1, "voice_count": 64,
                "pitch": {"transpose_st": -0, "select": "cycle"},
                "position": {"spray_ms": 7.0385307e-26, "base_ms": 1e-46}}],
    "macros": [{"id": "time", "targets": [{"param": "layer0.size_ms", "range": [2, 1]},
                                         {"param": "layer0.pan_spread", "range": [0, 1]}]},
               {"id": "activity", "display_name": "Smear", "targets": []}],
    "editor": {"detached": ["layer0.size_ms", "global.mix"], "ratio_gen": {"kind": "octaves", "n": 1.50}}})";
  // base_ms 1e-46 is below the smallest subnormal: +0, then the range [1, 5000] refuses it.
  Refused(messy, "E4", "/layers/0/position/base_ms");
  const std::string fixedText = [&] {
    std::string s = messy;
    s.replace(s.find("1e-46"), 5, "1e-40");  // a subnormal: +0, finding L1, then out of range
    return s;
  }();
  Refused(fixedText, "E4", "/layers/0/position/base_ms");
  std::string ok = messy;
  ok.replace(ok.find("1e-46"), 5, "300");
  const std::string fmt = Fmt(ok);
  REQUIRE(Fmt(fmt) == fmt);
  const json::Value v = Parse(fmt);
  // Key order, sets sorted, defaults and macros.
  REQUIRE(v.members[0].key == "schema_version");
  REQUIRE(v.members[1].key == "id");
  REQUIRE(v.members[2].key == "name");
  REQUIRE(v.members[3].key == "family");
  REQUIRE(v.members[4].key == "meta");
  const json::Value& sched = *v.Find("scheduler");
  REQUIRE(json::Serialize(*sched.Find("sources")) ==
          "[\"periodic\", \"footswitch\", \"midi_note\"]\n");
  REQUIRE(sched.Find("jitter")->text == "0.25");
  REQUIRE(sched.Find("subdiv") == nullptr);  // the default structure is not written
  const json::Value& layer = v.Find("layers")->items[0];
  REQUIRE(layer.Find("slot_share") == nullptr);
  REQUIRE(layer.Find("voice_count") == nullptr);  // a W1 leaf: not a Leaf row of this build
  REQUIRE(layer.Find("size_ms")->text == "100");
  REQUIRE(layer.Find("pitch")->Find("transpose_st")->text == "0");
  REQUIRE(layer.Find("pitch")->Find("select")->text == "cycle");  // core: the pitch set
  REQUIRE(layer.Find("position")->Find("spray_ms")->text == "7.0385307e-26");  // §6.4's exception
  REQUIRE(layer.Find("position")->Find("source")->text == "live");
  REQUIRE(layer.Find("window")->Find("sustain")->text == "0.3");  // every leaf, defaults too
  const json::Value& macros = *v.Find("macros");
  REQUIRE(macros.items.size() == 6u);  // the six performance macros, by id
  REQUIRE(macros.items[0].Find("id")->text == "activity");
  REQUIRE(macros.items[0].Find("display_name")->text == "Smear");
  REQUIRE(macros.items[0].Find("targets")->items.empty());
  REQUIRE(macros.items[3].Find("id")->text == "time");
  REQUIRE(macros.items[3].Find("targets")->items[0].Find("param")->text == "layer0.size_ms");
  REQUIRE(json::Serialize(*macros.items[3].Find("targets")->items[0].Find("range")) == "[2, 1]\n");
  REQUIRE(json::Serialize(*macros.items[3].Find("targets")->items[0].Find("in_range")) ==
          "[0, 1]\n");
  REQUIRE(macros.items[3].Find("targets")->items[0].Find("curve")->text == "1");
  REQUIRE(macros.items[3].Find("targets")->items[1].Find("param")->text == "layer0.pan_spread");
  REQUIRE(macros.items[4].Find("id")->text == "space");
  const json::Value& positions = *v.Find("controls")->Find("macro_positions");
  REQUIRE(positions.members.size() == 6u);
  REQUIRE(positions.members[3].key == "time");
  REQUIRE(positions.members[3].value.text == "0.5");
  const json::Value& editor = *v.Find("editor");
  REQUIRE(json::Serialize(*editor.Find("detached")) == "[\"global.mix\", \"layer0.size_ms\"]\n");
  REQUIRE(editor.Find("ratio_gen")->Find("n")->text == "1.5");  // editor numbers are canonical
  REQUIRE(v.members.back().key == "editor");
  // A subnormal leaf compiles to +0 with finding L1.
  Document             d;
  std::vector<Finding> f;
  REQUIRE(
      ReadDocumentText(With("layers[0].position.spray_ms", Num("1e-40")), ReadOptions{}, &d, &f));
  REQUIRE(f.empty());
  REQUIRE(d.notes.size() == 1u);
  REQUIRE(d.notes[0].code == "L1");
  REQUIRE(d.notes[0].at.pointer == "/layers/0/position/spray_ms");
  REQUIRE(d.LeafBits(static_cast<uint32_t>(ParamId::SprayMs)) == 0u);
  // The stamp: kept by fmt, computed by compile, ignored as input.
  const CompileResult r = Ok(ok);
  REQUIRE(Fmt(r.json) == r.json);
  const std::string stale =
      With("sound_hash", Str(std::string(64, 'a')), With("sound_rev", Num("7"), ok));
  REQUIRE(Fmt(stale).find("\"sound_rev\": 7,") != std::string::npos);
  REQUIRE(Ok(stale).package == r.package);
  Refused(With("sound_hash", Str("ABC"), With("sound_rev", Num("7"), ok)), "E4", "/sound_hash");
  RoundTrip(ok);
}

TEST_CASE("compile: editor.ratio_gen is validated and canonical (§2.2, §6.4)", "[compile]") {
  // Two spellings of the same editor data: one canonical text, one package.
  const std::string a =
      With("editor.ratio_gen", Parse(R"({"n": 1.50, "kind": "octaves", "x": 1E2, "z": -0,)"
                                     R"( "w": [2.50e0, {"b": 1, "a": -0.0E-0}]})"));
  const std::string b =
      With("editor.ratio_gen", Parse(R"({"kind": "octaves", "n": 1.5,)"
                                     R"( "w": [2.5, {"a": 0, "b": 1}], "x": 100, "z": 0})"));
  REQUIRE(Fmt(a) == Fmt(b));
  REQUIRE(Ok(a).package == Ok(b).package);
  const json::Value  v  = Parse(Fmt(a));
  const json::Value& rg = *v.Find("editor")->Find("ratio_gen");
  REQUIRE(rg.members.size() == 5u);  // members by key, numbers in canonical text
  REQUIRE(rg.members[0].key == "kind");
  REQUIRE(rg.members[1].key == "n");
  REQUIRE(rg.members[1].value.text == "1.5");
  REQUIRE(rg.members[2].key == "w");
  REQUIRE(rg.members[2].value.items[0].text == "2.5");  // arrays keep their order
  REQUIRE(rg.members[2].value.items[1].members[0].key == "a");
  REQUIRE(rg.members[2].value.items[1].members[0].value.text == "0");
  REQUIRE(rg.members[3].value.text == "100");
  REQUIRE(rg.members[4].value.text == "0");
  // Overflow is an error, as everywhere in the document; a subnormal is kept as 0, with L1.
  Refused(With("editor.ratio_gen", Parse(R"({"n": 1e999})")), "E4", "/editor/ratio_gen/n");
  Refused(With("editor.ratio_gen", Parse(R"({"a": {"b": [0, -1e39]}})")), "E4",
          "/editor/ratio_gen/a/b/1");
  Document             d;
  std::vector<Finding> f;
  REQUIRE(ReadDocumentText(With("editor.ratio_gen", Parse(R"({"tiny": 1e-40})")), ReadOptions{},
                           &d, &f));
  REQUIRE(d.notes.size() == 1u);
  REQUIRE(d.notes[0].code == "L1");
  REQUIRE(d.notes[0].at.pointer == "/editor/ratio_gen/tiny");
  REQUIRE(d.ratioGen.Find("tiny")->text == "0");
}

TEST_CASE("compile: the design's Engram example", "[compile]") {
  // §2.1, as written (the stamp left out: bspc stamp writes it).
  const std::string    engram = R"({
  "schema_version": 1,
  "id": "factory.engram",
  "name": "Engram",
  "family": "echoic",
  "global": { "mix": 0.35 },
  "scheduler": { "sources": ["periodic", "footswitch", "midi_note"],
                 "overlap": 0, "jitter": 0, "intermittency": 0,
                 "burst": { "count": 1, "spacing_ms": 0 } },
  "layers": [ {
    "voice_count": 64,
    "position": { "source": "live", "base_ms": 1, "spray_ms": 0, "repeat": 1 },
    "size_ms": 100, "decay_ms": 0,
    "window": { "sustain": 1, "skew": 0.5, "smoothness": 0 },
    "pitch": { "set": [ { "st": 0, "weight": 1 } ], "select": "cycle",
               "transpose_st": 0, "spread_cents": 0, "reverse_prob": 0 },
    "pan_spread": 0 } ],
  "feedback": { "amount": 0 },
  "post": { "mod": { "rate_hz": 0.6, "depth": 0.05 },
            "delay": { "time_ms": 405, "fb": 0.45, "mix": 1 },
            "reverb": { "time": 0.4, "mix": 0.12 } },
  "wet_trim_db": 0,
  "macros": [
    { "id": "activity", "display_name": "Smear", "targets": [
        { "param": "scheduler.overlap", "range": [0, 0.45], "curve": 2 },
        { "param": "layer0.position.spray_ms", "range": [0, 40], "curve": 2 },
        { "param": "scheduler.jitter", "range": [0, 0.5] } ] },
    { "id": "repeats", "targets": [ { "param": "post.delay.fb", "range": [0, 0.9] } ] },
    { "id": "shape", "display_name": "Contour", "targets": [
        { "param": "layer0.window.sustain", "range": [1, 0.25] },
        { "param": "layer0.window.smoothness", "range": [0, 1] } ] },
    { "id": "time", "targets": [
        { "param": "post.delay.time_ms", "range": [40, 1500], "curve": 2 } ] },
    { "id": "space", "targets": [ { "param": "post.reverb.mix", "range": [0, 0.5] } ] } ],
  "controls": { "macro_positions": { "activity": 0, "repeats": 0.5, "shape": 0,
                                     "time": 0.5, "space": 0.24, "filter": 1 } }
})";
  const CompileResult  r      = Ok(engram);
  const DecodedPackage p      = DecodePackage(r.package.data(), r.package.size());
  REQUIRE(p.info.flags == brainscape::kPackageFlagFactory);
  REQUIRE(p.meta.displayNameCount == 2u);
  REQUIRE(p.state->mode.macros.macroCount == 6u);  // filter by default
  REQUIRE(p.state->mode.macros.targetCount == 9u);
  RoundTrip(r.json);
}

TEST_CASE("compile: decompile, verify and diff", "[compile]") {
  const CompileResult a = Ok(With("layers[0].size_ms", Num("120")));
  const CompileResult b = Ok(With("layers[0].size_ms", Num("150")));
  REQUIRE(Diff(a.package.data(), a.package.size(), a.package.data(), a.package.size()).empty());
  REQUIRE(Diff(a.package.data(), a.package.size(), b.package.data(), b.package.size()) ==
          "/layers/0/size_ms: 120 -> 150");
  const CompileResult c = Ok(With("name", Str("Other")));
  const CompileResult d = Ok(Minimal());
  REQUIRE(Diff(d.package.data(), d.package.size(), c.package.data(), c.package.size()) ==
          "/name: \"Test\" -> \"Other\"");
  // A user copy of a factory preset (a new id and name, §9.1) with a leaf changed: diff shows
  // the leaf, not the FACTORY flag the id implies, nor the name.
  const std::string   factory = With("layers[0].size_ms", Num("120"), Minimal("factory.x"));
  const CompileResult fa      = Ok(factory);
  const CompileResult copy =
      Ok(With("layers[0].size_ms", Num("150"), With("name", Str("Copy"), Minimal("user.x"))));
  REQUIRE(Diff(fa.package.data(), fa.package.size(), copy.package.data(), copy.package.size()) ==
          "/layers/0/size_ms: 120 -> 150");
  // The same sound: the identity, before the flags.
  const CompileResult same = Ok(With("layers[0].size_ms", Num("120"), Minimal("user.x")));
  REQUIRE(Diff(fa.package.data(), fa.package.size(), same.package.data(), same.package.size()) ==
          "/id: \"factory.x\" -> \"user.x\"");
  // A package whose JSON section is another document's: verify finds it.
  brainscape::PackageContent content;
  content.json            = reinterpret_cast<const uint8_t*>(b.json.data());
  content.jsonLength      = static_cast<uint32_t>(b.json.size());
  const DecodedPackage pa = DecodePackage(a.package.data(), a.package.size());
  content.meta            = a.package.data() + pa.info.meta.offset;
  content.metaLength      = pa.info.meta.length;
  std::vector<uint8_t> mixed(brainscape::kMaxPackageBytes);
  const size_t n = brainscape::EncodePackage(*pa.state, content, mixed.data(), mixed.size());
  REQUIRE(n > 0u);
  mixed.resize(n);
  const std::vector<Finding>     v     = Verify(mixed.data(), mixed.size());
  const std::vector<std::string> codes = Codes(v);
  REQUIRE(std::find(codes.begin(), codes.end(), "V2") != codes.end());
  REQUIRE(HasErrors(v));
  // Decompile prints the JSON section; --rebuild ignores it.
  REQUIRE(Decompile(mixed.data(), mixed.size()).json == b.json);
  Document stamped  = a.doc;
  stamped.stamped   = true;
  stamped.soundRev  = brainscape::kSoundRevision;
  stamped.soundHash = a.soundHash;
  REQUIRE(Decompile(mixed.data(), mixed.size(), true).json == FormatDocument(stamped, false));
  // No JSON section: decompile rebuilds; a JSON_STALE package too.
  content.json       = nullptr;
  content.jsonLength = 0;
  mixed.assign(brainscape::kMaxPackageBytes, 0);
  mixed.resize(brainscape::EncodePackage(*pa.state, content, mixed.data(), mixed.size()));
  const DecompileResult rb = Decompile(mixed.data(), mixed.size());
  REQUIRE(rb.ok);
  REQUIRE(rb.rebuilt);
  REQUIRE(rb.json == FormatDocument(stamped, false));
  // A corrupt package.
  std::vector<uint8_t> bad = a.package;
  bad[200] ^= 1u;
  REQUIRE(HasErrors(Verify(bad.data(), bad.size())));
  REQUIRE_FALSE(Decompile(bad.data(), bad.size()).ok);
}

TEST_CASE("migrate: a BSWS v1 session becomes a preset document", "[compile]") {
  std::vector<uint8_t> s   = {'B', 'S', 'W', 'S', 1, 0, 0, 0};
  const auto           put = [&](uint32_t v) {
    for (int k = 0; k < 4; ++k) s.push_back(static_cast<uint8_t>(v >> (8 * k)));
  };
  put(4);
  put(5);  // size_ms
  put(Bits(123.0f));
  put(4);  // wet_trim_db
  put(Bits(-3.0f));
  put(27);  // the onset trigger, on
  put(Bits(1.0f));
  put(99);  // not a leaf
  put(Bits(1.0f));
  put(0);  // no settings
  Document             d;
  std::vector<Finding> f;
  REQUIRE(MigrateSession(s.data(), s.size(), "user.migrated", "Migrated", &d, &f));
  REQUIRE(f.size() == 3u);  // id 99, the trim, onset
  REQUIRE(d.LeafBits(static_cast<uint32_t>(ParamId::GrainSizeMs)) == Bits(123.0f));
  REQUIRE((d.state->mode.schedule.sources & brainscape::kSourceOnset) != 0u);
  const std::string text = FormatDocument(d);
  REQUIRE(text.find("\"sources\": [\"periodic\", \"onset\", \"footswitch\", \"midi_note\"]") !=
          std::string::npos);
  // Onset is sound revision 2's structure, which this build plays.
  REQUIRE(d.state->mode.features == brainscape::kModeFeatureOnset);
  Ok(text);
  s[4] = 2;
  REQUIRE_FALSE(MigrateSession(s.data(), s.size(), "x", "X", &d, &f));
}

TEST_CASE("migrate: rows 27 and 28 become the onset source and mark positioning", "[compile]") {
  // A session holding only the retired rows, by bit pattern. Revision 1 canonicalized them to
  // [0, 1] (NaN and infinities to 0) and read them at 0.5, so on is a finite value of 0.5 or more.
  struct Case {
    uint32_t onset, mark;
    bool     onsetOn, markOn;
  };
  const Case cases[] = {
      {Bits(0.0f), Bits(1.0f), false, true},      // 28 alone: mark positioning
      {Bits(0.5f), 0x3EFFFFFFu, true, false},     // the threshold: 0.5 on, the float below off
      {0x7F800000u, Bits(2.0f), false, true},     // +inf canonicalized to 0, 2 to 1
      {0xFF800000u, 0x7FC00000u, false, false},   // -inf, NaN
      {Bits(-1.0f), 0x00000001u, false, false},   // negative, subnormal
      {Bits(-0.0f), Bits(0.75f), false, true},
      {Bits(1.0f), Bits(1.0f), true, true},
  };
  for (const Case& c : cases) {
    INFO("27 = 0x" << std::hex << c.onset << ", 28 = 0x" << c.mark);
    std::vector<uint8_t> s   = {'B', 'S', 'W', 'S', 1, 0, 0, 0};
    const auto           put = [&](uint32_t v) {
      for (int k = 0; k < 4; ++k) s.push_back(static_cast<uint8_t>(v >> (8 * k)));
    };
    put(2);
    put(27);
    put(c.onset);
    put(28);
    put(c.mark);
    put(0);  // no settings
    Document             d;
    std::vector<Finding> f;
    REQUIRE(MigrateSession(s.data(), s.size(), "user.migrated", "Migrated", &d, &f));
    const brainscape::ModeBlob& m = d.state->mode;
    REQUIRE(((m.schedule.sources & brainscape::kSourceOnset) != 0u) == c.onsetOn);
    REQUIRE((m.layers[0].source == brainscape::PositionSource::Mark) == c.markOn);
    REQUIRE(m.features == ((c.onsetOn ? brainscape::kModeFeatureOnset : 0u) |
                           (c.markOn ? brainscape::kModeFeatureMarkPosition : 0u)));
    REQUIRE(f.size() == (c.onsetOn || c.markOn ? 1u : 0u));  // the note naming what moved
    const std::string text = FormatDocument(d);
    REQUIRE((text.find("\"source\": \"mark\"") != std::string::npos) == c.markOn);
    // It compiles; the package decodes and loads exact, with no leaf for 27 or 28.
    const CompileResult  r = Ok(text);
    const DecodedPackage p = DecodePackage(r.package.data(), r.package.size());
    REQUIRE(p.ok);
    REQUIRE(p.state->mode.features == m.features);
    REQUIRE(p.state->mode.layers[0].source == m.layers[0].source);
    REQUIRE(p.state->mode.schedule.sources == m.schedule.sources);
    REQUIRE(p.state->leafCount == brainscape::kNumLeafParams);
    for (uint32_t i = 0; i < p.state->leafCount; ++i) {
      REQUIRE(p.state->leaves[i].id != static_cast<uint32_t>(ParamId::OnsetTrigger));
      REQUIRE(p.state->leaves[i].id != static_cast<uint32_t>(ParamId::PositionSource));
    }
    REQUIRE(brainscape::CheckPreset(*p.state));
  }
}
