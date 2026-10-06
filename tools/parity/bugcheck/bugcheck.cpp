// Block-split invariance checker for brainscape::Engine (grain-engine.md §10 contract #1).
//
// Renders the same input + parameter/event schedule with block sizes 1, 7, 48, 64, 127, 512
// and a mixed pattern, and compares the float32 output bit patterns against the block-1
// render. Events (freeze on/off, parameter changes) are applied by SPLITTING the block at
// the event frame, so event timing is identical for every block size; each block is
// clamped to min(block, remaining) and to the next event frame.
//
// Usage:
//   bugcheck matrix            all 2^5 subsets of {spray, reverse, POS_MARK, freeze, pitch+12}
//   bugcheck case <name>       one named case, per-block-size detail
//   bugcheck all               every named case
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "brainscape/Engine.h"
#include "brainscape/HostArenas.h"

#if defined(BUGCHECK_PROBE)
extern unsigned long long g_bcAheadReads;   // reads whose taps touch (live, live+512]
extern unsigned long long g_bcAheadGrains;  // grain spans containing such a read
#endif

using namespace brainscape;

namespace {

struct Rng {
  uint32_t s = 0x1234567u;
  float Next() {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return (static_cast<float>(s & 0xFFFFFFu) / 8388608.0f) - 1.0f;  // [-1, 1)
  }
};

enum class Kind { Param, Freeze };
struct Ev {
  uint32_t frame;
  Kind     kind;
  ParamId  id;
  float    value;
};

struct Case {
  std::string name;
  std::string what;
  uint32_t    historyFrames = 1u << 22;
  uint32_t    frames        = 8u * 48000u;
  uint32_t    pluckCount    = 0xFFFFFFFFu;  // plucks every 0.3 s from 0.25 s; cap the count
  std::vector<std::pair<ParamId, float>> params;  // applied at frame 0, then Reset()
  std::vector<Ev> events;                          // sorted by frame
};

// Plucks every 0.3 s from 0.25 s on (onset-detector food) over a -60 dB noise floor.
// Deterministic within one build; this tool only ever compares renders of one build.
void MakeInput(uint32_t frames, uint32_t pluckCount, std::vector<float>* l,
               std::vector<float>* r) {
  l->assign(frames, 0.f);
  r->assign(frames, 0.f);
  Rng nl, nr;
  nr.s = 0xBEEF123u;
  for (uint32_t i = 0; i < frames; ++i) {
    (*l)[i] = 0.001f * nl.Next();
    (*r)[i] = 0.001f * nr.Next();
  }
  uint32_t k = 0;
  for (uint32_t at = 12000; at < frames && k < pluckCount; at += 14400, ++k) {
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

struct Result {
  std::vector<float> out;  // interleaved L,R
  uint64_t           hash = 0;
#if defined(BUGCHECK_PROBE)
  unsigned long long aheadReads = 0, aheadGrains = 0;
#endif
};

uint64_t Fnv(const std::vector<float>& v) {
  uint64_t h = 1469598103934665603ull;
  for (float f : v) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    for (int b = 0; b < 4; ++b) {
      h ^= (u >> (8 * b)) & 0xFFu;
      h *= 1099511628211ull;
    }
  }
  return h;
}

Result Render(const Case& c, const std::vector<float>& inL, const std::vector<float>& inR,
              const std::vector<uint32_t>& pattern) {
#if defined(BUGCHECK_PROBE)
  g_bcAheadReads  = 0;
  g_bcAheadGrains = 0;
#endif
  EngineConfig cfg;
  cfg.sampleRate    = 48000.0;
  cfg.maxBlockSize  = 512;
  cfg.historyFrames = c.historyFrames;
  host::HeapArenas arenas(PlanMemory(cfg));
  if (!arenas.ok()) {
    std::fprintf(stderr, "arena alloc failed\n");
    std::exit(2);
  }
  auto* e = new Engine();
  if (!e->Init(cfg, arenas.get())) {
    std::fprintf(stderr, "Init failed\n");
    std::exit(3);
  }
  for (const auto& p : c.params) e->SetParam(p.first, p.second);
  size_t ei = 0;
  e->Reset();  // preset load: drain + snap smoothers

  Result res;
  res.out.assign(2u * c.frames, 0.f);
  std::vector<float> oL(512), oR(512);
  uint32_t pos = 0;
  size_t   bi  = 0;
  while (pos < c.frames) {
    while (ei < c.events.size() && c.events[ei].frame <= pos) {
      const Ev& ev = c.events[ei++];
      if (ev.kind == Kind::Param) e->SetParam(ev.id, ev.value);
      else e->SetFreeze(ev.value != 0.f);
    }
    const uint32_t want = pattern[bi++ % pattern.size()];
    uint32_t end = pos + want;
    if (end > c.frames) end = c.frames;  // HARNESS TRAP: clamp to remaining
    if (ei < c.events.size() && c.events[ei].frame < end) end = c.events[ei].frame;
    const uint32_t n = end - pos;
    const float* ins[2]  = {inL.data() + pos, inR.data() + pos};
    float*       outs[2] = {oL.data(), oR.data()};
    Engine::ProcessContext ctx;
    ctx.in        = ins;
    ctx.out       = outs;
    ctx.numFrames = n;
    e->Process(ctx);
    for (uint32_t i = 0; i < n; ++i) {
      res.out[2u * (pos + i)]      = oL[i];
      res.out[2u * (pos + i) + 1u] = oR[i];
    }
    pos = end;
  }
  delete e;
  res.hash = Fnv(res.out);
#if defined(BUGCHECK_PROBE)
  res.aheadReads  = g_bcAheadReads;
  res.aheadGrains = g_bcAheadGrains;
#endif
  return res;
}

struct Cmp {
  bool     identical;
  int64_t  firstFrame;  // -1 if identical
  uint64_t diffSamples;
  double   maxAbs;
};

Cmp Compare(const std::vector<float>& a, const std::vector<float>& b) {
  Cmp c{true, -1, 0, 0.0};
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::memcmp(&a[i], &b[i], 4) != 0) {
      if (c.identical) c.firstFrame = static_cast<int64_t>(i / 2);
      c.identical = false;
      ++c.diffSamples;
      const double d = std::fabs(static_cast<double>(a[i]) - static_cast<double>(b[i]));
      if (d > c.maxAbs) c.maxAbs = d;
    }
  }
  return c;
}

