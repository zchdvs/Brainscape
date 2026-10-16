// Lint L1-L9, L12-L14 and derive (docs/design/mode-compiler.md §2.7, §3.5; docs/design/clock.md
// §11.1): each finding raised and not raised, --factory's errors, derived leaves and "solve
// position".
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
  // Triggered sources alone are a warning, also for --factory; no source at all never plays a
  // grain, which --factory makes an error.
  LintOptions factory;
  factory.factory = true;
  const auto l5 = [](const std::vector<Finding>& f) {
    const auto it =
        std::find_if(f.begin(), f.end(), [](const Finding& x) { return x.code == "L5"; });
    REQUIRE(it != f.end());
    return *it;
  };
  d = Read(With("scheduler.sources", Parse(R"(["footswitch"])"), base));
  REQUIRE(Count(Lint(d), "L5") == 1u);
  REQUIRE(Count(Lint(d, factory), "L5", true) == 0u);
  REQUIRE(l5(Lint(d)).message.find("silent until triggered") != std::string::npos);
  d = Read(With("scheduler.sources", Parse("[]"), base));
  REQUIRE(Count(Lint(d), "L5") == 1u);
  REQUIRE(Count(Lint(d), "L5", true) == 0u);
  REQUIRE(l5(Lint(d)).message.find("never plays a grain") != std::string::npos);
  REQUIRE(l5(Lint(d)).at.pointer == "/scheduler/sources");
  REQUIRE(Count(Lint(d, factory), "L5", true) == 1u);
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

TEST_CASE("lint: L4 and derive name leaves as editor.detached takes them", "[lint]") {
  // Minimal's leaves are not derived: L4 names each as schema 1 does (layer0.position.spray_ms,
  // not layers[0].position.spray_ms), so the name copied into editor.detached reads and clears
  // the finding.
  const std::string        base = Minimal();
  std::vector<std::string> names;
  for (const Finding& f : Lint(Read(base))) {
    if (f.code != "L4" || f.at.pointer == "/controls/macro_positions") continue;
    const std::string name = f.message.substr(0, f.message.find(" is "));
    INFO(f.message);
    REQUIRE(name.find('[') == std::string::npos);
    REQUIRE(SchemaLeaf(name) != nullptr);
    names.push_back(name);
  }
  REQUIRE(names.size() == 7u);
  REQUIRE(std::find(names.begin(), names.end(), "layer0.position.spray_ms") != names.end());
  json::Value detached = json::Value::Array();
  for (const std::string& n : names) detached.Push(Str(n));
  REQUIRE(Count(Lint(Read(With("editor.detached", std::move(detached), base))), "L4") == 6u);
  // derive's log names them the same way.
  Document                 d = Read(base);
  std::vector<std::string> log;
  Derive(&d, false, &log);
  REQUIRE(log.size() >= 7u);
  for (const std::string& line : log) {
    INFO(line);
    REQUIRE(SchemaLeaf(line.substr(0, line.find(':'))) != nullptr);
  }
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
    // ...and is the first position that gives its value.
    if (p > 0x00800000u) REQUIRE(value(p - 1u) != value(p));
  }
}

TEST_CASE("derive --solve: a derived document stays as it is", "[lint]") {
  // A derived document's positions already give its leaves: solving keeps every one, byte for
  // byte, though EvalMacro is flat over runs of positions (0.49999997 gives what 0.5 does).
  const std::string        clean = Clean();
  Document                 d     = Read(clean);
  std::vector<std::string> log;
  Derive(&d, true, &log);
  INFO(log.size());
  REQUIRE(log.empty());
  REQUIRE(FormatDocument(d) == clean);
  // Shape on a falling and a rising target (review: spray_ms moved by 0.00006 ms); spray_ms is
  // also activity's, whose value shape's overwrites: no net change, nothing logged.
  std::string t = With("macros", Parse(R"([{"id": "shape", "targets": [
      {"param": "layer0.window.sustain", "range": [0.9, 0.1]},
      {"param": "layer0.position.spray_ms", "range": [0, 2000]}]}])"),
                       clean);
  t             = With("controls.macro_positions.shape", Num("0.5"), t);
  d             = Read(t);
  Derive(&d, false, nullptr);
  const std::string derived = FormatDocument(d);
  d                         = Read(derived);
  Derive(&d, true, &log);
  for (const std::string& line : log) UNSCOPED_INFO(line);
  REQUIRE(log.empty());
  REQUIRE(FormatDocument(d) == derived);
  // The stored position stays only while it lands as near as any: a leaf moved off it solves.
  d = Read(With("layers[0].window.sustain", Num("0.7"), derived));
  Derive(&d, true, &log);
  REQUIRE(log.size() >= 2u);  // the position, then the leaves it gives
  REQUIRE(log[0].rfind("controls.macro_positions.shape: 0.5 -> ", 0) == 0u);
}

