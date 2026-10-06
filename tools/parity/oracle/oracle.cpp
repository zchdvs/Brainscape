// Parity oracle harness. ONE source, compiled for x86 (MSVC / g++) AND for the
// Cortex-M7 with the firmware flags (run under qemu-arm user mode via libgloss
// linux.specs). Renders a fixed input through fixed presets and prints a
// 64-bit FNV-1a hash of the output bit patterns per preset, plus per-second
// chunk hashes so the first divergent second can be located without the WAV.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "brainscape/Engine.h"
#if defined(_MSC_VER) && defined(_M_X64)
#include <math.h>  // _set_FMA3_enable
#endif

using namespace brainscape;

#if defined(__SSE2__) || defined(_M_X64)
#include <immintrin.h>
// Simulates a host thread that left a non-default rounding mode (MXCSR.RC = toward zero).
static void SetRoundTowardZero() { _mm_setcsr((_mm_getcsr() & ~0x6000u) | 0x6000u); }
#elif defined(__arm__)
static void SetRoundTowardZero() {  // FPSCR.RMode (bits 23:22) = 0b11 (RZ)
  uint32_t v; __asm__ volatile("vmrs %0, fpscr" : "=r"(v)); v |= (3u << 22);
  __asm__ volatile("vmsr fpscr, %0" ::"r"(v));
}
#endif

struct Preset {
  const char* name;
  void (*apply)(Engine&);
};

static void P_default(Engine&) {}
static void P_clean(Engine& e) {
  e.SetParam(ParamId::DelayMs, 300.f);
  e.SetParam(ParamId::Feedback, 0.5f);
  e.SetParam(ParamId::GrainSizeMs, 100.f);
  e.SetParam(ParamId::Overlap, 0.f);
  e.SetParam(ParamId::SprayMs, 0.f);
  e.SetParam(ParamId::Jitter, 0.f);
  e.SetParam(ParamId::WindowSustain, 1.f);
  e.SetParam(ParamId::WindowSmooth, 0.f);
  e.SetParam(ParamId::PanSpread, 0.f);
}
static void P_shimmer(Engine& e) {
  e.SetParam(ParamId::DelayMs, 375.0f);
  e.SetParam(ParamId::Mix, 0.5f);
  e.SetParam(ParamId::Feedback, 0.55f);
  e.SetParam(ParamId::GrainSizeMs, 120.0f);
  e.SetParam(ParamId::Overlap, 0.5f);
  e.SetParam(ParamId::SprayMs, 40.0f);
  e.SetParam(ParamId::PitchSt, 12.0f);
  e.SetParam(ParamId::SpreadCents, 8.0f);
  e.SetParam(ParamId::Jitter, 0.3f);
  e.SetParam(ParamId::WindowSustain, 0.4f);
  e.SetParam(ParamId::WindowSmooth, 0.8f);
  e.SetParam(ParamId::PanSpread, 0.7f);
  e.SetParam(ParamId::ModDepth, 0.2f);
  e.SetParam(ParamId::ModRateHz, 0.5f);
  e.SetParam(ParamId::ReverbMix, 0.4f);
  e.SetParam(ParamId::ReverbTime, 0.75f);
  e.SetParam(ParamId::FilterCutoffHz, 9500.0f);
  e.SetParam(ParamId::FilterRes, 0.15f);
}
static void P_syncpitch(Engine& e) {
  e.SetParam(ParamId::DelayMs, 400.f);
  e.SetParam(ParamId::Feedback, 0.3f);
  e.SetParam(ParamId::GrainSizeMs, 80.f);
  e.SetParam(ParamId::Overlap, 0.5f);
  e.SetParam(ParamId::SprayMs, 0.f);
  e.SetParam(ParamId::Jitter, 0.f);
  e.SetParam(ParamId::PitchSt, 7.f);
  e.SetParam(ParamId::PanSpread, 0.5f);
}
static void P_strum(Engine& e) {
  e.SetParam(ParamId::OnsetTrigger, 1.f);
  e.SetParam(ParamId::PositionSource, 1.f);
  e.SetParam(ParamId::TriggerSens, 0.6f);
  e.SetParam(ParamId::Overlap, 0.3f);
  e.SetParam(ParamId::GrainSizeMs, 150.f);
  e.SetParam(ParamId::Jitter, 0.f);
  e.SetParam(ParamId::SprayMs, 3.f);
}
static void P_selfosc(Engine& e) {
  e.SetParam(ParamId::Feedback, 1.05f);
  e.SetParam(ParamId::PitchSt, -12.f);
  e.SetParam(ParamId::ReverseProb, 0.3f);
  e.SetParam(ParamId::Jitter, 0.5f);
  e.SetParam(ParamId::SprayMs, 60.f);
}
static void P_post(Engine& e) {
  P_clean(e);
  e.SetParam(ParamId::Feedback, 0.f);
  e.SetParam(ParamId::ModDepth, 0.5f);
  e.SetParam(ParamId::ModRateHz, 1.3f);
  e.SetParam(ParamId::DelayMix, 0.5f);
  e.SetParam(ParamId::DelayFb, 0.6f);
  e.SetParam(ParamId::DelayTimeMs, 260.f);
  e.SetParam(ParamId::ReverbMix, 0.5f);
  e.SetParam(ParamId::ReverbTime, 0.8f);
  e.SetParam(ParamId::FilterCutoffHz, 2000.f);
  e.SetParam(ParamId::FilterRes, 0.5f);
  e.SetParam(ParamId::FilterMorph, 1.5f);
}
// Long frozen hold: exercises the freeze re-anchor (3/4-ring) rule, which IS ring-size dependent.
static void P_freeze(Engine& e) {
  P_shimmer(e);
  e.SetParam(ParamId::Feedback, 0.f);
}

