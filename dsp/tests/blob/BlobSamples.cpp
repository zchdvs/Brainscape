#include "BlobSamples.h"

#include <cstdio>
#include <cstring>

#include "../../src/blob/Blob.h"
#include "brainscape/Sha256.h"
#include "brainscape/SoundRevision.h"

namespace brainscape::blobtest {

uint32_t Bits(float v) {
  uint32_t u = 0;
  std::memcpy(&u, &v, sizeof u);
  return u;
}

float FromBits(uint32_t bits) {
  float f = 0.f;
  std::memcpy(&f, &bits, sizeof f);
  return f;
}

std::string Hex(const uint8_t* bytes, size_t length) {
  static const char kDigits[] = "0123456789abcdef";
  std::string       out;
  out.reserve(2 * length);
  for (size_t i = 0; i < length; ++i) {
    out.push_back(kDigits[bytes[i] >> 4]);
    out.push_back(kDigits[bytes[i] & 15u]);
  }
  return out;
}

std::unique_ptr<PresetState> CompleteState() {
  auto s      = std::make_unique<PresetState>();
  s->soundRev = kSoundRevision;
  for (size_t i = 0; i < kNumLeafParams; ++i) {
    const ParamDescriptor* d = FindParam(LeafId(i));
    s->leaves[s->leafCount++] = PresetLeaf{static_cast<uint32_t>(d->id), d->def};
  }
  return s;
}

void SetLeaf(PresetState& s, ParamId id, float value) {
  for (uint32_t i = 0; i < s.leafCount; ++i) {
    if (s.leaves[i].id == static_cast<uint32_t>(id)) s.leaves[i].value = value;
  }
}

void PutLeaf(PresetState& s, uint32_t id, float value) {
  uint32_t i = 0;
  while (i < s.leafCount && s.leaves[i].id < id) ++i;
  if (i < s.leafCount && s.leaves[i].id == id) {
    s.leaves[i].value = value;
    return;
  }
  for (uint32_t k = s.leafCount; k > i; --k) s.leaves[k] = s.leaves[k - 1];
  s.leaves[i] = PresetLeaf{id, value};
  ++s.leafCount;
}

Bytes MetaFor(const ModeBlob& mode, const char* id, const char* name, PresetFamily family,
              uint32_t tags, uint32_t displayNames) {
  static const char* const kTags[]  = {"fixture", "granular", "delay", "pad",
                                       "pluck",   "shimmer",  "glitch", "drone"};
  static const char* const kNames[] = {"Smear", "Echoes", "Contour", "Span",
                                       "Space", "Tone",   "Aux A",   "Aux B"};
  auto text = [](const char* s) { return MetaText{s, static_cast<uint32_t>(std::strlen(s))}; };
  MetaContent m;
  m.id          = text(id);
  m.name        = text(name);
  m.family      = family;
  m.author      = text("Brainscape tests");
  m.description = text("A package the tests build with the encoder.");
  m.tagCount    = static_cast<uint8_t>(tags);
  for (uint32_t i = 0; i < tags; ++i) m.tags[i] = text(kTags[i]);
  for (uint32_t i = 0; i < displayNames && i < mode.macros.macroCount; ++i) {
    m.displayMacro[i] = mode.macros.macros[i].id;
    m.displayName[i]  = text(kNames[i]);
    ++m.displayNameCount;
  }
  Bytes    out(2048);
  uint32_t n = 0;
  if (!EncodeMeta(m, mode, out.data(), out.size(), &n)) return Bytes();
  out.resize(n);
  return out;
}

Bytes Package(const PresetState& state, const Bytes* meta, const Bytes* json, uint16_t flags,
              uint32_t schemaVersion, PresetDiagnostic* diagnostic) {
  PackageContent c;
  c.flags         = flags;
  c.schemaVersion = schemaVersion;
  if (meta != nullptr) {
    c.meta       = meta->data();
    c.metaLength = static_cast<uint32_t>(meta->size());
  }
  if (json != nullptr) {
    c.json       = json->data();
    c.jsonLength = static_cast<uint32_t>(json->size());
  }
  Bytes        out(kMaxPackageBytes);
  const size_t n = EncodePackage(state, c, out.data(), out.size(), diagnostic);
  out.resize(n);
  return out;
}

uint32_t Rd32(const uint8_t* p) { return blob::Rd32(p); }
void     Wr32(uint8_t* p, uint32_t v) { blob::Wr32(p, v); }

namespace {

uint32_t Padded(uint32_t n) { return n + ((4u - (n & 3u)) & 3u); }

}  // namespace

size_t FindSection(const Bytes& b, uint32_t tag) {
  size_t off = kPackageHeaderBytes;
  while (b.size() >= 8 && off <= b.size() - 8) {
    const uint32_t t = Rd32(&b[off]), len = Rd32(&b[off + 4]);
    if (t == tag) return off;
    if (len > b.size() - off - 8) return 0;
    off += 8u + Padded(len);
  }
  return 0;
}

size_t FindChunk(const Bytes& b, uint32_t tag) {
  const size_t mode = FindSection(b, kTagMode);
  if (mode == 0) return 0;
  const size_t end = mode + 8 + Rd32(&b[mode + 4]);
  size_t       off = mode + 16;
  while (off + 8 <= end) {
    if (Rd32(&b[off]) == tag) return off;
    off += 8u + Rd32(&b[off + 4]);
  }
  return 0;
}

void InsertSection(Bytes& b, size_t at, uint32_t tag, const Bytes& payload) {
  Bytes s(8 + Padded(static_cast<uint32_t>(payload.size())), 0);
  Wr32(&s[0], tag);
  Wr32(&s[4], static_cast<uint32_t>(payload.size()));
  if (!payload.empty()) std::memcpy(&s[8], payload.data(), payload.size());
  b.insert(b.begin() + static_cast<std::ptrdiff_t>(at), s.begin(), s.end());
  Wr32(&b[24], Rd32(&b[24]) + 1);
}

void InsertChunk(Bytes& b, size_t at, uint32_t tag, const Bytes& payload) {
  const size_t mode = FindSection(b, kTagMode);
  Bytes        c(8 + payload.size(), 0);
  Wr32(&c[0], tag);
  Wr32(&c[4], static_cast<uint32_t>(payload.size()));
  if (!payload.empty()) std::memcpy(&c[8], payload.data(), payload.size());
  b.insert(b.begin() + static_cast<std::ptrdiff_t>(at), c.begin(), c.end());
  Wr32(&b[mode + 4], Rd32(&b[mode + 4]) + static_cast<uint32_t>(c.size()));
  Wr32(&b[mode + 12], Rd32(&b[mode + 12]) + 1);
}

void Rehash(Bytes& b, uint32_t sections) {
  if (b.size() < kPackageHeaderBytes) return;
  const auto n = static_cast<uint32_t>(b.size());
  Wr32(&b[20], n);
  if (sections > 0) Wr32(&b[24], sections);
  // STAT and MODE are the first two sections when they parse; CTRL the first so tagged.
  const uint8_t* part[2] = {nullptr, nullptr};
  uint32_t       len[2]  = {0, 0};
  const uint8_t* ctrl    = nullptr;
  uint32_t       ctrlLen = 0;
  uint32_t       off = kPackageHeaderBytes, index = 0;
  while (n - off >= 8u) {
    const uint32_t tag = Rd32(&b[off]), l = Rd32(&b[off + 4]);
    if (l > n - off - 8u) break;
    if (index < 2) {
      part[index] = b.data() + off + 8;  // may be the end, for an empty last section
      len[index]  = l;
    }
    if (tag == kTagCtrl && ctrl == nullptr) {
      ctrl    = b.data() + off + 8;
      ctrlLen = l;
    }
    ++index;
    const uint32_t step = 8u + Padded(l);
    if (step > n - off) break;
    off += step;
  }
  Digest32 h;
  if (part[0] != nullptr && part[1] != nullptr) {
    SoundHash(part[0], len[0], part[1], len[1], &h);
    std::memcpy(&b[32], h.bytes, 32);
  }
  ControlHash(ctrl != nullptr ? ctrl : b.data(), ctrlLen, &h);
  std::memcpy(&b[64], h.bytes, 32);
  std::memset(&b[96], 0, 32);
  Sha256Hasher::Digest(b.data(), b.size(), h.bytes);
  std::memcpy(&b[96], h.bytes, 32);
}

void FullMode(ModeBlob* mode, PresetState* state) {
  ModeBlob& m           = *mode;
  m                     = ModeBlob{};
  m.schedule.sources    = kSourceAll;
  m.schedule.layerCount = 2;
  m.schedule.subdiv     = Subdivision::Half;
  m.schedule.stepOrder  = StepOrder::Shuffle;
  ModeLayer& a          = m.layers[0];
  a.source              = PositionSource::Mark;
  a.baseSync            = 3;
  a.sprayLaw            = SprayLaw::Exp;
  a.markIndex           = 5;
  a.markWalk            = MarkWalk::Cascade;
  a.pinRearm            = PinRearm::Time;
  a.pitchSelect         = PitchSelect::Random;
  a.quantize            = QuantizeMode::Scale;
  a.quantizeRoot        = 2;
  a.modifier[0]         = ModifierOp::Svf;
  a.modifier[1]         = ModifierOp::Crush;
  a.svfBand             = SvfBand::Bandpass;
  a.svfCutoffSource     = CutoffSource::Lfo;
  a.scaleMask           = 0x0AB5;
  a.slotShare           = 0.5f;
  a.markJitter          = 0.25f;
  a.pinRearmMs          = 2500.0f;
  a.glideStStart        = -12.0f;
  a.glideStEnd          = 7.0f;
  ModeLayer& b          = m.layers[1];
  b                     = ModeLayer{};
  b.source              = PositionSource::Grid;
  b.modifier[0]         = ModifierOp::Crush;
  b.slotShare           = 0.5f;
  for (uint32_t l = 0; l < 2; ++l) {
    m.pitch[l].count = 8;
    for (uint32_t i = 0; i < 8; ++i) {
      m.pitch[l].entries[i].st     = -24.0f + 6.0f * static_cast<float>(i + l);
      m.pitch[l].entries[i].weight = static_cast<uint16_t>(1 + 2 * i);
    }
  }
  m.steps.countMax = 16;
  for (uint32_t i = 0; i < 16; ++i) {
    StepEntry& e = m.steps.entries[i];
    e.slot       = static_cast<uint8_t>(15 - i);
    e.ratioIdx   = static_cast<uint8_t>(i % 8);
    e.flags      = static_cast<uint8_t>(i & 1u);
    e.posSel     = 100.0f * static_cast<float>(i);
    e.gain       = 0.5f;
    e.prob       = 1.0f;
  }
  m.modulators[0] = Modulator{ModulatorType::Lfo, 2, 4, 0, 0.0f, 0.0f};
  m.modulators[1] = Modulator{ModulatorType::Envelope, 0, 0, 0, 10.0f, 200.0f};
  m.routes.count  = 8;
  for (uint8_t i = 0; i < 8; ++i) {
    m.routes.entries[i] = Route{static_cast<uint8_t>(i & 1u), static_cast<uint8_t>(i % 6),
                                static_cast<uint8_t>((i >> 1) & 1u), 0, i % 2 ? 0.5f : -0.5f};
  }
  m.links.count = 4;
  for (uint8_t i = 0; i < 4; ++i) {
    m.links.entries[i] = Route{i, static_cast<uint8_t>(i + 1), 0, 0, 0.25f};
  }
  m.dryDuck = DryDuck{10.0f, 120.0f};
  // 8 macros of 4 targets: Leaf rows of this build, not global.mix, distinct within a macro.
  static const uint32_t kLeaves[] = {1, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
                                     16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28};
  MacroTable& t = m.macros;
  t             = MacroTable{};
  t.macroCount  = 8;
  t.targetCount = 32;
  for (uint32_t k = 0; k < 8; ++k) {
    t.macros[k] = MacroDef{69 + k, static_cast<uint8_t>(4 * k), 4, 0};
    for (uint32_t j = 0; j < 4; ++j) {
      const uint32_t         id = kLeaves[(5 * k + j) % (sizeof kLeaves / sizeof kLeaves[0])];
      const ParamDescriptor* d  = FindParam(static_cast<ParamId>(id));
      t.targets[4 * k + j] =
          MacroTarget{id, j % 2 ? d->max : d->min, j % 2 ? d->min : d->max, 0.0f,
                      j == 3 ? 0.5f : 1.0f, j == 0 ? 0.0625f : j == 1 ? 16.0f : 1.0f};
    }
  }
  m.features = RequiredModeFeatures(m);
  ComputeModeHash(m, &m.modeHash);
  if (state != nullptr) {
    state->mode = m;
    // The two layers' voices fit the pool (E11); the leaves are Reserved rows today.
    PutLeaf(*state, static_cast<uint32_t>(ParamId::VoiceCount), 32.0f);
    PutLeaf(*state, static_cast<uint32_t>(ParamId::L1VoiceCount), 32.0f);
    ControlState& c = state->control;
    c               = ControlState{};
    c.macroCount    = 8;
    for (uint32_t k = 0; k < 8; ++k) c.positions[k] = MacroPosition{69 + k, 0.125f * k};
    c.exprCount      = 4;
    c.expressions[0] = ExpressionAssignment{5, 10.0f, 400.0f, 2.0f};
    c.expressions[1] = ExpressionAssignment{69, 0.0f, 1.0f, 1.0f};
    c.expressions[2] = ExpressionAssignment{2, 1.0f, 0.0f, 0.5f};
    c.expressions[3] = ExpressionAssignment{23, 20000.0f, 40.0f, 4.0f};
  }
}

namespace {

// Custom macros: all eight, aux1 and aux2 included, and an empty one.
void CustomMacros(ModeBlob& m) {
  MacroTable& t = m.macros;
  t             = MacroTable{};
  const uint8_t counts[8] = {3, 1, 2, 1, 0, 1, 2, 1};
  uint8_t       first     = 0;
  static const uint32_t kParams[] = {6, 7, 11, 19, 12, 14, 18, 23, 9, 10, 26};
  for (uint32_t k = 0; k < 8; ++k) {
    t.macros[k] = MacroDef{69 + k, first, counts[k], 0};
    first       = static_cast<uint8_t>(first + counts[k]);
  }
  t.macroCount  = 8;
  t.targetCount = first;
  for (uint32_t i = 0; i < first; ++i) {
    const ParamDescriptor* d = FindParam(static_cast<ParamId>(kParams[i]));
    t.targets[i] = MacroTarget{kParams[i], d->min, d->max, i == 2 ? 0.25f : 0.0f, 1.0f,
                               i % 3 == 1 ? 2.0f : 1.0f};
  }
  m.features = RequiredModeFeatures(m);
}

}  // namespace

std::vector<Bytes> Seeds() {
  std::vector<Bytes> seeds;
  {  // The default mode, every r1 leaf, CTRL at its default.
    auto s = CompleteState();
    seeds.push_back(Package(*s));
  }
  {  // Custom macros, CTRL with expression, META, JSON and two unknown sections.
    auto s = CompleteState();
    SetLeaf(*s, ParamId::Mix, 0.35f);
    SetLeaf(*s, ParamId::DelayTimeMs, 405.0f);
    CustomMacros(s->mode);
    ControlState& c = s->control;
    c               = ControlState{};
    c.macroCount    = 8;
    for (uint32_t k = 0; k < 8; ++k) c.positions[k] = MacroPosition{69 + k, k % 2 ? 1.0f : 0.25f};
    c.exprCount      = 2;
    c.expressions[0] = ExpressionAssignment{74, 0.0f, 1.0f, 1.0f};
    c.expressions[1] = ExpressionAssignment{22, 0.0f, 0.5f, 2.0f};
    const Bytes meta = MetaFor(s->mode, "seed.custom", "Custom", PresetFamily::Reverie, 3, 8);
    const char* text = "{\"schema_version\": 1}\n";
    const Bytes json(text, text + std::strlen(text));
    Bytes       b = Package(*s, &meta, &json);
    InsertSection(b, FindSection(b, kTagCtrl), PackageTag('X', 'T', 'R', 'A'), Bytes{1, 2, 3});
    InsertSection(b, b.size(), PackageTag('Z', 'E', 'N', 'D'), Bytes(9, 7));
    Rehash(b);
    seeds.push_back(b);
  }
  {  // FACTORY and JSON_STALE, no CTRL, a minimal META.
    auto s = CompleteState();
    std::memset(static_cast<void*>(&s->control), 0, sizeof s->control);
    const Bytes meta = MetaFor(s->mode, "seed.bare", "Bare", PresetFamily::None, 0, 0);
    seeds.push_back(Package(*s, &meta, nullptr, kPackageFlagFactory | kPackageFlagJsonStale));
  }
  {  // The full vocabulary: every feature, every chunk at its cap.
    auto s = CompleteState();
    FullMode(&s->mode, s.get());
    const Bytes meta = MetaFor(s->mode, "seed.full", "Full", PresetFamily::Misfire, 8, 8);
    seeds.push_back(Package(*s, &meta));
  }
  return seeds;
}

namespace {

uint64_t Mix(uint64_t& s) {  // SplitMix64
  uint64_t z = (s += 0x9e3779b97f4a7c15ull);
  z          = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
  z          = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
  return z ^ (z >> 31);
}

}  // namespace

FuzzResult Fuzz(uint64_t iterations, uint64_t seed) {
  static const uint32_t kInteresting[] = {
      0u,          1u,          2u,          3u,          4u,          7u,          8u,
      0x7Fu,       0x80u,       0xFFu,       0xFFFFu,     0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu,
      0x7F800000u, 0xFF800000u, 0x7FC00000u, 0x00000001u, 0x00800000u, 0x80000000u, 0x3F800000u,
      0x3D800000u, 0x41800000u, 69u,         76u,         77u,         128u,        2000u,
      kTagStat,    kTagMode,    kTagCtrl,    kTagMeta,    kTagJson,    kChunkPset,  kChunkMacr,
      kChunkDuck};
  const std::vector<Bytes> seeds = Seeds();
  FuzzResult               r;
  auto                     a = std::make_unique<PresetState>();
  auto                     b = std::make_unique<PresetState>();
  Sha256Hasher                   digest;
  Bytes                    again(kMaxPackageBytes);
  uint64_t                 s = seed;
  for (uint64_t it = 0; it < iterations; ++it) {
    Bytes          m  = seeds[static_cast<size_t>(it % seeds.size())];
    const uint64_t nm = 1 + Mix(s) % 4;
    for (uint64_t k = 0; k < nm; ++k) {
      const uint64_t op = Mix(s) % 7;
      const uint64_t sz = m.size();
      if (op == 0 && sz > 0) {
        m[static_cast<size_t>(Mix(s) % sz)] ^= static_cast<uint8_t>(1u << (Mix(s) % 8));
      } else if (op == 1 && sz > 0) {
        m[static_cast<size_t>(Mix(s) % sz)] = static_cast<uint8_t>(Mix(s));
      } else if (op == 2 && sz >= 4) {
        const auto     o = static_cast<size_t>(Mix(s) % (sz / 4)) * 4;
        const uint32_t v = kInteresting[Mix(s) % (sizeof kInteresting / sizeof kInteresting[0])];
        Wr32(&m[o], v);
      } else if (op == 3) {
        m.resize(static_cast<size_t>(Mix(s) % (sz + 1)));
      } else if (op == 4) {
        const uint64_t add = Mix(s) % 64;
        for (uint64_t i = 0; i < add; ++i) m.push_back(static_cast<uint8_t>(Mix(s)));
      } else if (op == 5 && sz > 8) {
        const auto     o = static_cast<size_t>(Mix(s) % (sz - 4));
        const uint32_t v = kInteresting[Mix(s) % (sizeof kInteresting / sizeof kInteresting[0])];
        Wr32(&m[o], v);
      } else if (op == 6 && sz > 16) {
        // A small byte value at a random offset: counts, enumerations and indices.
        m[static_cast<size_t>(Mix(s) % sz)] = static_cast<uint8_t>(Mix(s) % 18);
      }
    }
    if (Mix(s) % 4 != 0) Rehash(m);
    // An exactly sized copy, so AddressSanitizer sees any read past the end.
    const Bytes      exact(m);
    const uint8_t*   data = exact.empty() ? nullptr : exact.data();
    PresetDiagnostic da, db;
    const bool okA = DecodePreset(data, exact.size(), a.get(), &da);
    const bool okB = blob::DecodePresetWith(data, exact.size(), b.get(), &db, nullptr, nullptr,
                                            kModeFeatureAll);
    ++r.histogram[static_cast<size_t>(da.error)];
    uint8_t reencoded = 0;
    if (okB) {
      ++r.acceptedAll;
      // An accepted package re-encodes to its own bytes: STAT, MODE and CTRL from the state,
      // the rest carried.
      const size_t n  = ReencodePackage(*b, data, exact.size(), 0, again.data(), again.size());
      const bool   eq = n == exact.size() && std::memcmp(again.data(), data, n) == 0;
      if (!eq) ++r.reencodeMismatches;
      reencoded = eq ? 1 : 2;
      PresetDiagnostic dv;
      if (blob::ValidateModeWith(*b, kModeFeatureAll, &dv)) ++r.validated;
    }
    if (okA) {
      // What this build accepts, every feature accepts too, as the same state.
      ++r.accepted;
      if (!okB || std::memcmp(a.get(), b.get(), sizeof(PresetState)) != 0) ++r.reencodeMismatches;
    }
    uint8_t record[16];
    record[0] = static_cast<uint8_t>(da.error);
    Wr32(record + 1, da.detail);
    record[5] = static_cast<uint8_t>(db.error);
    Wr32(record + 6, db.detail);
    record[10] = reencoded;
    Wr32(record + 11, static_cast<uint32_t>(exact.size()));
    record[15] = 0;
    digest.Update(record, sizeof record);
    ++r.iterations;
  }
  digest.Final(r.digest);
  return r;
}

}  // namespace brainscape::blobtest
