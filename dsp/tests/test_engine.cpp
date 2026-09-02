#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

#include "brainscape/DenormalGuard.h"
#include "brainscape/Engine.h"
#include "brainscape/GrainMath.h"
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

// The degenerate clean-delay grain config the design predicts (§5 Pattern A):
// rectangular window, abutting unity grains, one voice, no randomness.
// overlap 0.25 -> target = 64 * 0.25^3 = 1.0 exactly.
std::vector<std::pair<ParamId, float>> DegenerateDelay(float delayMs, float extraFb = 0.f,
                                                       float mix = 1.f) {
  return {{ParamId::DelayMs, delayMs},   {ParamId::Mix, mix},
          {ParamId::Feedback, extraFb},  {ParamId::OutTrimDb, 0.0f},
          {ParamId::GrainSizeMs, 10.0f}, {ParamId::Overlap, 0.25f},
          {ParamId::SprayMs, 0.0f},      {ParamId::PitchSt, 0.0f},
          {ParamId::SpreadCents, 0.0f},  {ParamId::ReverseProb, 0.0f},
          {ParamId::Jitter, 0.0f},       {ParamId::WindowSustain, 1.0f},
          {ParamId::WindowSkew, 0.5f},   {ParamId::WindowSmooth, 0.0f},
          {ParamId::PanSpread, 0.0f}};
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
    const uint32_t want = blockSizes[blockIdx % blockSizes.size()];
    const auto     n    = static_cast<uint32_t>(std::min<size_t>(want, input.size() - pos));
    // Fire the change before the block CONTAINING atFrame, with the in-block
    // offset (the old >=-then-subtract form underflowed the offset and fired one
    // block late; review finding).
    if (!changed && change.atFrame >= pos && change.atFrame < pos + n) {
      engine.SetParam(change.id, change.value,
                      static_cast<uint32_t>(change.atFrame - pos));
      changed = true;
    }
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

// ── GrainMath unit tests ────────────────────────────────────────────────────────

TEST_CASE("write-head guard clamps per direction") {
  using grainmath::ClampDelayFrames;
  const uint32_t buf = 32768;
  const double   m   = 64.0;

  // Forward, unity rate: near guard is just the margin.
  REQUIRE(ClampDelayFrames(480.0, 480.0, 1.0, false, buf, m) == 480.0);
  REQUIRE(ClampDelayFrames(10.0, 480.0, 1.0, false, buf, m) == m);
  // Forward, pitched up: near guard grows by L*(r-1).
  REQUIRE(ClampDelayFrames(100.0, 480.0, 4.0, false, buf, m) == 480.0 * 3.0 + m);
  // Forward, pitched down: far guard shrinks by L*(1-r).
  REQUIRE(ClampDelayFrames(40000.0, 480.0, 0.5, false, buf, m) == buf - 240.0 - m);
  // Reverse: trivial near guard, far guard covers L*(1+r) of recession.
  REQUIRE(ClampDelayFrames(10.0, 480.0, 1.0, true, buf, m) == m);
  REQUIRE(ClampDelayFrames(40000.0, 480.0, 1.0, true, buf, m) == buf - 960.0 - m);
}

TEST_CASE("envelope geometry and exact mean compensation") {
  using grainmath::EnvValue;
  using grainmath::MakeEnv;

  // Rectangular: sustain=1 -> unity everywhere, gain exactly 1.
  const auto rect = MakeEnv(480.f, 1.0f, 0.5f);
  REQUIRE(rect.gain == 1.0f);
  REQUIRE(EnvValue(rect, 0.f) == 1.0f);
  REQUIRE(EnvValue(rect, 479.f) == 1.0f);

  // Symmetric triangle: sustain=0, skew=0.5.
  const auto tri = MakeEnv(400.f, 0.0f, 0.5f);
  REQUIRE(tri.gain == 2.0f);
  REQUIRE(EnvValue(tri, 0.f) == 0.0f);
  REQUIRE(EnvValue(tri, 100.f) == Approx(0.5f));
  REQUIRE(EnvValue(tri, 200.f) == Approx(1.0f));
  REQUIRE(EnvValue(tri, 400.f) == Approx(0.0f).margin(1e-6));

  // Skewed: attack takes skew of the non-flat portion.
  const auto skewed = MakeEnv(400.f, 0.5f, 0.25f);
  REQUIRE(skewed.attackEnd == Approx(50.f));
  REQUIRE(skewed.decayStart == Approx(250.f));
  REQUIRE(EnvValue(skewed, 150.f) == 1.0f);
}

TEST_CASE("counter RNG is deterministic and in range") {
  using grainmath::Draw;
  using grainmath::RandUnit;
  for (int64_t s : {int64_t{0}, int64_t{12345}, int64_t{1} << 33}) {
    for (auto d : {Draw::Interval, Draw::Spray, Draw::Pan}) {
      const float a = RandUnit(s, d);
      REQUIRE(a == RandUnit(s, d));
      REQUIRE(a >= 0.0f);
      REQUIRE(a < 1.0f);
    }
  }
  REQUIRE(RandUnit(7, grainmath::Draw::Spray) != RandUnit(7, grainmath::Draw::Pan));
  // The full 64-bit counter is folded in: the truncating key repeated the whole
  // draw stream every 2^29 samples (~3 h at 48 kHz; review finding).
  REQUIRE(RandUnit(0, grainmath::Draw::Spray) !=
          RandUnit(int64_t{1} << 29, grainmath::Draw::Spray));
  REQUIRE(grainmath::SemitonesToRatio(12.0f) == 2.0f);
  REQUIRE(grainmath::SemitonesToRatio(0.0f) == 1.0f);
}

TEST_CASE("spray reflection keeps a distribution instead of a rail pileup") {
  using grainmath::ComputeDelayBounds;
  using grainmath::ReflectIntoBounds;
  const auto b = ComputeDelayBounds(960.0, 1.0, false, 32768u, 64.0);
  REQUIRE(ReflectIntoBounds(500.0, b) == 500.0);          // in range: untouched
  REQUIRE(ReflectIntoBounds(40.0, b) == 64.0 + 24.0);     // below lo: mirrored
  REQUIRE(ReflectIntoBounds(-1e6, b) == b.lo);            // beyond one fold: clamped
}

// ── Engine tests ────────────────────────────────────────────────────────────────

TEST_CASE("PlanMemory sizes all three tiers") {
  EngineConfig cfg;  // defaults: 2^22 frames, maxBlockSize 512
  const MemoryPlan plan = PlanMemory(cfg);
  REQUIRE(plan.bytes[static_cast<size_t>(Tier::Bulk)] ==
          (size_t{1} << 22) * 2u * sizeof(int16_t) +
              detail::PostChain::BulkFloats(cfg.sampleRate) * sizeof(float));
  REQUIRE(plan.bytes[static_cast<size_t>(Tier::Hot)] ==
          (detail::kWindowLutSize + 2u * 512u) * sizeof(float));
  REQUIRE(plan.bytes[static_cast<size_t>(Tier::Warm)] ==
          (512u * 2u + detail::FeedbackTamer::WarmFloats(cfg.sampleRate) +
           detail::PostChain::WarmFloats(cfg.sampleRate) +
           detail::OnsetDetector::WarmFloats()) *
              sizeof(float));
  REQUIRE(plan.align[static_cast<size_t>(Tier::Bulk)] == 32);
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
    bad.historyFrames = 4;
    REQUIRE_FALSE(engine.Init(bad, arenas.get()));
    bad.historyFrames = 1u << 27;
    REQUIRE_FALSE(engine.Init(bad, arenas.get()));
  }
  SECTION("rejects an undersized bulk arena") {
    Arenas small = arenas.get();
    small.bytes[static_cast<size_t>(Tier::Bulk)] /= 2;
    REQUIRE_FALSE(engine.Init(cfg, small));
  }
  SECTION("rejects a null bulk arena") {
    Arenas nulls                                = arenas.get();
    nulls.base[static_cast<size_t>(Tier::Bulk)] = nullptr;
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
  engine.SetParam(ParamId::PitchSt, -99.0f);
  REQUIRE(engine.GetParam(ParamId::PitchSt) == -24.0f);
}

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
  engine.SetParam(ParamId::Mix, std::numeric_limits<float>::quiet_NaN());
  REQUIRE(engine.GetParam(ParamId::Mix) == 0.0f);

  std::vector<float> input(2048, 0.25f);
  const auto out = Render(cfg, input, {{ParamId::Mix, std::numeric_limits<float>::quiet_NaN()}},
                          false, {64});
  for (float v : out.l) REQUIRE(std::isfinite(v));
}

