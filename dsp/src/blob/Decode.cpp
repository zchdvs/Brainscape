#include "detail/FpProfilePrivate.h"

#include "brainscape/Preset.h"
#include "brainscape/Sha256.h"

#include "Blob.h"

// The package decoder (docs/design/mode-compiler.md §5.3, §6): bounds-checked byte parsing of
// the header, the sections, STAT, MODE's chunks, CTRL and META, then the structural rules of
// Validate.cpp on what it read. Firmware-shaped: every length is checked against the bytes that
// remain before it is used (the M7's size_t is 32 bits), nothing allocates, and no float passes
// through a floating-point register.
namespace brainscape {

namespace blob {

namespace {

// Known sections, in their order. Unknown sections may follow MODE anywhere.
int SectionRank(uint32_t tag) noexcept {
  switch (tag) {
    case kTagStat: return 0;
    case kTagMode: return 1;
    case kTagCtrl: return 2;
    case kTagMeta: return 3;
    case kTagJson: return 4;
    default: return -1;
  }
}

// MODE's chunks, in their order.
int ChunkRank(uint32_t tag) noexcept {
  switch (tag) {
    case kChunkSchd: return 0;
    case kChunkLayr: return 1;
    case kChunkPset: return 2;
    case kChunkStep: return 3;
    case kChunkMods: return 4;
    case kChunkRout: return 5;
    case kChunkLink: return 6;
    case kChunkDuck: return 7;
    case kChunkMacr: return 8;
    default: return -1;
  }
}

void ReadLayer(const uint8_t* p, ModeLayer* l) noexcept {
  l->source          = static_cast<PositionSource>(p[0]);
  l->baseSync        = p[1];
  l->sprayLaw        = static_cast<SprayLaw>(p[2]);
  l->markIndex       = p[3];
  l->markWalk        = static_cast<MarkWalk>(p[4]);
  l->pinRearm        = static_cast<PinRearm>(p[5]);
  l->pitchSelect     = static_cast<PitchSelect>(p[6]);
  l->quantize        = static_cast<QuantizeMode>(p[7]);
  l->quantizeRoot    = p[8];
  l->modifier[0]     = static_cast<ModifierOp>(p[9]);
  l->modifier[1]     = static_cast<ModifierOp>(p[10]);
  l->svfBand         = static_cast<SvfBand>(p[11]);
  l->svfCutoffSource = static_cast<CutoffSource>(p[12]);
  l->pad             = p[13];
  l->scaleMask       = Rd16(p + 14);
  SetBits(&l->slotShare, Rd32(p + 16));
  SetBits(&l->markJitter, Rd32(p + 20));
  SetBits(&l->pinRearmMs, Rd32(p + 24));
  SetBits(&l->glideStStart, Rd32(p + 28));
  SetBits(&l->glideStEnd, Rd32(p + 32));
}

void ReadPitchSet(const uint8_t* p, PitchSet* s) noexcept {
  s->count = p[0];
  for (int i = 0; i < 3; ++i) s->pad[i] = p[1 + i];
  for (uint32_t i = 0; i < kMaxPitchEntries; ++i) {
    const uint8_t* e = p + 4 + 8 * i;
    SetBits(&s->entries[i].st, Rd32(e));
    s->entries[i].weight = Rd16(e + 4);
    s->entries[i].pad    = Rd16(e + 6);
  }
}

void ReadRoutes(const uint8_t* p, uint8_t* count, uint8_t* pad, Route* entries) noexcept {
  *count = p[0];
  for (int i = 0; i < 3; ++i) pad[i] = p[1 + i];
  for (uint32_t i = 0; i < *count; ++i) {
    const uint8_t* e  = p + 4 + 8 * i;
    entries[i].from   = e[0];
    entries[i].to     = e[1];
    entries[i].layer  = e[2];
    entries[i].pad    = e[3];
    SetBits(&entries[i].amount, Rd32(e + 4));
  }
}

// MODE's payload into `mode`: chunk order, presence and lengths, and every count bounded
// before it sizes a copy. The values themselves are CheckMode's.
bool ReadMode(const uint8_t* p, uint32_t length, uint32_t supported, ModeBlob* mode,
              PresetDiagnostic* d) noexcept {
  if (length > kMaxModeBytes) return Fail(d, PresetError::ModeTooLarge);
  if (length < 8u) return Fail(d, PresetError::ModeLength);
  mode->features = Rd32(p);
  // Declared features this build lacks are named before the chunks are read.
  const uint32_t lacking = mode->features & ~(supported & kModeFeatureAll);
  if (lacking != 0u) return Fail(d, PresetError::UnsupportedFeature, lacking);
  const uint32_t chunkCount = Rd32(p + 4);
  uint32_t       off = 8, chunks = 0;
  int            last = -1;
  bool           sawPset = false, sawDuck = false;
  while (off < length) {
    if (length - off < 8u) return Fail(d, PresetError::ModeLength);
    const uint32_t tag = Rd32(p + off), len = Rd32(p + off + 4);
    off += 8;
    if (len > length - off) return Fail(d, PresetError::ChunkLength, tag);
    const int rank = ChunkRank(tag);
    // A chunk this build does not know is a feature it lacks: never skipped (§5.2).
    if (rank < 0) return Fail(d, PresetError::UnsupportedFeature, tag);
    if (rank <= last) return Fail(d, PresetError::ChunkOrder, tag);
    if (rank > 0 && last < 0) return Fail(d, PresetError::ChunkMissing, kChunkSchd);
    if (rank > 1 && last < 1) return Fail(d, PresetError::ChunkMissing, kChunkLayr);
    last                 = rank;
    const uint8_t* c     = p + off;
    const uint32_t layers = mode->schedule.layerCount;
    switch (tag) {
      case kChunkSchd:
        if (len != 8u) return Fail(d, PresetError::ChunkLength, tag);
        mode->schedule.sources    = c[0];
        mode->schedule.layerCount = c[1];
        mode->schedule.reserved   = c[2];
        mode->schedule.stepOrder  = static_cast<StepOrder>(c[3]);
        for (int i = 0; i < 4; ++i) mode->schedule.pad[i] = c[4 + i];
        // The layer count sizes LAYR and PSET.
        if (c[1] < 1u || c[1] > kMaxModeLayers) return Fail(d, PresetError::ModeCount, tag);
        break;
      case kChunkLayr:
        if (len != 36u * layers) return Fail(d, PresetError::ChunkLength, tag);
        for (uint32_t l = 0; l < layers; ++l) ReadLayer(c + 36 * l, &mode->layers[l]);
        break;
      case kChunkPset:
        if (len != 68u * layers) return Fail(d, PresetError::ChunkLength, tag);
        for (uint32_t l = 0; l < layers; ++l) ReadPitchSet(c + 68 * l, &mode->pitch[l]);
        sawPset = true;
        break;
      case kChunkStep:
        if (len != 260u) return Fail(d, PresetError::ChunkLength, tag);
        mode->steps.countMax = c[0];
        for (int i = 0; i < 3; ++i) mode->steps.pad[i] = c[1 + i];
        for (uint32_t i = 0; i < kMaxSteps; ++i) {
          const uint8_t* e = c + 4 + 16 * i;
          StepEntry&     s = mode->steps.entries[i];
          s.slot           = e[0];
          s.ratioIdx       = e[1];
          s.flags          = e[2];
          s.pad            = e[3];
          SetBits(&s.posSel, Rd32(e + 4));
          SetBits(&s.gain, Rd32(e + 8));
          SetBits(&s.prob, Rd32(e + 12));
        }
        if (mode->steps.countMax == 0u) return Fail(d, PresetError::ChunkDefault, tag);
        break;
      case kChunkMods:
        if (len != 24u) return Fail(d, PresetError::ChunkLength, tag);
        for (uint32_t k = 0; k < kMaxModulators; ++k) {
          const uint8_t* e = c + 12 * k;
          Modulator&     m = mode->modulators[k];
          m.type           = static_cast<ModulatorType>(e[0]);
          m.shape          = e[1];
          m.sync           = e[2];
          m.pad            = e[3];
          SetBits(&m.attackMs, Rd32(e + 4));
          SetBits(&m.releaseMs, Rd32(e + 8));
        }
        if (AllZero(c, len)) return Fail(d, PresetError::ChunkDefault, tag);
        break;
      case kChunkRout:
        if (len < 4u) return Fail(d, PresetError::ChunkLength, tag);
        if (c[0] > kMaxRoutes) return Fail(d, PresetError::ModeCount, tag);
        if (len != 4u + 8u * c[0]) return Fail(d, PresetError::ChunkLength, tag);
        ReadRoutes(c, &mode->routes.count, mode->routes.pad, mode->routes.entries);
        if (c[0] == 0u) return Fail(d, PresetError::ChunkDefault, tag);
        break;
      case kChunkLink:
        if (len < 4u) return Fail(d, PresetError::ChunkLength, tag);
        if (c[0] > kMaxLinks) return Fail(d, PresetError::ModeCount, tag);
        if (len != 4u + 8u * c[0]) return Fail(d, PresetError::ChunkLength, tag);
        ReadRoutes(c, &mode->links.count, mode->links.pad, mode->links.entries);
        if (c[0] == 0u) return Fail(d, PresetError::ChunkDefault, tag);
        break;
      case kChunkDuck:
        if (len != 8u) return Fail(d, PresetError::ChunkLength, tag);
        SetBits(&mode->dryDuck.attackMs, Rd32(c));
        SetBits(&mode->dryDuck.releaseMs, Rd32(c + 4));
        if (DryDuckDefault(mode->dryDuck)) return Fail(d, PresetError::ChunkDefault, tag);
        sawDuck = true;
        break;
      case kChunkMacr: {
        if (len < 4u) return Fail(d, PresetError::ChunkLength, tag);
        const uint32_t m = c[0], t = c[1];
        if (m > kMaxMacros || t > kMaxTargets) return Fail(d, PresetError::ModeCount, tag);
        if (len != 4u + 8u * m + 24u * t) return Fail(d, PresetError::ChunkLength, tag);
        MacroTable& table = mode->macros;
        table.macroCount  = c[0];
        table.targetCount = c[1];
        table.pad         = Rd16(c + 2);
        for (uint32_t i = 0; i < m; ++i) {
          const uint8_t* e     = c + 4 + 8 * i;
          table.macros[i].id    = Rd32(e);
          table.macros[i].first = e[4];
          table.macros[i].count = e[5];
          table.macros[i].pad   = Rd16(e + 6);
        }
        for (uint32_t i = 0; i < t; ++i) {
          const uint8_t* e      = c + 4 + 8 * m + 24 * i;
          table.targets[i].param = Rd32(e);
          SetBits(&table.targets[i].lo, Rd32(e + 4));
          SetBits(&table.targets[i].hi, Rd32(e + 8));
          SetBits(&table.targets[i].inLo, Rd32(e + 12));
          SetBits(&table.targets[i].inHi, Rd32(e + 16));
          SetBits(&table.targets[i].curve, Rd32(e + 20));
        }
        break;
      }
      default: break;
    }
    off += len;
    ++chunks;
  }
  if (last < 8) return Fail(d, PresetError::ChunkMissing, last < 0 ? kChunkSchd
                                                         : last < 1 ? kChunkLayr
                                                                    : kChunkMacr);
  if (chunks != chunkCount) return Fail(d, PresetError::ChunkCount);
  // Absent optional chunks hold their defaults; a present one must differ from them.
  const uint32_t layers = mode->schedule.layerCount;
  if (sawPset) {
    if (PitchSetsDefault(*mode)) return Fail(d, PresetError::ChunkDefault, kChunkPset);
  } else {
    for (uint32_t l = 0; l < layers; ++l) {
      std::memcpy(&mode->pitch[l], &kDefaultPitchSet, sizeof(PitchSet));
    }
  }
  if (!sawDuck) {
    SetBits(&mode->dryDuck.attackMs, kFDuckAtkDef);
    SetBits(&mode->dryDuck.releaseMs, kFDuckRelDef);
  }
  return true;
}

bool ReadControl(const uint8_t* p, uint32_t length, ControlState* c,
                 PresetDiagnostic* d) noexcept {
  if (length < 4u) return Fail(d, PresetError::CtrlLength);
  const uint32_t m = p[0], e = p[1];
  if (m > kMaxMacros || e > kMaxExpressions) return Fail(d, PresetError::CtrlCount);
  if (length != 4u + 8u * m + 16u * e) return Fail(d, PresetError::CtrlLength);
  if (Rd16(p + 2) != 0u) return Fail(d, PresetError::CtrlPadding);
  c->present    = 1;
  c->macroCount = p[0];
  c->exprCount  = p[1];
  for (uint32_t i = 0; i < m; ++i) {
    c->positions[i].macroId = Rd32(p + 4 + 8 * i);
    SetBits(&c->positions[i].position, Rd32(p + 8 + 8 * i));
  }
  for (uint32_t i = 0; i < e; ++i) {
    const uint8_t* x        = p + 4 + 8 * m + 16 * i;
    c->expressions[i].target = Rd32(x);
    SetBits(&c->expressions[i].lo, Rd32(x + 4));
    SetBits(&c->expressions[i].hi, Rd32(x + 8));
    SetBits(&c->expressions[i].curve, Rd32(x + 12));
  }
  return true;
}

// Strict UTF-8 (RFC 3629: no overlong form, surrogate or code point past U+10FFFF) without a
// control character (C0, DEL, C1).
bool ValidText(const uint8_t* s, uint32_t n) noexcept {
  uint32_t i = 0;
  while (i < n) {
    const uint8_t c = s[i];
    if (c < 0x80u) {
      if (c < 0x20u || c == 0x7Fu) return false;
      ++i;
      continue;
    }
    uint32_t extra;
    uint8_t  lo = 0x80u, hi = 0xBFu;  // the second byte's range
    if (c >= 0xC2u && c <= 0xDFu) {
      extra = 1;
      if (c == 0xC2u) lo = 0xA0u;  // U+0080-U+009F are the C1 controls
    } else if (c >= 0xE0u && c <= 0xEFu) {
      extra = 2;
      if (c == 0xE0u) lo = 0xA0u;
      if (c == 0xEDu) hi = 0x9Fu;
    } else if (c >= 0xF0u && c <= 0xF4u) {
      extra = 3;
      if (c == 0xF0u) lo = 0x90u;
      if (c == 0xF4u) hi = 0x8Fu;
    } else {
      return false;
    }
    if (n - i <= extra) return false;
    if (s[i + 1] < lo || s[i + 1] > hi) return false;
    for (uint32_t k = 2; k <= extra; ++k) {
      if (s[i + k] < 0x80u || s[i + k] > 0xBFu) return false;
    }
    i += extra + 1;
  }
  return true;
}

bool ValidId(const uint8_t* s, uint32_t n) noexcept {
  for (uint32_t i = 0; i < n; ++i) {
    const uint8_t c = s[i];
    const bool    ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                    c == '-';
    if (!ok) return false;
  }
  return true;
}

bool SameText(const uint8_t* s, uint32_t n, const char* word) noexcept {
  uint32_t i = 0;
  for (; i < n && word[i] != '\0'; ++i) {
    if (s[i] != static_cast<uint8_t>(word[i])) return false;
  }
  return i == n && word[i] == '\0';
}

// A reader over META that fails on any read past its end.
struct MetaReader {
  const uint8_t* p;
  uint32_t       length;
  uint32_t       base;  // META's offset in the package, for the spans
  uint32_t       off = 0;