const std::vector<std::vector<uint32_t>> kPatterns = {{1}, {7}, {48}, {64}, {127}, {512},
                                                      {48, 1, 127, 32}};

std::string PatName(const std::vector<uint32_t>& p) {
  std::string s;
  for (size_t i = 0; i < p.size(); ++i) s += (i ? "," : "") + std::to_string(p[i]);
  return s;
}

// Baseline: everything that can move a read position is OFF; jitter stays on (it is
// counter-keyed and must already be split-invariant), mix 1 so the wet path is the output.
std::vector<std::pair<ParamId, float>> Baseline() {
  return {{ParamId::DelayMs, 250.f},     {ParamId::Mix, 1.0f},
          {ParamId::Feedback, 0.f},      {ParamId::GrainSizeMs, 90.f},
          {ParamId::Overlap, 0.55f},     {ParamId::SprayMs, 0.f},
          {ParamId::PitchSt, 0.f},       {ParamId::SpreadCents, 0.f},
          {ParamId::ReverseProb, 0.f},   {ParamId::Jitter, 0.2f},
          {ParamId::PositionSource, 0.f}, {ParamId::OnsetTrigger, 0.f}};
}

void Set(std::vector<std::pair<ParamId, float>>* ps, ParamId id, float v) {
  for (auto& p : *ps) {
    if (p.first == id) {
      p.second = v;
      return;
    }
  }
  ps->push_back({id, v});
}

constexpr uint32_t kFreezeOn  = 2u * 48000u + 1000u;  // 2.02 s: not on any block grid
constexpr uint32_t kFreezeOff = 6u * 48000u + 333u;