// Design contract #2: the degenerate-delay grain config (rectangular window,
// abutting unity grains, one voice) nulls bit-exactly against the int16 ring.
// This is the whole one-engine bet in one test: Pattern A is a configuration.
TEST_CASE("degenerate-delay null: abutting unity grains are a bit-exact delay") {
  EngineConfig cfg    = SmallConfig();
  cfg.ditherRingWrite = false;  // contract #2's dither-off mode (design §12.3)
  const uint32_t d    = 480;    // DelayMs = 10 @ 48 kHz

  SECTION("impulse + noise via primed params") {
    Rng rng;
    std::vector<float> input(4000);
    input[0] = 1.0f;
    for (size_t i = 1; i < input.size(); ++i) input[i] = 0.5f * rng.Next();

    const auto out = Render(cfg, input, DegenerateDelay(10.0f), true, {64});
    for (size_t n = 0; n < out.l.size(); ++n) {
      const float expected =
          n >= d ? static_cast<float>(Quantize(input[n - d])) * (1.0f / 32767.0f) : 0.0f;
      REQUIRE(out.l[n] == expected);
      REQUIRE(out.r[n] == expected);
    }
  }

  SECTION("via SetParam alone — the path users take (smoothers must arrive)") {
    Rng rng;
    std::vector<float> input(48000);
    for (auto& x : input) x = 0.5f * rng.Next();
    const auto out = Render(cfg, input, DegenerateDelay(10.0f), false, {64});
    // The normalization smoother is τ = 100 ms (design §3); its stall-snap to the
    // exact target arrives after ~0.8 s from the default-config starting point.
    for (size_t n = 43000; n < out.l.size(); ++n) {
      const float expected =
          static_cast<float>(Quantize(input[n - d])) * (1.0f / 32767.0f);
      REQUIRE(out.l[n] == expected);
    }
  }
}

