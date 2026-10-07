// Lint L1-L9 and derive (docs/design/mode-compiler.md §2.7, §3.5): each finding raised and not
// raised, --factory's errors, derived leaves and "solve position".
#include <algorithm>
#include <string>
#include <vector>

#include "Compile.h"
#include "Lint.h"
#include "Text.h"
#include "TestUtil.h"
#include "brainscape/ModeEval.h"
#include "catch.hpp"

using namespace bsc;
using namespace bsctest;
using brainscape::ParamId;

namespace {

Document Read(const std::string& text, const ReadOptions& o = {}) {
  Document             d;
  std::vector<Finding> f;
  INFO(All(f));
  REQUIRE(ReadDocumentText(text, o, &d, &f));
  return d;
}

std::string With(const std::string& path, json::Value value, const std::string& base) {
  json::Value root = Parse(base);
  Set(root, path, std::move(value));
  return json::Serialize(root);
}

size_t Count(const std::vector<Finding>& f, const char* code, bool error = false) {
  return static_cast<size_t>(std::count_if(
      f.begin(), f.end(), [&](const Finding& x) { return x.code == code && (!error || x.error); }));
}

// A document whose targeted leaves are derived and positions written: lint-clean.
std::string Clean(const std::string& extra = std::string()) {
  Document d = Read(Minimal("test.clean"));
  if (!extra.empty()) d = Read(extra);
  Derive(&d, false, nullptr);
  d.omittedPositions.clear();
  return FormatDocument(d);
}

}  // namespace

TEST_CASE("lint: a derived document is clean, also for --factory", "[lint]") {
  const Document d = Read(Clean());
  REQUIRE(Lint(d).empty());
  LintOptions factory;
  factory.factory = true;
  REQUIRE(Lint(d, factory).empty());
}

TEST_CASE("lint: L1 subnormals, L3 empty macros, L5 silence", "[lint]") {
  const std::string base = Clean();
  Document          d    = Read(With("layers[0].pitch.spread_cents", Num("1e-45"), base));
  REQUIRE(Count(Lint(d), "L1") == 1u);
  d = Read(With("macros",
                Parse(R"([{"id": "repeats", "targets": []}, {"id": "time", "targets": []}])"),
                base));
  REQUIRE(Count(Lint(d), "L3") == 2u);
  REQUIRE(Count(Lint(Read(base)), "L3") == 0u);
  // L5 needs source selection (W1): no periodic, clock or onset.
  d = Read(With("scheduler.sources", Parse(R"(["footswitch", "midi_note"])"), base),
           AllFeatures().read);
  REQUIRE(Count(Lint(d), "L5") == 1u);
  d = Read(With("scheduler.sources", Parse(R"(["onset"])"), base), AllFeatures().read);
  REQUIRE(Count(Lint(d), "L5") == 0u);
}

TEST_CASE("lint: L2 the near guard for pitched grains", "[lint]") {
  // Layer 0 at +12 st: r = 2, so grains of 100 ms need base_ms >= 100.
  std::string t = With("layers[0].pitch.transpose_st", Num("12"),
                       With("layers[0].size_ms", Num("100"), Clean()));
  t             = With(
                  "macros",
                  Parse(
                      R"([{"id": "time", "targets": [{"param": "post.delay.time_ms", "range": [100, 200]}]}])"),
                  t);
  REQUIRE(Count(Lint(Read(With("layers[0].position.base_ms", Num("99"), t))), "L2") == 1u);
  REQUIRE(Count(Lint(Read(With("layers[0].position.base_ms", Num("100"), t))), "L2") == 0u);
  // A macro that reaches below it: the smallest base_ms its leaf and macros reach.
  const std::string m = With(
      "macros",
      Parse(
          R"([{"id": "time", "targets": [{"param": "layer0.position.base_ms", "range": [150, 50]}]}])"),
      With("layers[0].position.base_ms", Num("150"), t));
  REQUIRE(Count(Lint(Read(m)), "L2") == 1u);
  // A macro on size or transpose reaches higher.
  const std::string s = With(
      "macros",
      Parse(R"([{"id": "time", "targets": [{"param": "post.delay.time_ms", "range": [100, 200]}]},
                                       {"id": "shape", "targets": [{"param": "layer0.size_ms", "range": [100, 300]}]}])"),
      With("layers[0].position.base_ms", Num("150"), t));
  REQUIRE(Count(Lint(Read(s)), "L2") == 1u);
  // Pitch sets (W1): each entry, with the transpose.
  const std::string set = With("layers[0].pitch.set", Parse(R"([{"st": 0}, {"st": 12}])"),
                               With("layers[0].pitch.transpose_st", Num("0"),
                                    With("layers[0].position.base_ms", Num("99"), t)));
  REQUIRE(Count(Lint(Read(set, AllFeatures().read)), "L2") == 1u);
  // At or below unison there is no near guard to hit.
  REQUIRE(Count(Lint(Read(With("layers[0].pitch.transpose_st", Num("-5"),
                               With("layers[0].position.base_ms", Num("1"), t)))),
                "L2") == 0u);
}