static const Preset kPresets[] = {
    {"default", P_default}, {"clean", P_clean},     {"shimmer", P_shimmer}, {"syncpitch", P_syncpitch},
    {"strum", P_strum},     {"selfosc", P_selfosc}, {"post", P_post},       {"freeze", P_freeze},
};

static uint64_t Fnv(uint64_t h, const void* p, size_t n) {
  const auto* b = static_cast<const unsigned char*>(p);
  for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 0x100000001B3ull; }
  return h;
}

static void* AlignedAlloc(size_t bytes, size_t align) {
  if (bytes == 0) return nullptr;
  auto* raw = static_cast<unsigned char*>(std::malloc(bytes + align + sizeof(void*)));
  if (!raw) return nullptr;
  uintptr_t a = (reinterpret_cast<uintptr_t>(raw) + sizeof(void*) + align - 1) & ~(uintptr_t)(align - 1);
  reinterpret_cast<void**>(a)[-1] = raw;
  return reinterpret_cast<void*>(a);
}
static void AlignedFree(void* p) { if (p) std::free(reinterpret_cast<void**>(p)[-1]); }

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "oracle <in_mono.f32> <tag> [--hist log2] [--block N] [--only name] [--out dir] [--chunks] [--freeze-at s] [--loops K]\n");
    return 1;
  }
  const char* inPath = argv[1];
  const char* tag    = argv[2];
  uint32_t histLog2 = 22, block = 48, loops = 1;
  const char* only = nullptr;
  const char* outDir = nullptr;
  bool chunks = false;
  double freezeAt = 1.0;
  for (int i = 3; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--hist") && i + 1 < argc) histLog2 = (uint32_t)std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--block") && i + 1 < argc) block = (uint32_t)std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--only") && i + 1 < argc) only = argv[++i];
    else if (!std::strcmp(argv[i], "--out") && i + 1 < argc) outDir = argv[++i];
    else if (!std::strcmp(argv[i], "--chunks")) chunks = true;
    else if (!std::strcmp(argv[i], "--rz")) SetRoundTowardZero();
    else if (!std::strcmp(argv[i], "--freeze-at") && i + 1 < argc) freezeAt = std::atof(argv[++i]);
    else if (!std::strcmp(argv[i], "--loops") && i + 1 < argc) loops = (uint32_t)std::atoi(argv[++i]);
#if defined(_MSC_VER) && defined(_M_X64)
    else if (!std::strcmp(argv[i], "--nofma3libm")) _set_FMA3_enable(0);  // force UCRT software paths
