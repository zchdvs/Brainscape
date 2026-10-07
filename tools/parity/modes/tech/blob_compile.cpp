// PROBE (scratch): the desktop compiler side of the package sketch, plus the probe driver.
//   blob_probe hashes          compile three sample modes; print SHA-256 of each package
//   blob_probe fuzz [iters]    mutate packages; decode/validate under sanitizers; accepted
//                              packages must re-encode to the same bytes
// The compiler path is integer-only: numbers come from text through bsnum::Parse,
// canonicalization is a bit-pattern rule, serialization is explicit little-endian.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "blob_format.h"
#include "brainscape/Params.h"
#include "bsnum.h"

using namespace bsp;

namespace {

uint32_t FB(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
uint32_t Key(uint32_t b) { return (b & 0x80000000u) ? ~b : (b | 0x80000000u); }

// The canonical value rule (profile §3.7) with integer compares only.
uint32_t CanonBits(uint32_t b, float lo, float hi) {
  const uint32_t ex = b & 0x7F800000u;
  if (ex == 0x7F800000u) return FB(lo);
  if (ex == 0) b = 0;
  if (Key(b) < Key(FB(lo))) return FB(lo);
  if (Key(b) > Key(FB(hi))) return FB(hi);
  return b;
}
float Num(const char* text, float lo, float hi) {  // what the JSON reader does per field
  float v = 0;
  size_t used = 0;
  if (bsnum::Parse(text, std::strlen(text), &v, &used) != bsnum::Status::Ok || used != std::strlen(text)) {
    std::fprintf(stderr, "bad number %s\n", text);
    std::exit(2);
  }
  const uint32_t c = CanonBits(FB(v), lo, hi);
  float r;
  std::memcpy(&r, &c, 4);
  return r;
}
float Leaf(brainscape::ParamId id, const char* text) {
  const auto& d = brainscape::kParamTable[uint32_t(id) - 1];
  return Num(text, d.min, d.max);
}

struct W {
  std::vector<uint8_t> b;
  void U8(uint32_t v) { b.push_back(uint8_t(v)); }
  void U16(uint32_t v) { U8(v); U8(v >> 8); }
  void U32(uint32_t v) { U16(v); U16(v >> 16); }
  void F32(float f) { U32(FB(f)); }
  void Pad() { while (b.size() % 4) U8(0); }
};

void EncodeStat(const DecodedPackage& p, W& w) {
  w.U32(p.leafCount);
  for (uint32_t i = 0; i < p.leafCount; ++i) { w.U32(p.leaves[i].id); w.F32(p.leaves[i].value); }
  w.U8(p.globalReverse); w.U8(p.timeMode); w.U8(p.subdiv); w.U8(p.tempoSource); w.U32(p.usPerQuarter);
}
void EncodeMode(const DecodedPackage& p, W& w) {
  w.U8(p.layerCount); w.U8(p.pitchCount); w.U8(p.macroCount); w.U8(p.targetCount);
  w.U8(p.schedSources); w.U8(p.modeSubdiv); w.U16(p.burstCount); w.F32(p.burstSpacingMs);
  for (uint32_t l = 0; l < kMaxLayers; ++l) {
    const Layer& L = p.layers[l];
    w.U8(L.posSource); w.U8(L.sizeLaw); w.U8(L.select); w.U8(L.quantize); w.U16(L.scaleMask);
    w.U8(L.pitchFirst); w.U8(L.pitchN); w.U8(L.modOp[0]); w.U8(L.modOp[1]); w.U8(L.modBand[0]); w.U8(L.modBand[1]);
    w.F32(L.glideStStart); w.F32(L.glideStEnd); w.F32(L.glideCurve);
  }
  for (uint32_t i = 0; i < kMaxPitch; ++i) { w.F32(p.pitch[i].st); w.F32(p.pitch[i].weight); }
  for (uint32_t i = 0; i < kMaxMacros; ++i) { const Macro& m = p.macros[i]; w.U8(m.id); w.U8(m.first); w.U8(m.n); w.U8(0); }
  for (uint32_t i = 0; i < kMaxTargets; ++i) {
    const Target& t = p.targets[i];
    w.U32(t.param); w.F32(t.lo); w.F32(t.hi); w.F32(t.inLo); w.F32(t.inHi); w.F32(t.curve);
  }
}

std::vector<uint8_t> Encode(const DecodedPackage& p, const std::string& json) {
  W stat, mode;
  EncodeStat(p, stat);
  EncodeMode(p, mode);
  W pkg;
  pkg.b.insert(pkg.b.end(), {'B', 'S', 'P', 'K'});
  pkg.U16(kPackageFormat); pkg.U16(0);
  pkg.U32(p.soundRev); pkg.U32(kBlobFormat); pkg.U32(p.schemaVersion);
  pkg.U32(0); pkg.U32(3); pkg.U32(0);       // total_bytes patched below; 3 sections
  for (int i = 0; i < 64; ++i) pkg.U8(0);   // sound_hash, package_hash
  auto section = [&](uint32_t tag, const std::vector<uint8_t>& data) {
    pkg.U32(tag); pkg.U32(uint32_t(data.size()));
    pkg.b.insert(pkg.b.end(), data.begin(), data.end());
    pkg.Pad();
  };
  section(kTagStat, stat.b);
  section(kTagMode, mode.b);
  section(kTagJson, std::vector<uint8_t>(json.begin(), json.end()));
  const uint32_t total = uint32_t(pkg.b.size());
  for (int i = 0; i < 4; ++i) pkg.b[20 + i] = uint8_t(total >> (8 * i));
  const uint8_t* sp[2] = {stat.b.data(), mode.b.data()};
  const size_t sl[2] = {stat.b.size(), mode.b.size()};
  Sha256(sp, sl, 2, pkg.b.data() + 32);
  const uint8_t* pp[1] = {pkg.b.data()};
  const size_t pl[1] = {pkg.b.size()};
  Sha256(pp, pl, 1, pkg.b.data() + 64);  // computed with its own field still zero
  return pkg.b;
}

// Three sample "modes" as the JSON reader would hand them over (numbers as text).
DecodedPackage Sample(int which) {
  DecodedPackage p;
  std::memset(&p, 0, sizeof p);
  p.soundRev = 1; p.schemaVersion = 1;
  using brainscape::ParamId;
  // every leaf explicit, defaults included (complete state, companion §6.1)
  const char* vals[3][28] = {
      {"600", "0.5", "0.35", "0", "180", "0.55", "40", "0", "0", "0", "0", "0.75", "0.5", "0.6", "0.6",
       "0.4", "0.15", "350", "0.3", "0", "0.5", "0.35", "12000", "0.1", "0", "0.5", "0", "0"},
      {"250", "0.5", "0.2", "-1.5", "90", "0.85", "300", "0", "12.5", "0.1", "1", "0.3", "0.5", "0.7", "1",
       "0.4", "0", "375", "0.45", "0.25", "0.8", "0.5", "6000", "0.3", "0.3333333", "0.5", "0", "0"},
      {"40", "0.6", "0", "0", "35", "0.3", "3", "7", "0", "0", "0", "0.1", "0.05", "0.2", "0.4",
       "2.5", "0.05", "120", "0.6", "0.2", "0.3", "0.1", "20000", "0.1", "0", "0.8", "1", "1"}};
  p.leafCount = 28;
  for (uint32_t i = 0; i < 28; ++i) {
    p.leaves[i].id = i + 1;
    p.leaves[i].value = Leaf(ParamId(i + 1), vals[which][i]);
  }
  p.globalReverse = 0; p.timeMode = 1; p.subdiv = 2; p.tempoSource = 0; p.usPerQuarter = 500000;  // 120 BPM exact
  p.layerCount = which == 1 ? 2 : 1;
  p.schedSources = which == 2 ? 0x06 : 0x01; p.modeSubdiv = 2; p.burstCount = which == 2 ? 6 : 1;
  p.burstSpacingMs = Num(which == 2 ? "3" : "0", 0, 100);
  const char* st[3][5] = {{"-12", "0", "12", "24", ""}, {"0", "7.02", "-4.98", "", ""}, {"0", "", "", "", ""}};
  const char* wt[3][5] = {{"1", "2", "1", "1", ""}, {"1", "0.5", "0.5", "", ""}, {"1", "", "", "", ""}};
  for (int i = 0; i < 5 && st[which][i][0]; ++i) {
    p.pitch[i].st = Num(st[which][i], -24, 24);
    p.pitch[i].weight = Num(wt[which][i], 0, 16);
    p.pitchCount = uint8_t(i + 1);
  }
  for (uint32_t l = 0; l < p.layerCount; ++l) {
    Layer& L = p.layers[l];
    L.posSource = which == 2 ? 1 : 0; L.select = which == 0 ? 1 : 0; L.pitchFirst = 0; L.pitchN = p.pitchCount;
    L.modOp[0] = which == 1 ? 1 : 0;
    L.glideStStart = Num(l == 1 ? "-12" : "0", -24, 24); L.glideStEnd = Num(l == 1 ? "12" : "0", -24, 24);
    L.glideCurve = Num("0.5", 0, 1);
  }
  struct T { uint8_t macro; ParamId id; const char *lo, *hi, *inLo, *inHi, *curve; };
  const T ts[] = {
      {0, ParamId::Overlap, "0.1", "0.9", "0", "1", "3"},
      {1, ParamId::WindowSustain, "0.2", "0.9", "0", "1", "1"},
      {1, ParamId::WindowSkew, "0.3", "0.7", "0", "1", "1"},
      {2, ParamId::DelayMs, "600", "2000", "0", "1", "1.5"},
      {3, ParamId::SprayMs, "0", "400", "0.8", "1", "1"},
      {4, ParamId::DelayMix, "0", "0.6", "0", "1", "1"},
      {4, ParamId::ReverbMix, "0", "0.7", "0", "1", "0.5"},
  };
  for (const T& t : ts) {
    if (p.macroCount == 0 || p.macros[p.macroCount - 1].id != t.macro) {
      p.macros[p.macroCount] = Macro{t.macro, p.targetCount, 0, 0};
      ++p.macroCount;
    }
    Target& o = p.targets[p.targetCount++];
    const auto& d = brainscape::kParamTable[uint32_t(t.id) - 1];
    o.param = uint32_t(t.id);
    o.lo = Num(t.lo, d.min, d.max); o.hi = Num(t.hi, d.min, d.max);
    o.inLo = Num(t.inLo, 0, 1); o.inHi = Num(t.inHi, 0, 1); o.curve = Num(t.curve, 0.0625f, 16);
    ++p.macros[p.macroCount - 1].n;
  }
  return p;
}

std::string Hex(const uint8_t* h, int n) {
  std::string s;
  char b[3];
  for (int i = 0; i < n; ++i) { std::snprintf(b, sizeof b, "%02x", h[i]); s += b; }
  return s;
}

uint64_t Mix(uint64_t& s) {
  uint64_t z = (s += 0x9e3779b97f4a7c15ull);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
  return z ^ (z >> 31);
}

void Rehash(std::vector<uint8_t>& b) {  // fix total_bytes and both hashes after a mutation
  if (b.size() < kHeaderBytes) return;
  const uint32_t n = uint32_t(b.size());
  for (int i = 0; i < 4; ++i) b[20 + i] = uint8_t(n >> (8 * i));
  // sound hash over STAT||MODE if both parse at their first two section slots
  size_t off = kHeaderBytes;
  const uint8_t* parts[2] = {nullptr, nullptr};
  size_t lens[2] = {0, 0};
  for (int s = 0; s < 2 && off + 8 <= b.size(); ++s) {
    uint32_t len = 0;
    for (int i = 0; i < 4; ++i) len |= uint32_t(b[off + 4 + i]) << (8 * i);
    if (len > b.size() - off - 8) break;
    parts[s] = b.data() + off + 8; lens[s] = len;
    off += 8 + ((size_t(len) + 3) & ~size_t(3));
  }
  if (parts[0] && parts[1]) Sha256(parts, lens, 2, b.data() + 32);
  std::memset(b.data() + 64, 0, 32);
  const uint8_t* pp[1] = {b.data()};
  const size_t pl[1] = {b.size()};
  uint8_t h[32];
  Sha256(pp, pl, 1, h);
  std::memcpy(b.data() + 64, h, 32);
}

}  // namespace

int main(int argc, char** argv) {
  const std::string mode = argc > 1 ? argv[1] : "hashes";
  std::vector<std::vector<uint8_t>> pkgs;
  for (int i = 0; i < 3; ++i) pkgs.push_back(Encode(Sample(i), "{\"schema_version\": 1}\n"));
  if (mode == "hashes") {
    std::printf("sizeof(DecodedPackage)=%zu kModeBytes=%u\n", sizeof(DecodedPackage), kModeBytes);
    for (int i = 0; i < 3; ++i) {
      DecodedPackage d;
      const Error e = Decode(pkgs[size_t(i)].data(), pkgs[size_t(i)].size(), &d);
      const std::vector<uint8_t> again = Encode(d, "{\"schema_version\": 1}\n");
      const uint8_t* pp[1] = {pkgs[size_t(i)].data()};
      const size_t pl[1] = {pkgs[size_t(i)].size()};
      uint8_t h[32];
      Sha256(pp, pl, 1, h);
      std::printf("mode %d: %zu bytes  sha256 %s  decode=%s  re-encode identical=%s\n", i, pkgs[size_t(i)].size(),
                  Hex(h, 32).c_str(), ErrorName(e), again == pkgs[size_t(i)] ? "yes" : "NO");
    }
    return 0;
  }
  if (mode == "seeds") {  // seed corpus for the libFuzzer harness
    for (int i = 0; i < 3; ++i) {
      const std::string path = std::string(argc > 2 ? argv[2] : ".") + "/seed" + std::to_string(i) + ".bsp";
      FILE* f = std::fopen(path.c_str(), "wb");
      if (!f) return 1;
      std::fwrite(pkgs[size_t(i)].data(), 1, pkgs[size_t(i)].size(), f);
      std::fclose(f);
    }
    return 0;
  }
  if (mode == "fuzz") {
    const uint64_t iters = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 1000000ull;
    uint64_t hist[unsigned(Error::kCount)] = {};
    uint64_t accepted = 0, reencodeBad = 0, s = 12345;
    const uint32_t interesting[] = {0u, 1u, 2u, 3u, 0x7Fu, 0x80u, 0xFFu, 0xFFFFu, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu,
                                    0x7F800000u, 0xFF800000u, 0x7FC00000u, 0x00000001u, 0x00800000u, 0x3F800000u, 96u, 2000u};
    std::vector<uint8_t> b;
    for (uint64_t it = 0; it < iters; ++it) {
      b = pkgs[it % 3];
      const int nm = 1 + int(Mix(s) % 4);
      for (int k = 0; k < nm; ++k) {
        const uint64_t r = Mix(s);
        switch (r % 6) {
          case 0: if (!b.empty()) b[size_t(Mix(s) % b.size())] ^= uint8_t(1u << (Mix(s) % 8)); break;
          case 1: if (!b.empty()) b[size_t(Mix(s) % b.size())] = uint8_t(Mix(s)); break;
          case 2: if (b.size() >= 4) { const size_t o = size_t(Mix(s) % (b.size() / 4)) * 4; const uint32_t v = interesting[Mix(s) % (sizeof interesting / 4)];
                    for (int i = 0; i < 4; ++i) b[o + size_t(i)] = uint8_t(v >> (8 * i)); } break;
          case 3: b.resize(size_t(Mix(s) % (b.size() + 1))); break;
          case 4: { const size_t add = size_t(Mix(s) % 64); for (size_t i = 0; i < add; ++i) b.push_back(uint8_t(Mix(s))); } break;
          case 5: if (b.size() > 8) { const size_t o = size_t(Mix(s) % (b.size() - 4)); const uint32_t v = interesting[Mix(s) % (sizeof interesting / 4)];
                    for (int i = 0; i < 4; ++i) b[o + size_t(i)] = uint8_t(v >> (8 * i)); } break;
        }
      }
      if (Mix(s) % 4 != 0) Rehash(b);  // most iterations get past the hash checks
      DecodedPackage d;
      // copy into an exactly sized heap buffer so ASan sees any read past the end
      std::vector<uint8_t> exact(b);
      const uint8_t* data = exact.empty() ? nullptr : exact.data();
      const Error e = Decode(data, exact.size(), &d, false);
      ++hist[unsigned(e)];
      if (e == Error::None) {
        ++accepted;
        // An accepted package's STAT and MODE re-encode to the same bytes: one encoding
        // per decoded state (blob-level Compile -> Decompile -> Compile idempotence),
        // checked through sound_hash = SHA-256(STAT || MODE).
        std::vector<uint8_t> again = Encode(d, "{\"schema_version\": 1}\n");
        if (std::memcmp(again.data() + 32, exact.data() + 32, 32) != 0) ++reencodeBad;
      }
    }
    std::printf("fuzz %llu iterations: accepted %llu (re-encode mismatches %llu)\n", (unsigned long long)iters,
                (unsigned long long)accepted, (unsigned long long)reencodeBad);
    for (unsigned e = 0; e < unsigned(Error::kCount); ++e)
      if (hist[e]) std::printf("  %-18s %llu\n", ErrorName(Error(e)), (unsigned long long)hist[e]);
    return 0;
  }
  return 1;
}