TEST_CASE("lint: L4 leaves against positions, and derive", "[lint]") {
  // Minimal: no position written, leaves at their defaults.
  const Document             m = Read(Minimal());
  const std::vector<Finding> f = Lint(m);
  REQUIRE(Count(f, "L4") == 6u + 7u);  // six omitted positions; seven leaves not derived
  LintOptions factory;
  factory.factory = true;
  REQUIRE(Count(Lint(m, factory), "L4", true) == 13u);
  // Within the display resolution: same text on the display.
  std::string t = Clean();
  t             = With(
                  "controls.macro_positions.time", Num("0.479"),
                  With(
                      "macros",
                      Parse(
                          R"([{"id": "time", "targets": [{"param": "post.delay.time_ms", "range": [40, 1500], "curve": 2}]}])"),
                      t));
  t = With("post.delay.time_ms", Num("375"), t);  // 0.479 gives 374.983856 ms
  REQUIRE(Count(Lint(Read(t)), "L4") == 0u);
  t = With("post.delay.time_ms", Num("377"), t);
  REQUIRE(Count(Lint(Read(t)), "L4") == 1u);
  // editor.detached exempts a leaf.
  REQUIRE(Count(Lint(Read(With("editor.detached", Parse(R"(["post.delay.time_ms"])"), t))), "L4") ==
          0u);
  // derive rewrites it as EvalMacro at the position, exactly.
  Document                 d = Read(t);
  std::vector<std::string> log;
  Derive(&d, false, &log);
  REQUIRE(log.size() == 1u);
  brainscape::PresetLeaf out[8];
  REQUIRE(brainscape::EvalMacro(d.state->mode, ParamId::MacroTime, 0.479f, out, 8) == 1u);
  REQUIRE(d.LeafBits(static_cast<uint32_t>(ParamId::DelayTimeMs)) == Bits(out[0].value));
  REQUIRE(Bits(out[0].value) == 0x43BB7DEFu);  // record §2.6: 374.983856 ms
  REQUIRE(Count(Lint(d), "L4") == 0u);
  // ...but not a detached leaf.
  d = Read(With("editor.detached", Parse(R"(["post.delay.time_ms"])"), t));
  Derive(&d, false, nullptr);
  REQUIRE(d.LeafBits(static_cast<uint32_t>(ParamId::DelayTimeMs)) == Bits(377.0f));
}

