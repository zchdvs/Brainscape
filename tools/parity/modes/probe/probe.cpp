// Scratch probe: renders candidate factory-mode recipes (today's 28 params only) over
// three deterministic test scores and prints level / tail / periodicity figures.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "brainscape/Engine.h"
#include "brainscape/HostArenas.h"
#include "brainscape/PresetState.h"
#include "brainscape/TestSignal.h"

using namespace brainscape;
using P = ParamId;

struct Recipe {
  const char* name;
  std::vector<std::pair<ParamId, float>> params;
};

static const double kSr = 48000.0;

struct Score {
  const char* name;
  std::vector<testsignal::Note> notes;
  uint32_t inputEnd;  // frame where the last note ends
  uint32_t frames;
  uint32_t probeFrom, probeTo;  // window for periodicity
};

static testsignal::Note Pluck(uint32_t start, uint32_t len, double fHz, double lvl, uint32_t seed) {
  testsignal::Note n;
  n.kind    = testsignal::Kind::Pluck;
  n.start   = start;
  n.length  = len;
  n.level   = static_cast<int32_t>(lvl * 8388607.0);
  n.pitch   = static_cast<uint32_t>(kSr / fHz + 0.5);
  n.seed    = seed;
  n.attack  = 0;
  n.release = 2400;
  return n;
}
static testsignal::Note Tone(uint32_t start, uint32_t len, double fHz, double lvl) {
  testsignal::Note n;
  n.kind    = testsignal::Kind::Tone;
  n.start   = start;
  n.length  = len;
  n.level   = static_cast<int32_t>(lvl * 8388607.0);
  n.pitch   = static_cast<uint32_t>(fHz * 1000.0 + 0.5);
  n.attack  = 24000;
  n.release = 24000;
  return n;
}

static std::vector<Score> MakeScores() {
  std::vector<Score> s;
  {
    Score sc{"phrase", {}, 0, 12 * 48000, 0, 0};
    const double f[8] = {110, 146.8, 196, 220, 261.6, 329.6, 392, 440};
    for (int i = 0; i < 8; ++i) {
      const uint32_t st = 12000 + i * 24000;
      sc.notes.push_back(Pluck(st, 33600, f[i], 0.35, 100 + i));
    }
    sc.inputEnd = 12000 + 7 * 24000 + 33600;
    sc.probeFrom = sc.inputEnd + 4800;
    sc.probeTo   = sc.probeFrom + 96000;
    s.push_back(sc);
  }
  {
    Score sc{"chord", {}, 0, 12 * 48000, 0, 0};
    const double f[4] = {146.8, 220, 293.7, 370};
    for (int i = 0; i < 4; ++i) sc.notes.push_back(Pluck(14400 + i * 1200, 144000, f[i], 0.25, 200 + i));
    sc.inputEnd  = 14400 + 3 * 1200 + 144000;
    sc.probeFrom = 14400 + 24000;
    sc.probeTo   = sc.probeFrom + 96000;
    s.push_back(sc);
  }
  {
    Score sc{"pad", {}, 0, 12 * 48000, 0, 0};
    const double f[3] = {220, 277.18, 329.63};
    for (int i = 0; i < 3; ++i) sc.notes.push_back(Tone(12000, 192000, f[i], 0.15));
    sc.inputEnd  = 12000 + 192000;
    sc.probeFrom = 60000;
    sc.probeTo   = 156000;
    s.push_back(sc);
  }
  return s;
}

static double Db(double x) { return x > 1e-12 ? 20.0 * std::log10(x) : -240.0; }

static double Rms(const std::vector<float>& l, const std::vector<float>& r, uint32_t a, uint32_t b) {
  double acc = 0;
  for (uint32_t i = a; i < b; ++i) acc += 0.5 * (double(l[i]) * l[i] + double(r[i]) * r[i]);
  return std::sqrt(acc / double(b - a));
}

struct Run {
  std::vector<float> l, r;
  uint32_t onsets = 0;
};

static Run Render(const Recipe& rc, const Score& sc, float mixOverride, Engine& eng) {
  PresetState ps;
  // complete state: defaults overridden by the recipe
  size_t n = 0;
  const ParamDescriptor* d = Descriptors(&n);
  for (size_t i = 0; i < n; ++i) {
    float v = d[i].def;
    for (auto& kv : rc.params) if (kv.first == d[i].id) v = kv.second;
    if (d[i].id == P::Mix && mixOverride >= 0.f) v = mixOverride;
    ps.leaves[ps.leafCount++] = {static_cast<uint32_t>(d[i].id), v};
  }
  LoadReport rep;
  eng.LoadPreset(ps, LoadMode::Exact, &rep);
  if (!rep.exact) std::fprintf(stderr, "inexact load %s\n", rc.name);

  testsignal::Generator g;
  g.Start(sc.notes.data(), static_cast<uint32_t>(sc.notes.size()));
  Run out;
  out.l.resize(sc.frames);
  out.r.resize(sc.frames);
  std::vector<float> inL(512), inR(512);
  uint32_t pos = 0;
  while (pos < sc.frames) {
    const uint32_t m = sc.frames - pos < 512 ? sc.frames - pos : 512;
    g.Render(inL.data(), inR.data(), m);
    const float* ins[2] = {inL.data(), inR.data()};
    float* outs[2]      = {out.l.data() + pos, out.r.data() + pos};
    Engine::ProcessContext ctx;
    ctx.in        = ins;
    ctx.out       = outs;
    ctx.numFrames = m;
    eng.Process(ctx);
    out.onsets += eng.ConsumeOnsetCount();
    pos += m;
  }
  return out;
}

