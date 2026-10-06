// The floating-point environment at every entry point and the in-code flush
// (docs/design/determinism-profile.md §4.1, §4.3, §6.4).
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "FpEnvTestUtil.h"
#include "brainscape/Engine.h"
#include "brainscape/HostArenas.h"
#include "catch.hpp"
#include "detail/FlushTiny.h"

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

EngineConfig SmallConfig() {
  EngineConfig cfg;
  cfg.historyFrames = 1u << 15;
  return cfg;
}

// Every stage and stochastic feature engaged, with feedback.
const std::vector<std::pair<ParamId, float>> kBusyPreset = {
    {ParamId::DelayMs, 100.0f},         {ParamId::Mix, 0.7f},
    {ParamId::Feedback, 0.6f},          {ParamId::OutTrimDb, -3.0f},
    {ParamId::GrainSizeMs, 60.f},       {ParamId::Overlap, 0.55f},
    {ParamId::SprayMs, 50.0f},          {ParamId::PitchSt, 7.0f},
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

// Renders `input` (stereo) in blocks of `block` frames. With a host word, that word is
// installed around every engine call, as a careless host would leave it; the engine's
// guard must make it irrelevant, and must hand it back unchanged after each call.
Stereo RenderEvents(const Stereo& input, const std::vector<std::pair<ParamId, float>>& preset,
                    const std::vector<Event>& events, uint32_t block, bool hostile) {
  const EngineConfig cfg = SmallConfig();
  const detail::FpWord host = hostile ? testing::kHostileFpWord : detail::kFpProfileWord;
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
  auto setParam = [&](ParamId id, float v) { call([&] { engine.SetParam(id, v); }); };
  for (const auto& p : preset) setParam(p.first, p.second);
  call([&] { engine.Reset(); });

  const size_t frames = input.l.size();
  Stereo out;
  out.l.assign(frames, 0.f);
  out.r.assign(frames, 0.f);
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
    const float* ins[2]  = {input.l.data() + pos, input.r.data() + pos};
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

}  // namespace

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
    events.push_back({f, 0, ParamId::PitchSt, Bits(static_cast<float>(f % 7) - 3.0f)});
    events.push_back({f, 0, ParamId::Feedback, Bits(static_cast<float>(f % 11) * 0.1f)});
  }
  events.push_back({4800, 1, ParamId::Mix, 0u});
  events.push_back({9600, 2, ParamId::Mix, 0u});
  events.push_back({12000, 3, ParamId::Mix, 0u});

  const Stereo clean   = RenderEvents(input, kBusyPreset, events, 48, false);
  const Stereo hostile = RenderEvents(input, kBusyPreset, events, 48, true);
  REQUIRE(std::memcmp(clean.l.data(), hostile.l.data(), clean.l.size() * sizeof(float)) == 0);
  REQUIRE(std::memcmp(clean.r.data(), hostile.r.data(), clean.r.size() * sizeof(float)) == 0);
}
