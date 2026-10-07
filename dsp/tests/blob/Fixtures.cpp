#include <cstdio>
#include <cstring>

#include "../../src/blob/Blob.h"
#include "BlobSamples.h"
#include "brainscape/Sha256.h"

// The frozen fixtures (docs/design/mode-compiler.md §10.3) and the fuzzer's committed digest.
// The committed files under dsp/tests/golden/presets/frozen/ are the reference: their bytes are
// pinned by SHA-256 here and never re-stamped. A verdict that a later build changes on purpose
// (a feature becoming supported, sound revision 2 retiring IDs 27 and 28) is updated in the pull
// request that changes it, with the reason in `why`. The recipes below are frozen too: they spell
// out each revision's leaves rather than reading this build's table.
namespace brainscape::blobtest {

// Fuzz(kFuzzIterations, kFuzzSeed) on every leg, the M7 included (blob_tool). Re-minted at sound
// revision 2: onset and mark became supported, and the samples no longer target rows 27 and 28.
// Re-minted at sound revision 3 for the samples' soundRev alone (kSoundRevision): built with
// kSoundRevision 2, the same tree gives revision 2's digest. Re-minted at sound revision 4: the
// samples' soundRev and kSupportedModeFeatures (source selection), which the verdicts read; and
// at 5 for the same two (pitch sets).
const char* const kFuzzDigest = "5322523ab4447f9c4bcc95bd681c1e24a0d9d5b100c0f12ea04dbd2c0da46de2";

const Fixture kFixtures[] = {
    {"r1-default-mode.bsp",
     "058d6f9eb2a5be5fbfe1cda38d3a422fce9c4d6ae72eef2619ccd5d12e444494",
     PresetError::None, 0, 1, false, 0,
     "a sound-revision-1 package: every r1 leaf, the default mode, CTRL with an expression "
     "assignment, META. Decodes on every later build (blob_format 1); loaded exact at r1, and "
     "since r2 retired IDs 27 and 28 its leaves for them are unknown, so the load is inexact "
     "(every fixture built from it below too)",
     PresetError::None, 0},
    {"unknown-sections.bsp",
     "4c02c56bba6a609e65fa8584a94d871a093a8b59a7d2bcd80e4117675513498d",
     PresetError::None, 0, 1, false, 2,
     "sections this build does not know, after MODE and after META: decoded, skipped, and "
     "carried byte for byte and in place by a re-encode",
     PresetError::None, 0},
    {"unknown-chunk.bsp",
     "fe1a4c28f7c3e5068e145813798b4c66fb97ab94331a522bac965842404f45fa",
     PresetError::UnsupportedFeature, PackageTag('Z', 'Z', 'Z', 'Z'), 0,
     false, 0, "a MODE chunk this build does not know ('ZZZZ', never assigned): rejected, named",
     PresetError::None, 0},
    {"unknown-feature-bit.bsp",
     "ddf04b87af1a6aa153cf003b043f555a1219e2db906ef14fde8c768a86fa5676",
     PresetError::UnsupportedFeature, 0x80000000u, 0, false, 0,
     "feature bit 31 (never assigned): rejected, named",
     PresetError::None, 0},
    {"future-pitch-set.bsp",
     "842aae62f3765256fcbeab2d53293862d4700b344e5524bb473d5d3f9da954b9",
     PresetError::None, 0, 1, false, 0,
     "a wave-1 package (the pitch set {0, +12}): rejected as UnsupportedFeature until sound "
     "revision 5 played pitch sets (mode-compiler.md §7.5 R10); since then it decodes and "
     "validates, and loads inexact as the revision-1 package it was built from",
     PresetError::None, 0},
    {"blob-format-2.bsp",
     "d1b280ecfce08bd417da80f3bd5a469b98481913cba17078e68d9f598419e998",
     PresetError::BlobFormat, 2, 0, false, 0,
     "a newer STAT/MODE/CTRL layout: rejected",
     PresetError::None, 0},
    {"package-format-2.bsp",
     "c30ef80120f496ad44aabba133214494a19f7b69d39452ce13b261c77377c29a",
     PresetError::PackageFormat, 2, 0, false, 0,
     "a newer container: rejected",
     PresetError::None, 0},
    {"future-sound-rev.bsp",
     "2a122ebc0f42cc8eb0ec8983d9d06653aaa86a80dc3b578888da4d558ebd3e41",
     PresetError::None, 0, 1000, false, 0,
     "compiled by a build of sound revision 1000: decodes (LoadPreset counts it as this build's "
     "revision, §7.3)",
     PresetError::None, 0},
    {"newer-schema.bsp",
     "26194969c86b733f62f0e43591301877d9c2c5bc5b4bbb966b70f18b63544325",
     PresetError::None, 0, 1, false, 0,
     "a document schema newer than this build's: the package layer does not depend on it",
     PresetError::None, 0},
    {"factory-json-stale.bsp",
     "41fbcbbdcdd57248d246a1c3a25777e989aeaee043aeb4b06ea839bd07ebccae",
     PresetError::None, 0, 1, false, 0,
     "FACTORY and JSON_STALE flags and a JSON section: decoded and carried",
     PresetError::None, 0},
    {"unknown-flag.bsp",
     "c72f58a9dee4954d1f7eb9ddb022af937a8e401b0d9d14edd8d8ebb2696e8b3e",
     PresetError::HeaderFlags, 4, 0, false, 0,
     "header flag bit 2 (unassigned): rejected",
     PresetError::None, 0},
    {"no-ctrl.bsp",
     "3a59c9a029c7237ad2bece0b584d1a4195b9a58b77e3568d9aea9c9094afd970",
     PresetError::None, 0, 1, false, 0,
     "no CTRL section: decodes with control.present 0",
     PresetError::None, 0},
    {"w1-leaf-macro-target.bsp",
     "d182b5d3056a88c4f7eec92611c5bf291484bc35e5a6b34ee29f183f16bfcfc8",
     PresetError::None, 0, 1, false, 0,
     "macro activity targeting layer0.decay_ms (ID 30), a wave-1 leaf and a Reserved row here: "
     "decodes (macro targets are ValidateMode's), and ValidateMode names it UnsupportedTarget, "
     "newer content rather than a corrupt one. Its verdict changes in the W1 pull request that "
     "makes ID 30 a Leaf row",
     PresetError::UnsupportedTarget, 30},
    {"w1-leaf-expression.bsp",
     "257656e5151674c2dc28b0f1b13d2b954d91a5db507fe3652c0fc405abc8dc7f",
     PresetError::UnsupportedTarget, 30, 0, false, 0,
     "an expression assignment on layer0.decay_ms (ID 30), a wave-1 leaf and a Reserved row "
     "here: rejected as UnsupportedTarget, named, as an unknown chunk is UnsupportedFeature. Its "
     "verdict changes in the W1 pull request that makes ID 30 a Leaf row",
     PresetError::None, 0},
    {"r2-onset-marks.bsp",
     "42e1ac2002e3abf42a83531c3f8c8652075417fa7855a3da20c394df3e030d74",
     PresetError::None, 0, 2, true, 0,
     "a sound-revision-2 package: every r2 leaf, onsets on marks, custom macros, CTRL with "
     "expression assignments on a macro and a leaf, META. Loads exact, with no missing ID, on "
     "every later build: a leaf a later revision adds is not missing from it (sinceRev, §7.3)",
     PresetError::None, 0},
};
const size_t kFixtureCount = sizeof kFixtures / sizeof kFixtures[0];

namespace {

// Sound revision 1's leaves, ids 1-28 at their r1 defaults (27 and 28 were Leaf rows then).
std::unique_ptr<PresetState> R1State() {
  static const float kR1Defaults[28] = {250.0f, 0.5f,   0.0f,  0.0f,   90.0f,    0.55f, 20.0f,
                                        0.0f,   0.0f,   0.0f,  0.2f,   0.3f,     0.5f,  0.7f,
                                        0.5f,   0.4f,   0.0f,  350.0f, 0.3f,     0.0f,  0.5f,
                                        0.0f,   20000.0f, 0.1f, 0.0f,  0.5f,     0.0f,  0.0f};
  auto s      = std::make_unique<PresetState>();
  s->soundRev = 1;
  for (uint32_t id = 1; id <= 28; ++id) s->leaves[s->leafCount++] = {id, kR1Defaults[id - 1]};
  SetLeaf(*s, ParamId::Mix, 0.35f);
  SetLeaf(*s, ParamId::Feedback, 0.4f);
  SetLeaf(*s, ParamId::DelayTimeMs, 405.0f);
  SetLeaf(*s, ParamId::ReverbMix, 0.12f);
  s->control.exprCount      = 1;
  s->control.expressions[0] = ExpressionAssignment{74, 0.0f, 1.0f, 1.0f};
  return s;
}

Bytes R1Package(const PresetState& s, const Bytes* json = nullptr, uint16_t flags = 0,
                uint32_t schema = kSchemaVersion) {
  const Bytes meta = MetaFor(s.mode, "fixture.r1", "Frozen r1", PresetFamily::Echoic, 1, 1);
  return Package(s, &meta, json, flags, schema);
}

// Sound revision 2's package: ids 1-26 (27 and 28 retired) at r2's defaults with a few set, the
// onset source on marks, the default macros with Space on the reverb only, two expression
// assignments (a macro, a leaf).
std::unique_ptr<PresetState> R2State() {
  static const float kR2Defaults[26] = {250.0f, 0.5f,   0.0f,  0.0f,   90.0f,  0.55f,  20.0f,
                                        0.0f,   0.0f,   0.0f,  0.2f,   0.3f,   0.5f,   0.7f,
                                        0.5f,   0.4f,   0.0f,  350.0f, 0.3f,   0.0f,   0.5f,
                                        0.0f,   20000.0f, 0.1f, 0.0f,  0.5f};
  auto s      = std::make_unique<PresetState>();
  s->soundRev = 2;
  for (uint32_t id = 1; id <= 26; ++id) s->leaves[s->leafCount++] = {id, kR2Defaults[id - 1]};
  SetLeaf(*s, ParamId::Mix, 0.4f);
  SetLeaf(*s, ParamId::Feedback, 0.45f);
  SetLeaf(*s, ParamId::TriggerSens, 0.6f);
  SetLeaf(*s, ParamId::ReverbMix, 0.25f);
  s->mode.schedule.sources = static_cast<uint8_t>(s->mode.schedule.sources | kSourceOnset);
  s->mode.layers[0].source = PositionSource::Mark;
  // Space (macro 73, targets 6 and 7) on post.reverb.mix alone: target 6 removed.
  MacroTable& t = s->mode.macros;
  for (uint32_t i = 6; i + 1 < t.targetCount; ++i) t.targets[i] = t.targets[i + 1];
  t.targets[--t.targetCount] = MacroTarget{};
  t.macros[4].count          = 1;
  t.macros[5].first          = 7;
  s->mode.features           = RequiredModeFeatures(s->mode);
  ComputeModeHash(s->mode, &s->mode.modeHash);
  s->control.positions[4].position = 0.25f;
  s->control.exprCount      = 2;
  s->control.expressions[0] = ExpressionAssignment{72, 0.2f, 0.8f, 2.0f};
  s->control.expressions[1] = ExpressionAssignment{2, 0.0f, 1.0f, 1.0f};
  return s;
}

}  // namespace

Bytes MakeFixture(size_t index) {
  auto s = R1State();
  switch (index) {
    case 0: return R1Package(*s);
    case 1: {
      Bytes b = R1Package(*s);
      InsertSection(b, FindSection(b, kTagCtrl), PackageTag('X', 'T', 'R', 'A'),
                    Bytes{'c', 'a', 'r', 'r', 'i', 'e', 'd'});
      InsertSection(b, b.size(), PackageTag('Z', 'E', 'N', 'D'), Bytes{0xA5});
      Rehash(b);
      return b;
    }
    case 2: {
      Bytes b = R1Package(*s);
      // After MACR, the last chunk: where a chunk added later would be read.
      const size_t mode = FindSection(b, kTagMode);
      InsertChunk(b, mode + 8 + Rd32(&b[mode + 4]), PackageTag('Z', 'Z', 'Z', 'Z'),
                  Bytes{1, 0, 0, 0});
      Rehash(b);
      return b;
    }
    case 3: {
      Bytes        b    = R1Package(*s);
      const size_t mode = FindSection(b, kTagMode);
      Wr32(&b[mode + 8], Rd32(&b[mode + 8]) | 0x80000000u);
      Rehash(b);
      return b;
    }
    case 4: {
      s->mode.pitch[0].count      = 2;
      s->mode.pitch[0].entries[1] = PitchEntry{12.0f, 1, 0};
      s->mode.features            = RequiredModeFeatures(s->mode);
      return R1Package(*s);
    }
    case 5:
    case 6: {
      Bytes b = R1Package(*s);
      if (index == 5) Wr32(&b[12], 2);  // blob_format
      if (index == 6) b[4] = 2;         // package_format
      Rehash(b);
      return b;
    }
    case 7: s->soundRev = 1000; return R1Package(*s);
    case 8: return R1Package(*s, nullptr, 0, 2);
    case 9: {
      const char* text = "{\n  \"schema_version\": 1,\n  \"id\": \"fixture.r1\"\n}\n";
      const Bytes json(text, text + std::strlen(text));
      return R1Package(*s, &json, kPackageFlagFactory | kPackageFlagJsonStale);
    }
    case 10: {
      Bytes b = R1Package(*s);
      b[6] |= 4;  // flags
      Rehash(b);
      return b;
    }
    case 11:
      std::memset(static_cast<void*>(&s->control), 0, sizeof s->control);
      return R1Package(*s);
    case 12:
      // Activity's second target, layer0.position.spray_ms, becomes layer0.decay_ms.
      s->mode.macros.targets[1] = MacroTarget{30, 0.0f, 800.0f, 0.0f, 1.0f, 2.0f};
      return R1Package(*s);
    case 13: {
      // The encoder refuses the target, so the package is built with a Leaf row of a range
      // that holds both ends (layer0.position.base_ms) and its target rewritten.
      s->control.exprCount      = 2;
      s->control.expressions[1] = ExpressionAssignment{1, 1.0f, 800.0f, 1.0f};
      Bytes        b = R1Package(*s);
      const size_t x = FindSection(b, kTagCtrl) + 8 + 4 + 8 * s->control.macroCount + 16;
      if (Rd32(&b[x]) != 1u) return Bytes();
      Wr32(&b[x], 30);
      Rehash(b);
      return b;
    }
    case 14: {
      const auto  r2   = R2State();
      const Bytes meta = MetaFor(r2->mode, "fixture.r2", "Frozen r2", PresetFamily::Reverie, 1, 1);
      return Package(*r2, &meta);
    }
    default: return Bytes();
  }
}

std::string CheckFixture(const Fixture& f, const Bytes& bytes) {
  uint8_t digest[32];
  Sha256Hasher::Digest(bytes.data(), bytes.size(), digest);
  const std::string sha = Hex(digest, 32);
  if (sha != f.sha256) return std::string("bytes changed: sha256 ") + sha;
  auto             s = std::make_unique<PresetState>();
  PresetDiagnostic d;
  PackageInfo      info;
  const bool       ok = DecodePreset(bytes.data(), bytes.size(), s.get(), &d, &info);
  char             buf[160];
  if (d.error != f.error || d.detail != f.detail) {
    std::snprintf(buf, sizeof buf, "verdict %s/0x%08x, expected %s/0x%08x",
                  PresetErrorName(d.error), static_cast<unsigned>(d.detail),
                  PresetErrorName(f.error), static_cast<unsigned>(f.detail));
    return buf;
  }
  if (!ok) return std::string();
  if (s->soundRev != f.soundRev || info.unknownSections != f.unknownSections) {
    return "header or sections differ";
  }
  LoadReport report;
  if (CheckPreset(*s, &report) != f.exactLoad) return "CheckPreset verdict differs";
  PresetDiagnostic v;
  ValidateMode(*s, &v);
  if (v.error != f.validate || v.detail != f.validateDetail) {
    std::snprintf(buf, sizeof buf, "ValidateMode %s/0x%08x, expected %s/0x%08x",
                  PresetErrorName(v.error), static_cast<unsigned>(v.detail),
                  PresetErrorName(f.validate), static_cast<unsigned>(f.validateDetail));
    return buf;
  }
  Bytes        again(kMaxPackageBytes);
  const size_t n = ReencodePackage(*s, bytes.data(), bytes.size(), 0, again.data(), again.size());
  if (n != bytes.size() || std::memcmp(again.data(), bytes.data(), n) != 0) {
    return "a re-encode does not give back the bytes";
  }
  return std::string();
}

}  // namespace brainscape::blobtest
