#pragma once
// Shared by the wrapper tests and the hosted VST3 check: a deterministic input, a busy
// preset, and the reference render (the engine alone, canonical configuration, 48-frame
// blocks: the pedal's grid). No JUCE.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "brainscape/Engine.h"
#include "brainscape/HostArenas.h"

#if defined(_MSC_VER)
#include <crtdbg.h>
#include <stdlib.h>
#endif

namespace brainscape::testing {

// MSVC's debug CRT answers a failed assert or abort() with a dialog, which hangs an
// unattended run; report on stderr and exit instead.
inline void ReportCrtErrorsOnStderr() {
#if defined(_MSC_VER)
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
  for (int type : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
    _CrtSetReportMode(type, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(type, _CRTDBG_FILE_STDERR);
  }
#endif
}

using Preset = std::vector<std::pair<ParamId, float>>;

struct Stereo {
  std::vector<float> l, r;
};

inline uint32_t Bits(float v) {
  uint32_t u = 0;
  std::memcpy(&u, &v, sizeof u);
  return u;
}

inline uint32_t Xorshift(uint32_t& s) {
  s ^= s << 13;
  s ^= s >> 17;
  s ^= s << 5;
  return s;
}

// Integer-only, then scaled exactly by 2^-23, so the input bits are the same in every
// build: sawtooth plucks with a linear decay every 0.3 s (they fire onsets), plus noise.
inline Stereo MakeInput(int frames) {
  Stereo   s;
  uint32_t rng = 0x1234567u;
  s.l.resize(static_cast<size_t>(frames));
  s.r.resize(static_cast<size_t>(frames));
  for (int n = 0; n < frames; ++n) {
    const int     t      = n % 14400;
    const int     note   = (n / 14400) % 5;
    const int     period = 400 - 60 * note;
    const int64_t saw    = static_cast<int64_t>(t % period) * 65536 / period - 32768;  // ±2^15
    const int64_t env    = std::max<int64_t>(0, (1 << 20) - static_cast<int64_t>(t) * 73);
    const int64_t noise  = static_cast<int64_t>(Xorshift(rng) >> 20) - 2048;
    const int64_t l      = (saw * env >> 14) + noise * 64;  // about ±0.25 FS at the onset
    const int64_t r      = (saw * env >> 15) - noise * 48;
    s.l[static_cast<size_t>(n)] = static_cast<float>(l) * 0x1p-23f;
    s.r[static_cast<size_t>(n)] = static_cast<float>(r) * 0x1p-23f;
  }
  return s;
}

// Exercises every stage but stays outside the block-split bug's reach (Live positioning,
// no freeze; determinism profile §5.7), so any host block size must reproduce it.
inline Preset Busy() {
  return {{ParamId::DelayMs, 180.0f},     {ParamId::Mix, 0.7f},           {ParamId::Feedback, 0.6f},
          {ParamId::GrainSizeMs, 60.0f},  {ParamId::Overlap, 0.7f},       {ParamId::SprayMs, 50.0f},
          {ParamId::PitchSt, 7.0f},       {ParamId::SpreadCents, 12.0f},  {ParamId::ReverseProb, 0.3f},
          {ParamId::Jitter, 0.5f},        {ParamId::PanSpread, 0.8f},     {ParamId::ModRateHz, 1.3f},
          {ParamId::ModDepth, 0.3f},      {ParamId::DelayTimeMs, 230.0f}, {ParamId::DelayFb, 0.5f},
          {ParamId::DelayMix, 0.3f},      {ParamId::ReverbTime, 0.6f},    {ParamId::ReverbMix, 0.25f},
          {ParamId::FilterCutoffHz, 4000.0f}, {ParamId::FilterRes, 0.3f}, {ParamId::FilterMorph, 0.6f},
          {ParamId::TriggerSens, 0.6f},   {ParamId::OnsetTrigger, 1.0f}};
}

// Every leaf, defaults included: what a complete-state load applies.
inline Preset Complete(const Preset& p) {
  Preset all;
  for (const ParamDescriptor& d : kParamTable) all.push_back({d.id, d.def});
  for (const auto& v : p) all[static_cast<size_t>(v.first) - 1u].second = v.second;
  return all;
}

inline uint64_t Hash(const Stereo& s) {  // FNV-1a over the output bits, L then R
  uint64_t h = 0xcbf29ce484222325ull;
  for (const auto* ch : {&s.l, &s.r}) {
    for (float v : *ch) {
      const uint32_t u = Bits(v);
      for (int i = 0; i < 4; ++i) {
        h ^= (u >> (8 * i)) & 0xFFu;
        h *= 0x100000001b3ull;
      }
    }
  }
  return h;
}

inline bool SameBits(const std::vector<float>& a, const std::vector<float>& b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

inline size_t FirstDiff(const std::vector<float>& a, const std::vector<float>& b) {
  for (size_t i = 0; i < std::min(a.size(), b.size()); ++i) {
    if (Bits(a[i]) != Bits(b[i])) return i;
  }
  return a.size() == b.size() ? SIZE_MAX : std::min(a.size(), b.size());
}

struct RefEvent {
  int                          frame;
  std::function<void(Engine&)> apply;
};

// The engine alone in 48-frame blocks, split at each event's frame. Empty on failure.
inline Stereo RenderReference(const Preset& preset, const Stereo& in, double rate = 48000.0,
                              const std::vector<RefEvent>& events = {}) {
  EngineConfig cfg;
  cfg.sampleRate    = rate;
  cfg.maxBlockSize  = 512;
  cfg.historyFrames = 1u << 22;
  host::HeapArenas arenas(PlanMemory(cfg));
  auto             engine = std::make_unique<Engine>();
  if (!arenas.ok() || !engine->Init(cfg, arenas.get())) return {};
  for (const auto& p : preset) engine->SetParam(p.first, p.second);
  engine->Reset();

  const int frames = static_cast<int>(in.l.size());
  Stereo    out;
  out.l.assign(in.l.size(), 0.f);
  out.r.assign(in.l.size(), 0.f);
  size_t ei  = 0;
  int    pos = 0;
  while (pos < frames) {
    for (; ei < events.size() && events[ei].frame <= pos; ++ei) events[ei].apply(*engine);
    int end = std::min(frames, pos + 48);
    if (ei < events.size()) end = std::min(end, events[ei].frame);
    const float*           ins[2]  = {in.l.data() + pos, in.r.data() + pos};
    float*                 outs[2] = {out.l.data() + pos, out.r.data() + pos};
    Engine::ProcessContext ctx;
    ctx.in        = ins;
    ctx.out       = outs;
    ctx.numFrames = static_cast<uint32_t>(end - pos);
    engine->Process(ctx);
    pos = end;
  }
  return out;
}

}  // namespace brainscape::testing
