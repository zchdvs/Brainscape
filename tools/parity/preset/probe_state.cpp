// Preset-pipeline lens, probe P1/P2/P3 (scratch copy of dsp/ with probe-only hooks).
//
// P1 load-sequence: does the ORDER of preset-application calls change output?
// P2 restart equivalence: can an already-running engine be returned to the
//    canonical "fresh Init + preset" state without re-Init? (Reset / ClearHistory /
//    counter rebase / tamer-buffer clear, in combinations.)
// P3 spillover convergence: after a live preset change that KEEPS history
//    (pedal "spillover"), do two engines with different pasts reconverge to
//    bit-identical output given identical subsequent input? With/without a
//    counter rebase at load.
// P4 event timing: a parameter change at absolute sample t, delivered by a
//    48-frame (pedal) vs 512-frame (plugin) wrapper, (a) applied at the next
//    block start (today's SetParam semantics) vs (b) wrapper splits the block at t.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "brainscape/Engine.h"
#include "brainscape/HostArenas.h"

using namespace brainscape;
static constexpr double kSr = 48000.0;

// Deterministic synthetic "guitar": decaying plucks at pseudo-random times + -70 dB hiss.
static std::vector<float> MakeInput(uint32_t seed, double seconds) {
  const size_t n = static_cast<size_t>(seconds * kSr);
  std::vector<float> x(n, 0.f);
  uint32_t s = seed * 2654435761u + 12345u;
  auto rnd = [&]() { s = s * 1664525u + 1013904223u; return (s >> 8) * (1.0f / 16777216.0f); };
  size_t t = 0;
  while (t < n) {
    const double f0 = 82.4 * std::pow(2.0, std::floor(rnd() * 24.0) / 12.0);
    const float amp = 0.15f + 0.5f * rnd();
    const size_t len = static_cast<size_t>((0.3 + 1.2 * rnd()) * kSr);
    for (size_t i = 0; i < len && t + i < n; ++i) {
      const double tt = i / kSr;
      const double env = std::exp(-tt * 3.0) * (1.0 - std::exp(-tt * 800.0));
      double v = 0;
      for (int h = 1; h <= 5; ++h) v += std::sin(2 * 3.141592653589793 * f0 * h * tt) / (h * h);
      x[t + i] += static_cast<float>(amp * env * v);
    }
    t += static_cast<size_t>((0.15 + 0.9 * rnd()) * kSr);
  }
  for (auto& v : x) v += (rnd() - 0.5f) * 2.0f * 3.2e-4f;
  return x;
}

using PresetFn = void (*)(Engine&);
static void Q_jitterNoFb(Engine& e) {
  e.SetParam(ParamId::Feedback, 0.f);   e.SetParam(ParamId::Jitter, 0.5f);
  e.SetParam(ParamId::SprayMs, 40.f);   e.SetParam(ParamId::PitchSt, 7.f);
  e.SetParam(ParamId::SpreadCents, 10.f); e.SetParam(ParamId::GrainSizeMs, 120.f);
  e.SetParam(ParamId::DelayMs, 400.f);  e.SetParam(ParamId::PanSpread, 0.7f);
}
static void Q_shimmerFb(Engine& e) {
  e.SetParam(ParamId::DelayMs, 375.0f); e.SetParam(ParamId::Mix, 0.5f);
  e.SetParam(ParamId::Feedback, 0.55f); e.SetParam(ParamId::GrainSizeMs, 120.0f);
  e.SetParam(ParamId::Overlap, 0.5f);   e.SetParam(ParamId::SprayMs, 40.0f);
  e.SetParam(ParamId::PitchSt, 12.0f);  e.SetParam(ParamId::SpreadCents, 8.0f);
  e.SetParam(ParamId::Jitter, 0.3f);    e.SetParam(ParamId::WindowSustain, 0.4f);
  e.SetParam(ParamId::WindowSmooth, 0.8f); e.SetParam(ParamId::PanSpread, 0.7f);
  e.SetParam(ParamId::ModDepth, 0.2f);  e.SetParam(ParamId::ModRateHz, 0.5f);
  e.SetParam(ParamId::ReverbMix, 0.4f); e.SetParam(ParamId::ReverbTime, 0.75f);
  e.SetParam(ParamId::FilterCutoffHz, 9500.0f); e.SetParam(ParamId::FilterRes, 0.15f);
}
static void Q_fixedNoFb(Engine& e) {  // periodic schedule, no randomness reaching output
  e.SetParam(ParamId::DelayMs, 300.f); e.SetParam(ParamId::Feedback, 0.f);
  e.SetParam(ParamId::GrainSizeMs, 100.f); e.SetParam(ParamId::Overlap, 0.5f);
  e.SetParam(ParamId::SprayMs, 0.f);   e.SetParam(ParamId::Jitter, 0.f);
  e.SetParam(ParamId::PitchSt, 0.f);   e.SetParam(ParamId::PanSpread, 0.f);
}
static void Q_fixedFb(Engine& e) {  // periodic schedule + feedback (dither in the loop)
  Q_fixedNoFb(e); e.SetParam(ParamId::Feedback, 0.6f);
}
static void P_prior(Engine& e) {  // the "previous preset" the engine was running before
  e.SetParam(ParamId::DelayMs, 900.f); e.SetParam(ParamId::Feedback, 0.8f);
  e.SetParam(ParamId::PitchSt, -12.f); e.SetParam(ParamId::Jitter, 0.8f);
  e.SetParam(ParamId::SprayMs, 300.f); e.SetParam(ParamId::ReverbMix, 0.6f);
  e.SetParam(ParamId::DelayMix, 0.5f); e.SetParam(ParamId::ModDepth, 0.6f);
}

