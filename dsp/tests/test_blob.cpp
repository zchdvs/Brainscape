// The compiled preset and the .bsp package (docs/design/mode-compiler.md §5, §6; lane B): the
// SHA-256 core, the random-number key extension (§7.5, R8), the default mode, the encoder's
// one-encoding rule, every rule of the decoder and validator (§5.3) with its error code, the
// frozen fixtures (§10.3) and a short run of the mutation fuzzer (§10.2).
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "blob/Blob.h"
#include "blob/BlobSamples.h"
#include "brainscape/Engine.h"
#include "brainscape/Mode.h"
#include "brainscape/Preset.h"
#include "brainscape/Sha256.h"
#include "brainscape/SoundRevision.h"
#include "catch.hpp"
#include "detail/GrainMath.h"
#include "golden/Sha256.h"

using namespace brainscape;
using namespace brainscape::blobtest;

namespace {

constexpr uint32_t kAnyDetail = 0xFFFFFFFFu;

struct Decoded {
  bool                         ok = false;
  PresetDiagnostic             d;
  std::unique_ptr<PresetState> s = std::make_unique<PresetState>();
  PackageInfo                  info;
  PresetMeta                   meta;
};

Decoded Decode(const Bytes& b, uint32_t features = kSupportedModeFeatures) {
  Decoded r;
  r.ok = blob::DecodePresetWith(b.empty() ? nullptr : b.data(), b.size(), r.s.get(), &r.d,
                                &r.info, &r.meta, features);
  return r;
}

std::string Verdict(const PresetDiagnostic& d) {
  return std::string(PresetErrorName(d.error)) + "/" + std::to_string(d.detail);
}

void Expect(const Bytes& b, PresetError e, uint32_t detail = kAnyDetail,
            uint32_t features = kSupportedModeFeatures) {
  const Decoded r = Decode(b, features);
  INFO("verdict " << Verdict(r.d) << ", expected " << PresetErrorName(e) << "/" << detail);
  REQUIRE_FALSE(r.ok);
  REQUIRE(r.d.error == e);
  if (detail != kAnyDetail) REQUIRE(r.d.detail == detail);
}

void ExpectValid(const PresetState& s, PresetError e, uint32_t detail = kAnyDetail,
                 uint32_t features = kSupportedModeFeatures) {
  PresetDiagnostic d;
  const bool       ok = blob::ValidateModeWith(s, features, &d);
  INFO("verdict " << Verdict(d) << ", expected " << PresetErrorName(e) << "/" << detail);
  if (e == PresetError::None) {
    REQUIRE(ok);
    return;
  }
  REQUIRE_FALSE(ok);
  REQUIRE(d.error == e);
  if (detail != kAnyDetail) REQUIRE(d.detail == detail);
}

Bytes Text(const char* s) { return Bytes(s, s + std::strlen(s)); }

// A package with every section: the r1 leaves, the default mode, CTRL, META and JSON.
Bytes Base(PresetState* keep = nullptr) {
  auto        s    = CompleteState();
  const Bytes meta = MetaFor(s->mode, "test.base", "Base", PresetFamily::Echoic, 2, 6);
  const Bytes json = Text("{\"schema_version\": 1}\n");
  Bytes       b    = Package(*s, &meta, &json);
  if (keep != nullptr) *keep = *s;
  return b;
}

// Payload offset of a section or chunk.
size_t Payload(const Bytes& b, uint32_t tag, bool chunk = false) {
  const size_t at = chunk ? FindChunk(b, tag) : FindSection(b, tag);
  REQUIRE(at != 0u);
  return at + 8;
}

void RemoveChunk(Bytes& b, uint32_t tag) {
  const size_t mode = FindSection(b, kTagMode), at = FindChunk(b, tag);
  REQUIRE(at != 0u);
  const uint32_t length = 8 + Rd32(&b[at + 4]);
  b.erase(b.begin() + static_cast<std::ptrdiff_t>(at),
          b.begin() + static_cast<std::ptrdiff_t>(at + length));
  Wr32(&b[mode + 4], Rd32(&b[mode + 4]) - length);
  Wr32(&b[mode + 12], Rd32(&b[mode + 12]) - 1);
}

void FixPackageHash(Bytes& b) {
  std::memset(&b[96], 0, 32);
  uint8_t h[32];
  Sha256Hasher::Digest(b.data(), b.size(), h);
  std::memcpy(&b[96], h, 32);
}

std::string Sha(const void* data, size_t length) {
  uint8_t h[32];
  Sha256Hasher::Digest(data, length, h);
  return Hex(h, 32);
}

}  // namespace

// ── SHA-256 ───────────────────────────────────────────────────────────────────────────────

