// The NaN-free boundary and the floating-point environment at every entry point
// (docs/design/determinism-profile.md §3.7, §4.1, §6.4): canonical parameters, the
// input functions, a hostile host environment, and the boundary fuzz test.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "FpEnvTestUtil.h"
#include "brainscape/Engine.h"
#include "brainscape/HostArenas.h"
#include "brainscape/InputCondition.h"
#include "catch.hpp"
#include "detail/FlushTiny.h"
#include "detail/GrainMath.h"

using namespace brainscape;
using testing::HostileFpScope;

namespace {

uint32_t Bits(float x) {
  uint32_t u;
  std::memcpy(&u, &x, sizeof u);
  return u;
}
float FromBits(uint32_t u) {
  float x;
  std::memcpy(&x, &u, sizeof u);
  return x;
}

// Integer-only generator (SplitMix32), so test vectors never depend on the FP
// environment they are built in.
struct IntRng {
  uint32_t s;
  uint32_t Next() {
    uint32_t z = (s += 0x9E3779B9u);
    z ^= z >> 16;
    z *= 0x21F0AAADu;
    z ^= z >> 15;
    z *= 0x735A2D97u;
    z ^= z >> 15;
    return z;
  }
};

// NaNs of both signs (quiet and signalling payloads), infinities, zeros, subnormals at
// both ends (0x000116C2 is ~1e-40), the normal boundary, the largest finite values, and
// the flush threshold.
constexpr uint32_t kSpecialBits[] = {
    0x7FC00000u, 0xFFC00000u, 0x7F800001u, 0xFFBFFFFFu, 0x7FA00000u, 0x7F800000u,
    0xFF800000u, 0x00000000u, 0x80000000u, 0x00000001u, 0x80000001u, 0x000116C2u,
    0x007FFFFFu, 0x807FFFFFu, 0x00800000u, 0x80800000u, 0x7F7FFFFFu, 0xFF7FFFFFu,
    0x1E3CE508u, 0x9E3CE508u,
};

bool IsNonFinite(uint32_t u) { return (u & 0x7F800000u) == 0x7F800000u; }
bool IsNan(uint32_t u) { return IsNonFinite(u) && (u & 0x007FFFFFu) != 0u; }

EngineConfig SmallConfig() {
  EngineConfig cfg;
  cfg.historyFrames = 1u << 15;
  return cfg;
}

// Every stage and stochastic feature engaged, with feedback.
const std::vector<std::pair<ParamId, float>> kBusyPreset = {
    {ParamId::DelayMs, 100.0f},         {ParamId::Mix, 0.7f},
    {ParamId::Feedback, 0.6f},          {ParamId::WetTrimDb, -3.0f},
    {ParamId::GrainSizeMs, 60.f},       {ParamId::Overlap, 0.55f},
    {ParamId::SprayMs, 50.0f},          {ParamId::TransposeSt, 7.0f},
    {ParamId::SpreadCents, 20.f},       {ParamId::ReverseProb, 0.3f},
    {ParamId::Jitter, 1.0f},            {ParamId::WindowSmooth, 0.7f},
    {ParamId::ModDepth, 0.3f},          {ParamId::ModRateHz, 2.0f},
    {ParamId::DelayMix, 0.3f},          {ParamId::DelayTimeMs, 60.0f},
    {ParamId::ReverbMix, 0.4f},         {ParamId::ReverbTime, 0.7f},
    {ParamId::FilterCutoffHz, 9000.0f}, {ParamId::FilterRes, 0.3f},
    {ParamId::FilterMorph, 0.5f},       {ParamId::TriggerSens, 0.8f},
    {ParamId::OnsetTrigger, 1.0f},      {ParamId::PositionSource, 1.0f}};

// One scripted event: a parameter set (raw bits), a freeze toggle or a trigger,
// applied before the block that starts at `frame`.
struct Event {
  uint32_t frame;
  int      kind;  // 0 = parameter, 1 = freeze on, 2 = freeze off, 3 = trigger
  ParamId  id;
  uint32_t valueBits;
};

struct Stereo {
  std::vector<float> l, r;
};

// Renders `input` (stereo) in blocks of `block` frames. The host word is installed
// around every engine call, as a careless host would leave it; the engine's guard must
// make it irrelevant, and must hand it back unchanged after each call.
Stereo RenderEvents(const Stereo& input, const std::vector<std::pair<ParamId, float>>& preset,
                    const std::vector<Event>& events, uint32_t block, detail::FpWord host,
                    bool canonicalizeFirst, bool sanitizeInput) {
  const EngineConfig cfg = SmallConfig();
  MemoryPlan plan;
  {
    const HostileFpScope scope(host);
    plan = PlanMemory(cfg);
  }
  host::HeapArenas arenas(plan);
  REQUIRE(arenas.ok());
  Engine engine;
  bool ok = false;
  size_t wordsLost = 0;  // calls after which the caller's word was not restored
  auto call = [&](auto&& fn) {
    const HostileFpScope scope(host);
    fn();
    if (detail::ReadFpControl() != host) ++wordsLost;
  };
  call([&] { ok = engine.Init(cfg, arenas.get()); });
  REQUIRE(ok);
  auto setParam = [&](ParamId id, float v) {
    if (canonicalizeFirst) v = Canonicalize(id, v);
    call([&] { engine.SetParam(id, v); });
  };
  for (const auto& p : preset) setParam(p.first, p.second);
  call([&] { engine.Reset(); });

  const size_t frames = input.l.size();
  Stereo out;
  out.l.assign(frames, 0.f);
  out.r.assign(frames, 0.f);
  std::vector<float> inL(block), inR(block);
  size_t ei = 0, pos = 0;
  while (pos < frames) {
    while (ei < events.size() && events[ei].frame <= pos) {
      const Event& ev = events[ei++];
      switch (ev.kind) {
        case 0: setParam(ev.id, FromBits(ev.valueBits)); break;
        case 1: engine.SetFreeze(true); break;
        case 2: engine.SetFreeze(false); break;
        default: engine.Trigger(); break;
      }
    }
    const auto n = static_cast<uint32_t>(std::min<size_t>(block, frames - pos));
    if (sanitizeInput) {
      const HostileFpScope scope(host);
      SanitizeInput(input.l.data() + pos, inL.data(), n);
      SanitizeInput(input.r.data() + pos, inR.data(), n);
    } else {
      std::copy(input.l.begin() + pos, input.l.begin() + pos + n, inL.begin());
      std::copy(input.r.begin() + pos, input.r.begin() + pos + n, inR.begin());
    }
    const float* ins[2]  = {inL.data(), inR.data()};
    float*       outs[2] = {out.l.data() + pos, out.r.data() + pos};
    Engine::ProcessContext ctx;
    ctx.in        = ins;
    ctx.out       = outs;
    ctx.numFrames = n;
    call([&] { engine.Process(ctx); });
    pos += n;
  }
  REQUIRE(wordsLost == 0);
  return out;
}

// Audio-like input from integers: x = k * 2^-23 with |k| < 2^23, so every value is
// exact in any environment.
Stereo GridNoise(size_t frames, uint32_t seed, int32_t amplitude) {
  IntRng rng{seed};
  Stereo s;
  s.l.resize(frames);
  s.r.resize(frames);
  for (size_t i = 0; i < frames; ++i) {
    const auto kl = static_cast<int32_t>(rng.Next() % (2u * amplitude + 1u)) - amplitude;
    const auto kr = static_cast<int32_t>(rng.Next() % (2u * amplitude + 1u)) - amplitude;
    s.l[i] = static_cast<float>(kl) * 0x1p-23f;
    s.r[i] = static_cast<float>(kr) * 0x1p-23f;
  }
  return s;
}

// The canonical value, as the profile states it (§3.7), for comparison.
float ReferenceCanonical(const ParamDescriptor& d, uint32_t u) {
  const uint32_t exponent = u & 0x7F800000u;
  if (exponent == 0x7F800000u) return d.min;
  const float v = exponent == 0u ? 0.0f : FromBits(u);
  return v < d.min ? d.min : (v > d.max ? d.max : v);
}

// ConditionInput24 computed in integers only: round half away from zero of x * 2^23
// on the magnitude, clamped to [-2^23, 2^23 - 1].
float ReferenceGrid24(uint32_t u) {
  if (IsNan(u)) return 0.0f;
  const bool negative = (u >> 31) != 0u;
  const uint32_t biased = (u >> 23) & 0xFFu;
  uint64_t q;
  if (biased == 0xFFu) {
    q = uint64_t{1} << 40;  // ±inf: saturate
  } else {
    const uint64_t m  = biased == 0u ? (u & 0x7FFFFFu) : ((u & 0x7FFFFFu) | 0x800000u);
    const int32_t  e2 = (biased == 0u ? -149 : static_cast<int32_t>(biased) - 150) + 23;
    if (e2 >= 0) {
      q = e2 > 30 ? uint64_t{1} << 40 : m << e2;
    } else if (e2 < -40) {
      q = 0;
    } else {
      const int32_t  s    = -e2;
      const uint64_t half = uint64_t{1} << (s - 1);
      q = (m >> s) + ((m & ((uint64_t{1} << s) - 1u)) >= half ? 1u : 0u);
    }
  }
  const uint64_t limit = negative ? (uint64_t{1} << 23) : (uint64_t{1} << 23) - 1u;
  if (q > limit) q = limit;
  const auto i = negative ? -static_cast<int64_t>(q) : static_cast<int64_t>(q);
  return static_cast<float>(i) * 0x1p-23f;
}

}  // namespace