TEST_CASE("derive --solve: past the end of a range, the first position that reaches it",
          "[lint]") {
  // in_range [0, 0.5]: the value reaches the top of the range at 0.5 and stays there; a leaf
  // beyond it solves to the first position that gives the top, not to 1.
  const Document d = Read(With(
      "macros",
      Parse(
          R"([{"id": "time", "targets": [{"param": "post.delay.time_ms", "range": [40, 1500], "in_range": [0, 0.5]}]}])"),
      Clean()));
  const brainscape::ModeBlob& mode  = d.state->mode;
  const auto                  value = [&](uint32_t bits) {
    brainscape::PresetLeaf o[8];
    brainscape::EvalMacro(mode, ParamId::MacroTime, FromBits(bits), o, 8);
    return Bits(o[0].value);
  };
  const uint32_t time = static_cast<uint32_t>(ParamId::MacroTime);
  for (const float leaf : {1500.0f, 2000.0f, 5000.0f}) {
    const uint32_t p = SolvePosition(mode, time, 0, Bits(leaf));
    REQUIRE(value(p) == Bits(1500.0f));
    REQUIRE(value(p - 1u) != Bits(1500.0f));
    REQUIRE(p <= Bits(0.5f));
  }
  // Below the bottom: 0. A stored position in the top run stays.
  REQUIRE(SolvePosition(mode, time, 0, Bits(1.0f)) == 0u);
  REQUIRE(SolvePosition(mode, time, 0, Bits(2000.0f), Bits(0.75f)) == Bits(0.75f));
  REQUIRE(SolvePosition(mode, time, 0, Bits(2000.0f), Bits(1.0f)) == Bits(1.0f));
  REQUIRE(SolvePosition(mode, time, 0, Bits(40.0f), Bits(0.25f)) != Bits(0.25f));
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

TEST_CASE("lint: L2 for a synced base delay at its shortest (clock.md §11.1)", "[lint]") {
  // Layer 0 at +12 st with 100 ms grains needs base_ms >= 100. A synced 1/32 is 25 ms at 300 BPM
  // and TAP, 3.13 ms under ×8: below it whatever base_ms says; a 1/1 is 100 ms at 300 BPM and
  // ×8, not below, and a 1/2, 50 ms, is.
  // The time macro on the post delay, so no macro moves base_ms (as the L2 case above).
  const std::string t = With(
      "macros",
      Parse(R"([{"id": "time", "targets": [{"param": "post.delay.time_ms", "range": [100, 200]}]}])"),
      With("layers[0].pitch.transpose_st", Num("12"),
           With("layers[0].size_ms", Num("100"),
                With("layers[0].position.base_ms", Num("500"), Clean()))));
  REQUIRE(Count(Lint(Read(t)), "L2") == 0u);
  const std::vector<Finding> f = Lint(Read(With("layers[0].position.base_sync", Str("1/32"), t)));
  REQUIRE(Count(f, "L2") == 1u);
  const auto l2 = std::find_if(f.begin(), f.end(), [](const Finding& x) { return x.code == "L2"; });
  REQUIRE(l2->message.find("synced base delay at its shortest") != std::string::npos);
  REQUIRE(l2->at.pointer == "/layers/0/position/base_sync");
  REQUIRE(Count(Lint(Read(With("layers[0].position.base_sync", Str("1/1"), t))), "L2") == 0u);
  REQUIRE(Count(Lint(Read(With("layers[0].position.base_sync", Str("1/2"), t))), "L2") == 1u);
}

TEST_CASE("lint: L12 grain feedback under a synced base delay (clock.md §6.2, D6)", "[lint]") {
  // No macro on feedback.amount (the default table's Repeats has one).
  const std::string base = With(
      "macros", Parse(R"([{"id": "repeats", "targets": []}])"),
      With("feedback.amount", Num("0"), With("layers[0].position.base_sync", Str("1/4"), Clean())));
  REQUIRE(Count(Lint(Read(base)), "L12") == 0u);  // feedback.amount 0
  const std::vector<Finding> f = Lint(Read(With("feedback.amount", Num("0.3"), base)));
  REQUIRE(Count(f, "L12") == 1u);
  const auto l12 =
      std::find_if(f.begin(), f.end(), [](const Finding& x) { return x.code == "L12"; });
  REQUIRE(l12->message.find("10.67 ms later per pass") != std::string::npos);
  REQUIRE(l12->at.pointer == "/layers/0/position/base_sync");
  REQUIRE_FALSE(l12->error);
  // A macro corner or the expression pedal reaching above 0 counts as the stored value does.
  const std::string macro = With(
      "macros",
      Parse(R"([{"id": "repeats", "targets": [{"param": "feedback.amount", "range": [0, 0.5]}]}])"),
      base);
  REQUIRE(Count(Lint(Read(macro)), "L12") == 1u);
  REQUIRE(Count(Lint(Read(With("layers[0].position.base_sync", Str("1/4"), Clean()))), "L12") ==
          1u);  // the default table's Repeats
  const std::string expr = With(
      "controls.expression", Parse(R"([{"target": "feedback.amount", "lo": 0, "hi": 0.4}])"), base);
  REQUIRE(Count(Lint(Read(expr)), "L12") == 1u);
  // Feedback on the post delay is the tempo-exact way: nothing to say. Unsynced: nothing.
  REQUIRE(Count(Lint(Read(With("post.delay.fb", Num("0.6"), base))), "L12") == 0u);
  REQUIRE(Count(Lint(Read(With("feedback.amount", Num("0.3"), Clean()))), "L12") == 0u);
  // A warning, never an error, also for --factory.
  LintOptions factory;
  factory.factory = true;
  REQUIRE(Count(Lint(Read(With("feedback.amount", Num("0.3"), base)), factory), "L12", true) == 0u);
}

TEST_CASE("lint: L13 a target a sync overrides at every position (clock.md §6.5)", "[lint]") {
  const std::string timeMacro = With(
      "macros",
      Parse(R"([{"id": "time", "targets": [{"param": "post.delay.time_ms", "range": [100, 800]}]}])"),
      Clean());
  REQUIRE(Count(Lint(Read(timeMacro)), "L13") == 0u);  // sync 0: time_ms plays
  // Row 63 nonzero at its stored value and nothing moving it: the time macro does nothing.
  const std::vector<Finding> f = Lint(Read(With("post.delay.sync", Num("9"), timeMacro)));
  REQUIRE(Count(f, "L13") == 1u);
  const auto l13 =
      std::find_if(f.begin(), f.end(), [](const Finding& x) { return x.code == "L13"; });
  REQUIRE(l13->message.find("post.delay.time_ms") != std::string::npos);
  REQUIRE(l13->at.pointer == "/macros/0/targets/0");
  // A macro that can lift the sync to 0 lets time_ms play at one end: no finding.
  const std::string lifted = With(
      "macros",
      Parse(R"([{"id": "time", "targets": [{"param": "post.delay.time_ms", "range": [100, 800]}]},
                {"id": "aux1", "targets": [{"param": "post.delay.sync", "range": [0, 12]}]}])"),
      With("post.delay.sync", Num("9"), Clean()));
  REQUIRE(Count(Lint(Read(lifted)), "L13") == 0u);
  // The expression pedal reaching 0.4 only rounds to 0, so it lifts the sync too; at 0.5 it
  // reads 1/32, which does not.
  const std::string pedal = With(
      "controls.expression", Parse(R"([{"target": "post.delay.sync", "lo": 0.4, "hi": 9}])"),
      With("post.delay.sync", Num("9"), timeMacro));
  REQUIRE(Count(Lint(Read(pedal)), "L13") == 0u);
  const std::string pedal5 = With(
      "controls.expression", Parse(R"([{"target": "post.delay.sync", "lo": 0.5, "hi": 9}])"),
      With("post.delay.sync", Num("9"), timeMacro));
  REQUIRE(Count(Lint(Read(pedal5)), "L13") == 1u);
  // A layer's base_ms under its base_sync: every macro target and expression assignment on it.
  const std::string base = With(
      "controls.expression",
      Parse(R"([{"target": "layer0.position.base_ms", "lo": 100, "hi": 900}])"),
      With("macros",
           Parse(R"([{"id": "time", "targets": [{"param": "layer0.position.base_ms", "range": [50, 900]}]}])"),
           With("layers[0].position.base_sync", Str("1/8"), Clean())));
  REQUIRE(Count(Lint(Read(base)), "L13") == 2u);
}