  bool U8(uint8_t* v) noexcept {
    if (length - off < 1u) return false;
    *v = p[off++];
    return true;
  }
  bool U32(uint32_t* v) noexcept {
    if (length - off < 4u) return false;
    *v = Rd32(p + off);
    off += 4;
    return true;
  }
  bool Str(SectionSpan* s, const uint8_t** text) noexcept {
    if (length - off < 2u) return false;
    const uint32_t n = Rd16(p + off);
    off += 2;
    if (n > length - off) return false;
    s->offset = base + off;
    s->length = n;
    *text     = p + off;
    off += n;
    return true;
  }
};

}  // namespace

bool ParseMeta(const uint8_t* p, uint32_t length, uint32_t base, const ModeBlob& mode,
               PresetMeta* meta, PresetDiagnostic* d) noexcept {
  PresetMeta     local;
  PresetMeta&    m = meta != nullptr ? *meta : local;
  m                = PresetMeta{};
  MetaReader     r{p, length, base};
  const uint8_t* text = nullptr;
  SectionSpan    family;
  // id: [a-z0-9._-], 1-48 bytes; name: 1-32 bytes; family: its enumeration; author and
  // description: 0-64 and 0-512 bytes (§2.2).
  if (!r.Str(&m.id, &text)) return Fail(d, PresetError::MetaLength);
  if (m.id.length < 1u || m.id.length > kMaxIdBytes || !ValidId(text, m.id.length)) {
    return Fail(d, PresetError::MetaText, 0);
  }
  if (!r.Str(&m.name, &text)) return Fail(d, PresetError::MetaLength);
  if (m.name.length < 1u || m.name.length > kMaxNameBytes || !ValidText(text, m.name.length)) {
    return Fail(d, PresetError::MetaText, 1);
  }
  if (!r.Str(&family, &text)) return Fail(d, PresetError::MetaLength);
  static const char* const kFamilies[] = {"none", "recall", "reverie", "misfire", "echoic"};
  bool                     known       = false;
  for (uint8_t f = 0; f < 5u && !known; ++f) {
    if (SameText(text, family.length, kFamilies[f])) {
      m.family = static_cast<PresetFamily>(f);
      known    = true;
    }
  }
  if (!known) return Fail(d, PresetError::MetaFamily);
  if (!r.Str(&m.author, &text)) return Fail(d, PresetError::MetaLength);
  if (m.author.length > kMaxAuthorBytes || !ValidText(text, m.author.length)) {
    return Fail(d, PresetError::MetaText, 3);
  }
  if (!r.Str(&m.description, &text)) return Fail(d, PresetError::MetaLength);
  if (m.description.length > kMaxDescriptionBytes || !ValidText(text, m.description.length)) {
    return Fail(d, PresetError::MetaText, 4);
  }
  // Tags: up to 8, each 1-32 bytes, no repeats.
  if (!r.U8(&m.tagCount)) return Fail(d, PresetError::MetaLength);
  if (m.tagCount > kMaxTags) return Fail(d, PresetError::MetaText, 5);
  for (uint32_t i = 0; i < m.tagCount; ++i) {
    if (!r.Str(&m.tags[i], &text)) return Fail(d, PresetError::MetaLength);
    if (m.tags[i].length < 1u || m.tags[i].length > kMaxTagBytes ||
        !ValidText(text, m.tags[i].length)) {
      return Fail(d, PresetError::MetaText, 6);
    }
    for (uint32_t j = 0; j < i; ++j) {
      if (m.tags[j].length == m.tags[i].length &&
          SameBytes(p + (m.tags[j].offset - base), text, m.tags[i].length)) {
        return Fail(d, PresetError::MetaDuplicate, i);
      }
    }
  }
  // Display names: only for macros MACR defines, by ascending id, each 1-16 bytes.
  if (!r.U8(&m.displayNameCount)) return Fail(d, PresetError::MetaLength);
  if (m.displayNameCount > kMaxMacros) return Fail(d, PresetError::MetaDisplayName);
  for (uint32_t i = 0; i < m.displayNameCount; ++i) {
    uint32_t id = 0;
    if (!r.U32(&id)) return Fail(d, PresetError::MetaLength);
    bool defined = false;
    for (uint32_t k = 0; k < mode.macros.macroCount && k < kMaxMacros; ++k) {
      defined = defined || mode.macros.macros[k].id == id;
    }
    if (!defined || (i > 0 && id <= m.displayMacro[i - 1])) {
      return Fail(d, PresetError::MetaDisplayName, id);
    }
    m.displayMacro[i] = id;
    if (!r.Str(&m.displayName[i], &text)) return Fail(d, PresetError::MetaLength);
    if (m.displayName[i].length < 1u || m.displayName[i].length > kMaxDisplayNameBytes ||
        !ValidText(text, m.displayName[i].length)) {
      return Fail(d, PresetError::MetaText, 7);
    }
  }
  if (r.off != length) return Fail(d, PresetError::MetaLength);
  return true;
}

bool DecodePresetWith(const void* bytes, size_t length, PresetState* out, PresetDiagnostic* d,
                      PackageInfo* info, PresetMeta* meta, uint32_t supported) noexcept {
  if (d != nullptr) *d = PresetDiagnostic{};
  const auto* p = static_cast<const uint8_t*>(bytes);
  // The header.
  if (p == nullptr || length < kPackageHeaderBytes) return Fail(d, PresetError::Truncated);
  if (length > kMaxPackageBytes) return Fail(d, PresetError::TooLarge);
  const auto n = static_cast<uint32_t>(length);
  if (p[0] != 'B' || p[1] != 'S' || p[2] != 'P' || p[3] != 'K') {
    return Fail(d, PresetError::Magic);
  }
  if (Rd16(p + 4) != kPackageFormat) return Fail(d, PresetError::PackageFormat, Rd16(p + 4));
  const uint16_t flags = Rd16(p + 6);
  if ((flags & ~kPackageFlagsKnown) != 0u) {
    return Fail(d, PresetError::HeaderFlags, flags & ~kPackageFlagsKnown);
  }
  if (Rd32(p + 20) != n) return Fail(d, PresetError::TotalBytes);
  if (Rd32(p + 28) != 0u) return Fail(d, PresetError::HeaderReserved);
  if (Rd32(p + 12) != kBlobFormat) return Fail(d, PresetError::BlobFormat, Rd32(p + 12));
  if (Rd32(p + 16) == 0u) return Fail(d, PresetError::SchemaVersion);
  if (Rd32(p + 8) == 0u) return Fail(d, PresetError::SoundRevision);
  {
    // package_hash: the whole package with its own field zeroed.
    static const uint8_t kZeros[32] = {};
    Sha256Hasher         sha;
    uint8_t              digest[32];
    sha.Update(p, 96);
    sha.Update(kZeros, sizeof kZeros);
    sha.Update(p + kPackageHeaderBytes, n - kPackageHeaderBytes);
    sha.Final(digest);
    if (!SameBytes(digest, p + 96, 32)) return Fail(d, PresetError::PackageHash);
  }
  // The sections: STAT, then MODE, then CTRL, META and JSON in order, unknown ones anywhere
  // after MODE; every length within the package, padding zero, nothing after the last.
  const uint32_t sectionCount = Rd32(p + 24);
  SectionSpan    spans[5];
  uint32_t       unknown = 0, off = kPackageHeaderBytes;
  int            last    = -1;
  for (uint32_t s = 0; s < sectionCount; ++s) {
    if (n - off < 8u) return Fail(d, PresetError::SectionBounds);
    const uint32_t tag = Rd32(p + off), len = Rd32(p + off + 4);
    off += 8;
    if (len > n - off) return Fail(d, PresetError::SectionBounds, tag);
    const uint32_t padded = len + ((4u - (len & 3u)) & 3u);
    if (padded > n - off) return Fail(d, PresetError::SectionBounds, tag);
    if (!AllZero(p + off + len, padded - len)) return Fail(d, PresetError::SectionPadding, tag);
    const int rank = SectionRank(tag);
    if (s == 0u && rank != 0) {  // STAT first
      return Fail(d, rank == 1 ? PresetError::SectionOrder : PresetError::SectionMissing,
                  kTagStat);
    }
    if (s == 1u && rank != 1) {  // MODE second
      return Fail(d, rank == 0 ? PresetError::SectionDuplicate : PresetError::SectionMissing,
                  rank == 0 ? kTagStat : kTagMode);
    }
    if (rank < 0) {
      ++unknown;
    } else {
      if (rank == last || (rank < 2 && s >= 2u)) {
        return Fail(d, PresetError::SectionDuplicate, tag);
      }
      if (rank < last) return Fail(d, PresetError::SectionOrder, tag);
      last               = rank;
      spans[rank].offset = off;
      spans[rank].length = len;
    }
    off += padded;
  }
  if (off != n) return Fail(d, PresetError::SectionBounds);
  if (sectionCount < 2u) {
    return Fail(d, PresetError::SectionMissing, sectionCount == 0u ? kTagStat : kTagMode);
  }
  const SectionSpan& stat = spans[0];
  const SectionSpan& mode = spans[1];
  const SectionSpan& ctrl = spans[2];
  {
    // sound_hash and control_hash, over their sections' lengths and payloads.
    Digest32 h;
    SoundHash(p + stat.offset, stat.length, p + mode.offset, mode.length, &h);
    if (!SameBytes(h.bytes, p + 32, 32)) return Fail(d, PresetError::SoundHash);
    ControlHash(p + ctrl.offset, ctrl.length, &h);  // an absent CTRL: offset and length 0
    if (!SameBytes(h.bytes, p + 64, 32)) return Fail(d, PresetError::ControlHash);
  }
  // The decoded state starts at zero: every pad, unused entry and absent element.
  std::memset(static_cast<void*>(out), 0, sizeof *out);
  out->soundRev = Rd32(p + 8);
  // STAT: the leaves, then the performance state.
  {
    const uint8_t* s = p + stat.offset;
    if (stat.length < 4u) return Fail(d, PresetError::StatLength);
    const uint32_t count = Rd32(s);
    if (count > PresetState::kMaxLeaves) return Fail(d, PresetError::StatCount);
    if (stat.length != 4u + 8u * count + 8u) return Fail(d, PresetError::StatLength);
    out->leafCount = count;
    for (uint32_t i = 0; i < count; ++i) {
      out->leaves[i].id = Rd32(s + 4 + 8 * i);
      SetBits(&out->leaves[i].value, Rd32(s + 8 + 8 * i));
    }
    const uint8_t* perf          = s + 4 + 8 * count;
    out->performance.reverse     = perf[0];
    out->performance.timeMode    = static_cast<TimeMode>(perf[1]);
    out->performance.subdiv      = static_cast<Subdivision>(perf[2]);
    out->performance.reserved    = perf[3];
    out->performance.usPerQuarter = Rd32(perf + 4);
  }
  if (!ReadMode(p + mode.offset, mode.length, supported, &out->mode, d)) return false;
  if (ctrl.offset != 0u) {  // a payload never starts at 0
    if (!ReadControl(p + ctrl.offset, ctrl.length, &out->control, d)) return false;
  }
  Sha256Hasher::Digest(p + mode.offset, mode.length, out->mode.modeHash.bytes);
  if (!CheckStat(*out, d) || !CheckMode(out->mode, supported, d) ||
      !CheckControl(out->mode, out->control, d)) {
    return false;
  }
  if (spans[3].offset != 0u &&
      !ParseMeta(p + spans[3].offset, spans[3].length, spans[3].offset, out->mode, meta, d)) {
    return false;
  }
  if (spans[3].offset == 0u && meta != nullptr) *meta = PresetMeta{};
  if (info != nullptr) {
    info->packageFormat   = Rd16(p + 4);
    info->flags           = flags;
    info->soundRev        = Rd32(p + 8);
    info->blobFormat      = Rd32(p + 12);
    info->schemaVersion   = Rd32(p + 16);
    info->totalBytes      = n;
    info->sectionCount    = sectionCount;
    for (int i = 0; i < 32; ++i) {
      info->soundHash.bytes[i]   = p[32 + i];
      info->controlHash.bytes[i] = p[64 + i];
      info->packageHash.bytes[i] = p[96 + i];
    }
    info->stat            = spans[0];
    info->mode            = spans[1];
    info->ctrl            = spans[2];
    info->meta            = spans[3];
    info->json            = spans[4];
    info->unknownSections = unknown;
  }
  return true;
}

}  // namespace blob

bool DecodePreset(const void* bytes, size_t length, PresetState* out, PresetDiagnostic* diagnostic,
                  PackageInfo* info, PresetMeta* meta) noexcept {
  return blob::DecodePresetWith(bytes, length, out, diagnostic, info, meta,
                                kSupportedModeFeatures);
}

const char* PresetErrorName(PresetError error) noexcept {
  static const char* const kNames[] = {
      "None",           "Truncated",       "TooLarge",         "Magic",
      "PackageFormat",  "HeaderFlags",     "TotalBytes",       "HeaderReserved",
      "BlobFormat",     "SchemaVersion",   "SoundRevision",    "PackageHash",
      "SectionBounds",  "SectionPadding",  "SectionOrder",     "SectionDuplicate",
      "SectionMissing", "SoundHash",       "ControlHash",      "StatLength",
      "StatCount",      "StatOrder",       "StatValue",        "Performance",
      "StatPadding",    "ModeLength",      "ModeTooLarge",     "ChunkOrder",
      "ChunkMissing",   "ChunkLength",     "ChunkCount",       "ChunkDefault",
      "ModePadding",    "ModeCount",       "ModeEnum",         "ModeValue",
      "ModeRange",      "MacroLayout",     "FeatureMismatch",  "UnsupportedFeature",
      "UnsupportedTarget", "CtrlLength",   "CtrlPositions",    "CtrlValue",
      "CtrlCount",      "CtrlPadding",
      "MetaLength",     "MetaText",        "MetaFamily",       "MetaDisplayName",
      "MetaDuplicate",  "TargetNotLeaf",   "TargetMix",        "TargetAbsent",
      "TargetDuplicate", "TargetRange",    "ShapeEmpty",       "StepRatioIndex",
      "RouteEndpoint",  "LayerBudget",     "AbsentLeaf",       "ExpressionTarget",
      "ExpressionRange"};
  static_assert(sizeof kNames / sizeof kNames[0] == static_cast<size_t>(PresetError::kCount),
                "a name per PresetError");
  const auto i = static_cast<size_t>(error);
  return i < static_cast<size_t>(PresetError::kCount) ? kNames[i] : "?";
}

}  // namespace brainscape