static std::vector<float> DryOf(const Score& sc, std::vector<float>* r) {
  testsignal::Generator g;
  g.Start(sc.notes.data(), static_cast<uint32_t>(sc.notes.size()));
  std::vector<float> l(sc.frames);
  r->resize(sc.frames);
  g.Render(l.data(), r->data(), sc.frames);
  return l;
}

// tail: seconds after inputEnd until 50 ms RMS of wet stays below -70 dBFS
static double Tail(const Run& w, const Score& sc) {
  const uint32_t win = 2400;
  uint32_t last = sc.inputEnd;
  for (uint32_t a = sc.inputEnd; a + win <= sc.frames; a += win) {
    if (Db(Rms(w.l, w.r, a, a + win)) > -70.0) last = a + win;
  }
  return double(last - sc.inputEnd) / kSr;
}

// envelope autocorrelation peak lag (ms) in [40, 700] ms over the probe window
static std::pair<double, double> Period(const Run& w, const Score& sc) {
  const uint32_t hop = 240;  // 5 ms
  std::vector<double> env;
  for (uint32_t a = sc.probeFrom; a + hop <= sc.probeTo; a += hop) env.push_back(Rms(w.l, w.r, a, a + hop));
  double mean = 0;
  for (double e : env) mean += e;
  mean /= env.size();
  for (double& e : env) e -= mean;
  double r0 = 0;
  for (double e : env) r0 += e * e;
  if (r0 <= 0) return {0, 0};
  double best = -1;
  size_t bestLag = 0;
  for (size_t lag = 8; lag <= 140 && lag < env.size(); ++lag) {
    double acc = 0;
    for (size_t i = 0; i + lag < env.size(); ++i) acc += env[i] * env[i + lag];
    acc /= r0;
    if (acc > best) { best = acc; bestLag = lag; }
  }
  return {bestLag * 5.0, best};
}


static void Clicks(Engine& eng, const std::vector<Recipe>& recipes, const std::string& only) {
  Score sc{"click", {}, 0, 6 * 48000, 0, 0};
  testsignal::Note n;
  n.kind = testsignal::Kind::Noise; n.start = 12000; n.length = 96; n.level = 4000000; n.seed = 7;
  sc.notes.push_back(n);
  sc.inputEnd = 12096;
  for (const auto& rc : recipes) {
    if (!only.empty() && only != rc.name) continue;
    Run w = Render(rc, sc, 1.0f, eng);
    // envelope peaks: 1 ms windows, local maxima above -50 dBFS separated by >= 30 ms
    std::vector<double> e;
    for (uint32_t a = 0; a + 48 <= sc.frames; a += 48) e.push_back(Rms(w.l, w.r, a, a + 48));
    std::printf("%-22s peaks(ms after click):", rc.name);
    int found = 0; size_t lastIdx = 0;
    for (size_t i = 1; i + 1 < e.size() && found < 8; ++i) {
      if (Db(e[i]) < -50.0) continue;
      bool isMax = true;
      for (size_t j = (i > 30 ? i - 30 : 0); j < i + 30 && j < e.size(); ++j) if (e[j] > e[i]) { isMax = false; break; }
      if (isMax && (found == 0 || i - lastIdx >= 30)) {
        std::printf(" %.0f(%.0fdB)", double(i) - 250.0, Db(e[i]));
        lastIdx = i; ++found;
      }
    }
    std::printf("\n");
  }
}

int main(int argc, char** argv) {
  const std::string only = argc > 1 ? argv[1] : "";
  EngineConfig cfg;  // canonical: 48k, 2^22, stereo, dither
  host::HeapArenas arenas(PlanMemory(cfg));
  Engine eng;
  if (!eng.Init(cfg, arenas.get())) { std::fprintf(stderr, "init failed\n"); return 1; }

  const std::vector<Recipe> recipes = {
#include "recipes.inc"
  };
  const auto scores = MakeScores();
  if (only == "clicks") { Clicks(eng, recipes, argc > 2 ? argv[2] : ""); return 0; }

  std::printf("%-22s %-7s %7s %7s %7s %7s %7s %6s %6s %8s %5s\n", "recipe", "score", "dry", "wet",
              "w-d", "out", "peak", "tail", "ons", "period", "ac");
  for (const auto& rc : recipes) {
    if (!only.empty() && only != rc.name) continue;
    for (const auto& sc : scores) {
      std::vector<float> dr;
      std::vector<float> dl = DryOf(sc, &dr);
      const double dry = Db(Rms(dl, dr, 0, sc.inputEnd));
      Run wet = Render(rc, sc, 1.0f, eng);
      Run out = Render(rc, sc, -1.0f, eng);
      const double wetDb = Db(Rms(wet.l, wet.r, 0, sc.inputEnd));
      const double outDb = Db(Rms(out.l, out.r, 0, sc.inputEnd));
      double pk = 0;
      for (uint32_t i = 0; i < sc.frames; ++i) {
        pk = std::fmax(pk, std::fabs(out.l[i]));
        pk = std::fmax(pk, std::fabs(out.r[i]));
      }
      const auto per = Period(wet, sc);
      std::printf("%-22s %-7s %7.1f %7.1f %7.1f %7.1f %7.1f %6.2f %6u %8.0f %5.2f\n", rc.name, sc.name, dry,
                  wetDb, wetDb - dry, outDb, Db(pk), Tail(wet, sc), wet.onsets, per.first, per.second);
    }
  }
  return 0;
}
