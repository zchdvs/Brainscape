#include <cmath>
#include <cstring>
#include <vector>

#include "brainscape/DenormalGuard.h"
#include "brainscape/Engine.h"
#include "brainscape/HostArenas.h"
#include "catch.hpp"

using namespace brainscape;

namespace {

// Deterministic input generator (xorshift32) — same sequence every run/platform.
struct Rng {
  uint32_t s = 0x1234567u;
  float Next() {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return (static_cast<float>(s & 0xFFFFFFu) / 8388608.0f) - 1.0f;  // [-1, 1)
  }
};

EngineConfig SmallConfig() {
  EngineConfig cfg;
  cfg.sampleRate    = 48000.0;
  cfg.maxBlockSize  = 512;
  cfg.historyFrames = 1u << 15;  // 32768 frames — fast to clear in tests
  return cfg;
}

// Mirrors the engine's write-path quantizer so tests can compute exact expectations.
int16_t Quantize(float x) {
  float s = x * 32767.0f;
  if (s > 32767.0f) s = 32767.0f;
  if (s < -32768.0f) s = -32768.0f;
  return static_cast<int16_t>(std::lrintf(s));
}

struct RenderResult {
  std::vector<float> l, r;
};

// Render `input` (mono, duplicated to both channels) through a fresh engine using
// the given block sizes cycled in order.
RenderResult Render(const EngineConfig& cfg, const std::vector<float>& input,
                    const std::vector<std::pair<ParamId, float>>& params, bool primeParams,
                    const std::vector<uint32_t>& blockSizes) {
  host::HeapArenas arenas(PlanMemory(cfg));
  Engine engine;
  REQUIRE(engine.Init(cfg, arenas.get()));
  for (auto& p : params) engine.SetParam(p.first, p.second);
  if (primeParams) engine.Reset();  // drains pending + snaps smoothers

  RenderResult out;
  out.l.assign(input.size(), 0.f);
  out.r.assign(input.size(), 0.f);

  size_t pos = 0, blockIdx = 0;
  while (pos < input.size()) {
    const uint32_t want = blockSizes[blockIdx % blockSizes.size()];
    const auto n = static_cast<uint32_t>(
        std::min<size_t>(want, input.size() - pos));
    const float* ins[2]  = {input.data() + pos, input.data() + pos};
    float*       outs[2] = {out.l.data() + pos, out.r.data() + pos};
    Engine::ProcessContext ctx;
    ctx.in        = ins;
    ctx.out       = outs;
    ctx.numFrames = n;
    engine.Process(ctx);
    pos += n;
    ++blockIdx;
  }
  return out;
}

}  // namespace

TEST_CASE("PlanMemory sizes the bulk arena for the history ring") {
  EngineConfig cfg;  // defaults: 2^22 frames, stereo int16
  const MemoryPlan plan = PlanMemory(cfg);
  REQUIRE(plan.bytes[static_cast<size_t>(Tier::Bulk)] ==
          (size_t{1} << 22) * 2u * sizeof(int16_t));  // 16 MiB
  REQUIRE(plan.align[static_cast<size_t>(Tier::Bulk)] == 32);
  REQUIRE(plan.bytes[static_cast<size_t>(Tier::Hot)] == 0);
}

TEST_CASE("Init validates its inputs") {
  EngineConfig cfg = SmallConfig();
  host::HeapArenas arenas(PlanMemory(cfg));
  Engine engine;

  SECTION("rejects non-power-of-two historyFrames") {
    EngineConfig bad  = cfg;
    bad.historyFrames = 1000;
    REQUIRE_FALSE(engine.Init(bad, arenas.get()));
  }
  SECTION("rejects an undersized bulk arena") {
    Arenas small = arenas.get();
    small.bytes[static_cast<size_t>(Tier::Bulk)] /= 2;
    REQUIRE_FALSE(engine.Init(cfg, small));
  }
  SECTION("rejects a null bulk arena") {
    Arenas null_arenas                            = arenas.get();
    null_arenas.base[static_cast<size_t>(Tier::Bulk)] = nullptr;
    REQUIRE_FALSE(engine.Init(cfg, null_arenas));
  }
  SECTION("rejects invalid sample rate") {
    EngineConfig bad = cfg;
    bad.sampleRate   = 0.0;
    REQUIRE_FALSE(engine.Init(bad, arenas.get()));
  }
  SECTION("accepts a valid config") { REQUIRE(engine.Init(cfg, arenas.get())); }
}

TEST_CASE("parameter table and accessors") {
  size_t count = 0;
  REQUIRE(Descriptors(&count) != nullptr);
  REQUIRE(count == kNumParams);
  REQUIRE(FindParam(static_cast<ParamId>(999)) == nullptr);

  EngineConfig cfg = SmallConfig();
  host::HeapArenas arenas(PlanMemory(cfg));
  Engine engine;
  REQUIRE(engine.Init(cfg, arenas.get()));

  engine.SetParam(ParamId::Mix, 0.75f);
  REQUIRE(engine.GetParam(ParamId::Mix) == 0.75f);
  engine.SetParam(ParamId::Mix, 9.0f);  // clamped to descriptor range
  REQUIRE(engine.GetParam(ParamId::Mix) == 1.0f);
  engine.SetParam(ParamId::Feedback, -1.0f);
  REQUIRE(engine.GetParam(ParamId::Feedback) == 0.0f);
}