// The guards clamp a sub-margin delay up to the margin — the impulse emerges at
// exactly kGuardMarginFrames, not at the requested (unsafe) 48 frames.
TEST_CASE("write-head guard clamps sub-margin delays") {
  EngineConfig cfg    = SmallConfig();
  cfg.ditherRingWrite = false;
  const auto margin   = static_cast<size_t>(detail::kGuardMarginFrames);

  std::vector<float> input(2000, 0.f);
  input[0] = 1.0f;
  const auto out = Render(cfg, input, DegenerateDelay(1.0f), true, {64});  // 48 < margin

  for (size_t n = 0; n < margin; ++n) REQUIRE(out.l[n] == 0.0f);
  REQUIRE(out.l[margin] == static_cast<float>(Quantize(1.0f)) * (1.0f / 32767.0f));
}

// Coherent normalization (design §3): N identical unity grains sum to ~N*x and the
// coherence-resolved 1/N brings it back to x. Not bit-exact: sequentially summing
// 8 identical floats rounds at the odd multiples (3x, 5x, ...), so the null holds
// to a few ULP — the assertion below is the honest version of "level-exact".
TEST_CASE("coherent overlap is level-exact: 8 stacked unity grains null") {
  EngineConfig cfg    = SmallConfig();
  cfg.ditherRingWrite = false;

  auto params = DegenerateDelay(10.0f);
  for (auto& p : params) {
    if (p.first == ParamId::Overlap) p.second = 0.5f;  // 64 * 0.5^3 = 8 voices
  }

  Rng rng;
  std::vector<float> input(6000);
  for (auto& x : input) x = 0.5f * rng.Next();
  const auto out = Render(cfg, input, params, true, {64});

  const uint32_t d = 480;
  for (size_t n = 2000; n < out.l.size(); ++n) {
    const float expected = static_cast<float>(Quantize(input[n - d])) * (1.0f / 32767.0f);
    REQUIRE(out.l[n] == Approx(expected).margin(2e-6));  // few-ULP summation rounding
  }
}

// Design contract #1: identical output under any block splitting, with the full
// stochastic feature set active (jitter, spray, detune, reverse, dither) — every
// draw is keyed on the absolute sample counter, never on block structure.
TEST_CASE("block-splitting bit-exactness (no mid-render automation)") {
  EngineConfig cfg = SmallConfig();  // dither ON — its split-invariance is under test

  Rng rng;
  std::vector<float> input(4096);
  for (auto& x : input) x = 0.8f * rng.Next();

  const std::vector<std::pair<ParamId, float>> params = {
      {ParamId::DelayMs, 100.0f},      {ParamId::Mix, 0.7f},
      {ParamId::Feedback, 0.5f},       {ParamId::OutTrimDb, -3.0f},
      {ParamId::GrainSizeMs, 60.f},    {ParamId::Overlap, 0.55f},
      {ParamId::SprayMs, 50.0f},       {ParamId::PitchSt, 7.0f},
      {ParamId::SpreadCents, 20.f},    {ParamId::ReverseProb, 0.3f},
      {ParamId::Jitter, 1.0f},         {ParamId::WindowSmooth, 0.7f},
      {ParamId::ModDepth, 0.3f},       {ParamId::ModRateHz, 2.0f},
      {ParamId::DelayMix, 0.3f},       {ParamId::DelayTimeMs, 60.0f},
      {ParamId::ReverbMix, 0.4f},      {ParamId::ReverbTime, 0.7f},
      {ParamId::FilterCutoffHz, 9000.0f}, {ParamId::FilterRes, 0.3f},
      {ParamId::FilterMorph, 0.5f},       {ParamId::TriggerSens, 0.8f},
      {ParamId::OnsetTrigger, 1.0f},      {ParamId::PositionSource, 1.0f}};

  const auto ref = Render(cfg, input, params, false, {512});
  for (uint32_t split : {1u, 7u, 32u, 48u, 64u, 127u}) {
    const auto other = Render(cfg, input, params, false, {split});
    REQUIRE(std::memcmp(ref.l.data(), other.l.data(), ref.l.size() * sizeof(float)) == 0);
    REQUIRE(std::memcmp(ref.r.data(), other.r.data(), ref.r.size() * sizeof(float)) == 0);
  }
  const auto mixed = Render(cfg, input, params, false, {48u, 1u, 127u, 32u});
  REQUIRE(std::memcmp(ref.l.data(), mixed.l.data(), ref.l.size() * sizeof(float)) == 0);
}

// The stale-slot defect was invisible below ~4764 frames and needed a saturated
// pool to bite hardest (review finding) — so this variant renders 48000 frames at
// overlap 1.0 with short grains, where slot churn is maximal.
TEST_CASE("block-splitting bit-exactness under a saturated pool") {
  EngineConfig cfg = SmallConfig();
  Rng rng;
  std::vector<float> input(48000);
  for (auto& x : input) x = 0.8f * rng.Next();

  const std::vector<std::pair<ParamId, float>> params = {
      {ParamId::DelayMs, 100.0f},   {ParamId::Mix, 1.0f},
      {ParamId::GrainSizeMs, 20.f}, {ParamId::Overlap, 1.0f},
      {ParamId::SprayMs, 30.0f},    {ParamId::Jitter, 0.5f},
      {ParamId::PitchSt, 5.0f}};

  const auto ref = Render(cfg, input, params, true, {512});
  for (uint32_t split : {48u, 64u, 127u}) {
    const auto other = Render(cfg, input, params, true, {split});
    REQUIRE(std::memcmp(ref.l.data(), other.l.data(), ref.l.size() * sizeof(float)) == 0);
  }
}

