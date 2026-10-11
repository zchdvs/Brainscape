#include "bench/EngineMicro.h"

#include <cstring>
#include <new>

#include "detail/DetMath.h"
#include "detail/GrainMath.h"
#include "detail/Granular.h"

namespace brainscape::detail {

// The micro's access to the grain core's private steps (Granular.h names it a friend).
struct GranularCoreProbe {
  static Grain& Record(GranularCore& c, uint32_t i) { return c.grains_[i]; }
  static void   RenderRun(GranularCore& c, Grain& g, uint32_t s, uint32_t e, bool fade, float* l, float* r) {
    c.RenderRun(g, s, e, fade, l, r);
  }
  static void     NextPass(GranularCore& c, Grain& g) { c.NextPass(g); }
  static void     HeapPlace(GranularCore& c, uint32_t slot, int64_t end) { c.HeapPlace(slot, end); }
  static void     Expire(GranularCore& c, int64_t abs) { c.Expire(abs); }
  static uint32_t Sounding(const GranularCore& c) { return c.heapCount_; }
  static uint32_t LowestFreeSlot(const GranularCore& c) { return c.LowestFreeSlot(); }
  static void     ClearHeap(GranularCore& c) {
    c.heapCount_ = 0;
    c.live_[0] = c.live_[1] = 0;
  }
  // A trigger birth at birthAbs, the span rendered up to it (the next Process call starts there).
  static void FireExternal(GranularCore& c, const GranularParams& p, int64_t birthAbs, uint32_t anchor, uint32_t live,
                           float* l, float* r) {
    uint32_t renderedTo = 0;
    c.drawEpoch_        = 0;
    c.FireExternal(p, birthAbs, anchor, live, &renderedTo, 0u, birthAbs, l, r, 0u);
  }
  // The slot of the newest voice (the last birth's).
  static uint32_t NewestSlot(const GranularCore& c) { return c.grains_[c.order_[c.orderCount_ - 1u]].slot; }
};

}  // namespace brainscape::detail

