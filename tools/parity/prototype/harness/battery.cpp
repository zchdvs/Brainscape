// Determinism-profile battery: renders a fixed preset battery from one stored input
// file and prints a SHA-256 per preset (interleaved little-endian float32 output).
// Nothing on the hashed path does floating-point work outside the engine: preset
// and automation values are integer-derived with at most ONE float operation each,
// so even a contraction-enabled build of this TU cannot change them.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "brainscape/Engine.h"
#include "sha256.h"

#if defined(_MSC_VER) && defined(_M_X64)
#include <math.h>  // _set_FMA3_enable (UCRT libm FMA3 dispatch)
#endif
#if !defined(__arm__)
#include <chrono>
#define HAVE_CHRONO 1
#endif

using namespace brainscape;

namespace {

enum class EvKind { Param, Freeze, Trigger };
struct Ev {
  uint32_t frame;
  EvKind   kind;
  ParamId  id;
  float    value;
};

struct PresetDef {
  const char* name;
  void (*build)(std::vector<Ev>&, uint32_t frames);
};

constexpr uint32_t kSr = 48000;
inline void P(std::vector<Ev>& v, uint32_t f, ParamId id, float x) { v.push_back({f, EvKind::Param, id, x}); }
inline void Fz(std::vector<Ev>& v, uint32_t f, bool on) { v.push_back({f, EvKind::Freeze, ParamId::Mix, on ? 1.f : 0.f}); }
inline void Tr(std::vector<Ev>& v, uint32_t f) { v.push_back({f, EvKind::Trigger, ParamId::Mix, 0.f}); }
inline float I2F(int i) { return static_cast<float>(i); }

// 1. Engine defaults (jitter 0.2, spray 20 ms, pan 0.5) — no settings at all.
void B_default(std::vector<Ev>&, uint32_t) {}

// 2. Heavy: Poisson scheduler + wide spray + reverse + detune + pitch + full pan.
void B_heavy(std::vector<Ev>& v, uint32_t) {
  P(v, 0, ParamId::Jitter, 1.0f);
  P(v, 0, ParamId::SprayMs, 400.0f);
  P(v, 0, ParamId::ReverseProb, 0.5f);
  P(v, 0, ParamId::SpreadCents, 60.0f);
  P(v, 0, ParamId::PitchSt, 7.0f);
  P(v, 0, ParamId::Overlap, 0.85f);
  P(v, 0, ParamId::PanSpread, 1.0f);
  P(v, 0, ParamId::GrainSizeMs, 60.0f);
  P(v, 0, ParamId::WindowSustain, 0.1f);
  P(v, 0, ParamId::WindowSkew, 0.2f);
  P(v, 0, ParamId::WindowSmooth, 1.0f);
}

// 3. Feedback 0.5 on the default (jittered) grain cloud.
void B_fb05(std::vector<Ev>& v, uint32_t) {
  P(v, 0, ParamId::Feedback, 0.5f);
  P(v, 0, ParamId::DelayMs, 300.0f);
}

// 4. Self-oscillation: fb 1.05, down an octave, reverse, jitter, spray.
void B_selfosc(std::vector<Ev>& v, uint32_t) {
  P(v, 0, ParamId::Feedback, 1.05f);
  P(v, 0, ParamId::PitchSt, -12.0f);
  P(v, 0, ParamId::ReverseProb, 0.3f);
  P(v, 0, ParamId::Jitter, 0.5f);
  P(v, 0, ParamId::SprayMs, 60.0f);
}

// 5. Strum: ONSET trigger + POS_MARK, plus footswitch triggers.
void B_strum(std::vector<Ev>& v, uint32_t frames) {
  P(v, 0, ParamId::OnsetTrigger, 1.0f);
  P(v, 0, ParamId::PositionSource, 1.0f);
  P(v, 0, ParamId::TriggerSens, 0.6f);
  P(v, 0, ParamId::Overlap, 0.3f);
  P(v, 0, ParamId::GrainSizeMs, 150.0f);
  P(v, 0, ParamId::Jitter, 0.0f);
  P(v, 0, ParamId::SprayMs, 3.0f);
  P(v, 0, ParamId::Feedback, 0.2f);
  for (uint32_t f = 3u * kSr + 4800u; f < frames; f += 5u * kSr) Tr(v, f);
}

// 6. Freeze engaged at 4 s, released at 14 s (pitch +12, jitter, feedback).
void B_freeze(std::vector<Ev>& v, uint32_t) {
  P(v, 0, ParamId::PitchSt, 12.0f);
  P(v, 0, ParamId::Jitter, 0.4f);
  P(v, 0, ParamId::Feedback, 0.3f);
  P(v, 0, ParamId::SpreadCents, 15.0f);
  P(v, 0, ParamId::SprayMs, 80.0f);
  Fz(v, 4u * kSr, true);
  Fz(v, 14u * kSr, false);
}

// 7. Every post stage engaged on a jittered cloud with feedback.
void B_postall(std::vector<Ev>& v, uint32_t) {
  P(v, 0, ParamId::Feedback, 0.3f);
  P(v, 0, ParamId::ModDepth, 0.5f);
  P(v, 0, ParamId::ModRateHz, 1.3f);
  P(v, 0, ParamId::DelayMix, 0.5f);
  P(v, 0, ParamId::DelayFb, 0.6f);
  P(v, 0, ParamId::DelayTimeMs, 260.0f);
  P(v, 0, ParamId::ReverbMix, 0.5f);
  P(v, 0, ParamId::ReverbTime, 0.8f);
  P(v, 0, ParamId::FilterCutoffHz, 2000.0f);
  P(v, 0, ParamId::FilterRes, 0.5f);
  P(v, 0, ParamId::FilterMorph, 1.5f);
}

// 8. Pitch toggling +12 / -12 every 2 s with detune and feedback.
void B_pitch(std::vector<Ev>& v, uint32_t frames) {
  P(v, 0, ParamId::PitchSt, 12.0f);
  P(v, 0, ParamId::SpreadCents, 20.0f);
  P(v, 0, ParamId::Jitter, 0.3f);
  P(v, 0, ParamId::Feedback, 0.4f);
  int k = 0;
  for (uint32_t f = 2u * kSr; f < frames; f += 2u * kSr, ++k) {
    P(v, f, ParamId::PitchSt, (k & 1) ? 12.0f : -12.0f);
  }
}

// 9. Dense automation every 250 ms over (almost) every parameter — exercises every
// param-change transcendental (exp2 trim, pow normalization, expm1 tamer corner,
// SVF sin/sqrt/cos morph weights, SemitonesToRatio).
void B_automation(std::vector<Ev>& v, uint32_t frames) {
  int k = 0;
  for (uint32_t f = 0; f < frames; f += kSr / 4u, ++k) {
    P(v, f, ParamId::OutTrimDb, I2F((k * 7) % 49 - 24));
    P(v, f, ParamId::Feedback, I2F(k % 12) * 0.1f);
    P(v, f, ParamId::FilterCutoffHz, I2F(100 + 397 * (k % 51)));
    P(v, f, ParamId::FilterRes, I2F(k % 11) * 0.1f);
    P(v, f, ParamId::FilterMorph, I2F(k % 13) * 0.25f);
    P(v, f, ParamId::Overlap, I2F(k % 7) / 7.0f);
    P(v, f, ParamId::GrainSizeMs, I2F(5 + 37 * (k % 13)));
    P(v, f, ParamId::PitchSt, I2F((k * 5) % 49 - 24));
    P(v, f, ParamId::SpreadCents, I2F((k * 3) % 101));
    P(v, f, ParamId::Jitter, I2F(k % 5) * 0.25f);
    P(v, f, ParamId::SprayMs, I2F(25 * (k % 9)));
    P(v, f, ParamId::DelayMs, I2F(20 + 113 * (k % 40)));
    P(v, f, ParamId::ReverseProb, I2F(k % 4) * 0.25f);
    P(v, f, ParamId::WindowSustain, I2F(k % 6) * 0.2f);
    P(v, f, ParamId::WindowSkew, I2F((k * 3) % 5) * 0.25f);
    P(v, f, ParamId::WindowSmooth, I2F(k % 3) * 0.5f);
    P(v, f, ParamId::PanSpread, I2F(k % 5) * 0.25f);
    P(v, f, ParamId::ModRateHz, I2F(1 + k % 8) * 0.5f);
    P(v, f, ParamId::ModDepth, I2F(k % 3) * 0.4f);
    P(v, f, ParamId::DelayTimeMs, I2F(50 + 97 * (k % 20)));
    P(v, f, ParamId::DelayFb, I2F(k % 10) * 0.1f);
    P(v, f, ParamId::DelayMix, I2F(k % 3) * 0.5f);
    P(v, f, ParamId::ReverbTime, I2F(k % 5) * 0.25f);
    P(v, f, ParamId::ReverbMix, I2F((k + 1) % 3) * 0.5f);
    P(v, f, ParamId::TriggerSens, I2F(k % 11) * 0.1f);
    P(v, f, ParamId::Mix, I2F(k % 5) * 0.25f);
    P(v, f, ParamId::OnsetTrigger, I2F(k % 2));
    P(v, f, ParamId::PositionSource, I2F((k / 2) % 2));
    if (k % 17 == 5) Fz(v, f, true);
    if (k % 17 == 9) Fz(v, f, false);
    if (k % 6 == 3) Tr(v, f);
  }
}

// 10. Fixed-schedule control: clean delay through the tamer (no jitter, no spray).
void B_clean(std::vector<Ev>& v, uint32_t) {
  P(v, 0, ParamId::DelayMs, 300.0f);
  P(v, 0, ParamId::Feedback, 0.5f);
  P(v, 0, ParamId::GrainSizeMs, 100.0f);
  P(v, 0, ParamId::Overlap, 0.0f);
  P(v, 0, ParamId::SprayMs, 0.0f);
  P(v, 0, ParamId::Jitter, 0.0f);
  P(v, 0, ParamId::WindowSustain, 1.0f);
  P(v, 0, ParamId::WindowSmooth, 0.0f);
  P(v, 0, ParamId::PanSpread, 0.0f);
}

const PresetDef kPresets[] = {
    {"default", B_default}, {"heavy", B_heavy},   {"fb05", B_fb05},       {"selfosc", B_selfosc},
    {"strum", B_strum},     {"freeze", B_freeze}, {"postall", B_postall}, {"pitch12", B_pitch},
    {"automation", B_automation}, {"clean", B_clean},
};

struct Arena {
  std::vector<uint8_t> mem;
  void* base = nullptr;
};

void Apply(Engine& e, const Ev& ev) {
  switch (ev.kind) {
    case EvKind::Param: e.SetParam(ev.id, ev.value); break;
    case EvKind::Freeze: e.SetFreeze(ev.value != 0.f); break;
    case EvKind::Trigger: e.Trigger(); break;
  }
}

}  // namespace