Case Combo(unsigned mask) {
  Case c;
  c.params = Baseline();
  std::string n;
  if (mask & 1u) { Set(&c.params, ParamId::SprayMs, 20.f); n += "S"; }
  if (mask & 2u) { Set(&c.params, ParamId::ReverseProb, 0.5f); n += "R"; }
  if (mask & 4u) { Set(&c.params, ParamId::PositionSource, 1.f); n += "M"; }
  if (mask & 8u) {
    c.events.push_back({kFreezeOn, Kind::Freeze, ParamId::Mix, 1.f});
    c.events.push_back({kFreezeOff, Kind::Freeze, ParamId::Mix, 0.f});
    n += "F";
  }
  if (mask & 16u) { Set(&c.params, ParamId::PitchSt, 12.f); n += "P"; }
  c.name = n.empty() ? "-" : n;
  return c;
}

std::vector<Case> NamedCases() {
  std::vector<Case> v;
  auto freezeEv = [](Case* c, uint32_t on, uint32_t off) {
    c->events.push_back({on, Kind::Freeze, ParamId::Mix, 1.f});
    if (off) c->events.push_back({off, Kind::Freeze, ParamId::Mix, 0.f});
  };
  {  // The lens's reported minimal set.
    Case c;
    c.name   = "lens-minimal";
    c.what   = "spray 20 + reverse 0.5 + POS_MARK + freeze (2^22 ring)";
    c.params = Baseline();
    Set(&c.params, ParamId::SprayMs, 20.f);
    Set(&c.params, ParamId::ReverseProb, 0.5f);
    Set(&c.params, ParamId::PositionSource, 1.f);
    freezeEv(&c, kFreezeOn, kFreezeOff);
    v.push_back(c);
  }
  {
    Case c;
    c.name   = "mark-spray";
    c.what   = "POS_MARK + freeze + spray 20 (no reverse, no pitch)";
    c.params = Baseline();
    Set(&c.params, ParamId::SprayMs, 20.f);
    Set(&c.params, ParamId::PositionSource, 1.f);
    freezeEv(&c, kFreezeOn, kFreezeOff);
    v.push_back(c);
  }
  {
    Case c;
    c.name   = "mark-reverse";
    c.what   = "POS_MARK + freeze + reverse 0.5 (no spray, no pitch)";
    c.params = Baseline();
    Set(&c.params, ParamId::ReverseProb, 0.5f);
    Set(&c.params, ParamId::PositionSource, 1.f);
    freezeEv(&c, kFreezeOn, kFreezeOff);
    v.push_back(c);
  }
  {
    Case c;
    c.name   = "mark-pitch";
    c.what   = "POS_MARK + freeze + pitch +12 (no spray, no reverse)";
    c.params = Baseline();
    Set(&c.params, ParamId::PitchSt, 12.f);
    Set(&c.params, ParamId::PositionSource, 1.f);
    freezeEv(&c, kFreezeOn, kFreezeOff);
    v.push_back(c);
  }
  {
    Case c;
    c.name   = "mark-plain";
    c.what   = "POS_MARK + freeze only (unity forward, no spray)";
    c.params = Baseline();
    Set(&c.params, ParamId::PositionSource, 1.f);
    freezeEv(&c, kFreezeOn, kFreezeOff);
    v.push_back(c);
  }
  {  // The existing contract-#1 test's parameter set, plus a freeze — the gap in the suite.
    Case c;
    c.name          = "suite-params+freeze";
    c.what          = "test_engine.cpp:386-398 parameters, 2^15 ring, freeze at 0.02 s";
    c.historyFrames = 1u << 15;
    c.frames        = 4096u * 6u;
    c.params        = {{ParamId::DelayMs, 100.0f},        {ParamId::Mix, 0.7f},
                       {ParamId::Feedback, 0.5f},         {ParamId::OutTrimDb, -3.0f},
                       {ParamId::GrainSizeMs, 60.f},      {ParamId::Overlap, 0.55f},
                       {ParamId::SprayMs, 50.0f},         {ParamId::PitchSt, 7.0f},
                       {ParamId::SpreadCents, 20.f},      {ParamId::ReverseProb, 0.3f},
                       {ParamId::Jitter, 1.0f},           {ParamId::WindowSmooth, 0.7f},
                       {ParamId::ModDepth, 0.3f},         {ParamId::ModRateHz, 2.0f},
                       {ParamId::DelayMix, 0.3f},         {ParamId::DelayTimeMs, 60.0f},
                       {ParamId::ReverbMix, 0.4f},        {ParamId::ReverbTime, 0.7f},
                       {ParamId::FilterCutoffHz, 9000.0f}, {ParamId::FilterRes, 0.3f},
                       {ParamId::FilterMorph, 0.5f},      {ParamId::TriggerSens, 0.8f},
                       {ParamId::OnsetTrigger, 1.0f},     {ParamId::PositionSource, 1.0f}};
    freezeEv(&c, 1000u, 0u);
    v.push_back(c);
  }
  {
    Case c;
    c.name   = "mark-spray200";
    c.what   = "POS_MARK + freeze + spray 200 ms (no reverse, no pitch)";
    c.params = Baseline();
    Set(&c.params, ParamId::SprayMs, 200.f);
    Set(&c.params, ParamId::PositionSource, 1.f);
    freezeEv(&c, kFreezeOn, kFreezeOff);
    v.push_back(c);
  }
  {  // Control: the same sets WITHOUT freeze stay invariant.
    Case c;
    c.name   = "mark-rev-pitch-nofreeze";
    c.what   = "POS_MARK + reverse 0.5 + pitch +12 + spray 200, NO freeze (control)";
    c.params = Baseline();
    Set(&c.params, ParamId::SprayMs, 200.f);
    Set(&c.params, ParamId::ReverseProb, 0.5f);
    Set(&c.params, ParamId::PitchSt, 12.f);
    Set(&c.params, ParamId::PositionSource, 1.f);
    v.push_back(c);
  }
  {  // Far rail vs. the Pass-1 write-ahead, no freeze at all, small ring.
    Case c;
    c.name          = "farmargin";
    c.what          = "POS_LIVE, NO freeze, 2^15 ring, base 678 ms (start ~200 frames past the far rail of the write-ahead window)";
    c.historyFrames = 1u << 15;
    c.frames        = 2u * 48000u;
    c.params        = Baseline();
    Set(&c.params, ParamId::DelayMs, 678.0f);
    Set(&c.params, ParamId::GrainSizeMs, 20.f);
    v.push_back(c);
  }
  {  // Stale mark at the DEFAULT ring: one onset, then 90 s of noise floor, no freeze.
    Case c;
    c.name       = "stale-mark";
    c.what       = "POS_MARK, NO freeze, default 2^22 ring, one pluck then 90 s (mark ages past one ring)";
    c.frames     = 90u * 48000u;
    c.pluckCount = 1;
    c.params     = Baseline();
    Set(&c.params, ParamId::PositionSource, 1.f);
    Set(&c.params, ParamId::ReverseProb, 0.5f);
    v.push_back(c);
  }
  {  // POS_LIVE under freeze on a small ring: far guard is anchor-relative.
    Case c;
    c.name          = "live-farguard";
    c.what          = "POS_LIVE + freeze, 2^15 ring, base 400 ms, reverse 0.5 (no mark)";
    c.historyFrames = 1u << 15;
    c.frames        = 3u * 48000u;
    c.params        = Baseline();
    Set(&c.params, ParamId::DelayMs, 400.f);
    Set(&c.params, ParamId::ReverseProb, 0.5f);
    freezeEv(&c, 20000u, 0u);
    v.push_back(c);
  }
  {  // Re-anchor-on-wrap at the DEFAULT ring: freeze held past 3/4 of 2^22 frames (65.5 s).
    Case c;
    c.name   = "reanchor-default";
    c.what   = "POS_LIVE + freeze held 67 s, default 2^22 ring, no spray/reverse/mark";
    c.frames = 68u * 48000u;
    c.params = Baseline();
    freezeEv(&c, 48000u + 77u, 0u);
    v.push_back(c);
  }
  {  // Re-anchor-on-wrap decided at block start (Engine.cpp:405-414).
    Case c;
    c.name          = "reanchor";
    c.what          = "POS_LIVE + freeze held past 3/4 ring, 2^13 ring, no spray/reverse";
    c.historyFrames = 1u << 13;
    c.frames        = 48000u;
    c.params        = Baseline();
    Set(&c.params, ParamId::DelayMs, 20.f);
    Set(&c.params, ParamId::GrainSizeMs, 20.f);
    freezeEv(&c, 10000u, 0u);
    v.push_back(c);
  }
  return v;
}

