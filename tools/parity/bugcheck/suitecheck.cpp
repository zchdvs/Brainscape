// Does the existing contract-#1 test (test_engine.cpp:379-408) ever record an onset mark?
#include <cstdio>
#include <vector>
#include "brainscape/Engine.h"
#include "brainscape/HostArenas.h"
using namespace brainscape;
struct Rng { uint32_t s = 0x1234567u; float Next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5;
  return (static_cast<float>(s & 0xFFFFFFu) / 8388608.0f) - 1.0f; } };
int main() {
  EngineConfig cfg; cfg.sampleRate = 48000.0; cfg.maxBlockSize = 512; cfg.historyFrames = 1u << 15;
  Rng rng; std::vector<float> input(4096); for (auto& x : input) x = 0.8f * rng.Next();
  const std::pair<ParamId, float> params[] = {
      {ParamId::DelayMs, 100.0f}, {ParamId::Mix, 0.7f}, {ParamId::Feedback, 0.5f}, {ParamId::OutTrimDb, -3.0f},
      {ParamId::GrainSizeMs, 60.f}, {ParamId::Overlap, 0.55f}, {ParamId::SprayMs, 50.0f}, {ParamId::PitchSt, 7.0f},
      {ParamId::SpreadCents, 20.f}, {ParamId::ReverseProb, 0.3f}, {ParamId::Jitter, 1.0f}, {ParamId::WindowSmooth, 0.7f},
      {ParamId::ModDepth, 0.3f}, {ParamId::ModRateHz, 2.0f}, {ParamId::DelayMix, 0.3f}, {ParamId::DelayTimeMs, 60.0f},
      {ParamId::ReverbMix, 0.4f}, {ParamId::ReverbTime, 0.7f}, {ParamId::FilterCutoffHz, 9000.0f}, {ParamId::FilterRes, 0.3f},
      {ParamId::FilterMorph, 0.5f}, {ParamId::TriggerSens, 0.8f}, {ParamId::OnsetTrigger, 1.0f}, {ParamId::PositionSource, 1.0f}};
  host::HeapArenas arenas(PlanMemory(cfg)); Engine e; if (!e.Init(cfg, arenas.get())) return 1;
  for (auto& p : params) e.SetParam(p.first, p.second);
  std::vector<float> l(4096), r(4096); size_t pos = 0; uint32_t onsets = 0;
  while (pos < input.size()) {
    uint32_t n = 512; const float* ins[2] = {input.data() + pos, input.data() + pos};
    float* outs[2] = {l.data() + pos, r.data() + pos};
    Engine::ProcessContext ctx; ctx.in = ins; ctx.out = outs; ctx.numFrames = n; e.Process(ctx);
    onsets += e.ConsumeOnsetCount(); pos += n;
  }
  std::printf("onsets detected in the contract-#1 test render: %u\n", onsets);
  return 0;
}
