// Built against the engine compiled with the guard's test hooks (determinism profile
// §4.2, §6.4): flushing forced on inside the guard must reproduce the gradual-underflow
// render bit for bit, because the in-code flush (§4.3) keeps recursive state out of the
// subnormal range, and few silent-tail blocks may still raise a subnormal flag.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "FpEnvTestUtil.h"
#include "brainscape/Engine.h"
#include "brainscape/HostArenas.h"
#include "catch.hpp"

using namespace brainscape;

namespace {

struct TailRender {
  std::vector<float> l, r;
  size_t tailBlocks = 0, flaggedTailBlocks = 0, lastFlaggedTailBlock = 0;
};

// Integer-keyed noise on the 24-bit grid, then digital silence.
TailRender RenderTail(const std::vector<std::pair<ParamId, float>>& preset, bool forceFlush,
                      size_t noiseFrames, size_t totalFrames) {
  detail::fpenv_test::forceFlush = forceFlush;
  EngineConfig cfg;
  cfg.historyFrames = 1u << 16;
  host::HeapArenas arenas(PlanMemory(cfg));
  REQUIRE(arenas.ok());
  Engine engine;
  REQUIRE(engine.Init(cfg, arenas.get()));
  for (const auto& p : preset) engine.SetParam(p.first, p.second);
  engine.Reset();

  std::vector<float> in(totalFrames, 0.0f);
  uint32_t s = 0x2545F491u;
  for (size_t i = 0; i < noiseFrames; ++i) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    in[i] = static_cast<float>(static_cast<int32_t>(s >> 9) - (1 << 22)) * 0x1p-23f;  // ±0.5
  }
  TailRender out;
  out.l.assign(totalFrames, 0.f);
  out.r.assign(totalFrames, 0.f);
  constexpr uint32_t kBlock = 48;
  size_t pos = 0;
  while (pos < totalFrames) {
    const auto n = static_cast<uint32_t>(std::min<size_t>(kBlock, totalFrames - pos));
    const float* ins[2]  = {in.data() + pos, in.data() + pos};
    float*       outs[2] = {out.l.data() + pos, out.r.data() + pos};
    Engine::ProcessContext ctx;
    ctx.in        = ins;
    ctx.out       = outs;
    ctx.numFrames = n;
    detail::fpenv_test::flags = 0;
    engine.Process(ctx);
    if (pos >= noiseFrames) {
      ++out.tailBlocks;
      if ((detail::fpenv_test::flags & testing::kSubnormalFlagBits) != 0) {
        ++out.flaggedTailBlocks;
        out.lastFlaggedTailBlock = out.tailBlocks;
      }
    }
    pos += n;
  }
  detail::fpenv_test::forceFlush = false;
  return out;
}

}  // namespace

TEST_CASE("forced flushing reproduces the gradual-underflow render on silent tails") {
  const std::vector<std::pair<const char*, std::vector<std::pair<ParamId, float>>>> presets = {
      {"default", {}},
      {"feedback", {{ParamId::DelayMs, 300.0f}, {ParamId::Feedback, 0.5f}}},
      {"post stages",
       {{ParamId::Feedback, 0.3f},       {ParamId::ModDepth, 0.5f},
        {ParamId::ModRateHz, 1.3f},      {ParamId::DelayMix, 0.5f},
        {ParamId::DelayFb, 0.6f},        {ParamId::DelayTimeMs, 260.0f},
        {ParamId::ReverbMix, 0.5f},      {ParamId::ReverbTime, 0.2f},
        {ParamId::FilterCutoffHz, 2000.0f}, {ParamId::FilterRes, 0.5f},
        {ParamId::FilterMorph, 1.5f}}},
      {"reverb", {{ParamId::ReverbMix, 1.0f}, {ParamId::FilterCutoffHz, 5000.0f}}},
  };
  // 0.5 s of noise, then a 6.5 s tail of 48-frame blocks.
  constexpr size_t kNoise = 24000, kTotal = 336000;
  for (const auto& p : presets) {
    const TailRender ieee   = RenderTail(p.second, false, kNoise, kTotal);
    const TailRender forced = RenderTail(p.second, true, kNoise, kTotal);
    INFO(p.first << ": " << ieee.flaggedTailBlocks << " of " << ieee.tailBlocks
                 << " silent-tail blocks raised a subnormal flag, the last at block "
                 << ieee.lastFlaggedTailBlock);
    REQUIRE(std::memcmp(ieee.l.data(), forced.l.data(), ieee.l.size() * sizeof(float)) == 0);
    REQUIRE(std::memcmp(ieee.r.data(), forced.r.data(), ieee.r.size() * sizeof(float)) == 0);
    // §4.2 allows flags in 0.5 % of the blocks of a 120 s silent tail (600 of 120,000).
    // The flags are a transient while the flushed states decay to exact zero, so they
    // must also have stopped a second before this shorter tail ends.
    REQUIRE(ieee.flaggedTailBlocks <= 600u);
    REQUIRE(ieee.lastFlaggedTailBlock + 1000u <= ieee.tailBlocks);
  }
}