// §3.7: the canonical value is decided on the bit pattern, so a host's DAZ cannot store
// different bits (a comparison-based rule kept 1e-40 under DAZ and gave +0 without it,
// changing every grain's first sample). Canonicalize is the same rule, exported.
TEST_CASE("SetParam canonicalizes on the bit pattern, identically in every host environment") {
  const EngineConfig cfg = SmallConfig();
  host::HeapArenas arenas(PlanMemory(cfg));
  Engine engine;
  REQUIRE(engine.Init(cfg, arenas.get()));

  std::vector<uint32_t> patterns(std::begin(kSpecialBits), std::end(kSpecialBits));
  IntRng rng{0xC0FFEEu};
  for (int i = 0; i < 2048; ++i) patterns.push_back(rng.Next());
  size_t count = 0;
  const ParamDescriptor* table = Descriptors(&count);
  for (size_t p = 0; p < count; ++p) {  // the range ends and their neighbours
    for (const float edge : {table[p].min, table[p].max}) {
      patterns.push_back(Bits(edge));
      patterns.push_back(Bits(edge) + 1u);
      patterns.push_back(Bits(edge) - 1u);
    }
  }

  const detail::FpWord hosts[] = {detail::kFpProfileWord, testing::kFtzDazFpWord,
                                  testing::kHostileFpWord};
  size_t mismatches = 0, malformed = 0;
  for (size_t p = 0; p < count; ++p) {
    const ParamDescriptor& d = table[p];
    // SetParam stores Leaf and Global rows; for the other kinds it is a no-op and GetParam
    // gives +0 (mode-compiler.md §4.1). Canonicalize is defined for every row.
    const bool stored = d.kind == ParamKind::Leaf || d.kind == ParamKind::Global;
    for (const uint32_t u : patterns) {
      const uint32_t expected = Bits(ReferenceCanonical(d, u));
      for (const detail::FpWord host : hosts) {
        float exported;
        {
          const HostileFpScope scope(host);
          engine.SetParam(d.id, FromBits(u));
          exported = Canonicalize(d.id, FromBits(u));
        }
        if (Bits(engine.GetParam(d.id)) != (stored ? expected : 0u) || Bits(exported) != expected) {
          ++mismatches;
        }
      }
      // Never non-finite, subnormal or -0; always inside the range.
      const uint32_t e = expected & 0x7F800000u;
      const float    v = FromBits(expected);
      if (IsNonFinite(expected) || (e == 0u && expected != 0u) || v < d.min || v > d.max) {
        ++malformed;
      }
    }
  }
  REQUIRE(mismatches == 0);
  REQUIRE(malformed == 0);
  REQUIRE(Bits(Canonicalize(static_cast<ParamId>(999), 3.0f)) == 0u);
  // Subnormal skew, the route by which NaN once reached the output (§3.7).
  REQUIRE(Bits(Canonicalize(ParamId::WindowSkew, FromBits(0x000116C2u))) == 0u);
  REQUIRE(Canonicalize(ParamId::ModRateHz, FromBits(0x80000001u)) == 0.01f);
  REQUIRE(Canonicalize(ParamId::DelayMs, FromBits(0x7F800000u)) == 1.0f);  // +inf -> min
}