#endif
  }

  FILE* fp = std::fopen(inPath, "rb");
  if (!fp) { std::fprintf(stderr, "cannot open %s\n", inPath); return 2; }
  std::fseek(fp, 0, SEEK_END);
  const long bytes = std::ftell(fp);
  std::fseek(fp, 0, SEEK_SET);
  const size_t inFrames = (size_t)bytes / sizeof(float);
  float* in = static_cast<float*>(std::malloc(inFrames * sizeof(float)));
  if (!in || std::fread(in, sizeof(float), inFrames, fp) != inFrames) return 3;
  std::fclose(fp);
  const size_t frames = inFrames * loops;  // --loops repeats the input (longer renders)
  const uint64_t inHash = Fnv(0xCBF29CE484222325ull, in, inFrames * sizeof(float));
  std::printf("[%s] input fnv=%016llx frames=%u hist=2^%u block=%u\n", tag,
              (unsigned long long)inHash, (unsigned)frames, (unsigned)histLog2, (unsigned)block);

  float* oL = static_cast<float*>(std::malloc(block * sizeof(float)));
  float* oR = static_cast<float*>(std::malloc(block * sizeof(float)));
  for (const Preset& p : kPresets) {
    if (only && std::strcmp(only, p.name) != 0) continue;
    EngineConfig cfg;
    cfg.sampleRate    = 48000.0;
    cfg.maxBlockSize  = 512;
    cfg.historyFrames = 1u << histLog2;
    const MemoryPlan plan = PlanMemory(cfg);
    Arenas ar{};
    for (size_t t = 0; t < kNumTiers; ++t) {
      ar.bytes[t] = plan.bytes[t];
      ar.base[t]  = AlignedAlloc(plan.bytes[t], plan.align[t] < 64 ? 64 : plan.align[t]);
    }
    Engine* ep = new Engine();  // fresh object per preset: no cross-preset state
    Engine& e = *ep;
    if (!e.Init(cfg, ar)) { std::fprintf(stderr, "init failed\n"); return 5; }
    p.apply(e);
    e.Reset();
    const bool isFreeze = !std::strcmp(p.name, "freeze");
    const size_t freezeFrame = (size_t)(freezeAt * 48000.0);

    FILE* fo = nullptr;
    if (outDir) {
      char path[512];
      std::snprintf(path, sizeof path, "%s/%s_%s.f32", outDir, tag, p.name);
      fo = std::fopen(path, "wb");
    }
    uint64_t h = 0xCBF29CE484222325ull, hc = h;
    size_t pos = 0, chunkStart = 0;
    uint32_t onsets = 0;
    while (pos < frames) {
      size_t n = frames - pos < block ? frames - pos : block;  // clamp (harness trap)
      if (isFreeze && pos < freezeFrame && pos + n > freezeFrame) n = freezeFrame - pos;
      if (isFreeze && pos == freezeFrame) e.SetFreeze(true);
      // Input: loop the file; split at the loop seam so the pointer stays in range.
      const size_t ip = pos % inFrames;
      if (ip + n > inFrames) n = inFrames - ip;
      const float* ins[2] = {in + ip, in + ip};
      float* outs[2]      = {oL, oR};
      Engine::ProcessContext ctx;
      ctx.in = ins; ctx.out = outs; ctx.numFrames = (uint32_t)n;
      e.Process(ctx);
      onsets += e.ConsumeOnsetCount();
      for (size_t i = 0; i < n; ++i) {
        float fr[2] = {oL[i], oR[i]};
        h  = Fnv(h, fr, sizeof fr);
        hc = Fnv(hc, fr, sizeof fr);
        if (fo) std::fwrite(fr, sizeof(float), 2, fo);
        if (chunks && ((pos + i + 1) % 48000 == 0 || pos + i + 1 == frames)) {
          std::printf("  [%s] %-9s sec%-4u %016llx\n", tag, p.name, (unsigned)(chunkStart / 48000),
                      (unsigned long long)hc);
          hc = 0xCBF29CE484222325ull;
          chunkStart = pos + i + 1;
        }
      }
      pos += n;
    }
    if (fo) std::fclose(fo);
    std::printf("[%s] %-9s onsets=%4u fnv=%016llx\n", tag, p.name, onsets, (unsigned long long)h);
    delete ep;
    for (size_t t = 0; t < kNumTiers; ++t) AlignedFree(ar.base[t]);
  }
  return 0;
}