// Design contract #3 (level consistency): coherent unity-rate presets stay within
// ±1 dB of the input level across the whole overlap sweep — including fractional
// targets, which are dither-ceiled rather than truncated (a fixed integer ceiling
// against a fractional norm produced a 6 dB grain-rate tremolo; review finding).
TEST_CASE("level consistency across the overlap sweep (contract #3)") {
  EngineConfig cfg    = SmallConfig();
  cfg.ditherRingWrite = false;

  std::vector<float> input(48000);
  for (size_t i = 0; i < input.size(); ++i) {
    input[i] = 0.5f * std::sin(2.0 * 3.14159265358979 * 440.0 * (double(i) / 48000.0));
  }
  double inRms = 0;
  for (size_t i = 24000; i < 48000; ++i) inRms += double(input[i]) * input[i];
  inRms = std::sqrt(inRms / 24000.0);

  for (float overlap : {0.25f, 0.30f, 0.35f, 0.50f, 0.62f, 0.75f, 0.87f, 1.0f}) {
    auto params = DegenerateDelay(100.0f);
    for (auto& p : params) {
      if (p.first == ParamId::Overlap) p.second = overlap;
      if (p.first == ParamId::GrainSizeMs) p.second = 20.0f;
    }
    const auto out = Render(cfg, input, params, true, {256});
    double wetRms = 0;
    for (size_t i = 24000; i < 48000; ++i) wetRms += double(out.l[i]) * out.l[i];
    wetRms = std::sqrt(wetRms / 24000.0);
    const double db = 20.0 * std::log10(wetRms / inRms);
    INFO("overlap " << overlap << " -> " << db << " dB");
    REQUIRE(std::fabs(db) < 1.0);
  }
}

// The size knob is continuous: non-integral grain lengths must not open duty-cycle
// holes (spacing from the unrounded float lost up to 48.8% of the signal in
// whole-grain chunks across 80% of the knob range; review finding).
TEST_CASE("no duty-cycle holes at non-integral grain sizes") {
  EngineConfig cfg    = SmallConfig();
  cfg.ditherRingWrite = false;

  std::vector<float> input(30000, 0.5f);  // DC probe
  for (float sizeMs : {9.99f, 7.7f, 71.2f}) {
    auto params = DegenerateDelay(100.0f);
    for (auto& p : params) {
      if (p.first == ParamId::GrainSizeMs) p.second = sizeMs;
    }
    const auto out = Render(cfg, input, params, true, {128});
    for (size_t n = 10000; n < out.l.size(); ++n) {
      INFO("sizeMs " << sizeMs << " at n=" << n);
      REQUIRE(out.l[n] > 0.4f);
    }
  }
}

// Freeze flips the coherence assumption (pinned grains are time-shifted copies,
// not identical reads) — the normalization exponent must follow, or the wet path
// drops up to 16.7 dB at the footswitch (review finding).
TEST_CASE("freeze holds the wet level within 3 dB on a coherent preset") {
  EngineConfig cfg    = SmallConfig();
  cfg.historyFrames   = 1u << 17;
  cfg.ditherRingWrite = false;

  const size_t total = 96000, freezeAt = 24064;
  Rng rng;
  std::vector<float> input(total);
  for (auto& x : input) x = 0.5f * rng.Next();  // noise throughout — level comparison

  host::HeapArenas arenas(PlanMemory(cfg));
  REQUIRE(arenas.ok());
  Engine engine;
  REQUIRE(engine.Init(cfg, arenas.get()));
  auto params = DegenerateDelay(100.0f);
  for (auto& p : params) {
    if (p.first == ParamId::Overlap) p.second = 0.5f;  // 8 coherent voices
    if (p.first == ParamId::GrainSizeMs) p.second = 20.0f;
  }
  for (auto& p : params) engine.SetParam(p.first, p.second);
  engine.Reset();

  std::vector<float> outL(total), outR(total);
  size_t pos = 0;
  while (pos < total) {
    if (pos == freezeAt) engine.SetFreeze(true);
    const float* ins[2]  = {input.data() + pos, input.data() + pos};
    float*       outs[2] = {outL.data() + pos, outR.data() + pos};
    Engine::ProcessContext ctx;
    ctx.in        = ins;
    ctx.out       = outs;
    ctx.numFrames = 256;
    engine.Process(ctx);
    pos += 256;
  }

  auto rms = [&](size_t from, size_t len) {
    double acc = 0;
    for (size_t i = from; i < from + len; ++i) acc += double(outL[i]) * outL[i];
    return std::sqrt(acc / double(len));
  };
  // Before-freeze steady state vs. well after the τ=100 ms norm transition.
  const double before = rms(12000, 12000);
  const double after  = rms(60000, 24000);
  const double db     = 20.0 * std::log10(after / before);
  INFO("freeze level change: " << db << " dB");
  REQUIRE(std::fabs(db) < 3.0);
}

#if defined(NDEBUG)
// Release-only (the debug assert fires first, by design): an oversized block must
// zero-fill and return — it was a silent heap overrun past the wet buffers
// (review finding).
TEST_CASE("oversized numFrames yields silence, not a buffer overrun") {
  EngineConfig cfg  = SmallConfig();
  cfg.maxBlockSize  = 64;
  host::HeapArenas arenas(PlanMemory(cfg));
  Engine engine;
  REQUIRE(engine.Init(cfg, arenas.get()));

  std::vector<float> in(128, 0.5f), outL(128, 123.f), outR(128, 123.f);
  const float* ins[2]  = {in.data(), in.data()};
  float*       outs[2] = {outL.data(), outR.data()};
  Engine::ProcessContext ctx;
  ctx.in        = ins;
  ctx.out       = outs;
  ctx.numFrames = 128;  // > maxBlockSize
  engine.Process(ctx);
  for (float v : outL) REQUIRE(v == 0.0f);
  for (float v : outR) REQUIRE(v == 0.0f);
}
#endif