bool RunCase(const Case& c, bool detail) {
  std::vector<float> inL, inR;
  MakeInput(c.frames, c.pluckCount, &inL, &inR);
  const Result ref = Render(c, inL, inR, kPatterns[0]);
  bool allOk = true;
  if (detail) {
    std::printf("== %s: %s\n", c.name.c_str(), c.what.c_str());
    std::printf("   ring 2^%d, %u frames\n", static_cast<int>(std::log2(c.historyFrames)),
                c.frames);
  }
  std::string row;
  for (size_t pi = 0; pi < kPatterns.size(); ++pi) {
    const Result r   = pi == 0 ? ref : Render(c, inL, inR, kPatterns[pi]);
    const Cmp    cmp = Compare(ref.out, r.out);
    allOk            = allOk && cmp.identical;
    if (detail) {
#if defined(BUGCHECK_PROBE)
      std::printf("   block %-12s hash %016llx  %s", PatName(kPatterns[pi]).c_str(),
                  static_cast<unsigned long long>(r.hash),
                  cmp.identical ? "IDENTICAL to block 1" : "DIFFERS");
      std::printf("  [ahead reads %llu in %llu spans]", r.aheadReads, r.aheadGrains);
#else
      std::printf("   block %-12s hash %016llx  %s", PatName(kPatterns[pi]).c_str(),
                  static_cast<unsigned long long>(r.hash),
                  cmp.identical ? "IDENTICAL to block 1" : "DIFFERS");
#endif
      if (!cmp.identical) {
        std::printf(" first frame %lld (%.4f s), %llu samples differ, max |d| %.3g",
                    static_cast<long long>(cmp.firstFrame), cmp.firstFrame / 48000.0,
                    static_cast<unsigned long long>(cmp.diffSamples), cmp.maxAbs);
      }
      std::printf("\n");
    } else {
      row += cmp.identical ? "  ok " : " DIFF";
    }
  }
  if (!detail) {
    std::printf("%-6s %s   %s\n", c.name.c_str(), row.c_str(), allOk ? "INVARIANT" : "BROKEN");
  } else {
    std::printf("   => %s\n", allOk ? "INVARIANT" : "BROKEN");
  }
  return allOk;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string mode = argc > 1 ? argv[1] : "all";
  int broken = 0;
  if (mode == "matrix") {
    std::printf("S=spray 20ms R=reverse 0.5 M=POS_MARK F=freeze 2.02-6.0 s P=pitch +12; "
                "2^22 ring, 8 s; columns vs block 1:\n");
    std::printf("%-6s ", "combo");
    for (const auto& p : kPatterns) std::printf(" %4s", PatName(p).substr(0, 4).c_str());
    std::printf("\n");
    for (unsigned m = 0; m < 32u; ++m) broken += RunCase(Combo(m), false) ? 0 : 1;
  } else if (mode == "hashes") {
    // Block-48 (pedal grid) hash per case, to diff two dsp/ trees against each other.
    std::vector<Case> all = NamedCases();
    for (unsigned m = 0; m < 32u; ++m) all.push_back(Combo(m));
    for (const Case& c : all) {
      std::vector<float> inL, inR;
      MakeInput(c.frames, c.pluckCount, &inL, &inR);
      const Result r = Render(c, inL, inR, {48});
      std::printf("%-26s %016llx\n", c.name.c_str(), static_cast<unsigned long long>(r.hash));
    }
  } else if (mode == "case" && argc > 2) {
    for (const Case& c : NamedCases()) {
      if (c.name == argv[2]) broken += RunCase(c, true) ? 0 : 1;
    }
  } else {
    for (const Case& c : NamedCases()) broken += RunCase(c, true) ? 0 : 1;
  }
  std::printf("broken cases: %d\n", broken);
  return 0;
}