TEST_CASE("SanitizeInput passes finite bits and zeroes NaN and infinity, in any environment") {
  std::vector<uint32_t> patterns(std::begin(kSpecialBits), std::end(kSpecialBits));
  IntRng rng{0x5A17u};
  for (int i = 0; i < 65536; ++i) patterns.push_back(rng.Next());
  std::vector<float> in(patterns.size()), out(patterns.size()), inPlace(patterns.size());
  for (size_t i = 0; i < patterns.size(); ++i) in[i] = FromBits(patterns[i]);

  size_t mismatches = 0;
  for (const detail::FpWord host : {detail::kFpProfileWord, testing::kHostileFpWord}) {
    std::vector<float> scalar(patterns.size());
    inPlace = in;
    {
      const HostileFpScope scope(host);
      for (size_t i = 0; i < in.size(); ++i) scalar[i] = SanitizeInput(in[i]);
      SanitizeInput(in.data(), out.data(), in.size());
      SanitizeInput(inPlace.data(), inPlace.data(), inPlace.size());
    }
    for (size_t i = 0; i < patterns.size(); ++i) {
      const uint32_t expected = IsNonFinite(patterns[i]) ? 0u : patterns[i];
      if (Bits(scalar[i]) != expected || Bits(out[i]) != expected ||
          Bits(inPlace[i]) != expected) {
        ++mismatches;
      }
    }
  }
  REQUIRE(mismatches == 0);
}