TEST_CASE("stochastic renders are reproducible run to run") {
  EngineConfig cfg = SmallConfig();
  Rng rng;
  std::vector<float> input(8192);
  for (auto& x : input) x = 0.6f * rng.Next();
  const std::vector<std::pair<ParamId, float>> params = {
      {ParamId::Jitter, 1.0f}, {ParamId::SprayMs, 200.0f}, {ParamId::ReverseProb, 0.5f}};
  const auto a = Render(cfg, input, params, true, {128});
  const auto b = Render(cfg, input, params, true, {128});
  REQUIRE(std::memcmp(a.l.data(), b.l.data(), a.l.size() * sizeof(float)) == 0);
}

// Hidden until the SPSC event queue lands (design §9): a parameter change delivered
// mid-render at a sampleOffset must be split-invariant. Run explicitly with
// `brainscape_tests "[pending-spsc]"`.
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
// to true silence — the dithered write's random walk is absorbed at exact zero.
TEST_CASE("feedback is bounded and decays to silence") {
  EngineConfig cfg = SmallConfig();  // dither ON

  std::vector<float> input(240000, 0.0f);  // 5 s
  input[0] = 1.0f;

  const auto out = Render(cfg, input, DegenerateDelay(10.0f, 0.6f), true, {512});

  float peak = 0.f;
  for (float v : out.l) peak = std::max(peak, std::fabs(v));
  REQUIRE(peak <= 2.0f);

  for (size_t n = out.l.size() - 24000; n < out.l.size(); ++n) {
    REQUIRE(out.l[n] == 0.0f);
    REQUIRE(out.r[n] == 0.0f);
  }
}

TEST_CASE("high feedback remains bounded") {
  EngineConfig cfg = SmallConfig();
  std::vector<float> input(96000, 0.0f);
  input[0] = 1.0f;
  const auto out = Render(cfg, input, DegenerateDelay(10.0f, 0.95f), true, {256});
  float peak = 0.f;
  for (float v : out.l) peak = std::max(peak, std::fabs(v));
  REQUIRE(peak <= 2.0f);
}

TEST_CASE("reverse grains render bounded, finite, non-silent audio") {
  EngineConfig cfg = SmallConfig();
  Rng rng;
  std::vector<float> input(24000);
  for (auto& x : input) x = 0.5f * rng.Next();

  auto params = DegenerateDelay(50.0f);
  for (auto& p : params) {
    if (p.first == ParamId::ReverseProb) p.second = 1.0f;
  }
  const auto out = Render(cfg, input, params, true, {128});

  double energy = 0;
  float  peak   = 0.f;
  for (float v : out.l) {
    REQUIRE(std::isfinite(v));
    energy += double(v) * v;
    peak = std::max(peak, std::fabs(v));
  }
  REQUIRE(energy > 1.0);
  REQUIRE(peak <= 2.0f);
}

// Freeze (design §2.4): the pinned anchor keeps grains sourcing the frozen window
// while live input has gone silent; unfrozen, the wet path decays to nothing.
// The ring must outlast the render: the live write head keeps advancing during
// freeze and overwrites the pinned window after one ring length (the design's
// documented wraparound ceiling — the 32768-frame test ring demonstrated it).
TEST_CASE("freeze sustains the wet path from the pinned window") {
  EngineConfig cfg    = SmallConfig();
  cfg.historyFrames   = 1u << 17;  // 2.73 s — longer than the 2 s render
  cfg.ditherRingWrite = false;

  const size_t total = 96000, freezeAt = 24000;
  Rng rng;
  std::vector<float> input(total, 0.0f);
  for (size_t i = 0; i < freezeAt; ++i) input[i] = 0.5f * rng.Next();

  auto runOnce = [&](bool freeze) {
    host::HeapArenas arenas(PlanMemory(cfg));
    REQUIRE(arenas.ok());
    Engine engine;
    REQUIRE(engine.Init(cfg, arenas.get()));
    for (auto& p : std::vector<std::pair<ParamId, float>>{
             {ParamId::DelayMs, 100.0f}, {ParamId::Mix, 1.0f},
             {ParamId::Feedback, 0.0f},  {ParamId::SprayMs, 0.0f},
             {ParamId::Jitter, 0.0f}})
      engine.SetParam(p.first, p.second);
    engine.Reset();

    std::vector<float> outL(total), outR(total);
    size_t pos = 0;
    bool   engaged = false;
    while (pos < total) {
      if (freeze && !engaged && pos >= freezeAt) {
        engine.SetFreeze(true);
        engaged = true;
      }
      const uint32_t n     = 256;
      const float* ins[2]  = {input.data() + pos, input.data() + pos};
      float*       outs[2] = {outL.data() + pos, outR.data() + pos};
      Engine::ProcessContext ctx;
      ctx.in        = ins;
      ctx.out       = outs;
      ctx.numFrames = n;
      engine.Process(ctx);
      pos += n;
    }
    double tail = 0;
    for (size_t i = total - 24000; i < total; ++i) tail += double(outL[i]) * outL[i];
    return std::sqrt(tail / 24000.0);
  };

  const double frozenTail   = runOnce(true);
  const double unfrozenTail = runOnce(false);
  REQUIRE(frozenTail > 0.01);
  REQUIRE(unfrozenTail < frozenTail * 0.1);
}