static void ApplyDefaults(Engine& e) {
  size_t n = 0; const ParamDescriptor* d = Engine::Descriptors(&n);
  for (size_t i = 0; i < n; ++i) if (d[i].name) e.SetParam(d[i].id, d[i].def);
}
struct Rig {
  EngineConfig cfg;
  std::unique_ptr<host::HeapArenas> ar;
  Engine e;
  Rig() {
    cfg.sampleRate = kSr; cfg.maxBlockSize = 512;
    ar = std::make_unique<host::HeapArenas>(PlanMemory(cfg));
    if (!ar->ok() || !e.Init(cfg, ar->get())) { std::fprintf(stderr, "init fail\n"); std::exit(9); }
  }
};

// Renders x through e in fixed blocks (clamped to remaining); optional hook fires
// exactly once BEFORE the block that contains absolute frame `hookAt` (relative to
// the start of this render) — or, if split=true, the block is split so the hook
// fires exactly at that frame.
static std::vector<float> Render(Engine& e, const std::vector<float>& x, uint32_t block,
                                 int64_t hookAt = -1, std::function<void(Engine&)> hook = {},
                                 bool split = false) {
  std::vector<float> out(x.size() * 2);
  std::vector<float> oL(512), oR(512);
  size_t pos = 0;
  bool fired = false;
  while (pos < x.size()) {
    size_t n = std::min<size_t>(block, x.size() - pos);
    if (!fired && hookAt >= 0) {
      if (split) {
        if (static_cast<int64_t>(pos) == hookAt) { hook(e); fired = true; }
        else if (static_cast<int64_t>(pos) < hookAt && static_cast<int64_t>(pos + n) > hookAt)
          n = static_cast<size_t>(hookAt - static_cast<int64_t>(pos));
      } else if (static_cast<int64_t>(pos + n) > hookAt) { hook(e); fired = true; }
    }
    const float* ins[2] = {x.data() + pos, x.data() + pos};
    float* outs[2] = {oL.data(), oR.data()};
    Engine::ProcessContext ctx; ctx.in = ins; ctx.out = outs; ctx.numFrames = static_cast<uint32_t>(n);
    e.Process(ctx);
    for (size_t i = 0; i < n; ++i) { out[2 * (pos + i)] = oL[i]; out[2 * (pos + i) + 1] = oR[i]; }
    pos += n;
  }
  return out;
}