TEST_CASE("derive --solve: positions from leaves", "[lint]") {
  // §2.1's values: 405 ms on [40, 1500]^2 is 0.5; 0.45 on [0, 0.9] is 0.5; 0.12 on [0, 0.5] is
  // 0.24.
  std::string t = Clean();
  t             = With("macros", Parse(R"([
      {"id": "repeats", "targets": [{"param": "post.delay.fb", "range": [0, 0.9]}]},
      {"id": "time", "targets": [{"param": "post.delay.time_ms", "range": [40, 1500], "curve": 2}]},
      {"id": "space", "targets": [{"param": "post.reverb.mix", "range": [0, 0.5]}]},
      {"id": "shape", "targets": [{"param": "layer0.window.sustain", "range": [1, 0.25]}]}])"),
                       t);
  t             = With("post.delay.time_ms", Num("405"), t);
  t             = With("post.delay.fb", Num("0.45"), t);
  t             = With("post.reverb.mix", Num("0.12"), t);
  t             = With("layers[0].window.sustain", Num("0.625"), t);  // reversed: 0.5
  Document d    = Read(t);
  Derive(&d, true, nullptr);
  const brainscape::ControlState& c        = d.state->control;
  const auto                      position = [&](ParamId id) {
    for (uint32_t k = 0; k < c.macroCount; ++k) {
      if (c.positions[k].macroId == static_cast<uint32_t>(id)) return Bits(c.positions[k].position);
    }
    return 0xFFFFFFFFu;
  };
  // Each solved position lands exactly on its leaf (the smallest such position): Time at 0.5.
  REQUIRE(position(ParamId::MacroTime) == Bits(0.5f));
  for (const ParamId macro :
       {ParamId::MacroTime, ParamId::MacroRepeats, ParamId::MacroSpace, ParamId::MacroShape}) {
    brainscape::PresetLeaf o[8];
    REQUIRE(brainscape::EvalMacro(d.state->mode, macro, FromBits(position(macro)), o, 8) == 1u);
    REQUIRE(d.LeafBits(o[0].id) == Bits(o[0].value));
    REQUIRE(position(macro) <= Bits(0.5f));
  }
  // The leaves then equal the macros' values bit for bit, as written.
  REQUIRE(d.LeafBits(static_cast<uint32_t>(ParamId::DelayTimeMs)) == Bits(405.0f));
  REQUIRE(d.LeafBits(static_cast<uint32_t>(ParamId::DelayFb)) == Bits(0.45f));
  REQUIRE(d.LeafBits(static_cast<uint32_t>(ParamId::ReverbMix)) == Bits(0.12f));
  REQUIRE(d.LeafBits(static_cast<uint32_t>(ParamId::WindowSustain)) == Bits(0.625f));
  REQUIRE(Count(Lint(d), "L4") == 0u);
  // Beyond a range: the nearer end.
  const brainscape::ModeBlob& mode = d.state->mode;
  REQUIRE(SolvePosition(mode, static_cast<uint32_t>(ParamId::MacroTime), 0, Bits(2000.0f)) ==
          Bits(1.0f));
  REQUIRE(SolvePosition(mode, static_cast<uint32_t>(ParamId::MacroTime), 0, Bits(10.0f)) == 0u);
  // Every solved position is canonical and lands nearest: no neighbour does better.
  for (const float leaf : {41.0f, 77.7f, 404.99f, 1234.5f, 1499.99f}) {
    const uint32_t p =
        SolvePosition(mode, static_cast<uint32_t>(ParamId::MacroTime), 0, Bits(leaf));
    REQUIRE((p == 0u || (p >= 0x00800000u && p <= 0x3F800000u)));
    brainscape::PresetLeaf o[8];
    const auto             value = [&](uint32_t bits) {
      brainscape::EvalMacro(mode, ParamId::MacroTime, FromBits(bits), o, 8);
      const double v = static_cast<double>(o[0].value) - static_cast<double>(leaf);
      return v < 0 ? -v : v;
    };
    REQUIRE(value(p) <= value(p + 1u));
    if (p > 0x00800000u) REQUIRE(value(p) <= value(p - 1u));
  }
}