TEST_CASE("ConditionInput24 puts input on the codec grid, exactly, in any environment") {
  std::vector<uint32_t> patterns(std::begin(kSpecialBits), std::end(kSpecialBits));
  IntRng rng{0x24B17u};
  for (int i = 0; i < 65536; ++i) patterns.push_back(rng.Next());
  // Ties and their neighbours at several magnitudes, and the clamp edges.
  for (int32_t k : {0, 1, 2, 3, 1000, 4194303, 8388606, 8388607, 8388608}) {
    for (const int sign : {1, -1}) {
      const float tie = static_cast<float>(sign) * (static_cast<float>(k) + 0.5f) * 0x1p-23f;
      for (const int32_t delta : {-1, 0, 1}) patterns.push_back(Bits(tie) + delta);
      patterns.push_back(Bits(static_cast<float>(sign * k) * 0x1p-23f));
    }
  }
  patterns.push_back(Bits(1.0f));
  patterns.push_back(Bits(-1.0f));
  patterns.push_back(Bits(1.5f));
  patterns.push_back(Bits(-1.5f));

  std::vector<float> in(patterns.size());
  for (size_t i = 0; i < patterns.size(); ++i) in[i] = FromBits(patterns[i]);
  size_t mismatches = 0, offGrid = 0;
  for (const detail::FpWord host : {detail::kFpProfileWord, testing::kHostileFpWord}) {
    std::vector<float> scalar(in.size()), block(in.size()), again(in.size());
    {
      const HostileFpScope scope(host);
      for (size_t i = 0; i < in.size(); ++i) scalar[i] = ConditionInput24(in[i]);
      ConditionInput24(in.data(), block.data(), in.size());
      ConditionInput24(block.data(), again.data(), block.size());  // idempotent on the grid
    }
    for (size_t i = 0; i < in.size(); ++i) {
      const uint32_t expected = Bits(ReferenceGrid24(patterns[i]));
      if (Bits(scalar[i]) != expected || Bits(block[i]) != expected) ++mismatches;
      if (Bits(again[i]) != expected) ++offGrid;
    }
  }
  REQUIRE(mismatches == 0);
  REQUIRE(offGrid == 0);
  REQUIRE(ConditionInput24(FromBits(0x7F800000u)) == 8388607.0f * 0x1p-23f);
  REQUIRE(ConditionInput24(FromBits(0xFF800000u)) == -1.0f);
  REQUIRE(Bits(ConditionInput24(FromBits(0xFFC00000u))) == 0u);
  REQUIRE(ConditionInput24(2.5f * 0x1p-23f) == 3.0f * 0x1p-23f);    // ties away from zero
  REQUIRE(ConditionInput24(-2.5f * 0x1p-23f) == -3.0f * 0x1p-23f);
}

