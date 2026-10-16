#include "detail/FpProfilePrivate.h"

#include "brainscape/Preset.h"
#include "brainscape/Sha256.h"

#include "Blob.h"

// The package encoder (docs/design/mode-compiler.md §5.2, §6): every field written one by one,
// little-endian, floats as their bits, so one state has one encoding and decoding then
// encoding gives back the same STAT, MODE and CTRL bytes. The compiler packs with it (lane A),
// the pedal rewrites STAT and CTRL with it (companion-app.md §6.9), and the fuzzers re-encode
// every package they decode. Integer-only, like the decoder.
namespace brainscape {

namespace blob {

namespace {

// A bounded writer: every write past the capacity is dropped and remembered.
struct Writer {
  uint8_t* p;
  size_t   capacity;
  size_t   n  = 0;
  bool     ok = true;

  void Bytes(const void* data, size_t length) noexcept {
    if (!ok || capacity - n < length) {
      ok = false;
      return;
    }
    if (length > 0u) std::memcpy(p + n, data, length);
    n += length;
  }
  void Zeros(size_t length) noexcept {
    if (!ok || capacity - n < length) {
      ok = false;
      return;
    }
    if (length > 0u) std::memset(p + n, 0, length);
    n += length;
  }
  void U8(uint8_t v) noexcept { Bytes(&v, 1); }
  void U16(uint16_t v) noexcept {
    uint8_t b[2];
    Wr16(b, v);
    Bytes(b, 2);
  }
  void U32(uint32_t v) noexcept {
    uint8_t b[4];
    Wr32(b, v);
    Bytes(b, 4);
  }
  void F32(const float& v) noexcept { U32(BitsOf(v)); }
};

void WriteLayer(Writer& w, const ModeLayer& l) noexcept {
  w.U8(static_cast<uint8_t>(l.source));
  w.U8(l.baseSync);
  w.U8(static_cast<uint8_t>(l.sprayLaw));
  w.U8(l.markIndex);
  w.U8(static_cast<uint8_t>(l.markWalk));
  w.U8(static_cast<uint8_t>(l.pinRearm));
  w.U8(static_cast<uint8_t>(l.pitchSelect));
  w.U8(static_cast<uint8_t>(l.quantize));
  w.U8(l.quantizeRoot);
  w.U8(static_cast<uint8_t>(l.modifier[0]));
  w.U8(static_cast<uint8_t>(l.modifier[1]));
  w.U8(static_cast<uint8_t>(l.svfBand));
  w.U8(static_cast<uint8_t>(l.svfCutoffSource));
  w.U8(l.pad);
  w.U16(l.scaleMask);
  w.F32(l.slotShare);
  w.F32(l.markJitter);
  w.F32(l.pinRearmMs);
  w.F32(l.glideStStart);
  w.F32(l.glideStEnd);
}

void WriteRoutes(Writer& w, uint8_t count, const uint8_t* pad, const Route* entries) noexcept {
  w.U8(count);
  w.Bytes(pad, 3);
  for (uint32_t i = 0; i < count; ++i) {
    w.U8(entries[i].from);
    w.U8(entries[i].to);
    w.U8(entries[i].layer);
    w.U8(entries[i].pad);
    w.F32(entries[i].amount);
  }
}

// A chunk: tag, length, then the payload `body` writes.
template <typename Body>
void Chunk(Writer& w, uint32_t tag, uint32_t length, Body body) noexcept {
  w.U32(tag);
  w.U32(length);
  const size_t start = w.n;
  body();
  if (w.ok && w.n - start != length) w.ok = false;  // a layout slip, never a valid state
}

bool WriteMode(Writer& w, const ModeBlob& mode) noexcept {
  const uint32_t layers = mode.schedule.layerCount;
  const bool     pset   = !PitchSetsDefault(mode);
  const bool     step   = mode.steps.countMax != 0u;
  const bool     mods   = mode.modulators[0].type != ModulatorType::None;
  const bool     rout   = mode.routes.count != 0u;
  const bool     link   = mode.links.count != 0u;
  const bool     duck   = !DryDuckDefault(mode.dryDuck);
  const uint32_t chunks = 3u + (pset ? 1u : 0u) + (step ? 1u : 0u) + (mods ? 1u : 0u) +
                          (rout ? 1u : 0u) + (link ? 1u : 0u) + (duck ? 1u : 0u);
  w.U32(mode.features);
  w.U32(chunks);
  Chunk(w, kChunkSchd, 8, [&] {
    w.U8(mode.schedule.sources);
    w.U8(mode.schedule.layerCount);
    w.U8(mode.schedule.reserved);
    w.U8(static_cast<uint8_t>(mode.schedule.stepOrder));
    w.Bytes(mode.schedule.pad, 4);
  });
  Chunk(w, kChunkLayr, 36u * layers, [&] {
    for (uint32_t l = 0; l < layers; ++l) WriteLayer(w, mode.layers[l]);
  });
  if (pset) {
    Chunk(w, kChunkPset, 68u * layers, [&] {
      for (uint32_t l = 0; l < layers; ++l) {
        const PitchSet& s = mode.pitch[l];
        w.U8(s.count);
        w.Bytes(s.pad, 3);
        for (const PitchEntry& e : s.entries) {
          w.F32(e.st);
          w.U16(e.weight);
          w.U16(e.pad);
        }
      }
    });
  }
  if (step) {
    Chunk(w, kChunkStep, 260, [&] {
      w.U8(mode.steps.countMax);
      w.Bytes(mode.steps.pad, 3);
      for (const StepEntry& e : mode.steps.entries) {
        w.U8(e.slot);
        w.U8(e.ratioIdx);
        w.U8(e.flags);
        w.U8(e.pad);
        w.F32(e.posSel);
        w.F32(e.gain);
        w.F32(e.prob);
      }
    });
  }
  if (mods) {
    Chunk(w, kChunkMods, 24, [&] {
      for (const Modulator& m : mode.modulators) {
        w.U8(static_cast<uint8_t>(m.type));
        w.U8(m.shape);
        w.U8(m.sync);
        w.U8(m.pad);
        w.F32(m.attackMs);
        w.F32(m.releaseMs);
      }
    });
  }
  if (rout) {
    Chunk(w, kChunkRout, 4u + 8u * mode.routes.count,
          [&] { WriteRoutes(w, mode.routes.count, mode.routes.pad, mode.routes.entries); });
  }
  if (link) {
    Chunk(w, kChunkLink, 4u + 8u * mode.links.count,
          [&] { WriteRoutes(w, mode.links.count, mode.links.pad, mode.links.entries); });
  }
  if (duck) {
    Chunk(w, kChunkDuck, 8, [&] {
      w.F32(mode.dryDuck.attackMs);
      w.F32(mode.dryDuck.releaseMs);
    });
  }
  const MacroTable& t = mode.macros;
  Chunk(w, kChunkMacr, 4u + 8u * t.macroCount + 24u * t.targetCount, [&] {
    w.U8(t.macroCount);
    w.U8(t.targetCount);
    w.U16(t.pad);
    for (uint32_t i = 0; i < t.macroCount; ++i) {
      w.U32(t.macros[i].id);
      w.U8(t.macros[i].first);
      w.U8(t.macros[i].count);
      w.U16(t.macros[i].pad);
    }
    for (uint32_t i = 0; i < t.targetCount; ++i) {
      const MacroTarget& e = t.targets[i];
      w.U32(e.param);
      w.F32(e.lo);
      w.F32(e.hi);
      w.F32(e.inLo);
      w.F32(e.inHi);
      w.F32(e.curve);
    }
  });
  return w.ok;
}

void WriteStat(Writer& w, const PresetState& s) noexcept {
  w.U32(s.leafCount);
  for (uint32_t i = 0; i < s.leafCount; ++i) {
    w.U32(s.leaves[i].id);
    w.F32(s.leaves[i].value);
  }
  w.U8(s.performance.reverse);
  w.U8(static_cast<uint8_t>(s.performance.timeMode));
  w.U8(static_cast<uint8_t>(s.performance.subdiv));
  w.U8(s.performance.reserved);
  w.U32(s.performance.usPerQuarter);
}

void WriteControl(Writer& w, const ControlState& c) noexcept {
  w.U8(c.macroCount);
  w.U8(c.exprCount);
  w.U16(0);
  for (uint32_t i = 0; i < c.macroCount; ++i) {
    w.U32(c.positions[i].macroId);
    w.F32(c.positions[i].position);
  }
  for (uint32_t i = 0; i < c.exprCount; ++i) {
    const ExpressionAssignment& e = c.expressions[i];
    w.U32(e.target);
    w.F32(e.lo);
    w.F32(e.hi);
    w.F32(e.curve);
  }
}

uint32_t StatBytes(const PresetState& s) noexcept { return 4u + 8u * s.leafCount + 8u; }
uint32_t ControlBytes(const ControlState& c) noexcept {
  return c.present != 0u ? 4u + 8u * c.macroCount + 16u * c.exprCount : 0u;
}
uint32_t Padded(uint32_t n) noexcept { return n + ((4u - (n & 3u)) & 3u); }

// A section's tag and length; its payload follows, then the zero padding.
void SectionHeader(Writer& w, uint32_t tag, uint32_t length) noexcept {
  w.U32(tag);
  w.U32(length);
}

// Fills total_bytes, section_count and the three hashes of the package in out[0, n).
void Seal(uint8_t* out, uint32_t n, uint32_t sections, const SectionSpan& stat,
          const SectionSpan& mode, const SectionSpan& ctrl) noexcept {
  Wr32(out + 20, n);
  Wr32(out + 24, sections);
  Digest32 h;
  SoundHash(out + stat.offset, stat.length, out + mode.offset, mode.length, &h);
  std::memcpy(out + 32, h.bytes, 32);
  ControlHash(out + ctrl.offset, ctrl.length, &h);
  std::memcpy(out + 64, h.bytes, 32);
  std::memset(out + 96, 0, 32);
  Sha256Hasher::Digest(out, n, h.bytes);
  std::memcpy(out + 96, h.bytes, 32);
}

void WriteHeader(Writer& w, uint16_t flags, uint32_t soundRev, uint32_t schemaVersion) noexcept {
  w.Bytes("BSPK", 4);
  w.U16(kPackageFormat);
  w.U16(flags);
  w.U32(soundRev);
  w.U32(kBlobFormat);
  w.U32(schemaVersion);
  w.Zeros(kPackageHeaderBytes - 20);  // total_bytes, section_count, reserved, hashes: Seal
}

// Fail for the functions that return a length.
size_t Refuse(PresetDiagnostic* d, PresetError e, uint32_t detail = 0) noexcept {
  Fail(d, e, detail);
  return 0;
}

bool CheckState(const PresetState& s, PresetDiagnostic* d) noexcept {
  return CheckStat(s, d) && CheckMode(s.mode, kModeFeatureAll, d) &&
         CheckControl(s.mode, s.control, d);
}

}  // namespace

}  // namespace blob

using namespace blob;

void SoundHash(const uint8_t* stat, uint32_t statLength, const uint8_t* mode, uint32_t modeLength,
               Digest32* out) noexcept {
  uint8_t      length[4];
  Sha256Hasher sha;
  Wr32(length, statLength);
  sha.Update(length, 4);
  sha.Update(stat, statLength);
  Wr32(length, modeLength);
  sha.Update(length, 4);
  sha.Update(mode, modeLength);
  sha.Final(out->bytes);
}

void ControlHash(const uint8_t* ctrl, uint32_t ctrlLength, Digest32* out) noexcept {
  uint8_t      length[4];
  Sha256Hasher sha;
  Wr32(length, ctrlLength);
  sha.Update(length, 4);
  sha.Update(ctrl, ctrlLength);
  sha.Final(out->bytes);
}

bool EncodeStat(const PresetState& state, uint8_t* out, size_t capacity, uint32_t* written,
                PresetDiagnostic* diagnostic) noexcept {
  if (diagnostic != nullptr) *diagnostic = PresetDiagnostic{};
  if (!CheckStat(state, diagnostic)) return false;
  Writer w{out, capacity};
  WriteStat(w, state);
  if (!w.ok) return Fail(diagnostic, PresetError::TooLarge, StatBytes(state));
  *written = static_cast<uint32_t>(w.n);
  return true;
}

bool EncodeMode(const ModeBlob& mode, uint8_t* out, size_t capacity, uint32_t* written,
                PresetDiagnostic* diagnostic) noexcept {
  if (diagnostic != nullptr) *diagnostic = PresetDiagnostic{};
  if (!CheckMode(mode, kModeFeatureAll, diagnostic)) return false;
  Writer w{out, capacity};
  if (!WriteMode(w, mode)) return Fail(diagnostic, PresetError::TooLarge);
  *written = static_cast<uint32_t>(w.n);
  return true;
}

bool EncodeControl(const ModeBlob& mode, const ControlState& control, uint8_t* out,
                   size_t capacity, uint32_t* written, PresetDiagnostic* diagnostic) noexcept {
  if (diagnostic != nullptr) *diagnostic = PresetDiagnostic{};
  if (!CheckControl(mode, control, diagnostic)) return false;
  *written = 0;
  if (control.present == 0u) return true;
  Writer w{out, capacity};
  WriteControl(w, control);
  if (!w.ok) return Fail(diagnostic, PresetError::TooLarge, ControlBytes(control));
  *written = static_cast<uint32_t>(w.n);
  return true;
}

bool ComputeModeHash(const ModeBlob& mode, Digest32* out) noexcept {
  uint8_t  buffer[1536];  // the largest MODE is 1,528 bytes (§6.2)
  uint32_t n = 0;
  if (!EncodeMode(mode, buffer, sizeof buffer, &n)) return false;
  Sha256Hasher::Digest(buffer, n, out->bytes);
  return true;
}

bool EncodeMeta(const MetaContent& meta, const ModeBlob& mode, uint8_t* out, size_t capacity,
                uint32_t* written, PresetDiagnostic* diagnostic) noexcept {
  if (diagnostic != nullptr) *diagnostic = PresetDiagnostic{};
  static const char* const kFamilies[] = {"none", "recall", "reverie", "misfire", "echoic"};
  const auto               family      = static_cast<uint8_t>(meta.family);
  if (family >= 5u) return Fail(diagnostic, PresetError::MetaFamily);
  if (meta.tagCount > kMaxTags) return Fail(diagnostic, PresetError::MetaText, 5);
  if (meta.displayNameCount > kMaxMacros) return Fail(diagnostic, PresetError::MetaDisplayName);
  Writer w{out, capacity};
  bool   fits = true;
  auto   str  = [&](const MetaText& t) {
    fits = fits && t.length <= 0xFFFFu && (t.length == 0u || t.data != nullptr);
    if (!fits) return;
    w.U16(static_cast<uint16_t>(t.length));
    w.Bytes(t.data, t.length);
  };
  str(meta.id);
  str(meta.name);
  uint32_t familyLength = 0;
  while (kFamilies[family][familyLength] != '\0') ++familyLength;
  str(MetaText{kFamilies[family], familyLength});
  str(meta.author);
  str(meta.description);
  w.U8(meta.tagCount);
  for (uint32_t i = 0; i < meta.tagCount; ++i) str(meta.tags[i]);
  w.U8(meta.displayNameCount);
  for (uint32_t i = 0; i < meta.displayNameCount; ++i) {
    w.U32(meta.displayMacro[i]);
    str(meta.displayName[i]);
  }
  if (!fits) return Fail(diagnostic, PresetError::MetaText);
  if (!w.ok) return Fail(diagnostic, PresetError::TooLarge);
  // The decoder's rules, so what this writes always decodes.
  if (!ParseMeta(out, static_cast<uint32_t>(w.n), 0, mode, nullptr, diagnostic)) return false;
  *written = static_cast<uint32_t>(w.n);
  return true;
}

size_t EncodePackage(const PresetState& state, const PackageContent& content, uint8_t* out,
                     size_t capacity, PresetDiagnostic* diagnostic) noexcept {
  if (diagnostic != nullptr) *diagnostic = PresetDiagnostic{};
  if ((content.flags & ~kPackageFlagsKnown) != 0u) {
    return Refuse(diagnostic, PresetError::HeaderFlags, content.flags & ~kPackageFlagsKnown);
  }
  if (content.schemaVersion == 0u) return Refuse(diagnostic, PresetError::SchemaVersion);
  if (state.soundRev == 0u) return Refuse(diagnostic, PresetError::SoundRevision);
  if ((content.meta == nullptr && content.metaLength != 0u) ||
      (content.json == nullptr && content.jsonLength != 0u)) {
    return Refuse(diagnostic, PresetError::SectionBounds);
  }
  if (!CheckState(state, diagnostic)) return 0;
  if (content.meta != nullptr &&
      !ParseMeta(content.meta, content.metaLength, 0, state.mode, nullptr, diagnostic)) {
    return 0;
  }
  Writer w{out, capacity < kMaxPackageBytes ? capacity : kMaxPackageBytes};
  WriteHeader(w, content.flags, state.soundRev, content.schemaVersion);
  SectionSpan stat, mode, ctrl;
  uint32_t    sections = 2;
  SectionHeader(w, kTagStat, StatBytes(state));
  stat.offset = static_cast<uint32_t>(w.n);
  WriteStat(w, state);
  stat.length = static_cast<uint32_t>(w.n) - stat.offset;
  SectionHeader(w, kTagMode, 0);  // the length is patched below
  mode.offset = static_cast<uint32_t>(w.n);
  WriteMode(w, state.mode);
  mode.length = static_cast<uint32_t>(w.n) - mode.offset;
  if (w.ok) Wr32(out + mode.offset - 4, mode.length);
  if (state.control.present != 0u) {
    SectionHeader(w, kTagCtrl, ControlBytes(state.control));
    ctrl.offset = static_cast<uint32_t>(w.n);
    WriteControl(w, state.control);
    ctrl.length = static_cast<uint32_t>(w.n) - ctrl.offset;
    ++sections;
  }
  if (content.meta != nullptr) {
    SectionHeader(w, kTagMeta, content.metaLength);
    w.Bytes(content.meta, content.metaLength);
    w.Zeros(Padded(content.metaLength) - content.metaLength);
    ++sections;
  }
  if (content.json != nullptr) {
    SectionHeader(w, kTagJson, content.jsonLength);
    w.Bytes(content.json, content.jsonLength);
    w.Zeros(Padded(content.jsonLength) - content.jsonLength);
    ++sections;
  }
  if (!w.ok) return Refuse(diagnostic, PresetError::TooLarge);
  Seal(out, static_cast<uint32_t>(w.n), sections, stat, mode, ctrl);
  return w.n;
}

size_t ReencodePackage(const PresetState& state, const uint8_t* original, size_t originalLength,
                       uint16_t addFlags, uint8_t* out, size_t capacity,
                       PresetDiagnostic* diagnostic) noexcept {
  if (diagnostic != nullptr) *diagnostic = PresetDiagnostic{};
  // The original's container, as DecodePreset reads it (its hashes and contents are not
  // re-checked: the caller decoded it).
  if (original == nullptr || originalLength < kPackageHeaderBytes) {
    return Refuse(diagnostic, PresetError::Truncated);
  }
  if (originalLength > kMaxPackageBytes) return Refuse(diagnostic, PresetError::TooLarge);
  const auto     n     = static_cast<uint32_t>(originalLength);
  const uint16_t flags = static_cast<uint16_t>(Rd16(original + 6) | addFlags);
  if (original[0] != 'B' || original[1] != 'S' || original[2] != 'P' || original[3] != 'K') {
    return Refuse(diagnostic, PresetError::Magic);
  }
  if (Rd16(original + 4) != kPackageFormat) return Refuse(diagnostic, PresetError::PackageFormat);
  if (Rd32(original + 12) != kBlobFormat) return Refuse(diagnostic, PresetError::BlobFormat);
  if ((flags & ~kPackageFlagsKnown) != 0u) return Refuse(diagnostic, PresetError::HeaderFlags);
  if (state.soundRev == 0u) return Refuse(diagnostic, PresetError::SoundRevision);
  if (!CheckState(state, diagnostic)) return 0;
  // First pass: the sections' bounds and the two fixed places, and whether CTRL is there.
  const uint32_t sectionCount = Rd32(original + 24);
  bool           hadCtrl      = false;
  uint32_t       off          = kPackageHeaderBytes;
  for (uint32_t s = 0; s < sectionCount; ++s) {
    if (n - off < 8u) return Refuse(diagnostic, PresetError::SectionBounds);
    const uint32_t tag = Rd32(original + off), len = Rd32(original + off + 4);
    if (len > n - off - 8u || Padded(len) > n - off - 8u) {
      return Refuse(diagnostic, PresetError::SectionBounds, tag);
    }
    if ((s == 0u) != (tag == kTagStat) || (s == 1u) != (tag == kTagMode)) {
      return Refuse(diagnostic, PresetError::SectionOrder, tag);
    }
    hadCtrl = hadCtrl || tag == kTagCtrl;
    off += 8u + Padded(len);
  }
  if (off != n || sectionCount < 2u) return Refuse(diagnostic, PresetError::SectionBounds);
  // Second pass: the new package.
  Writer w{out, capacity < kMaxPackageBytes ? capacity : kMaxPackageBytes};
  WriteHeader(w, flags, state.soundRev, Rd32(original + 16));
  SectionSpan stat, mode, ctrl;
  uint32_t    sections = 0;
  auto        writeCtrl = [&] {
    if (state.control.present == 0u) return;  // a CTRL the state dropped
    SectionHeader(w, kTagCtrl, ControlBytes(state.control));
    ctrl.offset = static_cast<uint32_t>(w.n);
    WriteControl(w, state.control);
    ctrl.length = static_cast<uint32_t>(w.n) - ctrl.offset;
    ++sections;
  };
  off = kPackageHeaderBytes;
  for (uint32_t s = 0; s < sectionCount; ++s) {
    const uint32_t tag = Rd32(original + off), len = Rd32(original + off + 4);
    const uint8_t* payload = original + off + 8;
    off += 8u + Padded(len);
    if (tag == kTagStat) {
      SectionHeader(w, kTagStat, StatBytes(state));
      stat.offset = static_cast<uint32_t>(w.n);
      WriteStat(w, state);
      stat.length = static_cast<uint32_t>(w.n) - stat.offset;
      ++sections;
    } else if (tag == kTagMode) {
      SectionHeader(w, kTagMode, 0);  // the length is patched below
      mode.offset = static_cast<uint32_t>(w.n);
      WriteMode(w, state.mode);
      mode.length = static_cast<uint32_t>(w.n) - mode.offset;
      if (w.ok) Wr32(out + mode.offset - 4, mode.length);
      ++sections;
      if (!hadCtrl) writeCtrl();  // a new CTRL goes right after MODE
    } else if (tag == kTagCtrl) {
      writeCtrl();
    } else {
      // META (checked against the new mode's macros), JSON and unknown sections: verbatim.
      if (tag == kTagMeta && !ParseMeta(payload, len, 0, state.mode, nullptr, diagnostic)) {
        return 0;
      }
      SectionHeader(w, tag, len);
      w.Bytes(payload, len);
      w.Zeros(Padded(len) - len);
      ++sections;
    }
  }
  if (!w.ok) return Refuse(diagnostic, PresetError::TooLarge);
  Seal(out, static_cast<uint32_t>(w.n), sections, stat, mode, ctrl);
  return w.n;
}

}  // namespace brainscape