int main(int argc, char** argv) {
  const char* inPath  = nullptr;
  const char* tag     = "build";
  const char* dumpDir = nullptr;
  const char* only    = nullptr;
  uint32_t block = 48, seconds = 20, repeats = 1;
  const char* keep = nullptr;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--in") && i + 1 < argc) inPath = argv[++i];
    else if (!std::strcmp(argv[i], "--tag") && i + 1 < argc) tag = argv[++i];
    else if (!std::strcmp(argv[i], "--dump") && i + 1 < argc) dumpDir = argv[++i];
    else if (!std::strcmp(argv[i], "--only") && i + 1 < argc) only = argv[++i];
    else if (!std::strcmp(argv[i], "--block") && i + 1 < argc) block = (uint32_t)std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = (uint32_t)std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--repeat") && i + 1 < argc) repeats = (uint32_t)std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--nofma3")) {
#if defined(_MSC_VER) && defined(_M_X64)
      _set_FMA3_enable(0);  // force UCRT's software fmaf path
#endif
    }
    else if (!std::strcmp(argv[i], "--keep") && i + 1 < argc) keep = argv[++i];  // bisect: comma list of
                                                                                   // param ids, 100=freeze, 101=trigger
  }
  if (!inPath || block < 1 || block > 512) {
    std::fprintf(stderr, "battery --in input.f32 [--tag t] [--dump dir] [--block N<=512] [--seconds S] [--only p] [--repeat R]\n");
    return 1;
  }
  FILE* fp = std::fopen(inPath, "rb");
  if (!fp) { std::fprintf(stderr, "cannot open %s\n", inPath); return 2; }
  std::fseek(fp, 0, SEEK_END);
  const long bytes = std::ftell(fp);
  std::fseek(fp, 0, SEEK_SET);
  std::vector<float> in(static_cast<size_t>(bytes) / sizeof(float));
  if (std::fread(in.data(), sizeof(float), in.size(), fp) != in.size()) return 3;
  std::fclose(fp);
  const size_t inFrames = in.size() / 2;
  uint32_t frames = seconds * kSr;
  if (frames > inFrames) frames = static_cast<uint32_t>(inFrames);
  std::vector<float> inL(frames), inR(frames);
  for (uint32_t i = 0; i < frames; ++i) { inL[i] = in[2 * i]; inR[i] = in[2 * i + 1]; }
  in.clear();
  in.shrink_to_fit();

  EngineConfig cfg;
  cfg.sampleRate   = 48000.0;
  cfg.maxBlockSize = 512;
  const MemoryPlan plan = PlanMemory(cfg);
  Arena ar[kNumTiers];
  Arenas arenas{};
  for (size_t t = 0; t < kNumTiers; ++t) {
    arenas.bytes[t] = plan.bytes[t];
    if (plan.bytes[t] == 0) continue;
    ar[t].mem.resize(plan.bytes[t] + 64);
    auto p = reinterpret_cast<uintptr_t>(ar[t].mem.data());
    p = (p + 63u) & ~uintptr_t(63u);
    arenas.base[t] = reinterpret_cast<void*>(p);
  }

  Sha256 all;
  std::vector<float> oL(512), oR(512);
  std::vector<uint8_t> bytesOut(512 * 8);
  std::printf("# tag=%s block=%u seconds=%u\n", tag, block, frames / kSr);
  for (const PresetDef& pd : kPresets) {
    if (only && std::strcmp(only, pd.name) != 0) continue;
    std::vector<Ev> evs;
    pd.build(evs, frames);
    if (keep) {
      std::vector<Ev> kept;
      for (const Ev& ev : evs) {
        const int code = ev.kind == EvKind::Param ? int(ev.id) : (ev.kind == EvKind::Freeze ? 100 : 101);
        std::string list = std::string(",") + keep + ",";
        if (list.find("," + std::to_string(code) + ",") != std::string::npos) kept.push_back(ev);
      }
      evs.swap(kept);
    }
    // stable sort by frame (insertion order preserved within a frame)
    for (size_t i = 1; i < evs.size(); ++i)
      for (size_t j = i; j > 0 && evs[j - 1].frame > evs[j].frame; --j) std::swap(evs[j - 1], evs[j]);

    double bestMs = 1e30;
    std::string hex;
    for (uint32_t rep = 0; rep < repeats; ++rep) {
      Engine* e = new Engine();
      if (!e->Init(cfg, arenas)) { std::fprintf(stderr, "init failed\n"); return 5; }
      size_t ei = 0;
      while (ei < evs.size() && evs[ei].frame == 0) Apply(*e, evs[ei++]);
      e->Reset();  // preset load: snap smoothers to the loaded values

      FILE* dump = nullptr;
      if (dumpDir && rep == 0) {
        const std::string path = std::string(dumpDir) + "/" + tag + "_" + pd.name + ".f32";
        dump = std::fopen(path.c_str(), "wb");
      }
      Sha256 sh;
      uint32_t onsets = 0;
#if HAVE_CHRONO
      const auto t0 = std::chrono::steady_clock::now();
#endif
      uint32_t pos = 0;
      while (pos < frames) {
        while (ei < evs.size() && evs[ei].frame <= pos) Apply(*e, evs[ei++]);
        uint32_t end = pos + block;
        if (end > frames) end = frames;                       // clamp (harness trap)
        if (ei < evs.size() && evs[ei].frame < end) end = evs[ei].frame;
        const uint32_t n = end - pos;
        const float* ins[2] = {inL.data() + pos, inR.data() + pos};
        float* outs[2]      = {oL.data(), oR.data()};
        Engine::ProcessContext ctx;
        ctx.in        = ins;
        ctx.out       = outs;
        ctx.numFrames = n;
        e->Process(ctx);
        onsets += e->ConsumeOnsetCount();
        for (uint32_t i = 0; i < n; ++i) {
          uint32_t u;
          std::memcpy(&u, &oL[i], 4);
          for (int b = 0; b < 4; ++b) bytesOut[8 * i + b] = uint8_t(u >> (8 * b));
          std::memcpy(&u, &oR[i], 4);
          for (int b = 0; b < 4; ++b) bytesOut[8 * i + 4 + b] = uint8_t(u >> (8 * b));
        }
        sh.Update(bytesOut.data(), 8u * n);
        if (dump) std::fwrite(bytesOut.data(), 1, 8u * n, dump);
        pos = end;
      }
#if HAVE_CHRONO
      const double ms =
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      if (ms < bestMs) bestMs = ms;
#endif
      if (dump) std::fclose(dump);
      hex = sh.Hex();
      if (rep == 0) {
        all.Update(hex.data(), hex.size());
#if HAVE_CHRONO
        if (repeats == 1)
          std::printf("%-11s %s onsets=%u ms=%.1f\n", pd.name, hex.c_str(), onsets, ms);
#else
        std::printf("%-11s %s onsets=%u\n", pd.name, hex.c_str(), onsets);
#endif
      }
      delete e;
    }
#if HAVE_CHRONO
    if (repeats > 1)
      std::printf("%-11s %s best_ms=%.2f rtf=%.1f\n", pd.name, hex.c_str(), bestMs,
                  (1000.0 * frames / kSr) / bestMs);
#endif
  }
  std::printf("ALL         %s\n", all.Hex().c_str());
  return 0;
}