namespace brainscape::fw::bench {

using detail::GranularCore;
using detail::GranularCoreProbe;
using detail::GranularParams;
using detail::Grain;
using detail::TriggerEvents;

size_t MicroCoreBytes() { return sizeof(GranularCore); }
static_assert(sizeof(GranularCore) <= kMicroCoreBytes && alignof(GranularCore) <= 16u,
              "raise kMicroCoreBytes (EngineMicro.h)");

namespace {

constexpr uint32_t kSamples = 8;  // measurements a line

struct Acc {
  MicroLine line;
  void      Add(uint32_t c) {
    if (line.samples == 0 || c < line.min) line.min = c;
    if (c > line.max) line.max = c;
    line.sum += c;
    ++line.samples;
  }
};

std::string Num(uint64_t v) {
  char buf[24];
  int  n = 0;
  char tmp[24];
  do {
    tmp[n++] = static_cast<char>('0' + v % 10u);
    v /= 10u;
  } while (v != 0u);
  for (int i = 0; i < n; ++i) buf[i] = tmp[n - 1 - i];
  return std::string(buf, static_cast<size_t>(n));
}

// ---- RenderRun ------------------------------------------------------------------------------

constexpr uint32_t kPassLen = 4800;  // sustain 0.5, skew 0.5: attack [0, 1200), flat to 3600, decay

void SetGrain(Grain* g, uint32_t slot, int64_t inc, uint32_t envIdx, uint32_t startFrame) {
  *g            = Grain{};
  g->slot       = static_cast<uint8_t>(slot);
  g->inc        = inc;
  g->pos        = static_cast<uint64_t>(startFrame) << 32;
  g->startFrame = startFrame;
  g->passLen    = kPassLen;
  g->passes     = 1;
  g->pass       = 0;
  g->total      = kPassLen;
  g->rendered   = envIdx;
  g->fadeStart  = detail::kNoFade;
  g->env        = grainmath::MakeEnv(static_cast<float>(kPassLen), 0.5f, 0.5f);
  g->smoothness = 0.7f;
  g->gainL      = 0.7f;
  g->gainR      = 0.6f;
  g->decayGain  = 1.0f;
  g->active     = true;
}

void RenderRunMicros(const MicroClock& clk, const MicroMemory& m, GranularCore& c,
                     void (*emit)(const MicroLine&)) {
  struct Path {
    const char* name;
    uint32_t    slot;
    int64_t     inc;  // forward; the reverse runs negate it
  };
  constexpr int64_t kOne = int64_t{1} << 32;
  const Path        paths[] = {
      {"unity", 8, kOne},
      {"hermite r<=1", 0, kOne * 3 / 4},       {"hermite 1<r<=2", 0, kOne * 3 / 2},
      {"hermite r>2", 0, kOne * 7 / 2},        {"linear r<=1", 8, kOne * 3 / 4},
      {"linear 1<r<=2", 8, kOne * 3 / 2},      {"linear r>2", 8, kOne * 7 / 2},
  };
  struct Leg {
    const char* name;
    uint32_t    envIdx;
  };
  const Leg      legs[] = {{"attack", 100}, {"flat", 2000}, {"decay", 4000}};
  constexpr uint32_t kRun = 256;
  Grain&         g      = GranularCoreProbe::Record(c, 0);
  const uint32_t start  = m.ringFrames / 2u;
  for (const Path& path : paths) {
    for (const Leg& leg : legs) {
      for (int dir = 0; dir < 2; ++dir) {
        if (path.inc == kOne && dir == 1) continue;  // unity is forward only
        for (int cold = 0; cold < 2; ++cold) {
          Acc a;
          a.line.group = "renderrun";
          a.line.name  = std::string(path.name) + ", " + leg.name + (dir ? ", reverse" : ", forward") +
                        (cold ? ", ring cold" : ", ring warm");
          a.line.units = kRun;
          for (uint32_t k = 0; k < kSamples; ++k) {
            const int64_t inc = dir ? -path.inc : path.inc;
            if (!cold) {  // warm: the same reads once untimed
              SetGrain(&g, path.slot, inc, leg.envIdx, start);
              GranularCoreProbe::RenderRun(c, g, 0, kRun, false, m.wetL, m.wetR);
            }
            SetGrain(&g, path.slot, inc, leg.envIdx, start);
            clk.begin(cold != 0);
            const uint32_t t0 = clk.now();
            GranularCoreProbe::RenderRun(c, g, 0, kRun, false, m.wetL, m.wetR);
            const uint32_t t1 = clk.now();
            clk.end();
            a.Add(t1 - t0);
          }
          emit(a.line);
        }
      }
    }
  }
  // Under a FastCut fade (the fade's product per frame), linear r <= 2, the flat leg, warm.
  {
    Acc a;
    a.line.group = "renderrun";
    a.line.name  = "linear 1<r<=2, flat, forward, FastCut fade, ring warm";
    a.line.units = 100;
    for (uint32_t k = 0; k < kSamples; ++k) {
      for (int pass = 0; pass < 2; ++pass) {
        SetGrain(&g, 8, kOne * 3 / 2, 2000, start);
        g.fadeStart = 2000;
        g.total     = 2000 + detail::kGranularFastCutFrames;
        if (pass == 0) {
          GranularCoreProbe::RenderRun(c, g, 0, 100, true, m.wetL, m.wetR);
          continue;
        }
        clk.begin(false);
        const uint32_t t0 = clk.now();
        GranularCoreProbe::RenderRun(c, g, 0, 100, true, m.wetL, m.wetR);
        const uint32_t t1 = clk.now();
        clk.end();
        a.Add(t1 - t0);
      }
    }
    emit(a.line);
  }
  // One call: a single frame, warm (the per-run overhead a voice pays once per span and pass).
  {
    Acc a;
    a.line.group = "call";
    a.line.name  = "RenderRun, one frame, linear 1<r<=2, ring warm";
    a.line.units = 1;
    for (uint32_t k = 0; k < kSamples; ++k) {
      SetGrain(&g, 8, kOne * 3 / 2, 2000, start);
      GranularCoreProbe::RenderRun(c, g, 0, 1, false, m.wetL, m.wetR);
      SetGrain(&g, 8, kOne * 3 / 2, 2000, start);
      clk.begin(false);
      const uint32_t t0 = clk.now();
      GranularCoreProbe::RenderRun(c, g, 0, 1, false, m.wetL, m.wetR);
      const uint32_t t1 = clk.now();
      clk.end();
      a.Add(t1 - t0);
    }
    emit(a.line);
  }
  g = Grain{};
}

// ---- Births, the heap, a pass, the sweep -----------------------------------------------------

// The worst trigger birth's parameters (cpu-budget.md §8.2): mark positioning at +12 st (r = 2, so
// the mark capping runs), 50 cents of detune (no cached ratio), reverse at 0.5, 200 ms of spray,
// full pan spread, a 133 ms grain. The scheduler that builds the population runs at jitter 0, so
// exactly `voices` sound between its births; the jittered interval's LogF is timed on its own.
GranularParams WorstParams(uint32_t voices, uint32_t voiceCount) {
  GranularParams p{};
  p.baseDelayFrames = 4800.0;
  p.sprayFrames     = 9600.0f;
  p.totalFrames     = 6400;
  p.targetVoices    = static_cast<float>(voices);
  p.jitter          = 0.0f;
  p.pitchSt[0]      = 12.0f;
  p.pitchWeight[0]  = 1;
  p.pitchCount      = 1;
  p.pitchWeightSum  = 1;
  p.pitchRandom     = false;
  p.spreadCents     = 50.0f;
  p.reverseProb     = 0.5f;
  p.sustain         = 0.5f;
  p.skew            = 0.5f;
  p.smoothness      = 0.7f;
  p.panSpread       = 1.0f;
  p.onsetTrigger    = false;
  p.posFromMark     = true;
  p.periodic        = voices != 0;
  p.intermittency   = 0.0f;
  p.burstCount      = 1;
  p.burstSpacing    = 1;
  p.repeat          = 1;
  p.decayRate       = 0.0f;
  p.voiceCount      = voiceCount;
  return p;
}

// Runs the core's own scheduler, 48 frames a block from frame 0, until `voices` sound at a block's
// end (after one grain length at least, so lives are no longer mark-capped short); an onset mark
// at frame 0 gives every birth the mark path. Returns the next frame, or -1 if it never got there.
int64_t Populate(GranularCore& c, const MicroMemory& m, const GranularParams& p, uint32_t voices) {
  c.Init(m.ring, m.ringFrames - 1u, m.lut);
  const uint32_t mask   = m.ringFrames - 1u;
  uint32_t       anchor = 0;
  int64_t        abs    = 0;
  for (uint32_t b = 0; b < 2000u; ++b) {
    TriggerEvents ev;
    if (b == 0) {
      ev.onsetCount        = 1;
      ev.onsetOffset[0]    = 0;
      ev.onsetMarkFrame[0] = (1u - 256u) & mask;  // as the engine marks a hop's onset (kOnsetHop)
    }
    c.Process(p, ev, abs, 0, static_cast<uint32_t>(abs) & mask, false, &anchor, 48, m.wetL, m.wetR);
    abs += 48;
    if (abs >= 2 * static_cast<int64_t>(p.totalFrames) && GranularCoreProbe::Sounding(c) == voices) return abs;
    if (voices == 0 && b == 0) return abs;
  }
  return -1;
}

void BirthMicros(const MicroClock& clk, const MicroMemory& m, GranularCore& c, void (*emit)(const MicroLine&),
                 bool* ok) {
  struct Case {
    uint32_t voices, voiceCount;
  };
  const Case cases[] = {{0, 64}, {32, 32}, {64, 64}};
  const uint32_t mask = m.ringFrames - 1u;
  for (const Case& k : cases) {
    Acc a;
    a.line.group = "birth";
    a.line.name  = "trigger birth, mark path r=2, detune, reverse, spray, pan, " + Num(k.voices) + " voices" +
                  (k.voices == k.voiceCount ? ", a steal" : "");
    a.line.units = 1;
    uint32_t steals = 0, hermite = 0, sounding = 0;
    for (uint32_t s = 0; s < kSamples; ++s) {
      const GranularParams p   = WorstParams(k.voices, k.voiceCount);
      const int64_t        abs = Populate(c, m, p, k.voices);
      if (abs < 0) {
        *ok = false;
        a.line.detail = "\"setup\":\"the population never reached its voice count\"";
        break;
      }
      sounding               = GranularCoreProbe::Sounding(c);
      const uint64_t stolen0 = c.Stats().steals;
      const auto     live    = static_cast<uint32_t>(abs) & mask;
      clk.begin(false);
      const uint32_t t0 = clk.now();
      GranularCoreProbe::FireExternal(c, p, abs, live, live, m.wetL, m.wetR);
      const uint32_t t1 = clk.now();
      clk.end();
      a.Add(t1 - t0);
      steals += static_cast<uint32_t>(c.Stats().steals - stolen0);
      hermite += GranularCoreProbe::NewestSlot(c) < detail::kHiFiGrains ? 1u : 0u;
    }
    if (a.line.detail.empty()) {
      a.line.detail = "\"sounding\":" + Num(sounding) + ",\"steals\":" + Num(steals) + ",\"hermiteSlots\":" +
                      Num(hermite);
      if (k.voices == k.voiceCount && steals != a.line.samples) *ok = false;
    }
    emit(a.line);
  }

  // The heap: a push to 64 entries that sifts to the root, and a pop of the earliest end.
  {
    Acc push, pop;
    push.line.group = pop.line.group = "heap";
    push.line.name  = "HeapPlace, the 64th voice, sifting to the root";
    pop.line.name   = "Expire, one voice off a heap of 64";
    push.line.units = pop.line.units = 1;
    for (uint32_t s = 0; s < kSamples; ++s) {
      c.Init(m.ring, mask, m.lut);
      GranularCoreProbe::ClearHeap(c);
      for (uint32_t slot = 0; slot < 63u; ++slot) {
        GranularCoreProbe::HeapPlace(c, slot, 1000 + static_cast<int64_t>((slot * 7919u) % 9973u) * 10);
      }
      clk.begin(false);
      uint32_t t0 = clk.now();
      GranularCoreProbe::HeapPlace(c, 63, 1);
      uint32_t t1 = clk.now();
      clk.end();
      push.Add(t1 - t0);
      clk.begin(false);
      t0 = clk.now();
      GranularCoreProbe::Expire(c, 1);
      t1 = clk.now();
      clk.end();
      pop.Add(t1 - t0);
      if (GranularCoreProbe::Sounding(c) != 63u) *ok = false;
    }
    emit(push.line);
    emit(pop.line);
    GranularCoreProbe::ClearHeap(c);
  }

  // A repeat pass boundary: the next pass's decay gain (Exp2F). Its run's split is one more call.
  {
    Acc a;
    a.line.group = "pass";
    a.line.name  = "NextPass, decay on";
    a.line.units = 1;
    Grain& g     = GranularCoreProbe::Record(c, 0);
    for (uint32_t s = 0; s < kSamples; ++s) {
      SetGrain(&g, 8, int64_t{1} << 32, 0, 0);
      g.passes    = 4;
      g.passLen   = 1200;
      g.total     = 4800;
      g.rendered  = 1200;
      g.decayRate = 0.002f;
      g.age0      = 300;
      clk.begin(false);
      const uint32_t t0 = clk.now();
      GranularCoreProbe::NextPass(c, g);
      const uint32_t t1 = clk.now();
      clk.end();
      a.Add(t1 - t0);
    }
    g = Grain{};
    emit(a.line);
  }

  // The 64-slot sweep every scheduler attempt and trigger made before the heap (step 6), on a
  // population of 64: the lowest free slot and the count of sounding voices.
  {
    Acc a;
    a.line.group = "sweep";
    a.line.name  = "the 64-slot sweep before the heap, 64 voices";
    a.line.units = 1;
    const GranularParams p   = WorstParams(64, 64);
    const int64_t        abs = Populate(c, m, p, 64);
    if (abs < 0) *ok = false;
    for (uint32_t s = 0; s < kSamples && abs >= 0; ++s) {
      volatile uint32_t sinkSlot = 0, sinkCount = 0;
      clk.begin(false);
      const uint32_t t0 = clk.now();
      uint32_t       slot = 64, sounding = 0;
      for (uint32_t i = 0; i < 64u; ++i) {
        const Grain& g = GranularCoreProbe::Record(c, i);
        if (!g.active || g.endAbs <= abs) {
          if (slot == 64u) slot = i;
        } else {
          ++sounding;
        }
      }
      sinkSlot          = slot;
      sinkCount         = sounding;
      const uint32_t t1 = clk.now();
      clk.end();
      (void)sinkSlot;
      (void)sinkCount;
      a.Add(t1 - t0);
    }
    emit(a.line);
  }
}

// ---- DetMath --------------------------------------------------------------------------------

template <typename F>
void TimeCalls(const MicroClock& clk, const char* name, F call, void (*emit)(const MicroLine&)) {
  constexpr uint32_t kCalls = 64;
  Acc                a;
  a.line.group = "detmath";
  a.line.name  = name;
  a.line.units = kCalls;
  for (uint32_t s = 0; s < kSamples; ++s) {
    clk.begin(false);
    const uint32_t t0 = clk.now();
    for (uint32_t i = 0; i < kCalls; ++i) call(i);
    const uint32_t t1 = clk.now();
    clk.end();
    a.Add(t1 - t0);
  }
  emit(a.line);
}

volatile float   g_sinkF  = 0.f;
volatile double  g_sinkD  = 0.0;
volatile int64_t g_sinkI  = 0;
volatile float   g_srcF   = 0.37f;
volatile double  g_srcD   = 12345.678;
volatile int64_t g_srcI   = 1234567;

void DetMathMicros(const MicroClock& clk, void (*emit)(const MicroLine&)) {
  namespace grainmath = brainscape::grainmath;
  namespace detmath   = brainscape::detmath;
  TimeCalls(clk, "Exp2F (a pitch ratio, a decay gain)", [](uint32_t i) {
    g_sinkF = detmath::Exp2F(g_srcF + static_cast<float>(i) * 0.01f);
  }, emit);
  TimeCalls(clk, "LogF (the jittered interval)", [](uint32_t i) {
    g_sinkF = detmath::LogF(g_srcF + static_cast<float>(i) * 0.01f);
  }, emit);
  TimeCalls(clk, "SinCosD (the pan)", [](uint32_t i) {
    double s, c;
    detmath::SinCosD(static_cast<double>(g_srcF) + static_cast<double>(i) * 0.01, &s, &c);
    g_sinkD = s + c;
  }, emit);
  TimeCalls(clk, "RandUnit (a draw)", [](uint32_t i) {
    g_sinkF = grainmath::RandUnit(g_srcI + static_cast<int64_t>(i), grainmath::Draw::Spray);
  }, emit);
  TimeCalls(clk, "int64 to double (__aeabi_l2d)", [](uint32_t i) {
    g_sinkD = static_cast<double>(g_srcI + static_cast<int64_t>(i));
  }, emit);
  TimeCalls(clk, "double to int64 (__aeabi_d2lz)", [](uint32_t i) {
    g_sinkI = static_cast<int64_t>(g_srcD + static_cast<double>(i));
  }, emit);
}

}  // namespace

bool RunEngineMicros(const MicroClock& clock, const MicroMemory& mem, void (*emit)(const MicroLine&)) {
  // The ring: deterministic noise; the window table: a ramp (its values only steer the morph).
  uint32_t seed = 0x1234567u;
  for (uint32_t i = 0; i < 2u * mem.ringFrames; ++i) {
    seed        = seed * 1664525u + 1013904223u;
    mem.ring[i] = static_cast<int16_t>(seed >> 16);
  }
  for (uint32_t i = 0; i < detail::kWindowLutSize; ++i) {
    mem.lut[i] = static_cast<float>(i) / static_cast<float>(detail::kWindowLutSize - 1u);
  }
  auto* core = new (mem.core) GranularCore();
  core->Init(mem.ring, mem.ringFrames - 1u, mem.lut);
  bool ok = true;
  {  // what an empty measurement reads: subtract it from every line's cycles
    Acc a;
    a.line.group = "calibration";
    a.line.name  = "an empty measurement";
    a.line.units = 1;
    for (uint32_t s = 0; s < kSamples; ++s) {
      clock.begin(false);
      const uint32_t t0 = clock.now();
      const uint32_t t1 = clock.now();
      clock.end();
      a.Add(t1 - t0);
    }
    emit(a.line);
  }
  RenderRunMicros(clock, mem, *core, emit);
  BirthMicros(clock, mem, *core, emit, &ok);
  DetMathMicros(clock, emit);
  return ok;
}

}  // namespace brainscape::fw::bench