struct Cmp { bool identical; long long firstDiff, lastDiff; double maxAbs, nullDb; };
static Cmp Compare(const std::vector<float>& a, const std::vector<float>& b) {
  Cmp c{true, -1, -1, 0.0, -400.0};
  double se = 0, sr = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    uint32_t ua, ub; std::memcpy(&ua, &a[i], 4); std::memcpy(&ub, &b[i], 4);
    const double d = double(a[i]) - double(b[i]);
    se += d * d; sr += double(b[i]) * b[i];
    if (ua != ub) {
      c.identical = false;
      if (c.firstDiff < 0) c.firstDiff = static_cast<long long>(i / 2);
      c.lastDiff = static_cast<long long>(i / 2);
      c.maxAbs = std::max(c.maxAbs, std::fabs(d));
    }
  }
  if (!c.identical) c.nullDb = 10 * std::log10((se + 1e-300) / (sr + 1e-300));
  return c;
}
static void Report(const char* what, const Cmp& c, size_t frames) {
  if (c.identical) { std::printf("  %-62s IDENTICAL (all %zu frames)\n", what, frames); return; }
  std::printf("  %-62s DIFF first=%.4fs last=%.4fs (of %.1fs, %lld frames after lastDiff) maxAbs=%.3g null=%.1f dB%s\n", what,
              c.firstDiff / kSr, c.lastDiff / kSr, frames / kSr, (long long)frames - 1 - c.lastDiff, c.maxAbs, c.nullDb,
              (c.lastDiff + 1 < static_cast<long long>(frames)) ? "  -> reconverged" : "  -> still diverged at end");
}

