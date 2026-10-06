// review-numerics probe: does determinism-profile.md §5.7 "Fix 1" invariant
// ("no grain born at absolute sample t ever reads a ring frame written at or after t")
// hold for ordinary, block-split-invariant presets? Counts, per render:
//   doc  = taps reading a frame written at or after the grain's birth sample
//   ahead= taps inside the Pass-1 write-ahead window (1..512 frames ahead of live head)
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>

#include "brainscape/Engine.h"
#include "brainscape/HostArenas.h"

extern unsigned long long g_rnAheadReads, g_rnDocViol, g_rnDocViolGrains, g_rnTaps;

using namespace brainscape;

struct Rng {
  uint32_t s = 0x12345678u;
  float Next() {
    s = s * 1664525u + 1013904223u;
    return (static_cast<float>(s & 0xFFFFFFu) / 8388608.0f) - 1.0f;
  }
};

static void MakeInput(uint32_t frames, std::vector<float>* l, std::vector<float>* r) {
  l->assign(frames, 0.f);
  r->assign(frames, 0.f);
  Rng nl, nr;
  nr.s = 0xBEEF123u;
  for (uint32_t i = 0; i < frames; ++i) {
    (*l)[i] = 0.001f * nl.Next();
    (*r)[i] = 0.001f * nr.Next();
  }
  uint32_t k = 0;
  for (uint32_t at = 12000; at < frames; at += 14400, ++k) {
    Rng p;
    p.s = 0x9000u + k;
    for (uint32_t i = 0; i < 2400 && at + i < frames; ++i) {
      const float env = static_cast<float>(std::exp(-static_cast<double>(i) / 480.0));
      const float v   = 0.6f * p.Next() * env;
      (*l)[at + i] += v;
      (*r)[at + i] += 0.9f * v;
    }
  }
}

struct Out {
  std::vector<float> o;
  unsigned long long ahead, doc, docGrains, taps;
  uint32_t onsets;
};

static Out Render(const std::vector<std::pair<ParamId, float>>& ps, const std::vector<float>& inL,
                  const std::vector<float>& inR, const std::vector<uint32_t>& pat) {
  g_rnAheadReads = g_rnDocViol = g_rnDocViolGrains = g_rnTaps = 0;
  EngineConfig cfg;
  cfg.maxBlockSize = 512;
  host::HeapArenas ar(PlanMemory(cfg));
  auto* e = new Engine();
  if (!e->Init(cfg, ar.get())) { std::printf("init fail\n"); std::exit(1); }
  for (auto& p : ps) e->SetParam(p.first, p.second);
  e->Reset();
  const uint32_t frames = static_cast<uint32_t>(inL.size());
  Out res;
  res.o.assign(2u * frames, 0.f);
  res.onsets = 0;
  std::vector<float> oL(512), oR(512);
  uint32_t pos = 0;
  size_t bi = 0;
  while (pos < frames) {
    uint32_t n = pat[bi++ % pat.size()];
    if (pos + n > frames) n = frames - pos;
    const float* ins[2] = {inL.data() + pos, inR.data() + pos};
    float* outs[2] = {oL.data(), oR.data()};
    Engine::ProcessContext ctx;
    ctx.in = ins;
    ctx.out = outs;
    ctx.numFrames = n;
    e->Process(ctx);
    res.onsets += e->ConsumeOnsetCount();
    for (uint32_t i = 0; i < n; ++i) {
      res.o[2u * (pos + i)] = oL[i];
      res.o[2u * (pos + i) + 1u] = oR[i];
    }
    pos += n;
  }
  delete e;
  res.ahead = g_rnAheadReads;
  res.doc = g_rnDocViol;
  res.docGrains = g_rnDocViolGrains;
  res.taps = g_rnTaps;
  return res;
}

int main() {
  const uint32_t frames = 8u * 48000u;
  std::vector<float> inL, inR;
  MakeInput(frames, &inL, &inR);
  struct C {
    const char* name;
    std::vector<std::pair<ParamId, float>> ps;
  };
  std::vector<C> cs = {
      {"default (mix 1)", {{ParamId::Mix, 1.f}}},
      {"strum: POS_MARK + onset trigger, defaults",
       {{ParamId::Mix, 1.f}, {ParamId::PositionSource, 1.f}, {ParamId::OnsetTrigger, 1.f}}},
      {"strum + pitch +7",
       {{ParamId::Mix, 1.f}, {ParamId::PositionSource, 1.f}, {ParamId::OnsetTrigger, 1.f},
        {ParamId::PitchSt, 7.f}}},
      {"unity, delay 50 ms < grain 90 ms, no spray",
       {{ParamId::Mix, 1.f}, {ParamId::DelayMs, 50.f}, {ParamId::SprayMs, 0.f}}},
      {"pitch +12, delay 100 ms, grain 90 ms, no spray",
       {{ParamId::Mix, 1.f}, {ParamId::DelayMs, 100.f}, {ParamId::SprayMs, 0.f},
        {ParamId::PitchSt, 12.f}}},
  };
  const std::vector<std::vector<uint32_t>> pats = {{1}, {48}, {512}, {48, 1, 127, 32}};
  for (auto& c : cs) {
    std::printf("== %s\n", c.name);
    Out ref;
    for (size_t pi = 0; pi < pats.size(); ++pi) {
      Out r = Render(c.ps, inL, inR, pats[pi]);
      bool same = true;
      if (pi == 0) ref = r;
      else same = std::memcmp(r.o.data(), ref.o.data(), r.o.size() * 4) == 0;
      std::printf("   block %-12s identical-to-block1=%s onsets=%u taps=%llu aheadWindowReads=%llu "
                  "docInvariantViolations=%llu (grains %llu)\n",
                  pi == 0 ? "1" : (pi == 1 ? "48" : (pi == 2 ? "512" : "48,1,127,32")),
                  pi == 0 ? "(ref)" : (same ? "YES" : "NO"), r.onsets, r.taps, r.ahead, r.doc,
                  r.docGrains);
    }
  }
  return 0;
}