// §3.7: the MakeEnv guard. A leg shorter than 2^-20 frames is absent, geometry and
// reciprocal both, so EnvValue(0) is never 0 * inf = NaN, and a subnormal leg cannot
// put index 0 on different sides of attackEnd under different flush modes. (This
// replaces the reciprocal-only guard, under which such a leg's index 0 was 0.)
TEST_CASE("MakeEnv treats envelope legs shorter than 2^-20 frames as absent") {
  using grainmath::EnvValue;
  using grainmath::MakeEnv;
  for (const uint32_t skewBits : {0x00000001u, 0x000116C2u, 0x007FFFFFu, Bits(1e-30f)}) {
    const auto e = MakeEnv(480.f, 0.0f, FromBits(skewBits));
    REQUIRE(Bits(e.attackEnd) == 0u);
    REQUIRE(Bits(e.attackInv) == 0u);
    REQUIRE(EnvValue(e, 0.f) == 1.0f);  // the decay leg starts at once
    REQUIRE(EnvValue(e, 1.f) > 0.99f);
  }
  // The decay side: a 2^-24-frame decay leg is absent too.
  const auto shortDecay = MakeEnv(1.f, 0.0f, 0x1.fffffep-1f);
  REQUIRE(Bits(shortDecay.decayInv) == 0u);
  REQUIRE(shortDecay.decayStart == 1.0f);
  // A one-frame leg still gets its reciprocal.
  const auto one = MakeEnv(480.f, 0.0f, 1.0f / 480.0f);
  REQUIRE(one.attackInv > 0.99f);
  REQUIRE(Bits(EnvValue(one, 0.f)) == 0u);
}

#if defined(BRAINSCAPE_FPENV_X64)
// §3.7: the input functions need no guard because no FP environment changes them. That
// includes a host that unmasked FP exceptions: the binary64 version of ConditionInput24
// trapped on a subnormal operand and on its inexact truncation (review finding).
TEST_CASE("the input functions never trap, even with every FP exception unmasked") {
  std::vector<uint32_t> patterns(std::begin(kSpecialBits), std::end(kSpecialBits));
  IntRng rng{0x7EA9u};
  for (int i = 0; i < 65536; ++i) patterns.push_back(rng.Next());
  for (int i = 0; i < 4096; ++i) patterns.push_back(rng.Next() & 0x807FFFFFu);  // subnormals
  std::vector<float> in(patterns.size()), sanitized(in.size()), grid(in.size());
  for (size_t i = 0; i < patterns.size(); ++i) in[i] = FromBits(patterns[i]);
  {
    const HostileFpScope scope(testing::kTrapAllFpWord);
    SanitizeInput(in.data(), sanitized.data(), in.size());
    ConditionInput24(in.data(), grid.data(), in.size());
    for (size_t i = 0; i < in.size(); ++i) grid[i] = ConditionInput24(grid[i]);
  }
  size_t mismatches = 0;
  for (size_t i = 0; i < patterns.size(); ++i) {
    if (Bits(sanitized[i]) != (IsNonFinite(patterns[i]) ? 0u : patterns[i])) ++mismatches;
    if (Bits(grid[i]) != Bits(ReferenceGrid24(patterns[i]))) ++mismatches;
  }
  REQUIRE(mismatches == 0);
}
#endif

// §4.3: the flush decides |x| < 1e-20 on the bits; it must agree with the profile's
// comparison form for every input.
TEST_CASE("the flush threshold test matches the profile's comparison form") {
  std::vector<uint32_t> patterns(std::begin(kSpecialBits), std::end(kSpecialBits));
  IntRng rng{0x7140u};
  for (int i = 0; i < 65536; ++i) patterns.push_back(rng.Next());
  for (const uint32_t edge : {detail::kTinyBits, detail::kTinyBits | 0x80000000u}) {
    for (int32_t delta = -2; delta <= 2; ++delta) patterns.push_back(edge + delta);
  }
  constexpr float kTiny = 1e-20f;
  size_t mismatches = 0;
  for (const uint32_t u : patterns) {
    const float x = FromBits(u);
    if (detail::IsTiny(x) != (x < kTiny && x > -kTiny)) ++mismatches;
  }
  REQUIRE(mismatches == 0);
  REQUIRE(Bits(1e-20f) == detail::kTinyBits);
}