TEST_CASE("SHA-256: the FIPS 180-4 examples, whole and fed in pieces", "[blob][sha256]") {
  REQUIRE(Sha("", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  REQUIRE(Sha("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  const char* two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  REQUIRE(Sha(two, std::strlen(two)) ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  const std::string million(1000000, 'a');
  REQUIRE(Sha(million.data(), million.size()) ==
          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
  // Fed in uneven pieces, and through the tests' hex wrapper, which resets for reuse.
  golden::Sha256 sha;
  for (size_t i = 0; i < million.size();) {
    const size_t n = 1 + (i * 7919) % 131;
    sha.Update(million.data() + i, std::min(n, million.size() - i));
    i += n;
  }
  REQUIRE(sha.Hex() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
  sha.Update("abc", 3);
  REQUIRE(sha.Hex() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  {
    // The core is Sha256Hasher, not Sha256, so code that opens both namespaces (the golden
    // harness's mains, the firmware bench image) still names the tests' Sha256 unambiguously.
    using namespace brainscape::golden;
    Sha256 both;
    both.Update("abc", 3);
    REQUIRE(both.Hex() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  }
}

// ── The key extension (R8) ────────────────────────────────────────────────────────────────

TEST_CASE("Key extension: ext 0 is sound revision 1's key", "[blob][keys]") {
  using grainmath::Draw;
  using grainmath::DrawKey;
  using grainmath::RandUnit;
  const int64_t frames[] = {0, 1, 12345, (int64_t{1} << 29) - 1, int64_t{1} << 29,
                            (int64_t{1} << 40) + 77, (int64_t{1} << 62) + 5};
  for (const int64_t f : frames) {
    for (uint32_t p = 0; p < 8; ++p) {
      const auto d = static_cast<Draw>(p);
      REQUIRE(DrawKey(f, d, 0, 0) == DrawKey(f, d));
      REQUIRE(Bits(RandUnit(f, d, 0, 0)) == Bits(RandUnit(f, d)));
      REQUIRE(grainmath::DrawKeyExtension(0, 0, d) == 0u);
    }
  }
  REQUIRE(grainmath::DrawKeyExtension(1, 0, Draw::Spray) == 1u);
  REQUIRE(grainmath::DrawKeyExtension(0, 7, Draw::Spray) == 14u);
  REQUIRE(grainmath::DrawKeyExtension(0, 0, Draw::PitchSelect) == 16u);
  REQUIRE(grainmath::DrawKeyExtension(1, 3, Draw::RandomCutoff) == (1u | 6u | 16u));
  REQUIRE(static_cast<uint32_t>(Draw::kMaxPurpose) == 31u);
}

TEST_CASE("Key extension: extended keys at one frame are pairwise distinct", "[blob][keys]") {
  using grainmath::Hash32;
  // Two nonzero exts can only collide at one frame if Hash32(e1) ^ Hash32(e2) is the XOR of two
  // purposes' low bits (0-7): it never is.
  for (uint32_t e1 = 1; e1 < 64; ++e1) {
    for (uint32_t e2 = e1 + 1; e2 < 64; ++e2) REQUIRE((Hash32(e1) ^ Hash32(e2)) > 7u);
  }
  // Every (purpose, layer, ordinal) at a frame, purposes 0-31: 512 extended keys, distinct.
  for (const int64_t f : {int64_t{0}, int64_t{48000}, int64_t{1} << 33}) {
    std::vector<uint32_t> keys;
    for (uint32_t p = 0; p < 32; ++p) {
      for (uint32_t layer = 0; layer < 2; ++layer) {
        for (uint32_t ordinal = 0; ordinal < 8; ++ordinal) {
          const auto d = static_cast<grainmath::Draw>(p);
          if (grainmath::DrawKeyExtension(layer, ordinal, d) == 0u) continue;
          keys.push_back(grainmath::DrawKey(f, d, layer, ordinal));
        }
      }
    }
    REQUIRE(keys.size() == 32u * 16u - 8u);
    std::sort(keys.begin(), keys.end());
    REQUIRE(std::adjacent_find(keys.begin(), keys.end()) == keys.end());
  }
}

TEST_CASE("Key extension: no key aliases an r1 key at a fixed frame offset", "[blob][keys]") {
  using grainmath::Draw;
  using grainmath::DrawKey;
  // Draft v1 XOR-ed ext << 56 into the 64-bit counter before the fold, which equals moving the
  // frame by ext << 21: every layer-1 draw was the layer-0 draw 2^21 frames away (±2^22 for the
  // ordinal, ±2^25 for the high purpose bits; record §2.8). The negative control shows this test
  // sees that; the extension must alias at no such offset.
  auto v1 = [](int64_t f, uint32_t p, uint32_t ext) {
    const uint64_t k64 = (static_cast<uint64_t>(f) * 8u + (p & 7u)) ^ (uint64_t{ext} << 56);
    return static_cast<uint32_t>(k64) ^ static_cast<uint32_t>(k64 >> 32);
  };
  uint64_t v1Aliases = 0, v1Compared = 0, aliases = 0, compared = 0;
  for (uint32_t i = 0; i < 400; ++i) {
    const int64_t f = (int64_t{1} << 30) + static_cast<int64_t>(i) * 104729;
    for (uint32_t ext = 1; ext < 64; ++ext) {
      const uint32_t layer = ext & 1u, ordinal = (ext >> 1) & 7u, high = ext >> 4;
      const int64_t  shift = static_cast<int64_t>(ext) << 21;
      for (uint32_t low = 0; low < 8; ++low) {
        const auto     purpose = static_cast<Draw>(low | (high << 3));
        const uint32_t key     = DrawKey(f, purpose, layer, ordinal);
        for (const int64_t g : {f - shift, f + shift}) {
          for (uint32_t other = 0; other < 8; ++other) {
            aliases += key == DrawKey(g, static_cast<Draw>(other)) ? 1u : 0u;
            ++compared;
          }
        }
        if ((ext & (ext - 1u)) == 0u) {  // one bit: v1's key is the r1 key ext << 21 away
          const uint32_t old = v1(f, low, ext);
          v1Aliases += old == DrawKey(f + shift, static_cast<Draw>(low)) ||
                               old == DrawKey(f - shift, static_cast<Draw>(low))
                           ? 1u
                           : 0u;
          ++v1Compared;
        }
      }
    }
  }
  INFO(compared << " key pairs compared");
  REQUIRE(v1Compared == 400u * 6u * 8u);
  REQUIRE(v1Aliases == v1Compared);  // the control: v1's scheme aliases every one
  REQUIRE(aliases == 0u);
}

// ── Layout, limits, the default mode ──────────────────────────────────────────────────────

TEST_CASE("The bit-pattern limits are their floats", "[blob]") {
  REQUIRE(Bits(0.0f) == blob::kF0);
  REQUIRE(Bits(1.0f) == blob::kFOne);
  REQUIRE(Bits(-1.0f) == blob::kFMinusOne);
  REQUIRE(Bits(1.0f / 16.0f) == blob::kFCurveMin);
  REQUIRE(Bits(16.0f) == blob::kFCurveMax);
  REQUIRE(Bits(24.0f) == blob::kFSt);
  REQUIRE(Bits(-24.0f) == blob::kFMinusSt);
  REQUIRE(Bits(10.0f) == blob::kFRearmMin);
  REQUIRE(Bits(20000.0f) == blob::kFRearmMax);
  REQUIRE(Bits(1000.0f) == blob::kFRearmDef);
  REQUIRE(Bits(5000.0f) == blob::kFPosSelMax);
  REQUIRE(Bits(5000.0f) == blob::kFAttackMax);
  REQUIRE(Bits(20000.0f) == blob::kFReleaseMax);
  REQUIRE(Bits(0.1f) == blob::kFDuckAtkMin);
  REQUIRE(Bits(500.0f) == blob::kFDuckAtkMax);
  REQUIRE(Bits(1.0f) == blob::kFDuckRelMin);
  REQUIRE(Bits(5000.0f) == blob::kFDuckRelMax);
  REQUIRE(Bits(5.0f) == blob::kFDuckAtkDef);
  REQUIRE(Bits(80.0f) == blob::kFDuckRelDef);
  // OrderKey orders values; -0 sorts below +0, and canonical values never hold it.
  const float ordered[] = {-20000.0f, -1.0f, -1e-30f, 0.0f, 1e-30f, 0.5f, 1.0f, 3.0e38f};
  for (size_t i = 1; i < sizeof ordered / sizeof ordered[0]; ++i) {
    REQUIRE(blob::OrderKey(Bits(ordered[i - 1])) < blob::OrderKey(Bits(ordered[i])));
  }
  REQUIRE_FALSE(blob::Canonical(0x80000000u));
  REQUIRE_FALSE(blob::Canonical(0x00000001u));
  REQUIRE_FALSE(blob::Canonical(0x7F800000u));
  REQUIRE_FALSE(blob::Canonical(0x7FC00000u));
  REQUIRE(blob::Canonical(0x00800000u));
}

TEST_CASE("The default mode is r1's structure, and its hash is committed", "[blob]") {
  const auto s = std::make_unique<PresetState>();
  const ModeBlob& m = s->mode;
  REQUIRE(m.features == 0u);
  REQUIRE(RequiredModeFeatures(m) == 0u);
  REQUIRE(m.schedule.sources == kDefaultSources);
  REQUIRE(m.schedule.layerCount == 1u);
  REQUIRE(blob::AllZero(&m.layers[1], sizeof(ModeLayer)));
  REQUIRE(blob::AllZero(&m.pitch[1], sizeof(PitchSet)));
  REQUIRE(m.pitch[0].count == 1u);
  REQUIRE(m.macros.macroCount == 6u);
  REQUIRE(m.macros.targetCount == 9u);
  uint8_t  mode[1536];
  uint32_t n = 0;
  REQUIRE(EncodeMode(m, mode, sizeof mode, &n));
  REQUIRE(n == 344u);  // features, count; SCHD 8; LAYR 36; MACR 4 + 6 x 8 + 9 x 24
  const std::string hash = Sha(mode, n);
  INFO("the default mode's hash is " << hash);
  REQUIRE(hash == Hex(kDefaultModeHash.bytes, 32));
  Digest32 computed;
  REQUIRE(ComputeModeHash(m, &computed));
  REQUIRE(blob::SameBytes(computed.bytes, m.modeHash.bytes, 32));
  // A leafless default state and a complete one are valid; CTRL holds the default positions.
  ExpectValid(*s, PresetError::None);
  ExpectValid(*CompleteState(), PresetError::None);
  REQUIRE(s->control.present == 1u);
  REQUIRE(s->control.macroCount == 6u);
  REQUIRE(s->performance.usPerQuarter == 500000u);
  REQUIRE(s->soundRev == 0u);
}

// ── One encoding ──────────────────────────────────────────────────────────────────────────

TEST_CASE("A decoded package re-encodes to its own bytes", "[blob]") {
  const std::vector<Bytes> seeds = Seeds();
  REQUIRE(seeds.size() == 4u);
  for (size_t i = 0; i < seeds.size(); ++i) {
    INFO("seed " << i);
    const Bytes& b = seeds[i];
    REQUIRE(!b.empty());
    REQUIRE(b.size() <= kMaxPackageBytes);
    const Decoded mine = Decode(b);
    const Decoded all  = Decode(b, kModeFeatureAll);
    REQUIRE(all.ok);
    REQUIRE(mine.ok == (i != 3u));  // the full vocabulary needs features this build lacks
    if (i == 3u) {
      REQUIRE(mine.d.error == PresetError::UnsupportedFeature);
      REQUIRE(mine.d.detail == all.s->mode.features);
    }
    Bytes        again(kMaxPackageBytes);
    const size_t n = ReencodePackage(*all.s, b.data(), b.size(), 0, again.data(), again.size());
    again.resize(n);
    REQUIRE(again == b);
    ExpectValid(*all.s, PresetError::None, kAnyDetail, kModeFeatureAll);
  }
  // Encode, decode: the same state, word for word (zero past every count).
  auto        s    = CompleteState();
  const Bytes b    = Package(*s);
  const Decoded r  = Decode(b);
  REQUIRE(r.ok);
  REQUIRE(std::memcmp(s.get(), r.s.get(), sizeof(PresetState)) == 0);
  REQUIRE(EncodePackage(*r.s, PackageContent{}, std::vector<uint8_t>(kMaxPackageBytes).data(),
                        kMaxPackageBytes) == b.size());
}

TEST_CASE("The largest MODE is 1,528 bytes, every chunk at its cap", "[blob]") {
  auto s = CompleteState();
  FullMode(&s->mode, s.get());
  uint8_t  mode[1536];
  uint32_t n = 0;
  REQUIRE(EncodeMode(s->mode, mode, sizeof mode, &n));
  REQUIRE(n == 1528u);
  REQUIRE(s->mode.features == (kModeFeatureAll & ~kModeFeatureSources));  // every source
  ExpectValid(*s, PresetError::None, kAnyDetail, kModeFeatureAll);
  const Bytes   b = Package(*s);
  const Decoded r = Decode(b, kModeFeatureAll);
  REQUIRE(r.ok);
  REQUIRE(std::memcmp(&s->mode, &r.s->mode, sizeof(ModeBlob)) == 0);
  // This build plays none of it, and says which features it lacks.
  Expect(b, PresetError::UnsupportedFeature, s->mode.features);
  ExpectValid(*s, PresetError::UnsupportedFeature, s->mode.features);
}

TEST_CASE("Hashes: sound_hash is STAT and MODE, control_hash CTRL, modeHash MODE", "[blob]") {
  PresetState base;
  const Bytes a = Base(&base);
  // Another CTRL, META and JSON: the same sound_hash and modeHash.
  auto s = std::make_unique<PresetState>(base);
  s->control.positions[2].position = 0.75f;
  const Bytes meta = MetaFor(s->mode, "test.other", "Other", PresetFamily::None, 0, 0);
  const Bytes b    = Package(*s, &meta);
  REQUIRE(std::memcmp(&a[32], &b[32], 32) == 0);
  REQUIRE(std::memcmp(&a[64], &b[64], 32) != 0);
  const Decoded da = Decode(a), db = Decode(b);
  REQUIRE((da.ok && db.ok));
  REQUIRE(blob::SameBytes(da.s->mode.modeHash.bytes, db.s->mode.modeHash.bytes, 32));
  REQUIRE(blob::SameBytes(da.info.soundHash.bytes, &a[32], 32));
  // A leaf: another sound_hash, the same modeHash.
  SetLeaf(*s, ParamId::Mix, 0.25f);
  const Bytes c  = Package(*s, &meta);
  const Decoded dc = Decode(c);
  REQUIRE(std::memcmp(&b[32], &c[32], 32) != 0);
  REQUIRE(std::memcmp(&b[64], &c[64], 32) == 0);
  REQUIRE(blob::SameBytes(db.s->mode.modeHash.bytes, dc.s->mode.modeHash.bytes, 32));
  // No CTRL: control_hash is SHA-256 of a zero length.
  std::memset(static_cast<void*>(&s->control), 0, sizeof s->control);
  const Bytes  d     = Package(*s);
  const uint8_t zero[4] = {0, 0, 0, 0};
  REQUIRE(Hex(&d[64], 32) == Sha(zero, 4));
  REQUIRE(Decode(d).ok);
  REQUIRE(Decode(d).s->control.present == 0u);
}

// ── The header and the sections ───────────────────────────────────────────────────────────

TEST_CASE("Header rules, one error each", "[blob][decode]") {
  const Bytes base = Base();
  REQUIRE(Decode(base).ok);
  auto edit = [&](auto&& f, bool rehash = true) {
    Bytes b = base;
    f(b);
    if (rehash) Rehash(b);
    return b;
  };
  Expect(Bytes(base.begin(), base.begin() + 127), PresetError::Truncated);
  Expect(Bytes(), PresetError::Truncated);
  Expect(edit([](Bytes& b) { b.resize(kMaxPackageBytes + 1); }, false), PresetError::TooLarge);
  Expect(edit([](Bytes& b) { b[0] = 'X'; }), PresetError::Magic);
  Expect(edit([](Bytes& b) { b[4] = 2; }), PresetError::PackageFormat, 2);
  Expect(edit([](Bytes& b) { b[6] |= 0x10; }), PresetError::HeaderFlags, 0x10);
  Expect(edit([](Bytes& b) { Wr32(&b[20], Rd32(&b[20]) + 4); }, false), PresetError::TotalBytes);
  Expect(edit([](Bytes& b) { Wr32(&b[28], 1); }), PresetError::HeaderReserved);
  Expect(edit([](Bytes& b) { Wr32(&b[12], 2); }), PresetError::BlobFormat, 2);
  Expect(edit([](Bytes& b) { Wr32(&b[16], 0); }), PresetError::SchemaVersion);
  Expect(edit([](Bytes& b) { Wr32(&b[8], 0); }), PresetError::SoundRevision);
  Expect(edit([](Bytes& b) { b[200] ^= 1; }, false), PresetError::PackageHash);
  Expect(edit([](Bytes& b) { b[100] ^= 1; }, false), PresetError::PackageHash);
  // Newer schemas and other sound revisions decode: the package layer depends on neither.
  REQUIRE(Decode(edit([](Bytes& b) { Wr32(&b[16], 7); })).ok);
  REQUIRE(Decode(edit([](Bytes& b) { Wr32(&b[8], 99); })).s->soundRev == 99u);
}

TEST_CASE("Section rules: bounds, padding, order, duplicates, missing", "[blob][decode]") {
  const Bytes base = Base();
  const size_t stat = FindSection(base, kTagStat), mode = FindSection(base, kTagMode),
               meta = FindSection(base, kTagMeta), json = FindSection(base, kTagJson);
  REQUIRE((stat == kPackageHeaderBytes && mode != 0u && meta != 0u && json != 0u));
  {
    Bytes b = base;
    Wr32(&b[json + 4], 100000);
    Rehash(b);
    Expect(b, PresetError::SectionBounds, kTagJson);
  }
  {
    Bytes b = base;  // a byte after the last section
    b.push_back(0);
    Rehash(b);
    Expect(b, PresetError::SectionBounds);
  }
  {
    Bytes b = base;  // JSON's length is not a multiple of 4: its padding must be zero
    const uint32_t len = Rd32(&b[json + 4]);
    REQUIRE((len & 3u) != 0u);
    b[json + 8 + len] = 1;
    Rehash(b);
    Expect(b, PresetError::SectionPadding, kTagJson);
  }
  {
    Bytes b = base;  // CTRL after META
    Bytes c = base;
    const size_t ctrl = FindSection(c, kTagCtrl);
    const Bytes  payload(c.begin() + static_cast<std::ptrdiff_t>(ctrl + 8),
                         c.begin() + static_cast<std::ptrdiff_t>(ctrl + 8 + Rd32(&c[ctrl + 4])));
    b.erase(b.begin() + static_cast<std::ptrdiff_t>(ctrl),
            b.begin() + static_cast<std::ptrdiff_t>(ctrl + 8 + payload.size()));
    Wr32(&b[24], Rd32(&b[24]) - 1);
    InsertSection(b, b.size(), kTagCtrl, payload);
    Rehash(b);
    Expect(b, PresetError::SectionOrder, kTagCtrl);
  }
  {
    Bytes b = base;
    InsertSection(b, b.size(), kTagStat, Bytes(12, 0));
    Rehash(b);
    Expect(b, PresetError::SectionDuplicate, kTagStat);
    Bytes c = base;
    InsertSection(c, c.size(), kTagJson, Bytes{'{', '}'});
    Rehash(c);
    Expect(c, PresetError::SectionDuplicate, kTagJson);
  }
  {
    Bytes b = base;  // MODE renamed: STAT then an unknown section
    Wr32(&b[mode], PackageTag('M', 'O', 'D', 'X'));
    Rehash(b);
    Expect(b, PresetError::SectionMissing, kTagMode);
    Bytes c = base;  // STAT renamed
    Wr32(&c[stat], PackageTag('S', 'T', 'A', 'X'));
    Rehash(c);
    Expect(c, PresetError::SectionMissing, kTagStat);
    Bytes e = base;  // MODE first
    Wr32(&e[stat], kTagMode);
    Rehash(e);
    Expect(e, PresetError::SectionOrder, kTagStat);
    Bytes z(base.begin(), base.begin() + kPackageHeaderBytes);  // no section at all
    Wr32(&z[24], 0);
    Rehash(z);
    Expect(z, PresetError::SectionMissing, kTagStat);
  }
  {
    // Unknown sections after MODE: skipped, counted, carried in place.
    Bytes b = base;
    InsertSection(b, FindSection(b, kTagCtrl), PackageTag('N', 'E', 'W', '1'), Bytes{9});
    InsertSection(b, FindSection(b, kTagJson), PackageTag('N', 'E', 'W', '2'), Bytes(5, 3));
    InsertSection(b, b.size(), PackageTag('N', 'E', 'W', '3'), Bytes());
    Rehash(b);
    const Decoded r = Decode(b);
    REQUIRE(r.ok);
    REQUIRE(r.info.unknownSections == 3u);
    REQUIRE(r.info.sectionCount == 8u);
    Bytes        again(kMaxPackageBytes);
    const size_t n = ReencodePackage(*r.s, b.data(), b.size(), 0, again.data(), again.size());
    again.resize(n);
    REQUIRE(again == b);
    // Before MODE they are not allowed.
    Bytes c = base;
    InsertSection(c, mode, PackageTag('N', 'E', 'W', '0'), Bytes{1});
    Rehash(c);
    Expect(c, PresetError::SectionMissing, kTagMode);
  }
}

TEST_CASE("The sound and control hashes are checked", "[blob][decode]") {
  const Bytes base = Base();
  Bytes       b    = base;
  b[40] ^= 1;
  FixPackageHash(b);
  Expect(b, PresetError::SoundHash);
  Bytes c = base;
  c[70] ^= 1;
  FixPackageHash(c);
  Expect(c, PresetError::ControlHash);
}

// ── STAT ──────────────────────────────────────────────────────────────────────────────────

TEST_CASE("STAT rules: count, length, order, canonical values, performance", "[blob][decode]") {
  const Bytes  base = Base();
  const size_t stat = Payload(base, kTagStat);
  const uint32_t n  = Rd32(&base[stat]);
  REQUIRE(n == kNumLeafParams);
  auto edit = [&](auto&& f) {
    Bytes b = base;
    f(b);
    Rehash(b);
    return b;
  };
  Expect(edit([&](Bytes& b) { Wr32(&b[stat], 129); }), PresetError::StatCount);
  Expect(edit([&](Bytes& b) { Wr32(&b[stat], n - 1); }), PresetError::StatLength);
  Expect(edit([&](Bytes& b) { Wr32(&b[stat + 4 + 8 * 3], 1); }), PresetError::StatOrder, 1);
  const size_t mixAt = stat + 4 + 8 * 1;  // leaf 2, global.mix
  REQUIRE(Rd32(&base[mixAt]) == 2u);
  for (const uint32_t bad : {0x7FC00000u, 0x7F800000u, 0xFF800000u, 0x80000000u, 0x00000001u,
                             0x807FFFFFu}) {
    Expect(edit([&](Bytes& b) { Wr32(&b[mixAt + 4], bad); }), PresetError::StatValue, 2);
  }
  // Out of range is not invalid: it decodes, and the load canonicalizes it, inexact.
  {
    const Decoded r = Decode(edit([&](Bytes& b) { Wr32(&b[mixAt + 4], Bits(2.0f)); }));
    REQUIRE(r.ok);
    LoadReport report;
    REQUIRE_FALSE(CheckPreset(*r.s, &report));
    REQUIRE(report.changedValues == 1u);
  }
  // An id this build lacks decodes too; the load reports it.
  {
    auto s = CompleteState();
    PutLeaf(*s, 999, 1.0f);
    const Decoded r = Decode(Package(*s));
    REQUIRE(r.ok);
    LoadReport report;
    REQUIRE_FALSE(CheckPreset(*r.s, &report));
    REQUIRE(report.unknownIds == 1u);
  }
  // The performance state.
  const size_t perf = stat + 4 + 8 * n;
  for (const auto& f : std::vector<std::pair<size_t, uint32_t>>{
           {0, 2}, {1, 3}, {2, 6}, {3, 3}}) {
    Expect(edit([&](Bytes& b) { b[perf + f.first] = static_cast<uint8_t>(f.second); }),
           PresetError::Performance);
  }
  Expect(edit([&](Bytes& b) { Wr32(&b[perf + 4], kMinUsPerQuarter - 1); }),
         PresetError::Performance);
  Expect(edit([&](Bytes& b) { Wr32(&b[perf + 4], kMaxUsPerQuarter + 1); }),
         PresetError::Performance);
  const Decoded r = Decode(edit([&](Bytes& b) {
    b[perf]     = 1;
    b[perf + 1] = 2;
    b[perf + 2] = 5;
    b[perf + 3] = 2;
    Wr32(&b[perf + 4], kMinUsPerQuarter);
  }));
  REQUIRE(r.ok);
  REQUIRE(r.s->performance.reverse == 1u);
  REQUIRE(r.s->performance.tempoSource == TempoSource::Host);
}

// ── MODE ──────────────────────────────────────────────────────────────────────────────────

TEST_CASE("MODE rules: chunks, their order, lengths and counts", "[blob][decode]") {
  const Bytes base = Base();
  auto edit = [&](auto&& f) {
    Bytes b = base;
    f(b);
    Rehash(b);
    return b;
  };
  const size_t mode = Payload(base, kTagMode);
  Expect(edit([&](Bytes& b) {
           InsertChunk(b, FindChunk(b, kChunkMacr), PackageTag('B', 'I', 'G', '!'), Bytes(4000));
         }),
         PresetError::ModeTooLarge);
  Expect(edit([&](Bytes& b) {  // four stray bytes after the last chunk
           b.insert(b.begin() + static_cast<std::ptrdiff_t>(mode + Rd32(&b[mode - 4])), 4, 0);
           Wr32(&b[mode - 4], Rd32(&b[mode - 4]) + 4);
         }),
         PresetError::ModeLength);
  // A chunk this build does not know: UnsupportedFeature, named, never skipped.
  Expect(edit([&](Bytes& b) {
           InsertChunk(b, FindChunk(b, kChunkMacr), PackageTag('Q', 'U', 'U', 'X'), Bytes(4));
         }),
         PresetError::UnsupportedFeature, PackageTag('Q', 'U', 'U', 'X'));
  Expect(edit([&](Bytes& b) {  // SCHD again, after LAYR
           InsertChunk(b, FindChunk(b, kChunkMacr), kChunkSchd, Bytes(8));
         }),
         PresetError::ChunkOrder, kChunkSchd);
  Expect(edit([&](Bytes& b) { RemoveChunk(b, kChunkMacr); }), PresetError::ChunkMissing,
         kChunkMacr);
  Expect(edit([&](Bytes& b) { RemoveChunk(b, kChunkLayr); }), PresetError::ChunkMissing,
         kChunkLayr);
  Expect(edit([&](Bytes& b) { Wr32(&b[FindChunk(b, kChunkLayr) + 4], 40); }),
         PresetError::ChunkLength, kChunkLayr);
  Expect(edit([&](Bytes& b) { Wr32(&b[mode + 4], Rd32(&b[mode + 4]) + 1); }),
         PresetError::ChunkCount);
  // A present optional chunk equal to its absent default.
  {
    Bytes duck(8);
    Wr32(&duck[0], Bits(5.0f));
    Wr32(&duck[4], Bits(80.0f));
    Expect(edit([&](Bytes& b) { InsertChunk(b, FindChunk(b, kChunkMacr), kChunkDuck, duck); }),
           PresetError::ChunkDefault, kChunkDuck);
    Bytes pset(68, 0);
    pset[0] = 1;
    Wr32(&pset[4], 0);
    pset[8] = 1;  // weight 1
    Expect(edit([&](Bytes& b) { InsertChunk(b, FindChunk(b, kChunkMacr), kChunkPset, pset); }),
           PresetError::ChunkDefault, kChunkPset);
    Expect(edit([&](Bytes& b) {
             InsertChunk(b, FindChunk(b, kChunkMacr), kChunkRout, Bytes(4, 0));
           }),
           PresetError::ChunkDefault, kChunkRout);
    Expect(edit([&](Bytes& b) {
             InsertChunk(b, FindChunk(b, kChunkMacr), kChunkStep, Bytes(260, 0));
           }),
           PresetError::ChunkDefault, kChunkStep);
    Expect(edit([&](Bytes& b) {
             InsertChunk(b, FindChunk(b, kChunkMacr), kChunkMods, Bytes(24, 0));
           }),
           PresetError::ChunkDefault, kChunkMods);
  }
  // Counts that would overrun the decoded struct are refused before any copy.
  Expect(edit([&](Bytes& b) { b[Payload(b, kChunkSchd, true) + 1] = 3; }),
         PresetError::ModeCount, kChunkSchd);
  Expect(edit([&](Bytes& b) { b[Payload(b, kChunkMacr, true)] = 9; }), PresetError::ModeCount,
         kChunkMacr);
}

TEST_CASE("MODE rules: features declared, required and supported", "[blob][decode]") {
  const Bytes base = Base();
  const size_t mode = Payload(base, kTagMode);
  // Every feature bit, declared: this build names the ones it lacks.
  for (uint32_t bit = 0; bit < 32; ++bit) {
    Bytes b = base;
    Wr32(&b[mode], 1u << bit);
    Rehash(b);
    Expect(b, PresetError::UnsupportedFeature, 1u << bit);
  }
  // Content that needs a feature the package does not declare: FeatureMismatch, named.
  auto s = CompleteState();
  FullMode(&s->mode, s.get());
  const Bytes full = Package(*s);
  Bytes       b    = full;
  Wr32(&b[Payload(b, kTagMode)], s->mode.features & ~kModeFeaturePitchSet);
  Rehash(b);
  Expect(b, PresetError::FeatureMismatch, kModeFeaturePitchSet, kModeFeatureAll);
  // With nothing declared, the content's needs are named the same way.
  Bytes c = base;
  Bytes pset(68, 0);
  pset[0] = 2;
  pset[8] = 1;
  Wr32(&pset[12], Bits(12.0f));
  pset[16] = 1;
  InsertChunk(c, FindChunk(c, kChunkMacr), kChunkPset, pset);
  Rehash(c);
  Expect(c, PresetError::FeatureMismatch, kModeFeaturePitchSet);
  // Each feature, one at a time, in memory: required exactly when the content uses it.
  struct Case {
    uint32_t                         feature;
    void (*apply)(ModeBlob&);
  };
  const Case cases[] = {
      {kModeFeatureOnset, [](ModeBlob& m) { m.schedule.sources |= kSourceOnset; }},
      {kModeFeatureMarkPosition, [](ModeBlob& m) { m.layers[0].source = PositionSource::Mark; }},
      {kModeFeatureSources, [](ModeBlob& m) { m.schedule.sources = kSourcePeriodic; }},
      {kModeFeaturePitchSet, [](ModeBlob& m) { m.layers[0].pitchSelect = PitchSelect::Random; }},
      {kModeFeatureClock, [](ModeBlob& m) { m.schedule.subdiv = Subdivision::Tap; }},
      {kModeFeatureClock, [](ModeBlob& m) { m.schedule.sources |= kSourceClock; }},
      {kModeFeatureSteps, [](ModeBlob& m) { m.schedule.stepOrder = StepOrder::Random; }},
      {kModeFeatureMarkWalk, [](ModeBlob& m) { m.layers[0].markIndex = 3; }},
      {kModeFeatureTempoSync, [](ModeBlob& m) { m.layers[0].baseSync = 2; }},
      {kModeFeatureTwoLayers, [](ModeBlob& m) { m.layers[0].slotShare = 0.5f; }},
      {kModeFeaturePinPosition, [](ModeBlob& m) { m.layers[0].pinRearmMs = 50.0f; }},
      {kModeFeatureGridPosition, [](ModeBlob& m) { m.layers[0].source = PositionSource::Grid; }},
      {kModeFeatureExpSpray, [](ModeBlob& m) { m.layers[0].sprayLaw = SprayLaw::Exp; }},
      {kModeFeatureGlide, [](ModeBlob& m) { m.layers[0].glideStEnd = 12.0f; }},
      {kModeFeatureCrush, [](ModeBlob& m) { m.layers[0].modifier[0] = ModifierOp::Crush; }},
      {kModeFeatureDryDuck, [](ModeBlob& m) { m.dryDuck.attackMs = 6.0f; }},
  };
  for (const Case& k : cases) {
    auto t = CompleteState();
    k.apply(t->mode);
    INFO("feature " << k.feature);
    REQUIRE(RequiredModeFeatures(t->mode) == k.feature);
    ExpectValid(*t, PresetError::FeatureMismatch, k.feature, kModeFeatureAll);
    t->mode.features = k.feature;
    ExpectValid(*t, PresetError::None, kAnyDetail, kModeFeatureAll);
    ExpectValid(*t, PresetError::UnsupportedFeature, k.feature);
  }
}

TEST_CASE("MODE rules: values, ranges, padding, macro layout", "[blob][decode]") {
  const Bytes base = Base();
  auto edit = [&](auto&& f) {
    Bytes b = base;
    f(b);
    Rehash(b);
    return b;
  };
  const size_t schd = Payload(base, kChunkSchd, true), layr = Payload(base, kChunkLayr, true),
               macr = Payload(base, kChunkMacr, true);
  const uint32_t m = base[macr], t = base[macr + 1];
  REQUIRE((m == 6u && t == 9u));
  const size_t target = macr + 4 + 8 * m;  // the first target
  Expect(edit([&](Bytes& b) { b[schd + 4] = 1; }), PresetError::ModePadding, kChunkSchd);
  Expect(edit([&](Bytes& b) { b[schd] |= 0x20; }), PresetError::ModeEnum, kChunkSchd);
  Expect(edit([&](Bytes& b) { b[schd + 2] = 6; }), PresetError::ModeEnum, kChunkSchd);
  Expect(edit([&](Bytes& b) { b[layr] = 4; }), PresetError::ModeEnum, kChunkLayr);
  Expect(edit([&](Bytes& b) { b[layr + 13] = 1; }), PresetError::ModePadding, kChunkLayr);
  Expect(edit([&](Bytes& b) { b[layr + 3] = 16; }), PresetError::ModeRange, kChunkLayr);
  Expect(edit([&](Bytes& b) { Wr32(&b[layr + 16], 0); }), PresetError::ModeRange, kChunkLayr);
  Expect(edit([&](Bytes& b) { Wr32(&b[layr + 16], 0x80000000u); }), PresetError::ModeValue,
         kChunkLayr);
  Expect(edit([&](Bytes& b) { b[macr + 2] = 1; }), PresetError::ModePadding, kChunkMacr);
  Expect(edit([&](Bytes& b) { Wr32(&b[macr + 4], 68); }), PresetError::MacroLayout, 68);
  Expect(edit([&](Bytes& b) { Wr32(&b[macr + 4 + 8], 69); }), PresetError::MacroLayout, 69);
  Expect(edit([&](Bytes& b) { b[macr + 4 + 8 + 4] = 3; }), PresetError::MacroLayout, 70);
  Expect(edit([&](Bytes& b) { b[macr + 4 + 8 + 5] = 9; }), PresetError::ModeCount, 70);
  Expect(edit([&](Bytes& b) { Wr32(&b[target + 4], 0x7FC00000u); }), PresetError::ModeValue, 6);
  Expect(edit([&](Bytes& b) { Wr32(&b[target + 20], Bits(0.06f)); }), PresetError::ModeRange,
         6);
  Expect(edit([&](Bytes& b) { Wr32(&b[target + 20], Bits(16.5f)); }), PresetError::ModeRange,
         6);
  Expect(edit([&](Bytes& b) { Wr32(&b[target + 12], Bits(1.0f)); }), PresetError::ModeRange, 6);
  // In memory too: entries past a count, an absent layer, pads.
  auto s = CompleteState();
  s->mode.macros.targets[9].param = 1;
  ExpectValid(*s, PresetError::ModePadding, kChunkMacr);
  s = CompleteState();
  s->mode.layers[1].slotShare = 1.0f;
  ExpectValid(*s, PresetError::ModePadding, kChunkLayr);
  s = CompleteState();
  s->mode.macros.macros[0].count = 3;
  ExpectValid(*s, PresetError::MacroLayout);
  s = CompleteState();
  s->mode.layers[0].svfBand = SvfBand::Notch;  // an SVF field without an SVF
  ExpectValid(*s, PresetError::ModeRange, kChunkLayr, kModeFeatureAll);
  s = CompleteState();
  s->mode.layers[0].modifier[1] = ModifierOp::Crush;  // an op after None
  ExpectValid(*s, PresetError::ModePadding, kChunkLayr, kModeFeatureAll);
  s = CompleteState();
  s->mode.layers[0].modifier[0] = s->mode.layers[0].modifier[1] = ModifierOp::Crush;
  ExpectValid(*s, PresetError::ModeEnum, kChunkLayr, kModeFeatureAll);
  s = CompleteState();
  s->mode.layers[0].quantize = QuantizeMode::Scale;  // a scale needs a note
  ExpectValid(*s, PresetError::ModeRange, kChunkLayr, kModeFeatureAll);
}

TEST_CASE("MODE rules for the vocabulary of later waves", "[blob][decode]") {
  auto full = CompleteState();
  FullMode(&full->mode, full.get());
  struct Case {
    PresetError error;
    void (*apply)(ModeBlob&);
  };
  const Case cases[] = {
      {PresetError::ModeCount, [](ModeBlob& m) { m.pitch[0].count = 9; }},
      {PresetError::ModeRange, [](ModeBlob& m) { m.pitch[0].entries[3].weight = 17; }},
      {PresetError::ModeRange, [](ModeBlob& m) { m.pitch[1].entries[0].st = 24.5f; }},
      {PresetError::ModePadding, [](ModeBlob& m) { m.pitch[0].entries[7].pad = 1; }},
      {PresetError::ModeCount, [](ModeBlob& m) { m.steps.countMax = 17; }},
      {PresetError::ModeEnum, [](ModeBlob& m) { m.steps.entries[2].flags = 2; }},
      {PresetError::ModeRange, [](ModeBlob& m) { m.steps.entries[2].slot = 16; }},
      {PresetError::ModeRange, [](ModeBlob& m) { m.steps.entries[2].prob = 1.5f; }},
      {PresetError::ModeEnum, [](ModeBlob& m) { m.modulators[1].shape = 5; }},
      {PresetError::ModeRange, [](ModeBlob& m) { m.modulators[1].releaseMs = 30000.0f; }},
      {PresetError::ModeEnum, [](ModeBlob& m) { m.routes.entries[3].to = 6; }},
      {PresetError::ModeRange, [](ModeBlob& m) { m.routes.entries[3].amount = -2.0f; }},
      {PresetError::ModeRange, [](ModeBlob& m) { m.links.entries[0].layer = 2; }},
      {PresetError::ModeRange, [](ModeBlob& m) { m.dryDuck.attackMs = 0.05f; }},
      {PresetError::ModeRange, [](ModeBlob& m) { m.layers[0].quantizeRoot = 12; }},
      {PresetError::ModeRange, [](ModeBlob& m) { m.layers[0].scaleMask = 0x1000; }},
      {PresetError::ModeRange, [](ModeBlob& m) { m.layers[0].pinRearmMs = 5.0f; }},
      {PresetError::ModeRange, [](ModeBlob& m) { m.layers[1].glideStEnd = -25.0f; }},
  };
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
    INFO("case " << i);
    auto s = std::make_unique<PresetState>(*full);
    cases[i].apply(s->mode);
    s->mode.features = RequiredModeFeatures(s->mode);
    ExpectValid(*s, cases[i].error, kAnyDetail, kModeFeatureAll);
    uint8_t  out[1536];
    uint32_t n = 0;
    REQUIRE_FALSE(EncodeMode(s->mode, out, sizeof out, &n));
  }
  // The second modulator needs the first.
  auto s = std::make_unique<PresetState>(*full);
  s->mode.modulators[0] = Modulator{};
  ExpectValid(*s, PresetError::ModePadding, kChunkMods, kModeFeatureAll);
}

// ── CTRL ──────────────────────────────────────────────────────────────────────────────────

TEST_CASE("CTRL rules: one position per macro, canonical values, counts", "[blob][decode]") {
  const Bytes base = Base();
  auto edit = [&](auto&& f) {
    Bytes b = base;
    f(b);
    Rehash(b);
    return b;
  };
  const size_t ctrl = Payload(base, kTagCtrl);
  REQUIRE((base[ctrl] == 6u && base[ctrl + 1] == 0u));
  Expect(edit([&](Bytes& b) { Wr32(&b[ctrl + 4 + 8], 71); }), PresetError::CtrlPositions, 71);
  Expect(edit([&](Bytes& b) { Wr32(&b[ctrl + 8], Bits(1.5f)); }), PresetError::CtrlValue, 69);
  Expect(edit([&](Bytes& b) { Wr32(&b[ctrl + 8], 0x80000000u); }), PresetError::CtrlValue, 69);
  Expect(edit([&](Bytes& b) { b[ctrl + 1] = 5; }), PresetError::CtrlCount);
  Expect(edit([&](Bytes& b) { b[ctrl] = 9; }), PresetError::CtrlCount);
  Expect(edit([&](Bytes& b) { b[ctrl + 2] = 1; }), PresetError::CtrlPadding);
  Expect(edit([&](Bytes& b) { b[ctrl + 1] = 1; }), PresetError::CtrlLength);
  // Expression targets: a Leaf or Macro row of this build, both ends in its range.
  {
    auto e                    = CompleteState();
    e->control.exprCount      = 1;
    e->control.expressions[0] = ExpressionAssignment{5, 1.0f, 500.0f, 1.0f};
    const Bytes  b            = Package(*e);
    const size_t x            = Payload(b, kTagCtrl) + 4 + 8 * 6;
    REQUIRE(Decode(b).ok);
    Bytes t = b;
    Wr32(&t[x], 29);  // Reserved until W1
    Rehash(t);
    Expect(t, PresetError::ExpressionTarget, 29);
    Bytes r = b;
    Wr32(&r[x + 8], Bits(600.0f));
    Rehash(r);
    Expect(r, PresetError::ExpressionRange, 5);
  }
  // In memory: positions for another macro set, expressions past the count, the curve.
  auto s = CompleteState();
  s->control.macroCount = 5;
  ExpectValid(*s, PresetError::CtrlPositions);
  s = CompleteState();
  s->control.expressions[1].target = 2;
  ExpectValid(*s, PresetError::CtrlPadding);
  s = CompleteState();
  s->control.exprCount      = 1;
  s->control.expressions[0] = ExpressionAssignment{2, 0.0f, 1.0f, 0.0f};
  ExpectValid(*s, PresetError::CtrlValue, 2);
  s = CompleteState();
  s->control.present = 0;
  ExpectValid(*s, PresetError::CtrlPadding);  // an absent CTRL holds nothing
  std::memset(static_cast<void*>(&s->control), 0, sizeof s->control);
  ExpectValid(*s, PresetError::None);
}

// ── META ──────────────────────────────────────────────────────────────────────────────────

namespace {

// META bytes, field by field, for the decoder's rules.
struct MetaBytes {
  Bytes b;
  void  Str(const std::string& s) {
    b.push_back(static_cast<uint8_t>(s.size()));
    b.push_back(static_cast<uint8_t>(s.size() >> 8));
    b.insert(b.end(), s.begin(), s.end());
  }
  void U8(uint8_t v) { b.push_back(v); }
  void U32(uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
  }
};

Bytes WithMeta(const Bytes& meta) {
  auto s = CompleteState();
  Bytes b = Package(*s);
  InsertSection(b, b.size(), kTagMeta, meta);
  Rehash(b);
  return b;
}

Bytes MetaOf(const std::string& id, const std::string& name, const std::string& family,
             const std::vector<std::string>& tags,
             const std::vector<std::pair<uint32_t, std::string>>& names,
             const std::string& author = "", const std::string& description = "") {
  MetaBytes m;
  m.Str(id);
  m.Str(name);
  m.Str(family);
  m.Str(author);
  m.Str(description);
  m.U8(static_cast<uint8_t>(tags.size()));
  for (const auto& t : tags) m.Str(t);
  m.U8(static_cast<uint8_t>(names.size()));
  for (const auto& n : names) {
    m.U32(n.first);
    m.Str(n.second);
  }
  return m.b;
}

}  // namespace

TEST_CASE("META: text, limits, the family, tags and display names", "[blob][decode]") {
  {
    const Bytes   b = WithMeta(MetaOf("factory.deja-vu_2", "D\xC3\xA9j\xC3\xA0 Vu \xF0\x9F\x8E\xB8",
                                      "reverie", {"pad", "shimmer"}, {{69, "Smear"}, {74, "Tone"}},
                                      "A. Author", std::string(512, 'd')));
    const Decoded r = Decode(b);
    REQUIRE(r.ok);
    REQUIRE(r.meta.family == PresetFamily::Reverie);
    REQUIRE(r.meta.tagCount == 2u);
    REQUIRE(r.meta.displayNameCount == 2u);
    REQUIRE(r.meta.displayMacro[1] == 74u);
    const auto view = [&](const SectionSpan& s) {
      return std::string(reinterpret_cast<const char*>(&b[s.offset]), s.length);
    };
    REQUIRE(view(r.meta.id) == "factory.deja-vu_2");
    REQUIRE(view(r.meta.displayName[0]) == "Smear");
    REQUIRE(view(r.meta.tags[1]) == "shimmer");
    REQUIRE(r.meta.description.length == 512u);
  }
  struct Case {
    Bytes       meta;
    PresetError error;
    uint32_t    detail;
  };
  const std::string ok = "ok";
  const std::vector<Case> cases = {
      {MetaOf("Upper", ok, "none", {}, {}), PresetError::MetaText, 0},
      {MetaOf("", ok, "none", {}, {}), PresetError::MetaText, 0},
      {MetaOf(std::string(49, 'a'), ok, "none", {}, {}), PresetError::MetaText, 0},
      {MetaOf("a", "", "none", {}, {}), PresetError::MetaText, 1},
      {MetaOf("a", std::string(33, 'n'), "none", {}, {}), PresetError::MetaText, 1},
      {MetaOf("a", "tab\there", "none", {}, {}), PresetError::MetaText, 1},
      {MetaOf("a", "del\x7F", "none", {}, {}), PresetError::MetaText, 1},
      {MetaOf("a", "c1 \xC2\x85", "none", {}, {}), PresetError::MetaText, 1},
      {MetaOf("a", "overlong \xC0\xAF", "none", {}, {}), PresetError::MetaText, 1},
      {MetaOf("a", "overlong \xE0\x80\xAF", "none", {}, {}), PresetError::MetaText, 1},
      {MetaOf("a", "surrogate \xED\xA0\x80", "none", {}, {}), PresetError::MetaText, 1},
      {MetaOf("a", "past \xF4\x90\x80\x80", "none", {}, {}), PresetError::MetaText, 1},
      {MetaOf("a", "cut \xE2\x82", "none", {}, {}), PresetError::MetaText, 1},
      {MetaOf("a", "stray \x80", "none", {}, {}), PresetError::MetaText, 1},
      {MetaOf("a", ok, "jazz", {}, {}), PresetError::MetaFamily, 0},
      {MetaOf("a", ok, "none", {}, {}, std::string(65, 'x')), PresetError::MetaText, 3},
      {MetaOf("a", ok, "none", {}, {}, "", std::string(513, 'x')), PresetError::MetaText, 4},
      {MetaOf("a", ok, "none", std::vector<std::string>(9, "t"), {}), PresetError::MetaText, 5},
      {MetaOf("a", ok, "none", {""}, {}), PresetError::MetaText, 6},
      {MetaOf("a", ok, "none", {"x", "y", "x"}, {}), PresetError::MetaDuplicate, 2},
      {MetaOf("a", ok, "none", {}, {{75, "Aux"}}), PresetError::MetaDisplayName, 75},
      {MetaOf("a", ok, "none", {}, {{70, "B"}, {69, "A"}}), PresetError::MetaDisplayName, 69},
      {MetaOf("a", ok, "none", {}, {{70, "B"}, {70, "A"}}), PresetError::MetaDisplayName, 70},
      {MetaOf("a", ok, "none", {}, {{69, std::string(17, 'n')}}), PresetError::MetaText, 7},
  };
  for (size_t i = 0; i < cases.size(); ++i) {
    INFO("case " << i);
    Expect(WithMeta(cases[i].meta), cases[i].error, cases[i].detail);
  }
  Bytes extra = MetaOf("a", ok, "none", {}, {});
  extra.push_back(0);
  Expect(WithMeta(extra), PresetError::MetaLength);
  Bytes cut = MetaOf("a", ok, "none", {}, {});
  cut.pop_back();
  Expect(WithMeta(cut), PresetError::MetaLength);
  // The encoder applies the same rules.
  MetaContent m;
  m.id   = MetaText{"Bad", 3};
  m.name = MetaText{"ok", 2};
  uint8_t          out[256];
  uint32_t         n = 0;
  PresetDiagnostic d;
  REQUIRE_FALSE(EncodeMeta(m, ModeBlob{}, out, sizeof out, &n, &d));
  REQUIRE(d.error == PresetError::MetaText);
}

// ── ValidateMode's semantic rules ─────────────────────────────────────────────────────────

TEST_CASE("ValidateMode: macro targets (E8, E9) and the shape macro (E10)", "[blob][validate]") {
  auto target = [](uint32_t param, float lo, float hi) {
    auto s                          = CompleteState();
    s->mode.macros.targets[0].param = param;
    s->mode.macros.targets[0].lo    = lo;
    s->mode.macros.targets[0].hi    = hi;
    return s;
  };
  ExpectValid(*target(6, 0.0f, 1.0f), PresetError::None);
  ExpectValid(*target(27, 0.0f, 1.0f), PresetError::None);  // a Leaf row at r1
  ExpectValid(*target(2, 0.0f, 1.0f), PresetError::TargetMix, 2);
  ExpectValid(*target(29, 1.0f, 2.0f), PresetError::TargetNotLeaf, 29);  // Reserved until W1
  ExpectValid(*target(69, 0.0f, 1.0f), PresetError::TargetNotLeaf, 69);  // a Macro row
  ExpectValid(*target(82, 0.0f, 1.0f), PresetError::TargetNotLeaf, 82);  // a Global row
  ExpectValid(*target(999, 0.0f, 1.0f), PresetError::TargetNotLeaf, 999);
  ExpectValid(*target(7, 0.0f, 1.0f), PresetError::TargetDuplicate, 7);  // twice in activity
  ExpectValid(*target(6, 0.0f, 1.5f), PresetError::TargetRange, 6);
  ExpectValid(*target(6, -0.5f, 1.0f), PresetError::TargetRange, 6);
  ExpectValid(*target(1, 5000.0f, 1.0f), PresetError::None);  // reversed, within range
  auto s = CompleteState();
  // Shape (71) emptied: its two targets removed, the later macros moved up.
  MacroTable& t = s->mode.macros;
  t.macros[2].count = 0;
  for (uint32_t i = 3; i + 2 < t.targetCount; ++i) t.targets[i] = t.targets[i + 2];
  t.targets[7] = t.targets[8] = MacroTarget{};
  t.targetCount = 7;
  for (uint32_t m = 3; m < t.macroCount; ++m) {
    t.macros[m].first = static_cast<uint8_t>(t.macros[m].first - 2);
  }
  ExpectValid(*s, PresetError::ShapeEmpty, 71);
  // An empty activity macro is the lint's business (L3), not an error.
  auto e = CompleteState();
  MacroTable& u = e->mode.macros;
  u.macros[0].count = 0;
  for (uint32_t i = 0; i + 2 < u.targetCount; ++i) u.targets[i] = u.targets[i + 2];
  u.targets[7] = u.targets[8] = MacroTarget{};
  u.targetCount = 7;
  for (uint32_t m = 1; m < u.macroCount; ++m) {
    u.macros[m].first = static_cast<uint8_t>(u.macros[m].first - 2);
  }
  ExpectValid(*e, PresetError::None);
}

TEST_CASE("ValidateMode: absent elements and expression targets", "[blob][validate]") {
  // A one-layer mode's layer-1 leaf, an absent SVF's leaves: at their defaults or refused.
  auto s = CompleteState();
  PutLeaf(*s, 38, 250.0f);
  PutLeaf(*s, 34, 20000.0f);
  ExpectValid(*s, PresetError::None);
  PutLeaf(*s, 38, 300.0f);
  ExpectValid(*s, PresetError::AbsentLeaf, 38);
  s = CompleteState();
  PutLeaf(*s, 66, 0.5f);  // modulator0.depth without a modulator
  ExpectValid(*s, PresetError::AbsentLeaf, 66);
  // Which elements exist.
  ModeBlob m;
  REQUIRE(blob::ElementPresent(m, 1));
  REQUIRE(blob::ElementPresent(m, 31));
  REQUIRE_FALSE(blob::ElementPresent(m, 38));
  REQUIRE_FALSE(blob::ElementPresent(m, 61));
  REQUIRE_FALSE(blob::ElementPresent(m, 34));
  REQUIRE_FALSE(blob::ElementPresent(m, 37));
  REQUIRE_FALSE(blob::ElementPresent(m, 60));
  REQUIRE_FALSE(blob::ElementPresent(m, 65));
  m.schedule.layerCount   = 2;
  m.layers[1]             = ModeLayer{};
  m.layers[0].modifier[0] = ModifierOp::Svf;
  m.modulators[0].type    = ModulatorType::Lfo;
  REQUIRE(blob::ElementPresent(m, 38));
  REQUIRE(blob::ElementPresent(m, 61));
  REQUIRE(blob::ElementPresent(m, 34));
  REQUIRE_FALSE(blob::ElementPresent(m, 36));
  REQUIRE_FALSE(blob::ElementPresent(m, 53));
  REQUIRE(blob::ElementPresent(m, 66));
  REQUIRE_FALSE(blob::ElementPresent(m, 67));
  // Expression: a Leaf row of a present element, global.mix included, or a Macro row.
  auto expr = [](uint32_t target, float lo, float hi) {
    auto s                      = CompleteState();
    s->control.exprCount        = 1;
    s->control.expressions[0]   = ExpressionAssignment{target, lo, hi, 1.0f};
    return s;
  };
  ExpectValid(*expr(2, 0.0f, 1.0f), PresetError::None);
  ExpectValid(*expr(75, 1.0f, 0.0f), PresetError::None);  // an undefined macro does nothing
  ExpectValid(*expr(5, 1.0f, 500.0f), PresetError::None);
  ExpectValid(*expr(5, 1.0f, 600.0f), PresetError::ExpressionRange, 5);
  ExpectValid(*expr(29, 1.0f, 2.0f), PresetError::ExpressionTarget, 29);
  ExpectValid(*expr(77, 0.0f, 1.0f), PresetError::ExpressionTarget, 77);
  ExpectValid(*expr(0, 0.0f, 1.0f), PresetError::ExpressionTarget, 0);
}

TEST_CASE("ValidateMode: steps, routes and the two-layer budget (E8, E11)", "[blob][validate]") {
  auto full = CompleteState();
  FullMode(&full->mode, full.get());
  ExpectValid(*full, PresetError::None, kAnyDetail, kModeFeatureAll);
  auto edit = [&](auto&& f) {
    auto s = std::make_unique<PresetState>(*full);
    f(*s);
    s->mode.features = RequiredModeFeatures(s->mode);
    return s;
  };
  ExpectValid(*edit([](PresetState& s) {
                s.mode.pitch[0].count = 4;
                for (uint32_t i = 4; i < 8; ++i) s.mode.pitch[0].entries[i] = PitchEntry{};
              }),
              PresetError::StepRatioIndex, 4, kModeFeatureAll);
  ExpectValid(*edit([](PresetState& s) { s.mode.modulators[1] = Modulator{}; }),
              PresetError::RouteEndpoint, 1, kModeFeatureAll);
  ExpectValid(*edit([](PresetState& s) { s.mode.links.entries[2].to = 2; }),
              PresetError::RouteEndpoint, 0x102, kModeFeatureAll);
  // Slot shares sum to at most 1, exactly.
  auto shares = [&](float a, float b) {
    return edit([&](PresetState& s) {
      s.mode.layers[0].slotShare = a;
      s.mode.layers[1].slotShare = b;
    });
  };
  ExpectValid(*shares(0.25f, 0.75f), PresetError::None, kAnyDetail, kModeFeatureAll);
  ExpectValid(*shares(FromBits(0x00800000u), FromBits(0x3F7FFFFFu)), PresetError::None, kAnyDetail,
              kModeFeatureAll);
  ExpectValid(*shares(FromBits(Bits(0.75f) + 1u), 0.25f), PresetError::LayerBudget, 0,
              kModeFeatureAll);
  ExpectValid(*shares(FromBits(0x00800000u), 1.0f), PresetError::LayerBudget, 0, kModeFeatureAll);
  // voice_count maxima, rounded half away (§3.7), sum to at most 64.
  auto voices = [&](float a, float b) {
    return edit([&](PresetState& s) {
      PutLeaf(s, 31, a);
      PutLeaf(s, 43, b);
    });
  };
  ExpectValid(*voices(32.0f, 32.0f), PresetError::None, kAnyDetail, kModeFeatureAll);
  ExpectValid(*voices(31.5f, 32.5f), PresetError::LayerBudget, 1, kModeFeatureAll);  // 32 + 33
  ExpectValid(*voices(31.5f, 32.4f), PresetError::None, kAnyDetail, kModeFeatureAll);  // 32 + 32
  ExpectValid(*voices(1.0f, 63.4f), PresetError::None, kAnyDetail, kModeFeatureAll);
  ExpectValid(*voices(1.0f, 100.0f), PresetError::LayerBudget, 1, kModeFeatureAll);  // 1 + 64
  ExpectValid(*voices(0.25f, 63.0f), PresetError::None, kAnyDetail, kModeFeatureAll);  // 1 + 63
}

// ── The encoder ───────────────────────────────────────────────────────────────────────────

TEST_CASE("The encoder refuses what would not decode", "[blob][encode]") {
  uint8_t          out[kMaxPackageBytes];
  uint32_t         n = 0;
  PresetDiagnostic d;
  auto s = CompleteState();
  std::swap(s->leaves[0], s->leaves[1]);
  REQUIRE_FALSE(EncodeStat(*s, out, sizeof out, &n, &d));
  REQUIRE(d.error == PresetError::StatOrder);
  s = CompleteState();
  s->mode.features = kModeFeatureOnset;
  REQUIRE_FALSE(EncodeMode(s->mode, out, sizeof out, &n, &d));
  REQUIRE(d.error == PresetError::FeatureMismatch);
  s = CompleteState();
  s->soundRev = 0;
  REQUIRE(EncodePackage(*s, PackageContent{}, out, sizeof out, &d) == 0u);
  REQUIRE(d.error == PresetError::SoundRevision);
  s = CompleteState();
  PackageContent c;
  c.flags = 8;
  REQUIRE(EncodePackage(*s, c, out, sizeof out, &d) == 0u);
  REQUIRE(d.error == PresetError::HeaderFlags);
  REQUIRE(EncodePackage(*s, PackageContent{}, out, 200, &d) == 0u);
  REQUIRE(d.error == PresetError::TooLarge);
  // A JSON that would pass 16 KiB.
  const Bytes json(kMaxPackageBytes, '{');
  REQUIRE(Package(*s, nullptr, &json, 0, kSchemaVersion, &d).empty());
  REQUIRE(d.error == PresetError::TooLarge);
}

TEST_CASE("Re-encoding into an existing package: pedal-side edits", "[blob][encode]") {
  const Bytes   base = Base();
  const Decoded r    = Decode(base);
  REQUIRE(r.ok);
  Bytes out(kMaxPackageBytes);
  // A leaf and a position change; JSON_STALE set; everything else carried.
  auto s = std::make_unique<PresetState>(*r.s);
  SetLeaf(*s, ParamId::Mix, 0.8f);
  s->control.positions[0].position = 1.0f;
  size_t n = ReencodePackage(*s, base.data(), base.size(), kPackageFlagJsonStale, out.data(),
                             out.size());
  REQUIRE(n == base.size());
  Bytes edited(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(n));
  Decoded e = Decode(edited);
  REQUIRE(e.ok);
  REQUIRE(e.info.flags == kPackageFlagJsonStale);
  REQUIRE(std::memcmp(&e.s->leaves[1].value, &s->leaves[1].value, 4) == 0);
  const size_t json = FindSection(base, kTagJson);
  REQUIRE(std::memcmp(&base[json], &edited[FindSection(edited, kTagJson)], base.size() - json) ==
          0);
  // CTRL dropped, then put back right after MODE.
  std::memset(static_cast<void*>(&s->control), 0, sizeof s->control);
  n = ReencodePackage(*s, base.data(), base.size(), 0, out.data(), out.size());
  REQUIRE(n != 0u);
  Bytes dropped(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(n));
  REQUIRE(FindSection(dropped, kTagCtrl) == 0u);
  REQUIRE(Decode(dropped).ok);
  s->control = r.s->control;
  n = ReencodePackage(*s, dropped.data(), dropped.size(), 0, out.data(), out.size());
  Bytes back(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(n));
  REQUIRE(Decode(back).ok);
  REQUIRE(FindSection(back, kTagCtrl) > FindSection(back, kTagMode));
  REQUIRE(FindSection(back, kTagCtrl) < FindSection(back, kTagMeta));
  // META names macro 74; a state without it cannot keep that META.
  auto t = std::make_unique<PresetState>(*r.s);
  t->mode.macros.macroCount    = 5;
  t->mode.macros.targetCount   = 8;
  t->mode.macros.macros[5]     = MacroDef{};
  t->mode.macros.targets[8]    = MacroTarget{};
  t->control.macroCount        = 5;
  t->control.positions[5]      = MacroPosition{};
  PresetDiagnostic d;
  REQUIRE(ReencodePackage(*t, base.data(), base.size(), 0, out.data(), out.size(), &d) == 0u);
  REQUIRE(d.error == PresetError::MetaDisplayName);
}

// ── Frozen fixtures and the fuzzer ────────────────────────────────────────────────────────

TEST_CASE("Frozen fixtures: their bytes and verdicts", "[blob][fixtures]") {
  REQUIRE(kFixtureCount == 12u);
  for (size_t i = 0; i < kFixtureCount; ++i) {
    const Fixture&    f    = kFixtures[i];
    const std::string path = std::string(BRAINSCAPE_FROZEN_FIXTURES) + "/" + f.file;
    FILE*             in   = std::fopen(path.c_str(), "rb");
    INFO(f.file);
    REQUIRE(in != nullptr);
    Bytes   bytes;
    uint8_t buf[4096];
    size_t  n;
    while ((n = std::fread(buf, 1, sizeof buf, in)) > 0) bytes.insert(bytes.end(), buf, buf + n);
    std::fclose(in);
    REQUIRE(CheckFixture(f, bytes) == "");
    // The fixture is still what its recipe builds (a changed encoder would differ here first).
    REQUIRE(MakeFixture(i) == bytes);
  }
}

TEST_CASE("Mutation fuzzer, short run: every accepted package re-encodes to itself",
          "[blob][fuzz]") {
  const FuzzResult r = Fuzz(3000, 99);
  REQUIRE(r.iterations == 3000u);
  REQUIRE(r.reencodeMismatches == 0u);
  REQUIRE(r.accepted > 0u);
  REQUIRE(r.acceptedAll > r.accepted);
}

TEST_CASE("PresetErrorName names every code", "[blob]") {
  REQUIRE(std::string(PresetErrorName(PresetError::None)) == "None");
  REQUIRE(std::string(PresetErrorName(PresetError::UnsupportedFeature)) == "UnsupportedFeature");
  REQUIRE(std::string(PresetErrorName(PresetError::ExpressionRange)) == "ExpressionRange");
  REQUIRE(std::string(PresetErrorName(PresetError::kCount)) == "?");
  for (size_t e = 0; e < static_cast<size_t>(PresetError::kCount); ++e) {
    REQUIRE(std::string(PresetErrorName(static_cast<PresetError>(e))) != "?");
  }
}
