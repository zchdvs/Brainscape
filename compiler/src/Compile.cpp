#include "Compile.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <memory>

#include "Number.h"
#include "Text.h"
#include "brainscape/Sha256.h"
#include "brainscape/SoundRevision.h"

namespace bsc {

using namespace brainscape;

namespace {

constexpr uint32_t kPad4(uint32_t n) { return (n + 3u) & ~3u; }

bool DefaultDecode(const void* bytes, size_t length, PresetState* out, PresetDiagnostic* d,
                   PackageInfo* info, PresetMeta* meta, uint32_t /*supported*/) {
  return DecodePreset(bytes, length, out, d, info, meta);
}

bool DefaultValidate(const PresetState& state, uint32_t /*supported*/, PresetDiagnostic* d) {
  return ValidateMode(state, d);
}

Finding Internal(const std::string& message) {
  return Finding{"X1", true, Location{}, "internal: " + message};
}

bool StartsWith(std::string_view s, std::string_view prefix) {
  return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

std::string_view Span(const uint8_t* bytes, const SectionSpan& s) {
  return std::string_view(reinterpret_cast<const char*>(bytes) + s.offset, s.length);
}

}  // namespace

std::string Sha256Hex(const uint8_t* bytes, size_t length) {
  uint8_t digest[32];
  Sha256Hasher::Digest(bytes, length, digest);
  return Hex(digest, 32);
}

std::string DescribeDiagnostic(const PresetDiagnostic& d) {
  return std::string(PresetErrorName(d.error)) + " (detail " + Dec(d.detail) + ")";
}

// ── Compile ──────────────────────────────────────────────────────────────────────────────

CompileResult Compile(std::string_view text, const CompileOptions& options) {
  CompileResult r;
  if (!ReadDocumentText(text, options.read, &r.doc, &r.findings)) return r;
  CompileResult c = CompileDocument(r.doc, options);
  c.doc           = std::move(r.doc);
  return c;
}

CompileResult CompileDocument(const Document& doc, const CompileOptions& options) {
  CompileResult r;
  r.doc           = doc;
  const auto fail = [&](Finding f) {
    r.findings.push_back(std::move(f));
    r.package.clear();
    return r;
  };
  const auto     decode    = options.decode != nullptr ? options.decode : DefaultDecode;
  const auto     validate  = options.validate != nullptr ? options.validate : DefaultValidate;
  const uint32_t supported = options.read.supportedFeatures;

  // (5) The decoded state and META.
  auto state      = std::make_unique<PresetState>(*doc.state);
  state->soundRev = kSoundRevision;
  if (!ComputeModeHash(state->mode, &state->mode.modeHash))
    return fail(Internal("MODE does not encode"));
  MetaContent mc;
  const auto  text = [](const std::string& s) {
    return MetaText{s.data(), static_cast<uint32_t>(s.size())};
  };
  mc.id          = text(doc.id);
  mc.name        = text(doc.name);
  mc.family      = doc.family;
  mc.author      = text(doc.author);
  mc.description = text(doc.description);
  mc.tagCount    = static_cast<uint8_t>(doc.tags.size());
  for (size_t i = 0; i < doc.tags.size() && i < kMaxTags; ++i) mc.tags[i] = text(doc.tags[i]);
  const MacroTable& macros = state->mode.macros;
  for (uint32_t k = 0; k < macros.macroCount; ++k) {
    const uint32_t     macroId = macros.macros[k].id;
    const std::string& display =
        doc.displayName[macroId - static_cast<uint32_t>(ParamId::MacroActivity)];
    if (display.empty()) continue;
    mc.displayMacro[mc.displayNameCount] = macroId;
    mc.displayName[mc.displayNameCount]  = text(display);
    ++mc.displayNameCount;
  }
  std::vector<uint8_t> meta(kMaxPackageBytes);
  uint32_t             metaLength = 0;
  PresetDiagnostic     diag;
  if (!EncodeMeta(mc, state->mode, meta.data(), meta.size(), &metaLength, &diag)) {
    return fail(Internal("META refused: " + DescribeDiagnostic(diag)));
  }
  meta.resize(metaLength);

  // (6) STAT and MODE, hashed (§6.3), then the JSON section: the document formatted and
  // stamped with this build's revision and the package's sound_hash.
  std::vector<uint8_t> stat(kMaxPackageBytes), mode(kMaxPackageBytes), ctrl(kMaxPackageBytes);
  uint32_t             statLength = 0, modeLength = 0, ctrlLength = 0;
  if (!EncodeStat(*state, stat.data(), stat.size(), &statLength, &diag) ||
      !EncodeMode(state->mode, mode.data(), mode.size(), &modeLength, &diag) ||
      !EncodeControl(state->mode, state->control, ctrl.data(), ctrl.size(), &ctrlLength, &diag)) {
    if (diag.error == PresetError::ModeTooLarge) {
      return fail(Finding{"E12", true, Location{}, "MODE is over 4 KiB"});
    }
    return fail(Internal("STAT, MODE or CTRL refused: " + DescribeDiagnostic(diag)));
  }
  SoundHash(stat.data(), statLength, mode.data(), modeLength, &r.soundHash);
  ControlHash(ctrl.data(), ctrlLength, &r.controlHash);
  Document stamped  = doc;
  stamped.stamped   = true;
  stamped.soundRev  = kSoundRevision;
  stamped.soundHash = r.soundHash;
  r.json            = FormatDocument(stamped);

  // E12: the whole package within 16 KiB, MODE within 4 KiB.
  if (modeLength > kMaxModeBytes)
    return fail(Finding{"E12", true, Location{}, "MODE is over 4 KiB"});
  const uint64_t total = uint64_t{kPackageHeaderBytes} + 8u + kPad4(statLength) + 8u +
                         kPad4(modeLength) +
                         (state->control.present != 0u ? 8u + kPad4(ctrlLength) : 0u) + 8u +
                         kPad4(metaLength) + 8u + kPad4(static_cast<uint32_t>(r.json.size()));
  if (total > kMaxPackageBytes) {
    return fail(Finding{"E12", true, Location{},
                        "the package would be " + Dec(total) + " bytes; at most " +
                            Dec(kMaxPackageBytes) + " (the JSON section is " + Dec(r.json.size()) +
                            ")"});
  }

  // (7) The package and its hashes.
  PackageContent content;
  content.flags         = StartsWith(doc.id, "factory.") ? kPackageFlagFactory : uint16_t{0};
  content.schemaVersion = doc.schemaVersion;
  content.meta          = meta.data();
  content.metaLength    = metaLength;
  content.json          = reinterpret_cast<const uint8_t*>(r.json.data());
  content.jsonLength    = static_cast<uint32_t>(r.json.size());
  r.package.resize(kMaxPackageBytes);
  const size_t n = EncodePackage(*state, content, r.package.data(), r.package.size(), &diag);
  if (n == 0) return fail(Internal("EncodePackage refused: " + DescribeDiagnostic(diag)));
  r.package.resize(n);
  std::memcpy(r.packageHash.bytes, r.package.data() + 96, 32);

  // (8) Decode and validate what was written: the same state.
  auto        back = std::make_unique<PresetState>();
  PackageInfo info;
  if (!decode(r.package.data(), r.package.size(), back.get(), &diag, &info, nullptr, supported)) {
    return fail(Internal("the package does not decode: " + DescribeDiagnostic(diag)));
  }
  if (!validate(*back, supported, &diag)) {
    return fail(Internal("the package does not validate: " + DescribeDiagnostic(diag)));
  }
  if (std::memcmp(back.get(), state.get(), sizeof(PresetState)) != 0) {
    return fail(Internal("the package decodes to another state"));
  }
  r.ok = true;
  return r;
}

bool FormatText(std::string_view text, std::string* out, std::vector<Finding>* findings,
                const ReadOptions& options) {
  Document doc;
  if (!ReadDocumentText(text, options, &doc, findings)) return false;
  *out = FormatDocument(doc);
  return true;
}

// ── Packages ─────────────────────────────────────────────────────────────────────────────

DecodedPackage DecodePackage(const uint8_t* bytes, size_t length, const CompileOptions& options) {
  DecodedPackage p;
  p.state           = std::make_unique<PresetState>();
  const auto decode = options.decode != nullptr ? options.decode : DefaultDecode;
  p.ok              = decode(bytes, length, p.state.get(), &p.diagnostic, &p.info, &p.meta,
                             options.read.supportedFeatures);
  if (p.ok && p.info.json.offset != 0u) {
    p.hasJson = true;
    p.json.assign(Span(bytes, p.info.json));
  }
  return p;
}

bool DocumentFromPackage(const uint8_t* bytes, const DecodedPackage& p, Document* out,
                         std::vector<Finding>* findings) {
  const auto fail = [&](const std::string& message) {
    if (findings != nullptr) findings->push_back(Finding{"V3", true, Location{}, message});
    return false;
  };
  if (!p.ok) return fail("the package does not decode: " + DescribeDiagnostic(p.diagnostic));
  if (p.info.schemaVersion > kSchemaVersion) {
    return fail("schema " + Dec(p.info.schemaVersion) + " is newer than this compiler's");
  }
  if (p.info.meta.offset == 0u) return fail("the package has no META, so no id or name");
  Document d;
  d.schemaVersion = p.info.schemaVersion;
  d.id.assign(Span(bytes, p.meta.id));
  d.name.assign(Span(bytes, p.meta.name));
  d.family = p.meta.family;
  d.author.assign(Span(bytes, p.meta.author));
  d.description.assign(Span(bytes, p.meta.description));
  for (uint32_t i = 0; i < p.meta.tagCount; ++i) d.tags.emplace_back(Span(bytes, p.meta.tags[i]));
  for (uint32_t i = 0; i < p.meta.displayNameCount; ++i) {
    const uint32_t macroId = p.meta.displayMacro[i];
    if (MacroName(macroId) == nullptr) return fail("a display name for a row that is no macro");
    d.displayName[macroId - static_cast<uint32_t>(ParamId::MacroActivity)].assign(
        Span(bytes, p.meta.displayName[i]));
  }
  d.stamped      = true;
  d.soundRev     = p.info.soundRev;
  d.soundHash    = p.info.soundHash;
  *d.state       = *p.state;
  PresetState& s = *d.state;
  // The leaves the document has keys for: every Leaf row of a present element. A stored id
  // the document cannot say is refused; one the package lacks takes its default (an older
  // package), with a note.
  PresetState rebuilt = s;
  rebuilt.leafCount   = 0;
  for (PresetLeaf& leaf : rebuilt.leaves) leaf = PresetLeaf{};
  for (uint32_t i = 0; i < s.leafCount; ++i) {
    const uint32_t         leafId = s.leaves[i].id;
    const ParamDescriptor* row    = FindParam(static_cast<ParamId>(leafId));
    if (row == nullptr || row->kind != ParamKind::Leaf || !ElementPresent(s.mode, leafId)) {
      return fail("STAT holds id " + Dec(leafId) + ", which schema 1 has no key for here");
    }
    if (StructureLeaf(leafId) && BitsOf(s.leaves[i].value) != StructureLeafBits(s.mode, leafId)) {
      return fail(std::string(row->name) +
                  " disagrees with the structure, which schema 1 "
                  "writes instead (scheduler.sources, position.source)");
    }
  }
  for (const ParamDescriptor& row : kParamTable) {
    const auto leafId = static_cast<uint32_t>(row.id);
    if (row.kind != ParamKind::Leaf || !ElementPresent(s.mode, leafId)) continue;
    PresetLeaf& leaf = rebuilt.leaves[rebuilt.leafCount++];
    leaf.id          = leafId;
    leaf.value       = row.def;
    bool found       = false;
    for (uint32_t i = 0; i < s.leafCount; ++i) {
      if (s.leaves[i].id == leafId) {
        leaf.value = s.leaves[i].value;
        found      = true;
      }
    }
    if (!found && findings != nullptr) {
      findings->push_back(
          Finding{"V3", false, Location{},
                  std::string(row.name) + " is not stored; written at its default"});
    }
  }
  s = rebuilt;
  if (s.control.present == 0u) {
    // No CTRL: one position, 0.5, per defined macro (what an omitted position compiles to).
    ControlState c{};
    for (MacroPosition& m : c.positions) m = MacroPosition{0, 0.0f};
    c.present    = 1;
    c.macroCount = s.mode.macros.macroCount;
    for (uint32_t k = 0; k < c.macroCount; ++k) {
      c.positions[k].macroId = s.mode.macros.macros[k].id;
      uint32_t half          = 0x3F000000u;
      std::memcpy(&c.positions[k].position, &half, sizeof half);
    }
    s.control = c;
    if (findings != nullptr) {
      findings->push_back(
          Finding{"V3", false, Location{}, "no CTRL: macro positions written as 0.5"});
    }
  }
  *out = std::move(d);
  return true;
}

DecompileResult Decompile(const uint8_t* bytes, size_t length, bool rebuild,
                          const CompileOptions& options) {
  DecompileResult      r;
  const DecodedPackage p = DecodePackage(bytes, length, options);
  if (!p.ok) {
    r.findings.push_back(
        Finding{"V1", true, Location{},
                "the package does not decode: " + DescribeDiagnostic(p.diagnostic)});
    return r;
  }
  const bool stale = (p.info.flags & kPackageFlagJsonStale) != 0u;
  if (p.hasJson && !stale && !rebuild) {
    r.ok   = true;
    r.json = p.json;
    return r;
  }
  Document d;
  if (!DocumentFromPackage(bytes, p, &d, &r.findings)) return r;
  r.ok      = true;
  r.rebuilt = true;
  r.json    = FormatDocument(d, false);
  return r;
}

std::vector<Finding> Verify(const uint8_t* bytes, size_t length, const CompileOptions& options) {
  std::vector<Finding> out;
  const auto           note = [&](const char* code, bool error, const std::string& message) {
    out.push_back(Finding{code, error, Location{}, message});
  };
  const DecodedPackage p = DecodePackage(bytes, length, options);
  if (!p.ok) {
    note("V1", true, "DecodePreset refuses it: " + DescribeDiagnostic(p.diagnostic));
    return out;
  }
  const auto       validate = options.validate != nullptr ? options.validate : DefaultValidate;
  PresetDiagnostic diag;
  if (!validate(*p.state, options.read.supportedFeatures, &diag)) {
    note("V1", true, "ValidateMode refuses it: " + DescribeDiagnostic(diag));
    return out;
  }
  if (!p.hasJson) {
    note("V2", false, "no JSON section: nothing to recompile");
    return out;
  }
  if ((p.info.flags & kPackageFlagJsonStale) != 0u) {
    note("V2", false, "JSON_STALE: the JSON section predates pedal-side edits; not recompiled");
    return out;
  }
  const CompileResult c = Compile(p.json, options);
  if (!c.ok) {
    note("V2", true, "the JSON section does not compile:");
    for (const Finding& f : c.findings) out.push_back(f);
    return out;
  }
  const DecodedPackage q    = DecodePackage(c.package.data(), c.package.size(), options);
  const auto           same = [&](const SectionSpan& a, const SectionSpan& b) {
    return Span(bytes, a) == Span(c.package.data(), b);
  };
  if (!same(p.info.stat, q.info.stat))
    note("V2", true, "the JSON section compiles to another STAT");
  if (!same(p.info.mode, q.info.mode))
    note("V2", true, "the JSON section compiles to another MODE");
  if (!same(p.info.ctrl, q.info.ctrl))
    note("V2", false, "the JSON section compiles to another CTRL");
  if (!same(p.info.meta, q.info.meta))
    note("V2", false, "the JSON section compiles to another META");
  if (p.info.soundRev != kSoundRevision) {
    note("V2", false,
         "compiled at sound revision " + Dec(p.info.soundRev) + "; this build is " +
             Dec(kSoundRevision) + " (bspc stamp updates it)");
  }
  if (c.package.size() != length || std::memcmp(c.package.data(), bytes, length) != 0) {
    note("V2", false, "recompiling does not give these exact bytes");
  }
  return out;
}

// ── Diff ─────────────────────────────────────────────────────────────────────────────────

namespace {

std::string Compact(const json::Value& v) {
  std::string s = json::Serialize(v);
  std::string out;
  bool        inString = false, escaped = false, space = false;
  for (const char c : s) {
    if (inString) {
      out.push_back(c);
      if (escaped) {
        escaped = false;
      } else if (c == '\\') {
        escaped = true;
      } else if (c == '"') {
        inString = false;
      }
      continue;
    }
    if (c == '"') inString = true;
    if (c == '\n' || c == ' ') {
      space = true;
      continue;
    }
    if (space && !out.empty() && out.back() != '[' && out.back() != '{' && c != ']' && c != '}' &&
        out.back() != ':') {
      out.push_back(' ');
    }
    space = false;
    out.push_back(c);
    if (c == ':') out.push_back(' ');
  }
  return out;
}

// The first difference between two documents in document order, as "pointer: a -> b".
bool FirstDifference(const json::Value& a, const json::Value& b, const std::string& pointer,
                     std::string* out) {
  if (a.type != b.type || a.IsScalar()) {
    if (json::Equal(a, b)) return false;
    *out = (pointer.empty() ? "/" : pointer) + ": " + Compact(a) + " -> " + Compact(b);
    return true;
  }
  if (a.type == json::Type::Array) {
    for (size_t i = 0; i < a.items.size() && i < b.items.size(); ++i) {
      if (FirstDifference(a.items[i], b.items[i], pointer + "/" + Dec(i), out)) return true;
    }
    if (a.items.size() != b.items.size()) {
      *out = pointer + ": " + Dec(a.items.size()) + " entries -> " + Dec(b.items.size());
      return true;
    }
    return false;
  }
  // Objects: members by key, in a's order, then b's extra keys.
  for (const json::Member& m : a.members) {
    const json::Value* other = b.Find(m.key);
    const std::string  p     = pointer + "/" + json::PointerToken(m.key);
    if (other == nullptr) {
      *out = p + ": " + Compact(m.value) + " -> (absent)";
      return true;
    }
    if (FirstDifference(m.value, *other, p, out)) return true;
  }
  for (const json::Member& m : b.members) {
    if (a.Find(m.key) == nullptr) {
      *out = pointer + "/" + json::PointerToken(m.key) + ": (absent) -> " + Compact(m.value);
      return true;
    }
  }
  return false;
}

}  // namespace

std::string Diff(const uint8_t* a, size_t aLength, const uint8_t* b, size_t bLength,
                 const CompileOptions& options) {
  if (aLength == bLength && std::memcmp(a, b, aLength) == 0) return std::string();
  const DecodedPackage pa = DecodePackage(a, aLength, options);
  const DecodedPackage pb = DecodePackage(b, bLength, options);
  if (!pa.ok || !pb.ok) {
    return std::string("the packages differ; ") + (!pa.ok ? "the first" : "the second") +
           " does not decode: " + DescribeDiagnostic(!pa.ok ? pa.diagnostic : pb.diagnostic);
  }
  struct Field {
    const char* name;
    uint32_t    x, y;
  };
  // The formats first: packages of different formats or revisions differ there.
  const Field header[] = {
      {"package_format", pa.info.packageFormat, pb.info.packageFormat},
      {"sound_rev", pa.info.soundRev, pb.info.soundRev},
      {"blob_format", pa.info.blobFormat, pb.info.blobFormat},
      {"schema_version", pa.info.schemaVersion, pb.info.schemaVersion},
  };
  for (const Field& f : header) {
    if (f.x != f.y) return std::string("header ") + f.name + ": " + Dec(f.x) + " -> " + Dec(f.y);
  }
  Document             da, db;
  std::vector<Finding> ignored;
  if (DocumentFromPackage(a, pa, &da, &ignored) && DocumentFromPackage(b, pb, &db, &ignored)) {
    // The stamp is derived (sound_hash follows the fields); the fields are what differs. What
    // the preset plays and its controls first, then its identity, META and display names, so a
    // user copy of a factory preset (a new id and name, §9.1) shows the field that sounds
    // different.
    da.stamped = db.stamped = false;
    // a's sound and controls under b's identity.
    Document sound    = da;
    sound.id          = db.id;
    sound.name        = db.name;
    sound.family      = db.family;
    sound.author      = db.author;
    sound.description = db.description;
    sound.tags        = db.tags;
    for (uint32_t k = 0; k < kMaxMacros; ++k) sound.displayName[k] = db.displayName[k];
    std::string out;
    if (FirstDifference(WriteDocument(sound, false), WriteDocument(db, false), "", &out))
      return out;
    if (FirstDifference(WriteDocument(da, false), WriteDocument(db, false), "", &out)) return out;
  } else {
    // What schema 1 cannot say: compare the sections.
    const char* const names[] = {"STAT", "MODE", "CTRL", "META"};
    const SectionSpan sa[]    = {pa.info.stat, pa.info.mode, pa.info.ctrl, pa.info.meta};
    const SectionSpan sb[]    = {pb.info.stat, pb.info.mode, pb.info.ctrl, pb.info.meta};
    for (int k = 0; k < 4; ++k) {
      if (Span(a, sa[k]) != Span(b, sb[k])) return std::string(names[k]) + " differs";
    }
  }
  // The flags follow from the id (FACTORY) and the JSON section (JSON_STALE).
  if (pa.info.flags != pb.info.flags)
    return "header flags: " + Dec(pa.info.flags) + " -> " + Dec(pb.info.flags);
  if (pa.info.soundHash.bytes[0] != pb.info.soundHash.bytes[0] ||
      !std::equal(std::begin(pa.info.soundHash.bytes), std::end(pa.info.soundHash.bytes),
                  std::begin(pb.info.soundHash.bytes))) {
    return "sound_hash: " + Hex(pa.info.soundHash.bytes, 32) + " -> " +
           Hex(pb.info.soundHash.bytes, 32);
  }
  if (pa.json != pb.json || pa.hasJson != pb.hasJson) return "the JSON sections differ";
  if (pa.info.unknownSections != pb.info.unknownSections) return "the unknown sections differ";
  return "the bytes differ (section layout)";
}

}  // namespace bsc