static void runPriorG(Rig& r, const std::vector<float>& hist);
int main() {
  const std::vector<float> X = MakeInput(1, 10.0);   // history before the "load"
  const std::vector<float> Y = MakeInput(2, 10.0);   // a different history
  const std::vector<float> Z = MakeInput(3, 40.0);   // the material after the load
  struct NQ { const char* name; PresetFn q; } qs[] = {
      {"jitterNoFb", Q_jitterNoFb}, {"shimmerFb", Q_shimmerFb},
      {"fixedNoFb", Q_fixedNoFb},  {"fixedFb", Q_fixedFb}};

  for (const NQ& q : qs) {
    std::printf("=== preset %s\n", q.name);
    // Canonical reference C: fresh Init -> apply -> Reset (snap) -> render Z.
    Rig rc; q.q(rc.e); rc.e.Reset();
    const auto C = Render(rc.e, Z, 48);

    { // sanity: plugin-sized blocks on the canonical sequence
      Rig r; q.q(r.e); r.e.Reset();
      Report("C@512 vs C@48 (block-split invariance)", Compare(Render(r.e, Z, 512), C), Z.size());
    }
    { // P1: no Reset after applying -> smoothers glide from Init defaults
      Rig r; q.q(r.e);
      Report("P1 Init->Set->Process (no Reset) vs canonical", Compare(Render(r.e, Z, 48), C), Z.size());
    }
    auto runPrior = [&](Rig& r, const std::vector<float>& hist) {
      P_prior(r.e); r.e.Reset(); Render(r.e, hist, 48);
      Engine::ProcessContext dummy{}; (void)dummy;
    };
    // P2: restart an already-running engine
    {
      Rig r; runPrior(r, X); q.q(r.e); r.e.Reset(); r.e.ClearHistory(); r.e.ProbeClearTamerBuffers(); r.e.ProbeRebaseCounter(0);
      Report("P2-delta: preset applied as DELTA over prior params (all clears)", Compare(Render(r.e, Z, 48), C), Z.size());
    }
    {
      Rig r; runPrior(r, X); ApplyDefaults(r.e); q.q(r.e); r.e.Reset(); r.e.ClearHistory();
      Report("P2a Reset+ClearHistory (counter continues)", Compare(Render(r.e, Z, 48), C), Z.size());
    }
    {
      Rig r; runPrior(r, X); ApplyDefaults(r.e); q.q(r.e); r.e.Reset(); r.e.ClearHistory(); r.e.ProbeRebaseCounter(0);
      Report("P2b Reset+ClearHistory+rebase(0)", Compare(Render(r.e, Z, 48), C), Z.size());
    }
    {
      Rig r; runPrior(r, X); ApplyDefaults(r.e); q.q(r.e); r.e.Reset(); r.e.ClearHistory(); r.e.ProbeClearTamerBuffers();
      r.e.ProbeRebaseCounter(0);
      Report("P2c Reset+ClearHistory+tamerClear+rebase(0)", Compare(Render(r.e, Z, 48), C), Z.size());
    }
    // P3: spillover (history kept) — two engines with different pasts
    {
      Rig a; runPrior(a, X); ApplyDefaults(a.e); q.q(a.e); a.e.Reset(); a.e.ProbeRebaseCounter(0);
      Rig b; runPrior(b, Y); ApplyDefaults(b.e); q.q(b.e); b.e.Reset(); b.e.ProbeRebaseCounter(0);
      const auto A = Render(a.e, Z, 48), B = Render(b.e, Z, 48);
      Report("P3a spillover+rebase(0): pastX vs pastY", Compare(A, B), Z.size());
      Report("P3a spillover+rebase(0): pastX vs canonical", Compare(A, C), Z.size());
    }
    {
      Rig a; runPrior(a, X); ApplyDefaults(a.e); q.q(a.e); a.e.Reset();
      Rig b; runPrior(b, X); Render(b.e, MakeInput(9, 0.5), 48);  // same past, +0.5 s offset
      ApplyDefaults(b.e); q.q(b.e); b.e.Reset();
      const auto A = Render(a.e, Z, 48), B = Render(b.e, Z, 48);
      Report("P3b spillover, NO rebase (counters differ by 0.5 s)", Compare(A, B), Z.size());
    }
  }

  // P3c: spillover+rebase, then input goes SILENT: does the feedback loop decay
  // to exact zero and the two engines reconverge bit-exactly?
  {
    std::vector<float> ZS = MakeInput(3, 20.0);
    ZS.resize(ZS.size() + static_cast<size_t>(60.0 * kSr), 0.f);
    for (const NQ& q : qs) {
      Rig rc; q.q(rc.e); rc.e.Reset();
      const auto C = Render(rc.e, ZS, 48);
      Rig a; runPriorG(a, X); ApplyDefaults(a.e); q.q(a.e); a.e.Reset(); a.e.ProbeRebaseCounter(0);
      const auto A = Render(a.e, ZS, 48);
      std::string w = std::string("P3c ") + q.name + ": spillover+rebase vs canonical, 20 s input + 60 s silence";
      Report(w.c_str(), Compare(A, C), ZS.size());
      double mc = 0, md = 0; size_t nzc = 0;
      for (size_t i = ZS.size() * 2 - static_cast<size_t>(20 * kSr) * 2; i < ZS.size() * 2; ++i) {
        mc = std::max(mc, (double)std::fabs(C[i])); md = std::max(md, (double)std::fabs(A[i] - C[i]));
        if (C[i] != 0.f) ++nzc;
      }
      std::printf("      last 20 s: max|canonical|=%.3g (%zu nonzero samples) max|A-C|=%.3g\n", mc, nzc, md);
    }
  }
  // P4: event timing. Preset shimmerFb running; at t = 100000 (not a multiple of 48
  // or 512) the user changes pitch and mix.
  std::printf("=== P4 parameter event at absolute frame 100000\n");
  auto ev = [](Engine& e) { e.SetParam(ParamId::PitchSt, 7.f); e.SetParam(ParamId::Mix, 0.8f); };
  const auto Z10 = MakeInput(3, 10.0);
  std::vector<float> outs[4];
  int k = 0;
  for (bool split : {false, true})
    for (uint32_t blk : {48u, 512u}) {
      Rig r; Q_shimmerFb(r.e); r.e.Reset();
      outs[k++] = Render(r.e, Z10, blk, 100000, ev, split);
    }
  Report("P4a apply-at-next-block-start: 48 vs 512", Compare(outs[0], outs[1]), Z10.size());
  Report("P4b wrapper splits block at event: 48 vs 512", Compare(outs[2], outs[3]), Z10.size());
  return 0;
}

static void runPriorG(Rig& r, const std::vector<float>& hist) { P_prior(r.e); r.e.Reset(); Render(r.e, hist, 48); }
