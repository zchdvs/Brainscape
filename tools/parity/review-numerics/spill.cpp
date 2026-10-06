// review-numerics probe: does a Spillover load AS SPECIFIED (complete preset applied,
// history ring / voices / scheduler phase / feedback FIFO kept, RNG epoch restarted at the
// load frame) reconverge to the canonical Exact render for feedback-free presets?
// Contrast with what the preset-lens probe measured (Engine::Reset at the load, which
// kills voices, re-arms the scheduler, zeroes the FIFO and resets the detector).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "brainscape/Engine.h"
#include "brainscape/HostArenas.h"

extern int64_t g_rnEpoch;
using namespace brainscape;

struct Rng {
  uint32_t s;
  float Next() {
    s = s * 1664525u + 1013904223u;
    return (static_cast<float>(s & 0xFFFFFFu) / 8388608.0f) - 1.0f;
  }
};

static std::vector<float> Input(uint32_t frames, uint32_t seed) {
  std::vector<float> v(2u * frames, 0.f);
  Rng n{seed};
  for (uint32_t i = 0; i < frames; ++i) {
    v[2u * i] = 0.001f * n.Next();
    v[2u * i + 1] = 0.001f * n.Next();
  }
  uint32_t k = 0;
  for (uint32_t at = 12000; at < frames; at += 14400, ++k) {
    Rng p{seed ^ (0x9000u + k)};
    for (uint32_t i = 0; i < 2400 && at + i < frames; ++i) {
      const float env = static_cast<float>(std::exp(-static_cast<double>(i) / 480.0));
      const float x = 0.6f * p.Next() * env;
      v[2u * (at + i)] += x;
      v[2u * (at + i) + 1] += 0.9f * x;
    }
  }
  return v;
}

using PS = std::vector<std::pair<ParamId, float>>;

static void Apply(Engine& e, const PS& ps) {
  size_t n = 0;
  const ParamDescriptor* t = Descriptors(&n);
  for (size_t i = 0; i < n; ++i) e.SetParam(t[i].id, t[i].def);  // complete state: defaults first
  for (auto& p : ps) e.SetParam(p.first, p.second);
}

static void Run(Engine& e, const std::vector<float>& in, std::vector<float>* out) {
  const uint32_t frames = static_cast<uint32_t>(in.size() / 2);
  if (out) out->assign(in.size(), 0.f);
  std::vector<float> iL(48), iR(48), oL(48), oR(48);
  for (uint32_t pos = 0; pos < frames; pos += 48) {
    const uint32_t n = frames - pos < 48 ? frames - pos : 48;
    for (uint32_t i = 0; i < n; ++i) { iL[i] = in[2 * (pos + i)]; iR[i] = in[2 * (pos + i) + 1]; }
    const float* ins[2] = {iL.data(), iR.data()};
    float* outs[2] = {oL.data(), oR.data()};
    Engine::ProcessContext c;
    c.in = ins;
    c.out = outs;
    c.numFrames = n;
    e.Process(c);
    if (out)
      for (uint32_t i = 0; i < n; ++i) { (*out)[2 * (pos + i)] = oL[i]; (*out)[2 * (pos + i) + 1] = oR[i]; }
  }
}

int main() {
  EngineConfig cfg;
  cfg.maxBlockSize = 512;
  const uint32_t priorFrames = 10u * 48000u;  // multiple of 512 and of 48
  const auto Z = Input(30u * 48000u, 0x1234567u);
  const auto X = Input(priorFrames, 0xABCDEFu);
  const PS prior = {{ParamId::DelayMs, 400.f}, {ParamId::GrainSizeMs, 60.f}, {ParamId::Jitter, 0.6f},
                    {ParamId::Overlap, 0.8f}, {ParamId::Mix, 1.f}};
  struct P { const char* name; PS ps; } presets[] = {
      {"default (jitter 0.2, spray 20, fb 0), mix 1", {{ParamId::Mix, 1.f}}},
      {"periodic: jitter 0, spray 0, fb 0, mix 1",
       {{ParamId::Mix, 1.f}, {ParamId::Jitter, 0.f}, {ParamId::SprayMs, 0.f}}},
  };
  for (auto& pr : presets) {
    std::vector<float> C, S1, S2;
    {  // canonical: fresh Init, complete preset, Reset (snap), counter 0, epoch 0
      host::HeapArenas ar(PlanMemory(cfg));
      Engine e;
      e.Init(cfg, ar.get());
      g_rnEpoch = 0;
      Apply(e, pr.ps);
      e.Reset();
      Run(e, Z, &C);
    }
    for (int mode = 0; mode < 2; ++mode) {
      host::HeapArenas ar(PlanMemory(cfg));
      Engine e;
      e.Init(cfg, ar.get());
      g_rnEpoch = 0;
      Apply(e, prior);
      e.Reset();
      Run(e, X, nullptr);
      g_rnEpoch = e.SampleCounter();  // epoch restart at the load frame
      Apply(e, pr.ps);
      if (mode == 1) e.Reset();  // preset-lens probe semantics: voices killed, scheduler re-armed, FIFO zeroed
      Run(e, Z, mode == 0 ? &S1 : &S2);
    }
    auto Report = [&](const char* what, const std::vector<float>& S) {
      int64_t last = -1;
      uint64_t diff = 0;
      for (size_t i = 0; i < S.size(); ++i)
        if (std::memcmp(&S[i], &C[i], 4) != 0) { last = static_cast<int64_t>(i / 2); ++diff; }
      if (last < 0) std::printf("   %-58s identical from the load\n", what);
      else
        std::printf("   %-58s last differing frame %lld (%.3f s after load of %.0f s), %llu samples differ%s\n",
                    what, (long long)last, last / 48000.0, Z.size() / 2 / 48000.0,
                    (unsigned long long)diff, last + 1 >= (int64_t)(Z.size() / 2) ? "  -> NEVER reconverged" : "");
    };
    std::printf("== %s\n", pr.name);
    Report("Spillover as specified (epoch only, trails kept)", S1);
    Report("preset-lens probe (Reset + epoch at load)", S2);
  }
  return 0;
}
