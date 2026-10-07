// Properties over many documents (docs/design/mode-compiler.md §10.1, §10.2): random valid
// documents round-trip and compile to fixed points, and a mutation fuzz of the reader, whose
// accepted documents format and compile to fixed points. Both digests are committed, so every
// host must produce the same packages and verdicts (§8.3's cross-host identity).
#include <cstdint>
#include <string>
#include <vector>

#include "Compile.h"
#include "Text.h"
#include "TestUtil.h"
#include "brainscape/Sha256.h"
#include "brainscape/SoundRevision.h"
#include "catch.hpp"

using namespace bsc;
using namespace bsctest;
using brainscape::ParamDescriptor;
using brainscape::ParamKind;

namespace {

struct Rng {
  uint64_t s;
  uint64_t Next() {
    uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z          = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z          = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  uint32_t Below(uint32_t n) { return static_cast<uint32_t>(Next() % n); }
  bool     Chance(uint32_t percent) { return Below(100) < percent; }
};

// A float in [lo, hi] (bits, finite, lo <= hi), canonical: often an end or a short decimal,
// otherwise uniform over the representable values between them.
uint32_t RandomValue(Rng& r, uint32_t lo, uint32_t hi) {
  switch (r.Below(5)) {
    case 0:
      return lo;
    case 1:
      return hi;
    case 2: {
      // A short decimal, read exactly. One draw per statement: the order in which a
      // compiler evaluates the operands of + is unspecified (Clang and GCC differ).
      const int64_t     mantissa = static_cast<int64_t>(r.Below(20001)) - 10000;
      const uint32_t    exponent = r.Below(4);
      const std::string text     = DecSigned(mantissa) + "e-" + Dec(exponent);
      float f = 0.0f;
      ParseJsonNumber(text.data(), text.size(), &f);
      const uint32_t b = Bits(f);
      if (!LessBits(b, lo) && !LessBits(hi, b) && (b & 0x7F800000u) != 0u) return b;
      return lo;
    }
    default: {
      const uint32_t a = OrderKey(lo), z = OrderKey(hi);
      uint32_t       k = a + static_cast<uint32_t>(r.Next() % (static_cast<uint64_t>(z) - a + 1u));
      uint32_t       b = (k & 0x80000000u) != 0u ? (k & 0x7FFFFFFFu) : ~k;  // OrderKey's inverse
      if ((b & 0x7F800000u) == 0u) b = 0u;  // canonical: no subnormal, no -0
      if (LessBits(b, lo)) b = lo;
      return b;
    }
  }
}

std::string RandomText(Rng& r, uint32_t maxBytes, bool allowEmpty) {
  static const char* const pieces[] = {"a",
                                       "b",
                                       "z",
                                       "0",
                                       "9",
                                       " ",
                                       ".",
                                       "-",
                                       "\"",
                                       "\\",
                                       "/",
                                       "\xC3\xA9",
                                       "\xE2\x82\xAC",
                                       "\xF0\x9F\x8E\xB8",
                                       "Q",
                                       "_"};
  std::string              out;
  const uint32_t           n = r.Below(maxBytes) + (allowEmpty ? 0u : 1u);
  while (out.size() < n) {
    const std::string piece = pieces[r.Below(16)];
    if (out.size() + piece.size() > maxBytes) break;
    out += piece;
  }
  if (out.empty() && !allowEmpty) out = "x";
  return out;
}

std::vector<const ParamDescriptor*> PlayableLeaves() {
  std::vector<const ParamDescriptor*> out;
  for (const ParamDescriptor& d : brainscape::kParamTable) {
    if (d.kind == ParamKind::Leaf && IsSchemaLeaf(static_cast<uint32_t>(d.id))) out.push_back(&d);
  }
  return out;
}

json::Value NumValue(uint32_t bits) { return json::Value::Number(NumberText(bits)); }

// A random document this build compiles.
std::string RandomDocument(Rng& r, uint32_t index) {
  static const std::vector<const ParamDescriptor*> leaves = PlayableLeaves();
  static const char* const families[] = {"none", "recall", "reverie", "misfire", "echoic"};
  static const char* const macros[]   = {"activity", "repeats", "shape", "time",
                                         "space",    "filter",  "aux1",  "aux2"};
  json::Value              root       = json::Value::Object();
  root.Add("schema_version", Num("1"));
  root.Add("id", Str("rand." + Dec(index)));
  root.Add("name", Str(RandomText(r, 24, false)));
  if (r.Chance(70)) root.Add("family", Str(families[r.Below(5)]));
  if (r.Chance(60)) {
    json::Value meta = json::Value::Object();
    meta.Add("author", Str(RandomText(r, 20, true)));
    meta.Add("description", Str(RandomText(r, 60, true)));
    json::Value tags = json::Value::Array();
    for (uint32_t i = 0, n = r.Below(4); i < n; ++i)
      tags.Push(Str("t" + Dec(i) + RandomText(r, 6, true)));
    meta.Add("tags", std::move(tags));
    root.Add("meta", std::move(meta));
  }
  for (const ParamDescriptor* d : leaves) {
    if (!r.Chance(60)) continue;
    Set(root, LeafPath(d->name), NumValue(RandomValue(r, Bits(d->min), Bits(d->max))));
  }
  // Wave 1's structure as it lands: source selection (sound revision 4), any subset.
  if (r.Chance(30)) {
    static const char* const sources[] = {"periodic", "onset", "footswitch", "midi_note"};
    json::Value              set       = json::Value::Array();
    for (const char* s : sources) {
      if (r.Chance(50)) set.Push(Str(s));
    }
    Set(root, "scheduler.sources", std::move(set));
  }
  // Macros: a random subset, 0-4 targets each, within 32 targets with the defaults.
  json::Value list       = json::Value::Array();
  uint32_t    total      = 0;
  bool        defined[8] = {};
  for (uint32_t k = 0; k < 8; ++k) {
    if (!r.Chance(40)) continue;
    json::Value m = json::Value::Object();
    m.Add("id", Str(macros[k]));
    if (r.Chance(30)) m.Add("display_name", Str(RandomText(r, 16, false)));
    json::Value           targets = json::Value::Array();
    std::vector<uint32_t> used;
    const uint32_t        n = (k == 2 ? 1u : 0u) + r.Below(4);
    for (uint32_t i = 0; i < n && total < 16u; ++i) {
      const ParamDescriptor* d = leaves[r.Below(static_cast<uint32_t>(leaves.size()))];
      if (d->id == brainscape::ParamId::Mix) continue;
      bool again = false;
      for (const uint32_t u : used) again = again || u == static_cast<uint32_t>(d->id);
      if (again) continue;
      used.push_back(static_cast<uint32_t>(d->id));
      json::Value t = json::Value::Object();
      t.Add("param", Str(d->name));
      json::Value range = json::Value::Array();
      range.Push(NumValue(RandomValue(r, Bits(d->min), Bits(d->max))));
      range.Push(NumValue(RandomValue(r, Bits(d->min), Bits(d->max))));
      t.Add("range", std::move(range));
      if (r.Chance(30)) {
        uint32_t a = RandomValue(r, 0u, 0x3F7FFFFFu), b = RandomValue(r, 0u, 0x3F800000u);
        if (!LessBits(a, b)) {
          a = 0u;
          b = 0x3F800000u;
        }
        json::Value in = json::Value::Array();
        in.Push(NumValue(a));
        in.Push(NumValue(b));
        t.Add("in_range", std::move(in));
      }
      if (r.Chance(50)) t.Add("curve", NumValue(RandomValue(r, 0x3D800000u, 0x41800000u)));
      targets.Push(std::move(t));
      ++total;
    }
    if (k == 2 && targets.items.empty()) continue;  // E10: a written shape macro does something
    m.Add("targets", std::move(targets));
    list.Push(std::move(m));
    defined[k] = true;
  }
  if (!list.items.empty()) root.Add("macros", std::move(list));
  json::Value controls  = json::Value::Object();
  json::Value positions = json::Value::Object();
  for (uint32_t k = 0; k < 8; ++k) {
    if ((k < 6 || defined[k]) && r.Chance(80))
      positions.Add(macros[k], NumValue(RandomValue(r, 0u, 0x3F800000u)));
  }
  controls.Add("macro_positions", std::move(positions));
  if (r.Chance(40)) {
    json::Value expr = json::Value::Array();
    for (uint32_t i = 0, n = 1 + r.Below(4); i < n; ++i) {
      json::Value a = json::Value::Object();
      if (r.Chance(30)) {
        a.Add("target", Str(std::string("macro.") + macros[r.Below(8)]));
        a.Add("lo", NumValue(RandomValue(r, 0u, 0x3F800000u)));
      } else {
        const ParamDescriptor* d = leaves[r.Below(static_cast<uint32_t>(leaves.size()))];
        a.Add("target", Str(d->name));
        a.Add("hi", NumValue(RandomValue(r, Bits(d->min), Bits(d->max))));
      }
      if (r.Chance(50)) a.Add("curve", NumValue(RandomValue(r, 0x3D800000u, 0x41800000u)));
      expr.Push(std::move(a));
    }
    controls.Add("expression", std::move(expr));
  }
  root.Add("controls", std::move(controls));
  if (r.Chance(20)) {
    json::Value editor = json::Value::Object();
    editor.Add("ratio_gen", Parse("{\"kind\": \"octaves\", \"n\": " + Dec(r.Below(5)) + "}"));
    json::Value detached = json::Value::Array();
    detached.Push(Str(leaves[r.Below(static_cast<uint32_t>(leaves.size()))]->name));
    editor.Add("detached", std::move(detached));
    root.Add("editor", std::move(editor));
  }
  return json::Serialize(root);
}

// Committed digests (§8.3): the same on every host. Re-minted only when the compiler's output
// or verdicts change on purpose (the package rule, §8.3). Re-minted at sound revision 2: the
// header's sound_rev, STAT without the retired rows 27 and 28, onset and mark compiled. Re-minted
// at sound revision 3 for the header's sound_rev alone: built with kSoundRevision 2, the same
// tree gives revision 2's digests. Re-minted at sound revision 4: the header's sound_rev, STAT
// with leaves 57-59 (the random documents draw them too), random source subsets.
constexpr uint32_t kRandomDocuments = 400;
const char* const  kRandomDigest =
    "18ba6c584e87a7734bc425b0b49b2b2c3f8aa9676b5cc4c3441d74ce84564f4d";
constexpr uint32_t kFuzzMutants = 20000;
const char* const  kFuzzDigest = "334852c79bc3bbd0951efd5069f162fa13a0a205a4e6aac594887c3a4127ddd2";

}  // namespace

TEST_CASE("property: random documents compile, round-trip and give one digest", "[property]") {
  Rng                      r{20261007};
  brainscape::Sha256Hasher h;
  for (uint32_t i = 0; i < kRandomDocuments; ++i) {
    const std::string text = RandomDocument(r, i);
    INFO(text);
    const CompileResult c = Compile(text);
    INFO(All(c.findings));
    REQUIRE(c.ok);
    // Decompile(Compile(J)) is the JSON section; rebuilt, Fmt(J) minus editor, stamped.
    const DecompileResult rb      = Decompile(c.package.data(), c.package.size(), true);
    Document              stamped = c.doc;
    stamped.stamped               = true;
    stamped.soundRev              = brainscape::kSoundRevision;
    stamped.soundHash             = c.soundHash;
    REQUIRE(rb.ok);
    REQUIRE(rb.json == FormatDocument(stamped, false));
    // The JSON section is a fixed point.
    const CompileResult again = Compile(c.json);
    REQUIRE(again.ok);
    REQUIRE(again.package == c.package);
    std::string fmt;
    REQUIRE(FormatText(c.json, &fmt, nullptr));
    REQUIRE(fmt == c.json);
    h.Update(c.package.data(), c.package.size());
  }
  uint8_t digest[32];
  h.Final(digest);
  const std::string got = Hex(digest, 32);
  INFO("random-document digest: " << got);
  CHECK(got == kRandomDigest);
}

TEST_CASE("property: the reader fuzz's accepted documents are fixed points", "[property]") {
  const std::string seeds[] = {
      Minimal(),
      R"({"schema_version": 1, "id": "fuzz.rich", "name": "Rich \u00e9\ud83c\udfb8", "family": "reverie",
          "meta": {"author": "a", "description": "d \"q\" \\ /", "tags": ["x", "y"]},
          "global": {"mix": 0.35}, "scheduler": {"overlap": 0.75, "jitter": 1},
          "layers": [{"size_ms": 140, "position": {"base_ms": 300, "spray_ms": 600},
                      "pitch": {"transpose_st": 12, "spread_cents": 25}}],
          "macros": [{"id": "activity", "display_name": "Swarm", "targets": [
                        {"param": "scheduler.overlap", "range": [0.55, 0.95]},
                        {"param": "layer0.position.spray_ms", "range": [100, 1500], "curve": 2, "in_range": [0.1, 0.9]}]},
                     {"id": "aux2", "targets": [{"param": "post.filter.morph", "range": [3, 0], "curve": 0.0625}]}],
          "controls": {"macro_positions": {"activity": 0.5, "aux2": 1, "time": 0.125},
                       "expression": [{"target": "macro.activity"}, {"target": "global.mix", "lo": 0.2, "hi": 0.8}]},
          "editor": {"ratio_gen": {"kind": "octaves"}, "detached": ["global.mix"]}})",
      R"({"schema_version": 1, "id": "fuzz.engram", "name": "E", "post": {"delay": {"time_ms": 405, "fb": 0.45, "mix": 1}},
          "macros": [{"id": "time", "targets": [{"param": "post.delay.time_ms", "range": [40, 1500], "curve": 2}]}],
          "controls": {"macro_positions": {"time": 0.5}}})",
  };
  static const char        alphabet[] = "{}[]\":, 0123456789.-+eE\\ux/tfn";
  Rng                      r{777};
  brainscape::Sha256Hasher h;
  uint32_t                 accepted = 0, compiled = 0;
  for (uint32_t i = 0; i < kFuzzMutants; ++i) {
    std::string    m     = seeds[r.Below(3)];
    const uint32_t edits = 1 + r.Below(3);
    for (uint32_t e = 0; e < edits && !m.empty(); ++e) {
      const size_t at = r.Below(static_cast<uint32_t>(m.size()));
      switch (r.Below(5)) {
        case 0:
          m[at] = static_cast<char>(m[at] ^ static_cast<char>(1u << r.Below(8)));
          break;
        case 1:
          m.insert(at, 1, alphabet[r.Below(sizeof alphabet - 1)]);
          break;
        case 2:
          m.erase(at, 1 + r.Below(4));
          break;
        case 3: {
          // One draw per statement (argument order is unspecified).
          const size_t from = r.Below(static_cast<uint32_t>(m.size()));
          const size_t span = 1 + r.Below(12);
          m.insert(at, m.substr(from, span));
          break;
        }
        default:
          if (m[at] >= '0' && m[at] <= '9') m[at] = static_cast<char>('0' + r.Below(10));
          break;
      }
    }
    std::vector<Finding> f;
    std::string          fmt;
    const bool           ok      = FormatText(m, &fmt, &f);
    std::string          verdict = ok ? "ok" : (f.empty() ? "?" : f[0].code);
    if (ok) {
      ++accepted;
      std::string again;
      REQUIRE(FormatText(fmt, &again, nullptr));
      REQUIRE(again == fmt);
      const CompileResult c = Compile(m);
      if (c.ok) {
        ++compiled;
        const CompileResult c2 = Compile(c.json);
        REQUIRE(c2.ok);
        REQUIRE(c2.package == c.package);
        REQUIRE(Decompile(c.package.data(), c.package.size()).json == c.json);
        verdict += " " + Hex(c.packageHash.bytes, 8);
      } else {
        verdict += " " + c.findings[0].code;  // E12 only, or a bug
        REQUIRE(c.findings[0].code == "E12");
      }
    }
    verdict += "\n";
    h.Update(verdict.data(), verdict.size());
  }
  uint8_t digest[32];
  h.Final(digest);
  const std::string got = Hex(digest, 32);
  INFO("fuzz digest: " << got << ", accepted " << accepted << ", compiled " << compiled);
  CHECK(accepted > 1000u);
  CHECK(got == kFuzzDigest);
}