// §4.1 and §6.4: every entry point (PlanMemory, Init, SetParam, Reset, Process) owns
// the control word, so a host that leaves FTZ, DAZ and round-toward-zero installed
// gets the clean render bit for bit — and its own word back after every call.
TEST_CASE("a hostile host FP environment reproduces the clean render") {
  const Stereo input = GridNoise(24000, 0x0DDBA11u, 6000000);
  std::vector<Event> events;
  for (uint32_t f = 2400; f < 24000; f += 2400) {
    events.push_back({f, 0, ParamId::TransposeSt, Bits(static_cast<float>(f % 7) - 3.0f)});
    events.push_back({f, 0, ParamId::Feedback, Bits(static_cast<float>(f % 11) * 0.1f)});
  }
  events.push_back({4800, 1, ParamId::Mix, 0u});
  events.push_back({9600, 2, ParamId::Mix, 0u});
  events.push_back({12000, 3, ParamId::Mix, 0u});

  const Stereo clean =
      RenderEvents(input, kBusyPreset, events, 48, detail::kFpProfileWord, false, false);
  const Stereo hostile =
      RenderEvents(input, kBusyPreset, events, 48, testing::kHostileFpWord, false, false);
  REQUIRE(std::memcmp(clean.l.data(), hostile.l.data(), clean.l.size() * sizeof(float)) == 0);
  REQUIRE(std::memcmp(clean.r.data(), hostile.r.data(), clean.r.size() * sizeof(float)) == 0);
#if defined(BRAINSCAPE_FPENV_X64)
  // Every exception unmasked: an FP operation in an entry point before its guard writes
  // the word, or after it restores the host's, traps here.
  const Stereo trapping =
      RenderEvents(input, kBusyPreset, events, 48, testing::kTrapAllFpWord, false, false);
  REQUIRE(std::memcmp(clean.l.data(), trapping.l.data(), clean.l.size() * sizeof(float)) == 0);
  REQUIRE(std::memcmp(clean.r.data(), trapping.r.data(), clean.r.size() * sizeof(float)) == 0);
#endif
}

// §3.7's CI fuzz test: NaN, ±inf and subnormal inputs and parameters, under a hostile
// host environment, give finite output equal to the run fed the sanitized input and the
// canonical parameter values in a clean environment. Several seeds: the largest finite
// inputs overflowed the output only where they met a positive trim and enough dry
// signal, which one seed happened to avoid (review finding).
TEST_CASE("fuzz: non-finite and subnormal inputs and parameters stay out of the output") {
  constexpr size_t kFrames = 19200;
  const std::pair<uint32_t, uint32_t> seeds[] = {
      {0xF022u, 0xBADF00Du}, {3u, 0x5EED3u}, {0x51EEDu, 0xA11CEu}, {0xC0DAu, 0x0B0Eu}};
  for (const auto& seed : seeds) {
    INFO("seeds " << seed.first << ", " << seed.second);
    IntRng rng{seed.first};
    Stereo input = GridNoise(kFrames, seed.second, 12000000);  // up to +3.1 dBFS
    for (size_t i = 0; i < kFrames; ++i) {
      for (float* x : {&input.l[i], &input.r[i]}) {
        const uint32_t roll = rng.Next() % 100u;
        if (roll < 3u) {
          *x = FromBits(kSpecialBits[rng.Next() % (sizeof kSpecialBits / sizeof kSpecialBits[0])]);
        } else if (roll < 5u) {
          *x = FromBits((rng.Next() & 0x807FFFFFu) | 1u);  // a subnormal
        }
      }
    }
    // The preset leaves (Leaf rows; the other kinds are no-ops for SetParam, tested in
    // test_params.cpp).
    std::vector<Event> events;
    for (uint32_t f = 0; f < kFrames; f += 64) {
      const uint32_t roll = rng.Next() % 8u;
      const ParamId  id   = LeafId(rng.Next() % kNumLeafParams);
      if (roll < 3u) {
        events.push_back({f, 0, id, kSpecialBits[rng.Next() % (sizeof kSpecialBits / sizeof kSpecialBits[0])]});
      } else if (roll < 4u) {
        events.push_back({f, 0, id, (rng.Next() & 0x807FFFFFu) | 1u});
      } else if (roll < 6u) {
        events.push_back({f, 0, id, rng.Next()});  // any bit pattern, mostly out of range
      } else if (roll == 6u) {
        events.push_back({f, 1 + static_cast<int>(rng.Next() % 3u), id, 0u});
      }
    }

    // Run A sanitizes inside the render, as a wrapper does, and hands SetParam the raw
    // bits; run B gets pre-sanitized input and canonical values.
    Stereo sanitized = input;
    SanitizeInput(sanitized.l.data(), sanitized.l.data(), kFrames);
    SanitizeInput(sanitized.r.data(), sanitized.r.data(), kFrames);
    const Stereo a =
        RenderEvents(input, kBusyPreset, events, 64, testing::kHostileFpWord, false, true);
    const Stereo b =
        RenderEvents(sanitized, kBusyPreset, events, 64, detail::kFpProfileWord, true, false);

    size_t nonFinite = 0;
    for (size_t i = 0; i < kFrames; ++i) {
      nonFinite += IsNonFinite(Bits(a.l[i])) + IsNonFinite(Bits(a.r[i]));
    }
    REQUIRE(nonFinite == 0);
    REQUIRE(std::memcmp(a.l.data(), b.l.data(), kFrames * sizeof(float)) == 0);
    REQUIRE(std::memcmp(a.r.data(), b.r.data(), kFrames * sizeof(float)) == 0);
  }
}

