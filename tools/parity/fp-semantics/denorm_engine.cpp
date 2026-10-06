// fp-isa lens: whole-engine subnormal experiment (x86, MSVC).
// For each preset render 2 s of 24-bit-grid noise followed by 38 s of digital silence, once
// with the guard in FTZ|DAZ mode (repo behaviour) and once in IEEE mode (gradual underflow).
// Report: MXCSR DE/UE activity per segment, cost per sample in the silent tail, and whether the
// two renders are bit-identical (if they are, the FTZ setting - and therefore x86-vs-Arm FZ
// semantics - provably cannot matter for that render).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "brainscape/Engine.h"
#include "brainscape/HostArenas.h"

namespace brainscape {
unsigned g_fpMode    = 0;
unsigned g_mxcsrAccum = 0;
}  // namespace brainscape

using namespace brainscape;

static void P_clean(Engine& e) {
  e.SetParam(ParamId::DelayMs, 300.f); e.SetParam(ParamId::Feedback, 0.5f);
  e.SetParam(ParamId::GrainSizeMs, 100.f); e.SetParam(ParamId::Overlap, 0.f);
  e.SetParam(ParamId::SprayMs, 0.f); e.SetParam(ParamId::Jitter, 0.f);
  e.SetParam(ParamId::WindowSustain, 1.f); e.SetParam(ParamId::WindowSmooth, 0.f);
  e.SetParam(ParamId::PanSpread, 0.f);
}
static void P_shimmer(Engine& e) {
  e.SetParam(ParamId::DelayMs, 375.0f); e.SetParam(ParamId::Mix, 0.5f);
  e.SetParam(ParamId::Feedback, 0.55f); e.SetParam(ParamId::GrainSizeMs, 120.0f);
  e.SetParam(ParamId::Overlap, 0.5f); e.SetParam(ParamId::SprayMs, 40.0f);
  e.SetParam(ParamId::PitchSt, 12.0f); e.SetParam(ParamId::SpreadCents, 8.0f);
  e.SetParam(ParamId::Jitter, 0.3f); e.SetParam(ParamId::WindowSustain, 0.4f);
  e.SetParam(ParamId::WindowSmooth, 0.8f); e.SetParam(ParamId::PanSpread, 0.7f);
  e.SetParam(ParamId::ModDepth, 0.2f); e.SetParam(ParamId::ModRateHz, 0.5f);
  e.SetParam(ParamId::ReverbMix, 0.4f); e.SetParam(ParamId::ReverbTime, 0.75f);
  e.SetParam(ParamId::FilterCutoffHz, 9500.0f); e.SetParam(ParamId::FilterRes, 0.15f);
}
static void P_post(Engine& e) {
  P_clean(e);
  e.SetParam(ParamId::Feedback, 0.f); e.SetParam(ParamId::ModDepth, 0.5f);
  e.SetParam(ParamId::ModRateHz, 1.3f); e.SetParam(ParamId::DelayMix, 0.5f);
  e.SetParam(ParamId::DelayFb, 0.6f); e.SetParam(ParamId::DelayTimeMs, 260.f);
  e.SetParam(ParamId::ReverbMix, 0.5f); e.SetParam(ParamId::ReverbTime, 0.8f);
  e.SetParam(ParamId::FilterCutoffHz, 2000.f); e.SetParam(ParamId::FilterRes, 0.5f);
  e.SetParam(ParamId::FilterMorph, 1.5f);
}
static void P_verb(Engine& e) {
  P_clean(e);
  e.SetParam(ParamId::Feedback, 0.f); e.SetParam(ParamId::ReverbMix, 1.0f);
  e.SetParam(ParamId::ReverbTime, 1.0f); e.SetParam(ParamId::FilterCutoffHz, 5000.f);
}
static void P_default(Engine&) {}

struct Preset { const char* name; void (*apply)(Engine&); };
static const Preset kPresets[] = {
    {"default", P_default}, {"clean", P_clean}, {"shimmer", P_shimmer}, {"post", P_post}, {"verb", P_verb}};

struct Run {
  std::vector<float> out;
  uint64_t deBurst = 0, ueBurst = 0, deTail = 0, ueTail = 0, blocks = 0;
  double tailNsPerSample = 0;
  uint64_t subnormalOutputs = 0;
};

