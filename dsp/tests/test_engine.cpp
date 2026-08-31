#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>
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

// Mirrors the engine's write-path quantizer (rounding-mode independent,
// NaN-safe, symmetric clamp) so tests can compute exact expectations.
int16_t Quantize(float x) {
  float s = x * 32767.0f;
  if (!(s < 32767.0f)) s = 32767.0f;
  if (!(s > -32767.0f)) s = -32767.0f;
  return static_cast<int16_t>(s >= 0.0f ? s + 0.5f : s - 0.5f);
}

struct RenderResult {
  std::vector<float> l, r;
};

struct MidRenderChange {
  size_t  atFrame = SIZE_MAX;  // absolute frame; applied before the block containing it
  ParamId id      = ParamId::Mix;
  float   value   = 0.f;
};

// Render `input` (mono, duplicated to both channels) through a fresh engine using
// the given block sizes cycled in order.
RenderResult Render(const EngineConfig& cfg, const std::vector<float>& input,
                    const std::vector<std::pair<ParamId, float>>& params, bool primeParams,
                    const std::vector<uint32_t>& blockSizes,
                    const MidRenderChange& change = {}) {
  host::HeapArenas arenas(PlanMemory(cfg));
  REQUIRE(arenas.ok());
  Engine engine;
  REQUIRE(engine.Init(cfg, arenas.get()));
  for (auto& p : params) engine.SetParam(p.first, p.second);
  if (primeParams) engine.Reset();  // drains pending + snaps smoothers

  RenderResult out;
  out.l.assign(input.size(), 0.f);
  out.r.assign(input.size(), 0.f);

  size_t pos = 0, blockIdx = 0;
  bool changed = false;
  while (pos < input.size()) {
    if (!changed && pos >= change.atFrame) {
      engine.SetParam(change.id, change.value,
                      static_cast<uint32_t>(change.atFrame - pos));
      changed = true;
    }
    const uint32_t want = blockSizes[blockIdx % blockSizes.size()];
    const auto     n    = static_cast<uint32_t>(std::min<size_t>(want, input.size() - pos));
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
  REQUIRE(arenas.ok());
  Engine engine;

  SECTION("rejects non-power-of-two historyFrames") {
    EngineConfig bad  = cfg;
    bad.historyFrames = 1000;
    REQUIRE_FALSE(engine.Init(bad, arenas.get()));
  }
  SECTION("rejects out-of-bounds historyFrames (floor and 32-bit overflow guard)") {
    EngineConfig bad = cfg;
    bad.historyFrames = 4;  // power of two, but below the floor
    REQUIRE_FALSE(engine.Init(bad, arenas.get()));
    bad.historyFrames = 1u << 27;  // power of two, above the ceiling
    REQUIRE_FALSE(engine.Init(bad, arenas.get()));
  }
  SECTION("rejects an undersized bulk arena") {
    Arenas small = arenas.get();
    small.bytes[static_cast<size_t>(Tier::Bulk)] /= 2;
    REQUIRE_FALSE(engine.Init(cfg, small));
  }
  SECTION("rejects a null bulk arena") {
    Arenas nulls                                  = arenas.get();
    nulls.base[static_cast<size_t>(Tier::Bulk)]   = nullptr;
    REQUIRE_FALSE(engine.Init(cfg, nulls));
  }
  SECTION("rejects a misaligned bulk arena") {
    // Offset the base by 2 bytes and shrink the config so the byte count still
    // passes — only the alignment check can be the reason Init refuses.
    Arenas misaligned = arenas.get();
    misaligned.base[static_cast<size_t>(Tier::Bulk)] =
        static_cast<char*>(misaligned.base[static_cast<size_t>(Tier::Bulk)]) + 2;
    misaligned.bytes[static_cast<size_t>(Tier::Bulk)] -= 2;
    EngineConfig tiny  = cfg;
    tiny.historyFrames = 1u << 14;
    REQUIRE_FALSE(engine.Init(tiny, misaligned));
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
  REQUIRE(Engine::Descriptors(&count) == Descriptors(nullptr));  // both spellings agree
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

// A single bad automation value must never poison the engine: NaN through the
// old comparison clamps latched the smoothers at NaN forever (review finding).
TEST_CASE("non-finite parameter values are rejected at the boundary") {
  EngineConfig cfg = SmallConfig();
  host::HeapArenas arenas(PlanMemory(cfg));
  Engine engine;
  REQUIRE(engine.Init(cfg, arenas.get()));

  const float bads[] = {std::numeric_limits<float>::quiet_NaN(),
                        std::numeric_limits<float>::infinity(),
                        -std::numeric_limits<float>::infinity()};
  size_t count = 0;
  const ParamDescriptor* table = Descriptors(&count);
  for (float bad : bads) {
    for (size_t i = 0; i < count; ++i) {
      engine.SetParam(table[i].id, bad);
      REQUIRE(std::isfinite(engine.GetParam(table[i].id)));
    }
  }
  // NaN maps to min deterministically.
  engine.SetParam(ParamId::Mix, std::numeric_limits<float>::quiet_NaN());
  REQUIRE(engine.GetParam(ParamId::Mix) == 0.0f);

  // And the audio stays finite afterwards.
  std::vector<float> input(2048, 0.25f);
  const auto out = Render(cfg, input, {{ParamId::Mix, std::numeric_limits<float>::quiet_NaN()}},
                          false, {64});
  for (float v : out.l) REQUIRE(std::isfinite(v));
}

// Design contract #2 (unity-rate null): with mix=1, feedback=0, trim=0 dB and
// dither off, the output equals the int16-quantized input delayed by exactly
// delayFrames — bit-exact.
TEST_CASE("delay null test: Tu path is sample-exact through the int16 ring") {
  EngineConfig cfg    = SmallConfig();
  cfg.ditherRingWrite = false;  // contract #2's dither-off mode (design §12.3)

  SECTION("d = 480 via primed params") {
    const uint32_t delayFrames = 480;  // DelayMs = 10 @ 48 kHz
    Rng rng;
    std::vector<float> input(3000);
    input[0] = 1.0f;
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

  SECTION("d = 480 via SetParam alone — the path users take (smoother must arrive)") {
    const uint32_t delayFrames = 480;
    Rng rng;
    std::vector<float> input(48000);
    for (auto& x : input) x = 0.5f * rng.Next();

    // primeParams=false: Mix ramps 0.5 -> 1.0 through the smoother, which must
    // snap to exactly 1.0 (the bare one-pole stalls 1.4e-5 short; review finding).
    const auto out = Render(cfg, input,
                            {{ParamId::DelayMs, 10.0f},
                             {ParamId::Mix, 1.0f},
                             {ParamId::Feedback, 0.0f},
                             {ParamId::OutTrimDb, 0.0f}},
                            /*primeParams=*/false, {64});

    for (size_t n = 24000; n < out.l.size(); ++n) {
      const float expected =
          static_cast<float>(Quantize(input[n - delayFrames])) * (1.0f / 32767.0f);
      REQUIRE(out.l[n] == expected);
    }
  }

  SECTION("d = 1 boundary: read-before-write ordering at the minimum tap") {
    EngineConfig lowRate    = cfg;
    lowRate.sampleRate      = 1000.0;  // DelayMs = 1 -> exactly 1 frame
    std::vector<float> input(64, 0.f);
    input[0] = 1.0f;
    const auto out = Render(lowRate, input,
                            {{ParamId::DelayMs, 1.0f},
                             {ParamId::Mix, 1.0f},
                             {ParamId::Feedback, 0.0f},
                             {ParamId::OutTrimDb, 0.0f}},
                            true, {16});
    REQUIRE(out.l[0] == 0.0f);
    REQUIRE(out.l[1] == static_cast<float>(Quantize(1.0f)) * (1.0f / 32767.0f));
    REQUIRE(out.l[2] == 0.0f);
  }
}

// Design contract #1: rendering the same input under different block splittings
// produces identical samples — smoother trajectories (per-sample, never per-block),
// parameter drain timing, and the counter-keyed dither must all be split-invariant.
// Precondition (honest scope): no parameter changes during the render; the
// mid-render case is the hidden [.pending-spsc] test below.
TEST_CASE("block-splitting bit-exactness (no mid-render automation)") {
  EngineConfig cfg = SmallConfig();  // dither ON — its split-invariance is under test

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
  const auto mixed = Render(cfg, input, params, false, {48u, 1u, 127u, 32u});
  REQUIRE(std::memcmp(ref.l.data(), mixed.l.data(), ref.l.size() * sizeof(float)) == 0);
}

// Hidden until the SPSC event queue lands (design §9): a parameter change delivered
// mid-render at a sampleOffset must be split-invariant. Run explicitly with
// `brainscape_tests "[pending-spsc]"`. Enabling this in the default suite is the
// acceptance criterion for the queue.
TEST_CASE("mid-render automation is split-invariant", "[.][pending-spsc]") {
  EngineConfig cfg = SmallConfig();
  Rng rng;
  std::vector<float> input(4096);
  for (auto& x : input) x = 0.8f * rng.Next();
  const std::vector<std::pair<ParamId, float>> params = {
      {ParamId::DelayMs, 10.0f}, {ParamId::Mix, 0.3f}};
  // 700 is not a multiple of either block size, so without sample-offset support
  // the change lands at boundary 1024 (512-split) vs 704 (64-split) — divergence.
  MidRenderChange change;
  change.atFrame = 700;
  change.id      = ParamId::Mix;
  change.value   = 0.9f;

  const auto ref   = Render(cfg, input, params, true, {512}, change);
  const auto other = Render(cfg, input, params, true, {64}, change);
  REQUIRE(std::memcmp(ref.l.data(), other.l.data(), ref.l.size() * sizeof(float)) == 0);
}

// Design contract #4 (skeleton scope): feedback below unity is bounded AND decays
// to true silence. Without dither, int16 rounding has fixed points in the loop and
// a single impulse leaves a permanent tone (measured -70 dBFS at fb 0.95; review
// finding) — the dithered write's random walk must be absorbed at exact zero.
TEST_CASE("feedback is bounded and decays to silence") {
  EngineConfig cfg = SmallConfig();  // dither ON

  std::vector<float> input(240000, 0.0f);  // 5 s
  input[0] = 1.0f;

  const auto out = Render(cfg, input,
                          {{ParamId::DelayMs, 10.0f},
                           {ParamId::Mix, 1.0f},
                           {ParamId::Feedback, 0.6f},
                           {ParamId::OutTrimDb, 0.0f}},
                          true, {512});

  float peak = 0.f;
  for (float v : out.l) peak = std::max(peak, std::fabs(v));
  REQUIRE(peak <= 2.0f);

  // The tail must be EXACTLY zero — not just quiet.
  for (size_t n = out.l.size() - 24000; n < out.l.size(); ++n) {
    REQUIRE(out.l[n] == 0.0f);
    REQUIRE(out.r[n] == 0.0f);
  }
}

TEST_CASE("high feedback remains bounded") {
  EngineConfig cfg = SmallConfig();
  std::vector<float> input(96000, 0.0f);
  input[0] = 1.0f;
  const auto out = Render(cfg, input,
                          {{ParamId::DelayMs, 1.0f},
                           {ParamId::Mix, 1.0f},
                           {ParamId::Feedback, 0.95f},
                           {ParamId::OutTrimDb, 0.0f}},
                          true, {256});
  float peak = 0.f;
  for (float v : out.l) peak = std::max(peak, std::fabs(v));
  REQUIRE(peak <= 2.0f);
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
  ctx.in               = ins;
  ctx.out              = outs;
  ctx.numFrames        = 100;
  ctx.transportPlaying = false;  // counter must advance regardless of transport
  engine.Process(ctx);
  ctx.numFrames = 48;
  engine.Process(ctx);
  REQUIRE(engine.SampleCounter() == 148);
}

TEST_CASE("Process on an un-Init'd engine outputs silence, not stale memory") {
  Engine engine;  // never Init'd
  std::vector<float> in(64, 0.5f), outL(64, 123.f), outR(64, 123.f);
  const float* ins[2]  = {in.data(), in.data()};
  float*       outs[2] = {outL.data(), outR.data()};
  Engine::ProcessContext ctx;
  ctx.in        = ins;
  ctx.out       = outs;
  ctx.numFrames = 64;
  engine.Process(ctx);
  for (float v : outL) REQUIRE(v == 0.0f);
  for (float v : outR) REQUIRE(v == 0.0f);
}

#if defined(BRAINSCAPE_DENORMAL_SSE)
TEST_CASE("denormal guard sets and restores FTZ/DAZ (SSE)") {
  const unsigned before = _mm_getcsr();
  {
    ScopedDenormalGuard guard;
    REQUIRE((_mm_getcsr() & 0x8040u) == 0x8040u);
  }
  REQUIRE(_mm_getcsr() == before);
}
#elif defined(BRAINSCAPE_DENORMAL_AARCH64)
TEST_CASE("denormal guard sets and restores FZ (AArch64 FPCR)") {
  auto readFpcr = [] {
    uint64_t v;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(v));
    return v;
  };
  const uint64_t before = readFpcr();
  {
    ScopedDenormalGuard guard;
    REQUIRE((readFpcr() & (1ull << 24)) != 0);
  }
  REQUIRE(readFpcr() == before);
}
#endif