// §3.7: finite input gives finite output. At Mix 0 under +24 dB of trim the dry path
// carries FLT_MAX * 15.8, which overflowed to ±inf (review finding); the final mix now
// saturates it. Equal signs in both channels also overflow the detector's mono sum.
TEST_CASE("the largest finite input saturates at the output instead of overflowing") {
  constexpr size_t kFrames = 4800;
  constexpr float  kMax    = 0x1.fffffep127f;
  Stereo input;
  input.l.resize(kFrames);
  input.r.resize(kFrames);
  for (size_t i = 0; i < kFrames; ++i) {
    const float v = i % 3u == 0u ? kMax : (i % 3u == 1u ? -kMax : 0.25f);
    input.l[i]    = v;
    input.r[i]    = i % 4u < 2u ? v : -v;
  }
  const Stereo out = RenderEvents(input, {{ParamId::Mix, 0.0f}, {ParamId::WetTrimDb, 24.0f}},
                                  {}, 48, detail::kFpProfileWord, false, false);
  size_t nonFinite = 0, wrong = 0;
  for (size_t i = 0; i < kFrames; ++i) {
    nonFinite += IsNonFinite(Bits(out.l[i])) + IsNonFinite(Bits(out.r[i]));
    if (i % 3u != 2u && (out.l[i] != input.l[i] || out.r[i] != input.r[i])) ++wrong;
  }
  REQUIRE(nonFinite == 0);
  REQUIRE(wrong == 0);  // ±FLT_MAX in, ±FLT_MAX out
}

// §3.7: finite input never latches a non-finite value into engine state. The onset
// detector squares its input, and one sample past about 2e19 put +inf into its
// whitening memory: inf / inf = NaN inside, and no onset until Reset (review finding).
TEST_CASE("one huge finite input sample does not deafen the onset detector") {
  constexpr size_t kFrames = 384000, kSpikeAt = 48000;  // 8 s, spike at 1 s
  // Decaying grid-noise plucks every 250 ms.
  std::vector<float> clean(kFrames, 0.0f);
  IntRng rng{0x9E11u};
  for (size_t at = 6000; at < kFrames; at += 12000) {
    for (size_t i = 0; i < 2400 && at + i < kFrames; ++i) {
      const auto k = static_cast<int32_t>(rng.Next() % 8388607u) - 4194303;
      clean[at + i] = static_cast<float>(k) * 0x1p-23f * static_cast<float>(2400 - i) / 2400.0f;
    }
  }
  // Onsets in the last 3 s, once the whitening memory has decayed from the spike.
  auto lateOnsets = [&](float spike) {
    std::vector<float> in = clean;
    in[kSpikeAt]          = spike;
    host::HeapArenas arenas(PlanMemory(SmallConfig()));
    Engine engine;
    REQUIRE(engine.Init(SmallConfig(), arenas.get()));
    std::vector<float> l(kFrames), r(kFrames);
    uint32_t late = 0;
    for (size_t pos = 0; pos < kFrames;) {
      const auto   n       = static_cast<uint32_t>(std::min<size_t>(480, kFrames - pos));
      const float* ins[2]  = {in.data() + pos, in.data() + pos};
      float*       outs[2] = {l.data() + pos, r.data() + pos};
      Engine::ProcessContext ctx;
      ctx.in        = ins;
      ctx.out       = outs;
      ctx.numFrames = n;
      engine.Process(ctx);
      const uint32_t onsets = engine.ConsumeOnsetCount();
      if (pos >= kFrames - 144000) late += onsets;
      pos += n;
    }
    return late;
  };
  const uint32_t control = lateOnsets(0.0f);
  INFO("control: " << control << " late onsets");
  REQUIRE(control >= 10u);
  REQUIRE(lateOnsets(1e20f) == control);
  REQUIRE(lateOnsets(0x1.fffffep127f) == control);
}