// ── Post chain (design §2.6) ───────────────────────────────────────────────────

TEST_CASE("reverb tail length follows reverb time") {
  EngineConfig cfg = SmallConfig();

  std::vector<float> input(96000, 0.0f);  // 2 s
  Rng rng;
  for (size_t i = 0; i < 24000; ++i) input[i] = 0.5f * rng.Next();  // 0.5 s burst

  auto tailRms = [&](float time) {
    auto params = DegenerateDelay(100.0f);
    params.push_back({ParamId::ReverbMix, 1.0f});
    params.push_back({ParamId::ReverbTime, time});
    const auto out = Render(cfg, input, params, true, {256});
    double acc = 0;
    for (size_t i = 60000; i < 96000; ++i) acc += double(out.l[i]) * out.l[i];
    return std::sqrt(acc / 36000.0);
  };

  const double shortTail = tailRms(0.1f);
  const double longTail  = tailRms(0.9f);
  REQUIRE(longTail > 1e-4);
  REQUIRE(longTail > shortTail * 3.0);
}

TEST_CASE("post delay echoes at its own time, after the grain delay") {
  EngineConfig cfg    = SmallConfig();
  cfg.ditherRingWrite = false;

  std::vector<float> input(24000, 0.0f);
  input[0] = 1.0f;
  auto params = DegenerateDelay(100.0f);  // grain delay: 4800 frames
  params.push_back({ParamId::DelayMix, 1.0f});
  params.push_back({ParamId::DelayTimeMs, 100.0f});  // post delay: another 4800
  params.push_back({ParamId::DelayFb, 0.0f});
  const auto out = Render(cfg, input, params, true, {256});

  // Insert semantics at mix 1, with Reset() priming the post smoothers from the
  // actual params (an unprimed mix leaked 37% of the un-delayed signal; review
  // finding): the whole pre-echo window is EXACT silence.
  for (size_t n = 0; n < 9600; n += 7) REQUIRE(out.l[n] == 0.0f);
  REQUIRE(std::fabs(out.l[9600]) > 0.5f);
}

TEST_CASE("reverb produces early energy, not a delayed slap") {
  EngineConfig cfg = SmallConfig();
  std::vector<float> input(9600, 0.0f);
  input[0] = 1.0f;
  auto params = DegenerateDelay(1.0f);  // grain delay clamps to the 64-frame margin
  params.push_back({ParamId::ReverbMix, 1.0f});
  params.push_back({ParamId::ReverbTime, 0.5f});
  const auto out = Render(cfg, input, params, true, {256});

  // The impulse reaches the tank at ~64 frames; the multi-tap wet must put
  // audible energy inside the first 30 ms (raw line-end outputs were silent for
  // 107 ms; review finding).
  double early = 0;
  for (size_t n = 64; n < 64 + 1440; ++n) early += double(out.l[n]) * out.l[n];
  REQUIRE(std::sqrt(early / 1440.0) > 1e-3);
}

TEST_CASE("filter is bounded at the resonance stop") {
  EngineConfig cfg = SmallConfig();
  std::vector<float> input(480000);  // 10 s
  for (size_t i = 0; i < input.size(); ++i) {
    input[i] = 0.25f * std::sin(2.0 * 3.14159265358979 * 1000.0 * (double(i) / 48000.0));
  }
  auto params = DegenerateDelay(50.0f);
  params.push_back({ParamId::FilterCutoffHz, 1000.0f});
  params.push_back({ParamId::FilterRes, 1.0f});  // the descriptor max
  params.push_back({ParamId::FilterMorph, 0.0f});
  const auto out = Render(cfg, input, params, true, {512});
  float peak = 0.f;
  for (float v : out.l) peak = std::max(peak, std::fabs(v));
  // Extreme resonance is allowed to be loud, but it must be bounded — the
  // un-floored damp ran away to 520+ and climbing (review finding).
  REQUIRE(peak < 120.0f);
  for (float v : out.l) REQUIRE(std::isfinite(v));
}

TEST_CASE("filter re-engage after bypass starts from silence, no burst") {
  EngineConfig cfg    = SmallConfig();
  cfg.ditherRingWrite = false;

  const size_t total = 192000;  // 4 s
  std::vector<float> input(total, 0.0f);
  for (size_t i = 0; i < 96000; ++i) {
    input[i] = 0.1f * static_cast<float>(
                          std::sin(2.0 * 3.14159265358979 * 300.0 * (double(i) / 48000.0)));
  }

  host::HeapArenas arenas(PlanMemory(cfg));
  Engine engine;
  REQUIRE(engine.Init(cfg, arenas.get()));
  auto params = DegenerateDelay(50.0f);
  params.push_back({ParamId::FilterCutoffHz, 300.0f});
  params.push_back({ParamId::FilterRes, 0.95f});
  for (auto& p : params) engine.SetParam(p.first, p.second);
  engine.Reset();

  std::vector<float> outL(total), outR(total);
  size_t pos = 0;
  while (pos < total) {
    if (pos == 96000) engine.SetParam(ParamId::FilterCutoffHz, 20000.0f);  // bypass
    if (pos == 144000) engine.SetParam(ParamId::FilterCutoffHz, 300.0f);   // re-engage
    const float* ins[2]  = {input.data() + pos, input.data() + pos};
    float*       outs[2] = {outL.data() + pos, outR.data() + pos};
    Engine::ProcessContext ctx;
    ctx.in        = ins;
    ctx.out       = outs;
    ctx.numFrames = 256;
    engine.Process(ctx);
    pos += 256;
  }
  // Input has been silent since 2.0 s; re-engaging at 3.0 s must not discharge
  // stored resonant state (measured a 3.9-peak burst pre-fix; review finding).
  float burst = 0.f;
  for (size_t i = 144000; i < total; ++i) burst = std::max(burst, std::fabs(outL[i]));
  REQUIRE(burst < 0.05f);
}