// Design contract #2 (unity-rate null): with mix=1, feedback=0, trim=0 dB, the
// output equals the int16-quantized input delayed by exactly delayFrames — bit-exact.
TEST_CASE("delay null test: Tu path is sample-exact through the int16 ring") {
  EngineConfig cfg = SmallConfig();
  const uint32_t delayFrames = 480;  // DelayMs = 10 @ 48 kHz

  Rng rng;
  std::vector<float> input(3000);
  input[0] = 1.0f;  // impulse on top of noise
  for (size_t i = 1; i < input.size(); ++i) input[i] = 0.5f * rng.Next();

  const auto out = Render(cfg, input,
                          {{ParamId::DelayMs, 10.0f},
                           {ParamId::Mix, 1.0f},
                           {ParamId::Feedback, 0.0f},
                           {ParamId::OutTrimDb, 0.0f}},
                          /*primeParams=*/true, {64});

  for (size_t n = 0; n < out.l.size(); ++n) {
    const float expected =
        n >= delayFrames
            ? static_cast<float>(Quantize(input[n - delayFrames])) * (1.0f / 32767.0f)
            : 0.0f;
    REQUIRE(out.l[n] == expected);
    REQUIRE(out.r[n] == expected);
  }
}

// Design contract #1: rendering the same input under different block splittings
// produces identical samples — including smoother trajectories (per-sample, never
// per-block) and parameter drain timing (all pending at first block).
TEST_CASE("block-splitting bit-exactness") {
  EngineConfig cfg = SmallConfig();

  Rng rng;
  std::vector<float> input(4096);
  for (auto& x : input) x = 0.8f * rng.Next();

  const std::vector<std::pair<ParamId, float>> params = {
      {ParamId::DelayMs, 10.0f},
      {ParamId::Mix, 0.7f},
      {ParamId::Feedback, 0.5f},
      {ParamId::OutTrimDb, -3.0f}};

  // primeParams=false: smoothers ramp from defaults — exercises per-sample smoothing.
  const auto ref = Render(cfg, input, params, false, {512});
  for (uint32_t split : {1u, 7u, 32u, 48u, 64u, 127u}) {
    const auto other = Render(cfg, input, params, false, {split});
    REQUIRE(std::memcmp(ref.l.data(), other.l.data(), ref.l.size() * sizeof(float)) == 0);
    REQUIRE(std::memcmp(ref.r.data(), other.r.data(), ref.r.size() * sizeof(float)) == 0);
  }
  // Mixed block sizes too.
  const auto mixed = Render(cfg, input, params, false, {48u, 1u, 127u, 32u});
  REQUIRE(std::memcmp(ref.l.data(), mixed.l.data(), ref.l.size() * sizeof(float)) == 0);
}

// Design contract #4 (skeleton scope): feedback below unity decays, never rails.
TEST_CASE("feedback is bounded and decays") {
  EngineConfig cfg = SmallConfig();

  std::vector<float> input(48000, 0.0f);
  input[0] = 1.0f;

  const auto out = Render(cfg, input,
                          {{ParamId::DelayMs, 1.0f},  // 48-frame loop
                           {ParamId::Mix, 1.0f},
                           {ParamId::Feedback, 0.95f},
                           {ParamId::OutTrimDb, 0.0f}},
                          true, {256});

  float peak = 0.f;
  for (float v : out.l) peak = std::max(peak, std::fabs(v));
  REQUIRE(peak <= 2.0f);

  auto rms = [&](size_t from, size_t len) {
    double acc = 0;
    for (size_t i = from; i < from + len; ++i) acc += double(out.l[i]) * out.l[i];
    return std::sqrt(acc / double(len));
  };
  REQUIRE(rms(43200, 4800) < rms(0, 4800));  // tail quieter than head
}

TEST_CASE("sample counter is free-running") {
  EngineConfig cfg = SmallConfig();
  host::HeapArenas arenas(PlanMemory(cfg));
  Engine engine;
  REQUIRE(engine.Init(cfg, arenas.get()));

  std::vector<float> silence(100, 0.f);
  std::vector<float> outL(100), outR(100);
  const float* ins[2]  = {silence.data(), silence.data()};
  float*       outs[2] = {outL.data(), outR.data()};
  Engine::ProcessContext ctx;
  ctx.in        = ins;
  ctx.out       = outs;
  ctx.numFrames = 100;
  ctx.transportPlaying = false;  // counter must advance regardless of transport
  engine.Process(ctx);
  ctx.numFrames = 48;
  engine.Process(ctx);
  REQUIRE(engine.SampleCounter() == 148);
}

#if defined(BRAINSCAPE_DENORMAL_SSE)
TEST_CASE("denormal guard sets and restores FTZ/DAZ") {
  const unsigned before = _mm_getcsr();
  {
    ScopedDenormalGuard guard;
    REQUIRE((_mm_getcsr() & 0x8040u) == 0x8040u);
  }
  REQUIRE(_mm_getcsr() == before);
}
#endif