TEST_CASE("lint: L14 reachable codes that fold at the stored tempo and Subdiv (clock.md §5.2)",
          "[lint]") {
  // At the stored 120 BPM and TAP nothing folds: 2/1 is 4 s, the line's end, inclusive.
  REQUIRE(Count(Lint(Read(With("post.delay.sync", Num("16"), Clean()))), "L14") == 0u);
  // At 119 BPM it does.
  const std::string slow = With("performance.tempo_us_per_quarter", Num("504202"),
                                With("post.delay.sync", Num("16"), Clean()));
  const std::vector<Finding> f = Lint(Read(slow));
  REQUIRE(Count(f, "L14") == 1u);
  const auto l14 =
      std::find_if(f.begin(), f.end(), [](const Finding& x) { return x.code == "L14"; });
  REQUIRE(l14->message.find("119.0 BPM and TAP") != std::string::npos);
  REQUIRE(l14->message.find("2/1 \xE2\x86\x92 1/1") != std::string::npos);
  REQUIRE(l14->at.pointer == "/post/delay/sync");
  // A macro sweeping the code reaches the codes between its ends: at 50 BPM 1/1 and 2/1 fold to a
  // half note (§5.2's example), 1/2D does not.
  const std::string sweep = With(
      "macros",
      Parse(R"([{"id": "time", "targets": [{"param": "post.delay.sync", "range": [9, 16]}]}])"),
      With("performance.tempo_us_per_quarter", Num("1200000"),
           With("post.delay.sync", Num("9"), Clean())));
  const std::vector<Finding> g = Lint(Read(sweep));
  REQUIRE(Count(g, "L14") == 1u);
  const auto m = std::find_if(g.begin(), g.end(), [](const Finding& x) { return x.code == "L14"; });
  REQUIRE(m->message.find("1/1 \xE2\x86\x92 1/2") != std::string::npos);
  REQUIRE(m->message.find("2/1 \xE2\x86\x92 1/2") != std::string::npos);
  REQUIRE(m->message.find("1/2D \xE2\x86\x92") == std::string::npos);
  // The stored Subdiv scales it (×1/4 makes a 1/4 a whole note: 2 s at 120 BPM, no fold; 1/2 a
  // 2/1, 4 s; 1/1 folds); Tempo time mode forces TAP.
  const std::string sub = With("performance.subdiv", Str("x1/4"),
                               With("post.delay.sync", Num("15"), Clean()));
  REQUIRE(Count(Lint(Read(sub)), "L14") == 1u);
  REQUIRE(Count(Lint(Read(With("performance.time_mode", Str("tempo"), sub))), "L14") == 0u);
  // base_sync: its stored code. 2/1 at 60 BPM is 8 s, beyond base_ms's 5 s.
  const std::string base = With("performance.tempo_us_per_quarter", Num("1000000"),
                                With("layers[0].position.base_sync", Str("2/1"), Clean()));
  const std::vector<Finding> b = Lint(Read(base));
  REQUIRE(Count(b, "L14") == 1u);
  REQUIRE(std::find_if(b.begin(), b.end(), [](const Finding& x) { return x.code == "L14"; })
              ->at.pointer == "/layers/0/position/base_sync");
  REQUIRE(Count(Lint(Read(With("layers[0].position.base_sync", Str("1/1"), Clean()))), "L14") == 0u);
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
