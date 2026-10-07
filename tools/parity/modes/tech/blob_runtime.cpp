// PROBE (scratch): the dsp/ side of the package sketch — structural validator + decoder.
// Firmware-shaped: no allocation, no exceptions, no FP arithmetic (float checks are integer
// tests on the bit pattern, so no FP environment can change a verdict), no libc beyond
// memcpy/memset. Every read is bounds-checked against n before it happens.
#include "blob_format.h"

#include <cstring>

#include "brainscape/Params.h"

namespace bsp {
namespace {

uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint16_t Rd16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
float    BitsF(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }
uint32_t FBits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }

// Finite, and not a subnormal or -0: what canonicalization leaves (profile §3.7).
bool CanonicalShape(uint32_t b) {
  const uint32_t ex = b & 0x7F800000u;
  if (ex == 0x7F800000u) return false;
  if (ex == 0) return b == 0;  // only +0
  return true;
}
// A total order on canonical-shaped values as unsigned integers.
uint32_t Key(uint32_t b) { return (b & 0x80000000u) ? ~b : (b | 0x80000000u); }
bool InRange(uint32_t b, float lo, float hi) { return Key(b) >= Key(FBits(lo)) && Key(b) <= Key(FBits(hi)); }

// ── SHA-256 (FIPS 180-4) ──
struct Sha {
  uint32_t h[8]; uint8_t buf[64]; uint32_t used; uint64_t bytes;
  static uint32_t R(uint32_t x, int r) { return (x >> r) | (x << (32 - r)); }
  void Init() {
    static const uint32_t k0[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    for (int i = 0; i < 8; ++i) h[i] = k0[i];
    used = 0; bytes = 0;
  }
  void Block(const uint8_t* p) {
    static const uint32_t K[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) w[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 | uint32_t(p[4 * i + 2]) << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; ++i) {
      const uint32_t s0 = R(w[i - 15], 7) ^ R(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 = R(w[i - 2], 17) ^ R(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
      const uint32_t t1 = hh + (R(e, 6) ^ R(e, 11) ^ R(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
      const uint32_t t2 = (R(a, 2) ^ R(a, 13) ^ R(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
      hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
  }
  void Update(const uint8_t* p, size_t n) {
    bytes += n;
    while (n > 0) {
      const size_t take = (64 - used) < n ? (64 - used) : n;
      for (size_t i = 0; i < take; ++i) buf[used + i] = p[i];
      used += uint32_t(take); p += take; n -= take;
      if (used == 64) { Block(buf); used = 0; }
    }
  }
  void Final(uint8_t out[32]) {
    const uint64_t bits = bytes * 8u;
    const uint8_t one = 0x80, zero = 0;
    Update(&one, 1);
    while (used != 56) Update(&zero, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; ++i) len[i] = uint8_t(bits >> (56 - 8 * i));
    Update(len, 8);
    for (int i = 0; i < 8; ++i) for (int j = 0; j < 4; ++j) out[4 * i + j] = uint8_t(h[i] >> (24 - 8 * j));
  }
};

bool Eq32(const uint8_t* a, const uint8_t* b) {  // no memcmp: the symbol audit allows none
  uint8_t d = 0;
  for (int i = 0; i < 32; ++i) d = uint8_t(d | (a[i] ^ b[i]));
  return d == 0;
}

}  // namespace

void Sha256(const uint8_t* const* parts, const size_t* lens, int count, uint8_t out[32]) noexcept {
  Sha s;
  s.Init();
  for (int i = 0; i < count; ++i) s.Update(parts[i], lens[i]);
  s.Final(out);
}

const char* ErrorName(Error e) noexcept {
  static const char* const k[] = {"None", "Truncated", "Magic", "PackageFormat", "BlobFormat", "TotalBytes",
      "SectionBounds", "SectionDup", "SectionMissing", "PackageHash", "SoundHash", "StatCount", "StatOrder",
      "StatNonFinite", "StatNonCanonical", "PerfRange", "ModeLength", "ModeCount", "ModeEnum", "ModeIndex",
      "ModeNonFinite", "ModeRange", "ModePadding"};
  return unsigned(e) < unsigned(Error::kCount) ? k[unsigned(e)] : "?";
}

Error Decode(const uint8_t* p, size_t n, DecodedPackage* out, bool skipHashes) noexcept {
  std::memset(out, 0, sizeof *out);
  if (n < kHeaderBytes) return Error::Truncated;
  if (p[0] != 'B' || p[1] != 'S' || p[2] != 'P' || p[3] != 'K') return Error::Magic;
  if (Rd16(p + 4) != kPackageFormat) return Error::PackageFormat;
  if (Rd32(p + 12) != kBlobFormat) return Error::BlobFormat;
  if (Rd32(p + 20) != n || Rd32(p + 28) != 0) return Error::TotalBytes;
  out->soundRev = Rd32(p + 8);
  out->blobFormat = Rd32(p + 12);
  out->schemaVersion = Rd32(p + 16);
  const uint32_t sections = Rd32(p + 24);
  // Section walk: every length is checked against what remains before it is used.
  const uint8_t* stat = nullptr; uint32_t statLen = 0;
  const uint8_t* mode = nullptr; uint32_t modeLen = 0;
  size_t off = kHeaderBytes;
  for (uint32_t s = 0; s < sections; ++s) {
    if (n - off < 8) return Error::SectionBounds;
    const uint32_t tag = Rd32(p + off), len = Rd32(p + off + 4);
    off += 8;
    if (len > n - off) return Error::SectionBounds;
    const size_t padded = (size_t(len) + 3u) & ~size_t(3);
    if (padded > n - off) return Error::SectionBounds;
    for (size_t i = len; i < padded; ++i) if (p[off + i] != 0) return Error::ModePadding;
    if (tag == kTagStat) { if (stat) return Error::SectionDup; stat = p + off; statLen = len; }
    else if (tag == kTagMode) { if (mode) return Error::SectionDup; mode = p + off; modeLen = len; }
    off += padded;  // unknown sections are skipped (additive evolution)
  }
  if (off != n) return Error::SectionBounds;
  if (!stat || !mode) return Error::SectionMissing;
  if (!skipHashes) {
    uint8_t h[32];
    static const uint8_t zeros[32] = {};
    const uint8_t* parts[3] = {p, zeros, p + 96};
    const size_t lens[3] = {64, 32, n - 96};
    Sha256(parts, lens, 3, h);
    if (!Eq32(h, p + 64)) return Error::PackageHash;
    const uint8_t* sp[2] = {stat, mode};
    const size_t sl[2] = {statLen, modeLen};
    Sha256(sp, sl, 2, h);
    if (!Eq32(h, p + 32)) return Error::SoundHash;
  }
  for (int i = 0; i < 32; ++i) out->soundHash[i] = p[32 + i];

  // STAT
  if (statLen < 4) return Error::StatCount;
  const uint32_t leaves = Rd32(stat);
  if (leaves > kMaxLeaves || statLen != 4 + 8u * leaves + 8u) return Error::StatCount;
  uint32_t prev = 0;
  for (uint32_t i = 0; i < leaves; ++i) {
    const uint32_t id = Rd32(stat + 4 + 8 * i), bits = Rd32(stat + 8 + 8 * i);
    if (i > 0 && id <= prev) return Error::StatOrder;
    prev = id;
    if (!CanonicalShape(bits)) return Error::StatNonFinite;
    if (const brainscape::ParamDescriptor* d = id >= 1 && id <= brainscape::kNumParams ? &brainscape::kParamTable[id - 1] : nullptr) {
      if (!InRange(bits, d->min, d->max)) return Error::StatNonCanonical;
    }
    out->leaves[i].id = id;
    out->leaves[i].value = BitsF(bits);
  }
  out->leafCount = leaves;
  const uint8_t* perf = stat + 4 + 8 * leaves;
  if (perf[0] > 1 || perf[1] > 1 || perf[2] > 5 || perf[3] > 2) return Error::PerfRange;
  const uint32_t us = Rd32(perf + 4);
  if (us < 150000u || us > 2000000u) return Error::PerfRange;  // 30..400 BPM as integer us/quarter
  out->globalReverse = perf[0]; out->timeMode = perf[1]; out->subdiv = perf[2]; out->tempoSource = perf[3];
  out->usPerQuarter = us;

  // MODE (fixed length for blob_format 1)
  if (modeLen != kModeBytes) return Error::ModeLength;
  const uint8_t* m = mode;
  out->layerCount = m[0]; out->pitchCount = m[1]; out->macroCount = m[2]; out->targetCount = m[3];
  if (m[0] < 1 || m[0] > kMaxLayers || m[1] > kMaxPitch || m[2] > kMaxMacros || m[3] > kMaxTargets) return Error::ModeCount;
  out->schedSources = m[4]; out->modeSubdiv = m[5];
  if ((m[4] & ~0x1Fu) != 0 || m[4] == 0 || m[5] > 5) return Error::ModeEnum;
  out->burstCount = Rd16(m + 6);
  if (out->burstCount < 1 || out->burstCount > 8) return Error::ModeRange;
  const uint32_t sp = Rd32(m + 8);
  if (!CanonicalShape(sp)) return Error::ModeNonFinite;
  if (!InRange(sp, 0.0f, 100.0f)) return Error::ModeRange;
  out->burstSpacingMs = BitsF(sp);
  const uint8_t* L = m + 12;
  for (uint32_t l = 0; l < kMaxLayers; ++l, L += kLayerBytes) {
    Layer& o = out->layers[l];
    if (l >= out->layerCount) {  // unused slots must be all zero: one encoding per mode
      for (uint32_t i = 0; i < kLayerBytes; ++i) if (L[i] != 0) return Error::ModePadding;
      continue;
    }
    o.posSource = L[0]; o.sizeLaw = L[1]; o.select = L[2]; o.quantize = L[3];
    if (L[0] > 3 || L[1] > 2 || L[2] > 3 || L[3] > 2) return Error::ModeEnum;
    o.scaleMask = Rd16(L + 4);
    if (o.scaleMask > 0x0FFFu) return Error::ModeEnum;
    o.pitchFirst = L[6]; o.pitchN = L[7];
    if (o.pitchN < 1 || uint32_t(o.pitchFirst) + o.pitchN > out->pitchCount) return Error::ModeIndex;
    for (int k = 0; k < 2; ++k) { o.modOp[k] = L[8 + k]; o.modBand[k] = L[10 + k]; if (L[8 + k] > 5 || L[10 + k] > 3) return Error::ModeEnum; }
    const uint32_t g0 = Rd32(L + 12), g1 = Rd32(L + 16), gc = Rd32(L + 20);
    if (!CanonicalShape(g0) || !CanonicalShape(g1) || !CanonicalShape(gc)) return Error::ModeNonFinite;
    if (!InRange(g0, -24.f, 24.f) || !InRange(g1, -24.f, 24.f) || !InRange(gc, 0.f, 1.f)) return Error::ModeRange;
    o.glideStStart = BitsF(g0); o.glideStEnd = BitsF(g1); o.glideCurve = BitsF(gc);
  }
  const uint8_t* P = m + 12 + kMaxLayers * kLayerBytes;
  for (uint32_t i = 0; i < kMaxPitch; ++i, P += 8) {
    const uint32_t st = Rd32(P), w = Rd32(P + 4);
    if (i >= out->pitchCount) { if (st != 0 || w != 0) return Error::ModePadding; continue; }
    if (!CanonicalShape(st) || !CanonicalShape(w)) return Error::ModeNonFinite;
    if (!InRange(st, -24.f, 24.f) || !InRange(w, 0.f, 16.f)) return Error::ModeRange;
    out->pitch[i].st = BitsF(st); out->pitch[i].weight = BitsF(w);
  }
  const uint8_t* M = P;
  for (uint32_t i = 0; i < kMaxMacros; ++i, M += 4) {
    if (i >= out->macroCount) { if (Rd32(M) != 0) return Error::ModePadding; continue; }
    Macro& o = out->macros[i];
    o.id = M[0]; o.first = M[1]; o.n = M[2];
    if (M[3] != 0) return Error::ModePadding;
    if (M[0] >= 8) return Error::ModeEnum;
    if (uint32_t(o.first) + o.n > out->targetCount) return Error::ModeIndex;
  }
  const uint8_t* T = M;
  for (uint32_t i = 0; i < kMaxTargets; ++i, T += 24) {
    if (i >= out->targetCount) { for (int k = 0; k < 24; ++k) if (T[k] != 0) return Error::ModePadding; continue; }
    Target& o = out->targets[i];
    o.param = Rd32(T);
    if (o.param < 1 || o.param > brainscape::kNumParams) return Error::ModeIndex;
    const brainscape::ParamDescriptor& d = brainscape::kParamTable[o.param - 1];
    uint32_t v[5];
    for (int k = 0; k < 5; ++k) { v[k] = Rd32(T + 4 + 4 * k); if (!CanonicalShape(v[k])) return Error::ModeNonFinite; }
    if (!InRange(v[0], d.min, d.max) || !InRange(v[1], d.min, d.max)) return Error::ModeRange;
    if (!InRange(v[2], 0.f, 1.f) || !InRange(v[3], 0.f, 1.f) || Key(v[2]) >= Key(v[3])) return Error::ModeRange;
    if (!InRange(v[4], 0.0625f, 16.f)) return Error::ModeRange;  // curve exponent > 0 (PowF domain)
    o.lo = BitsF(v[0]); o.hi = BitsF(v[1]); o.inLo = BitsF(v[2]); o.inHi = BitsF(v[3]); o.curve = BitsF(v[4]);
  }
  return Error::None;
}

}  // namespace bsp