static Run Render(const Preset& p, unsigned mode, const std::vector<float>& in, size_t burstFrames) {
  g_fpMode = mode;
  EngineConfig cfg;
  cfg.sampleRate = 48000.0;
  cfg.maxBlockSize = 48;
  host::HeapArenas arenas(PlanMemory(cfg));
  Engine e;
  if (!arenas.ok() || !e.Init(cfg, arenas.get())) { std::fprintf(stderr, "init failed\n"); std::exit(2); }
  p.apply(e);
  e.Reset();
  Run r;
  const size_t frames = in.size();
  r.out.resize(frames * 2);
  std::vector<float> oL(48), oR(48);
  size_t pos = 0;
  const size_t tailStart = frames - 10 * 48000;  // time the last 10 s
  double tailNs = 0;
  while (pos < frames) {
    const auto n = static_cast<uint32_t>(std::min<size_t>(48, frames - pos));
    const float* ins[2] = {in.data() + pos, in.data() + pos};
    float* outs[2] = {oL.data(), oR.data()};
    Engine::ProcessContext ctx;
    ctx.in = ins; ctx.out = outs; ctx.numFrames = n;
    g_mxcsrAccum = 0;
    const auto t0 = std::chrono::steady_clock::now();
    e.Process(ctx);
    const auto t1 = std::chrono::steady_clock::now();
    if (pos >= tailStart) tailNs += std::chrono::duration<double, std::nano>(t1 - t0).count();
    const bool de = (g_mxcsrAccum & 0x02u) != 0, ue = (g_mxcsrAccum & 0x10u) != 0;
    if (pos < burstFrames) { r.deBurst += de; r.ueBurst += ue; } else { r.deTail += de; r.ueTail += ue; }
    ++r.blocks;
    for (uint32_t i = 0; i < n; ++i) {
      r.out[2 * (pos + i)] = oL[i];
      r.out[2 * (pos + i) + 1] = oR[i];
      if (std::fpclassify(oL[i]) == FP_SUBNORMAL || std::fpclassify(oR[i]) == FP_SUBNORMAL) ++r.subnormalOutputs;
    }
    pos += n;
  }
  r.tailNsPerSample = tailNs / (10.0 * 48000.0);
  return r;
}

static uint64_t Fnv(const std::vector<float>& v) {
  uint64_t h = 1469598103934665603ull;
  for (float f : v) { uint32_t u; std::memcpy(&u, &f, 4); for (int k = 0; k < 4; ++k) { h ^= (u >> (8 * k)) & 0xFF; h *= 1099511628211ull; } }
  return h;
}

int main() {
  std::printf("== denorm_engine (BS_P1FLUSH=%d)\n", BS_P1FLUSH);
  const size_t sr = 48000, burst = 2 * sr, total = 40 * sr;
  std::vector<float> in(total, 0.0f);
  uint32_t s = 12345;
  for (size_t i = 0; i < burst; ++i) {
    s = s * 1664525u + 1013904223u;
    const float x = (static_cast<float>(s >> 8) * (1.0f / 16777216.0f) * 2.0f - 1.0f) * 0.5f;
    in[i] = std::nearbyint(x * 8388608.0f) / 8388608.0f;  // 24-bit codec grid
  }
  for (const Preset& p : kPresets) {
    const Run ftz = Render(p, 0, in, burst);
    const Run iee = Render(p, 1, in, burst);
    size_t ndiff = 0, firstDiff = SIZE_MAX, firstBig = SIZE_MAX;
    double maxAbs = 0;
    for (size_t i = 0; i < ftz.out.size(); ++i) {
      uint32_t a, b; std::memcpy(&a, &ftz.out[i], 4); std::memcpy(&b, &iee.out[i], 4);
      if (a != b) {
        ++ndiff;
        if (firstDiff == SIZE_MAX) firstDiff = i / 2;
        const double d = std::fabs(double(ftz.out[i]) - double(iee.out[i]));
        maxAbs = std::max(maxAbs, d);
        if (d > 1e-9 && firstBig == SIZE_MAX) firstBig = i / 2;
      }
    }
    std::printf("[%-8s] FTZ : DE-blocks burst/tail=%llu/%llu  tail %.1f ns/sample  hash=%016llx\n", p.name,
                (unsigned long long)ftz.deBurst, (unsigned long long)ftz.deTail, ftz.tailNsPerSample,
                (unsigned long long)Fnv(ftz.out));
    std::printf("[%-8s] IEEE: DE-blocks burst/tail=%llu/%llu UE-blocks burst/tail=%llu/%llu of %llu; subnormal output "
                "samples=%llu; tail %.1f ns/sample  hash=%016llx\n",
                p.name, (unsigned long long)iee.deBurst, (unsigned long long)iee.deTail,
                (unsigned long long)iee.ueBurst, (unsigned long long)iee.ueTail, (unsigned long long)iee.blocks,
                (unsigned long long)iee.subnormalOutputs, iee.tailNsPerSample, (unsigned long long)Fnv(iee.out));
    std::printf("[%-8s] FTZ vs IEEE: %s; differing samples=%zu first at frame %lld (%.3f s); max |diff|=%.3g; "
                "first |diff|>1e-9 at %lld\n",
                p.name, ndiff ? "DIFFER" : "BIT-IDENTICAL", ndiff, firstDiff == SIZE_MAX ? -1LL : (long long)firstDiff,
                firstDiff == SIZE_MAX ? -1.0 : firstDiff / 48000.0, maxAbs,
                firstBig == SIZE_MAX ? -1LL : (long long)firstBig);
  }
  return 0;
}