TEST_CASE("filter morph: HP kills DC, LP passes it") {
  EngineConfig cfg    = SmallConfig();
  cfg.ditherRingWrite = false;

  std::vector<float> input(48000, 0.5f);  // DC probe through the wet path

  auto steady = [&](float morph) {
    auto params = DegenerateDelay(50.0f);
    params.push_back({ParamId::FilterCutoffHz, 1000.0f});
    params.push_back({ParamId::FilterRes, 0.1f});
    params.push_back({ParamId::FilterMorph, morph});
    const auto out = Render(cfg, input, params, true, {256});
    double acc = 0;
    for (size_t i = 36000; i < 48000; ++i) acc += double(out.l[i]) * out.l[i];
    return std::sqrt(acc / 12000.0);
  };

  REQUIRE(steady(0.0f) > 0.3);   // LP at 1 kHz passes DC
  REQUIRE(steady(2.0f) < 0.02);  // HP at 1 kHz blocks DC
}

// Feedback past unity is the design's self-oscillation feature (§2.3, §11): the
// taming chain's saturator bounds it into a sustained limit cycle, not a rail.
// The peak band has a LOW side too, so an absent/identity saturator (which rails
// at exactly 1.0 with different dynamics) or a dead loop also fails — the first
// version of this test passed under every taming-chain mutation (review finding).
TEST_CASE("feedback above unity self-oscillates bounded") {
  EngineConfig cfg = SmallConfig();
  std::vector<float> input(192000, 0.0f);  // 4 s
  input[0] = 1.0f;
  const auto out = Render(cfg, input, DegenerateDelay(50.0f, 1.1f), true, {256});

  float peak = 0.f;
  for (float v : out.l) peak = std::max(peak, std::fabs(v));
  REQUIRE(peak <= 1.5f);
  REQUIRE(peak >= 0.1f);

  double tail = 0;
  for (size_t i = 144000; i < 192000; ++i) tail += double(out.l[i]) * out.l[i];
  const double tailRms = std::sqrt(tail / 48000.0);
  REQUIRE(tailRms > 1e-3);   // still singing, not decayed
  REQUIRE(tailRms < 0.75);   // ...and not railed at full scale
}

// The taming chain's HP stages: recirculating DC must not raise the steady-state
// DC level at all — the output mean at high feedback must match the feedback-free
// mean. (The old peak<=2 bound was structurally unreachable and passed with the
// entire taming chain deleted; review finding.)
TEST_CASE("feedback loop rejects DC") {
  EngineConfig cfg = SmallConfig();
  std::vector<float> input(144000, 0.5f);  // 3 s of DC

  auto steadyMean = [&](float fb) {
    const auto out = Render(cfg, input, DegenerateDelay(50.0f, fb), true, {256});
    double acc = 0;
    for (size_t i = 96000; i < 144000; ++i) acc += out.l[i];
    return acc / 48000.0;
  };
  const double atZero = steadyMean(0.0f);
  const double atHigh = steadyMean(0.9f);
  REQUIRE(atZero > 0.3);  // sanity: the DC probe actually flows
  REQUIRE(std::fabs(atHigh - atZero) < 0.05 * atZero);
}

// ── Onset detector and trigger layer (design §4) ───────────────────────────────

namespace {

// Renders through an Engine while exposing it — the shared Render() helper hides
// the instance, and the trigger tests need ConsumeOnsetCount / Trigger.
struct LiveEngine {
  host::HeapArenas arenas;
  Engine           engine;
  explicit LiveEngine(const EngineConfig& cfg,
                      const std::vector<std::pair<ParamId, float>>& params)
      : arenas(PlanMemory(cfg)) {
    REQUIRE(arenas.ok());
    REQUIRE(engine.Init(cfg, arenas.get()));
    for (auto& p : params) engine.SetParam(p.first, p.second);
    engine.Reset();
  }
  void Run(const std::vector<float>& input, std::vector<float>* outL = nullptr) {
    std::vector<float> l(input.size()), r(input.size());
    size_t pos = 0;
    while (pos < input.size()) {
      const auto n = static_cast<uint32_t>(std::min<size_t>(256, input.size() - pos));
      const float* ins[2]  = {input.data() + pos, input.data() + pos};
      float*       outs[2] = {l.data() + pos, r.data() + pos};
      Engine::ProcessContext ctx;
      ctx.in        = ins;
      ctx.out       = outs;
      ctx.numFrames = n;
      engine.Process(ctx);
      pos += n;
    }
    if (outL) *outL = std::move(l);
  }
};

// A percussive pluck: sharp noise attack, exponential decay.
void AddPluck(std::vector<float>* buf, size_t at, float amp, uint32_t seed) {
  Rng rng;
  rng.s = seed;
  for (size_t i = 0; i < 2400 && at + i < buf->size(); ++i) {
    (*buf)[at + i] += amp * rng.Next() * std::exp(-double(i) / 480.0);
  }
}

}  // namespace