TEST_CASE("lint: L6, L7, L8", "[lint]") {
  const std::string base = Clean();
  // L6: mark positioning with decay_ms 0 (mark is sound revision 2's; decay_ms W1's).
  Document d = Read(With("layers[0].position.source", Str("mark"), base), AllFeatures().read);
  REQUIRE(Count(Lint(d), "L6") == 1u);
  REQUIRE(Count(Lint(Read(base)), "L6") == 0u);
  // L7: a Shift secondary as a target.
  d = Read(
      With("macros",
           Parse(R"([{"id": "aux1", "targets": [{"param": "post.filter.res", "range": [0, 1]}]},
                                     {"id": "repeats", "targets": [{"param": "post.mod.depth", "range": [0, 1]}]}])"),
           base));
  LintOptions factory;
  factory.factory = true;
  REQUIRE(Count(Lint(d), "L7") == 2u);
  REQUIRE(Count(Lint(d), "L7", true) == 0u);
  REQUIRE(Count(Lint(d, factory), "L7", true) == 2u);
  // L8: Filter from the kill to bypass; Space adds no wet at 0.
  d = Read(With(
      "macros",
      Parse(
          R"([{"id": "filter", "targets": [{"param": "post.filter.cutoff_hz", "range": [100, 20000]}]}])"),
      base));
  REQUIRE(Count(Lint(d, factory), "L8", true) == 1u);
  d = Read(With(
      "macros",
      Parse(
          R"([{"id": "filter", "targets": [{"param": "post.filter.cutoff_hz", "range": [20000, 40]}]}])"),
      base));
  REQUIRE(Count(Lint(d), "L8") == 1u);
  d = Read(With(
      "macros",
      Parse(
          R"([{"id": "filter", "targets": [{"param": "post.filter.cutoff_hz", "range": [40, 20000], "curve": 3}, {"param": "post.filter.morph", "range": [0, 1]}]}])"),
      base));
  REQUIRE(Count(Lint(d), "L8") == 0u);
  d = Read(With("macros", Parse(R"([{"id": "filter", "targets": []}])"), base));
  REQUIRE(Count(Lint(d), "L8") == 1u);
  d = Read(With(
      "macros",
      Parse(R"([{"id": "space", "targets": [{"param": "post.reverb.mix", "range": [0.1, 0.5]}]}])"),
      base));
  REQUIRE(Count(Lint(d), "L8") == 1u);
  d = Read(With(
      "macros",
      Parse(
          R"([{"id": "space", "targets": [{"param": "post.reverb.mix", "range": [0, 0.5]}, {"param": "post.reverb.time", "range": [0.2, 0.9]}]}])"),
      base));
  REQUIRE(Count(Lint(d), "L8") == 0u);
  REQUIRE(Count(Lint(d), "L7") == 1u);  // reverb.time is Space's Shift secondary
}

TEST_CASE("lint: L9 other makers' marks in product strings", "[lint]") {
  const std::string base = Clean();
  LintOptions       factory;
  factory.factory = true;
  REQUIRE(Count(Lint(Read(With("name", Str("Haze Machine"), base)), factory), "L9", true) == 1u);
  REQUIRE(Count(Lint(Read(With("name", Str("Hazel"), base)), factory), "L9") == 0u);  // whole words
  REQUIRE(Count(Lint(Read(With("meta.description", Str("A DREAM-SEQUENCE of echoes"), base))),
                "L9") == 1u);
  REQUIRE(Count(Lint(Read(With("meta.tags", Parse(R"(["ambient", "Chroma  Console"])"), base))),
                "L9") == 1u);
  REQUIRE(Count(Lint(Read(With("id", Str("factory.blooper-like"), base))), "L9") == 1u);
  REQUIRE(
      Count(Lint(Read(With("macros",
                           Parse(R"([{"id": "activity", "display_name": "Warp", "targets": []}])"),
                           base))),
            "L9") == 1u);
  REQUIRE(Count(Lint(Read(With("meta.author", Str("Microcosmos"), base))), "L9") == 0u);
  REQUIRE(Count(Lint(Read(With("name", Str("Engram"), base))), "L9") == 0u);
  // The list is record §2.2's, Hologram's marks first.
  REQUIRE(std::find(Denylist().begin(), Denylist().end(), "fathom") != Denylist().end());
  REQUIRE(std::find(Denylist().begin(), Denylist().end(), "dream sequence") != Denylist().end());
}