TEST_CASE("onset detector counts plucks, ignores silence and steady tones") {
  EngineConfig cfg = SmallConfig();

  SECTION("eight plucks are counted, within min-IOI tolerance") {
    std::vector<float> input(120000, 0.0f);  // 2.5 s
    for (int k = 0; k < 8; ++k) AddPluck(&input, 12000 + k * 12000, 0.6f, 77u + k);
    LiveEngine live(cfg, DegenerateDelay(100.0f));
    live.Run(input);
    const uint32_t count = live.engine.ConsumeOnsetCount();
    INFO("onsets: " << count);
    REQUIRE(count >= 6);
    REQUIRE(count <= 10);
    REQUIRE(live.engine.ConsumeOnsetCount() == 0);  // exchange(0) drains
  }
  SECTION("silence yields zero onsets") {
    std::vector<float> input(48000, 0.0f);
    LiveEngine live(cfg, DegenerateDelay(100.0f));
    live.Run(input);
    REQUIRE(live.engine.ConsumeOnsetCount() == 0);
  }
  SECTION("a steady tone triggers at most its own attack") {
    std::vector<float> input(96000);
    for (size_t i = 0; i < input.size(); ++i) {
      input[i] = 0.4f * static_cast<float>(
                            std::sin(2.0 * 3.14159265358979 * 220.0 * (double(i) / 48000.0)));
    }
    LiveEngine live(cfg, DegenerateDelay(100.0f));
    live.Run(input);
    REQUIRE(live.engine.ConsumeOnsetCount() <= 2);
  }
  SECTION("adaptive whitening hears quiet plucks too") {
    std::vector<float> input(96000, 0.0f);
    for (int k = 0; k < 4; ++k) AddPluck(&input, 12000 + k * 18000, 0.03f, 300u + k);
    LiveEngine live(cfg, DegenerateDelay(100.0f));
    live.Run(input);
    REQUIRE(live.engine.ConsumeOnsetCount() >= 2);
  }
}

// ONSET as an OR'd trigger source with POS_MARK positioning: grains sound the
// marked audio immediately, while the free-running scheduler is parked 2 s in
// the past (still silent ring) — so all early wet energy is onset-driven.
TEST_CASE("onset-triggered grains sound the marked audio") {
  EngineConfig cfg    = SmallConfig();
  cfg.historyFrames   = 1u << 17;  // > 2 s so DelayMs 2000 is a legal position
  cfg.ditherRingWrite = false;

  std::vector<float> input(96000, 0.0f);  // 2 s
  for (int k = 0; k < 4; ++k) AddPluck(&input, 9600 + k * 19200, 0.6f, 500u + k);

  auto params = DegenerateDelay(2000.0f);  // periodic grains read silent history
  for (auto& p : params) {
    if (p.first == ParamId::GrainSizeMs) p.second = 80.0f;
  }

  auto energyUpTo = [&](bool onsetTrigger) {
    auto ps = params;
    if (onsetTrigger) {
      ps.push_back({ParamId::OnsetTrigger, 1.0f});
      ps.push_back({ParamId::PositionSource, 1.0f});
    }
    LiveEngine live(cfg, ps);
    std::vector<float> out;
    live.Run(input, &out);
    double acc = 0;
    for (size_t i = 0; i < 86400; ++i) acc += double(out[i]) * out[i];  // first 1.8 s
    return acc;
  };

  const double withTriggers    = energyUpTo(true);
  const double withoutTriggers = energyUpTo(false);
  REQUIRE(withTriggers > 1.0);
  REQUIRE(withoutTriggers < withTriggers * 0.01);
}

// The external Trigger() fallback (design §4: explicit triggers never drop).
TEST_CASE("manual Trigger fires an extra grain") {
  EngineConfig cfg    = SmallConfig();
  cfg.ditherRingWrite = false;

  std::vector<float> input(48000, 0.5f);  // DC probe
  auto params = DegenerateDelay(100.0f);

  LiveEngine live(cfg, params);
  std::vector<float> out(input.size()), r(input.size());
  size_t pos = 0;
  while (pos < input.size()) {
    if (pos == 24064) live.engine.Trigger();  // between blocks, steady state
    const auto n = static_cast<uint32_t>(std::min<size_t>(256, input.size() - pos));
    const float* ins[2]  = {input.data() + pos, input.data() + pos};
    float*       outs[2] = {out.data() + pos, r.data() + pos};
    Engine::ProcessContext ctx;
    ctx.in        = ins;
    ctx.out       = outs;
    ctx.numFrames = n;
    live.engine.Process(ctx);
    pos += n;
  }
  // Steady degenerate delay sits at ~0.5; the manually fired grain overlaps the
  // periodic voice, briefly doubling the wet sum.
  float before = 0.f, after = 0.f;
  for (size_t i = 20000; i < 24000; ++i) before = std::max(before, std::fabs(out[i]));
  for (size_t i = 24064; i < 29000; ++i) after = std::max(after, std::fabs(out[i]));
  REQUIRE(before < 0.6f);
  REQUIRE(after > 0.8f);
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
