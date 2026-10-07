// Image B, BENCH (firmware/README.md): the DWT measurement pass of determinism-profile.md
// §4.2, §7 and §8.4 step 13 and grain-engine.md §8. Every number is DWT cycles of the 480 MHz
// core with interrupts off around the measured call; per-block results stream as JSON lines
// for tools/hil/bench_report.py, which turns them into budget percentages (10,000 cycles per
// sample, 480,000 per 48-frame block) and the profile's decision checks.
//
// Built three times from this source: brainscape_bench (the shipping engine archive, its
// code in ITCM), brainscape_bench_xip (the same archive executing in place from QSPI: the
// code-placement A/B of profile §7.1) and brainscape_bench_hooks (the engine built with the
// FP guard's test hooks: renders with flushing forced on inside the guard, and the
// exception flags of every block: profile §4.2's FZ = 0/1 silent-tail test and flag census).
// The firmware's memcpy, memmove and memset (platform/MemFunctions.c) sit with the engine's
// code: in ITCM in brainscape_bench and _hooks, in QSPI in _xip.
//
// Commands: info | run [quick] | memory | micro | restart | blocks [quick] | stages [quick]
//           | births [quick] | tail | dfu
// Suites:
//   memory   PlanMemory, the placement and the linker map
//   micro    FPU latency with normal and subnormal operands at FZ = 0 and 1 (§4.2 test a),
//            and the cost of the engine's flush idiom on 8 independent chains and on 1
//            (§4.3, §8.3 Q5)
//   restart  Restart (the SDRAM clear of the canonical config), LoadPreset(Exact), Reset,
//            ClearHistory, a raw 16 MiB SDRAM clear (memset, and an 8-register STM loop: the
//            hardware floor) and Init
//   blocks   cycles per 48-frame block, nominal to pessimistic, caches warm and cold, and the
//            pessimistic configuration under a stream of events (parameter sweeps every
//            block, trigger bursts, freeze toggles, Spillover loads that switch modes, Trails
//            and FastCut)
//   stages   per-stage cost by difference: the pessimistic configuration with one stage off
//            (the engine has no per-stage counters, and none are added: they would be
//            instrumentation inside dsp/)
//   births   an upper bound on the per-birth cost at the maximum birth rate (one per
//            sample): equal voice counts, 48 vs 2.4 births per block
//   tail     §4.2 test b: 123 s of the golden tail vector and 2 s of noise then 120 s of
//            silence, at FZ = 0 and (hooks build) FZ = 1, with statistics for the active
//            part and the silent tail separately and (hooks build) the blocks raising IDC or
//            UFC; output hashes for the golden comparison
//
// Sound revision 2 (mode-compiler.md §4.2, §7): onset grains and mark positioning are mode
// structure, so the configurations that use them take a corpus package's mode (kOnsetMarks)
// instead of setting rows 27 and 28, which are retired.
#include <algorithm>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "Corpus.h"
#include "EventScript.h"
#include "JsonLine.h"
#include "Render.h"
#include "Sha256.h"
#include "bench/Microbench.h"
#include "brainscape/Engine.h"
#include "brainscape/EventQueue.h"
#include "brainscape/Params.h"
#include "brainscape/TestSignal.h"
#include "platform/Placement.h"
#include "platform/Platform.h"
#include "platform/UsbSerial.h"
#include "stm32h7xx.h"
#if defined(BRAINSCAPE_FPENV_TEST_HOOKS)
#include "detail/FpEnvGuard.h"
#endif

using namespace brainscape;
using namespace brainscape::golden;
using brainscape::fw::Serial;
using brainscape::fw::UsbSerial;
namespace mb = brainscape::fw::bench;

namespace {

#if defined(BRAINSCAPE_FPENV_TEST_HOOKS)
constexpr bool kHooks = true;
#else
constexpr bool kHooks = false;
#endif

constexpr uint32_t kBlock       = 48;
constexpr uint32_t kMaxBlocks   = 124u * 1000u;  // 124 s of 48-frame blocks
constexpr uint32_t kFpscrFz     = 1u << 24;
constexpr uint32_t kFlagIdc     = 0x80u;  // input denormal
constexpr uint32_t kFlagUfc     = 0x08u;  // underflow

// Per-block cycles and FPSCR flags of the current run, and a scratch copy for percentiles, in
// SDRAM (written between measurements only).
__attribute__((section(".bss.brainscape_sdram_cycles"))) uint32_t g_cycles[kMaxBlocks];
__attribute__((section(".bss.brainscape_sdram_sorted"))) uint32_t g_sorted[kMaxBlocks];
__attribute__((section(".bss.brainscape_sdram_flags"))) uint8_t   g_flags[kMaxBlocks];

Engine*                  g_engine = nullptr;
fw::EnginePlacement      g_place;
std::vector<VectorCase>* g_corpus   = nullptr;
uint32_t                 g_overhead = 0;  // DWT cycles of an empty measurement

// The "events" configuration's transport, as the live image delivers events.
EventQueue         g_queue;
Engine::BlockEvent g_blockEvents[EventQueue::kCapacity];
std::unique_ptr<PresetState> g_loadA, g_loadB;  // Spillover payloads (never modified)

// The structure of the pessimistic configuration: onset-triggered grains positioned at marks,
// the mode of a corpus package (dsp/tests/golden/presets) with the default macros and CTRL,
// which the live image's `onset on` and `marks on` load too (firmware/live/LivePresets.h).
constexpr const char* kOnsetMarks = "strum_marks";

// A complete preset: `preset` (a package's leaves, mode and CTRL, or the defaults, with its
// parameters over them), with the mode and CTRL of the package `structure` when given.
std::unique_ptr<PresetState> Build(const PresetSource& preset, const char* structure) {
  std::unique_ptr<PresetState> mode;
  if (structure != nullptr) {
    mode = CompletePreset(PresetSource{structure, {}});
    if (mode == nullptr) return nullptr;
  }
  return CompletePreset(preset, 0, mode.get());
}

void Emit(const std::string& json) {
  Serial().WriteLine(json, UsbSerial::Mode::Block);
  Serial().Flush(2000);
}

const VectorCase* FindVector(const char* name) {
  for (const VectorCase& v : *g_corpus) {
    if (std::strcmp(v.name, name) == 0) return &v;
  }
  return nullptr;
}

const PresetCase* FindPreset(const char* vector, const char* name) {
  const VectorCase* v = FindVector(vector);
  if (v == nullptr) return nullptr;
  for (const PresetCase& p : v->presets) {
    if (std::strcmp(p.name, name) == 0) return &p;
  }
  return nullptr;
}

// ---- One measured render -----------------------------------------------------------------

struct Spec {
  std::string                   config;   // what the line calls it
  std::string                   input;    // which notes
  PresetSource                  preset;   // loaded Exact: a corpus package or the defaults,
                                          // with parameters over them
  const char*                   structure = nullptr;  // a package's mode and CTRL instead
  std::vector<testsignal::Note> notes;
  uint32_t                      frames     = 0;
  bool                          cold       = false;  // clean+invalidate D and I caches per block
  bool                          forceFlush = false;  // hooks build: FZ inside the guard
  bool                          events     = false;  // the event stream of PushBlockEvents
  // > 0: statistics for the active part [0, tail) and the silent tail [tail, end) too, the
  // tail starting at the first block after the last input frame.
  uint32_t                      tailStartFrame = 0;
};

// Statistics of blocks [first, first + blocks) of the run.
struct Stats {
  uint32_t first = 0, blocks = 0;
  uint64_t sum = 0;
  uint32_t min = 0, max = 0, maxIndex = 0, p50 = 0, p90 = 0, p99 = 0, p999 = 0;
  uint32_t idcBlocks = 0, ufcBlocks = 0, flaggedBlocks = 0, worstFlaggedCycles = 0;
};

struct Result {
  bool        loaded = false;
  uint32_t    blocks = 0;
  Stats       whole, active, tail;
  bool        segmented      = false;
  uint32_t    tailStartBlock = 0;
  uint32_t    eventsPushed = 0, eventsRefused = 0;
  std::string hash;
};

Stats Summarize(uint32_t first, uint32_t end) {
  Stats s;
  s.first  = first;
  s.blocks = end > first ? end - first : 0;
  if (s.blocks == 0) return s;
  s.min = s.max = g_cycles[first];
  s.maxIndex    = first;
  for (uint32_t b = first; b < end; ++b) {
    const uint32_t c = g_cycles[b];
    s.sum += c;
    if (c < s.min) s.min = c;
    if (c > s.max) {
      s.max      = c;
      s.maxIndex = b;
    }
    const uint8_t f = g_flags[b];
    if (f & kFlagIdc) ++s.idcBlocks;
    if (f & kFlagUfc) ++s.ufcBlocks;
    if (f & (kFlagIdc | kFlagUfc)) {
      ++s.flaggedBlocks;
      if (c > s.worstFlaggedCycles) s.worstFlaggedCycles = c;
    }
  }
  std::copy(g_cycles + first, g_cycles + end, g_sorted);
  std::sort(g_sorted, g_sorted + s.blocks);
  auto pct = [&](uint32_t permille) {
    const uint64_t i = static_cast<uint64_t>(s.blocks - 1u) * permille / 1000u;
    return g_sorted[i];
  };
  s.p50  = pct(500);
  s.p90  = pct(900);
  s.p99  = pct(990);
  s.p999 = pct(999);
  return s;
}

std::string StatsJson(const Stats& s) {
  JsonObj o;
  o.UInt("firstBlock", s.first)
      .UInt("blocks", s.blocks)
      .UInt("sum", s.sum)
      .UInt("mean", s.blocks != 0 ? s.sum / s.blocks : 0)
      .UInt("min", s.min)
      .UInt("p50", s.p50)
      .UInt("p90", s.p90)
      .UInt("p99", s.p99)
      .UInt("p999", s.p999)
      .UInt("max", s.max)
      .UInt("maxBlock", s.maxIndex);
  if (kHooks) {
    o.UInt("idcBlocks", s.idcBlocks).UInt("ufcBlocks", s.ufcBlocks).UInt("flaggedBlocks", s.flaggedBlocks);
    o.UInt("worstFlaggedCycles", s.worstFlaggedCycles);
  }
  return o.Done();
}

void PackFrames(const float* l, const float* r, uint32_t n, uint8_t* bytes) {
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t u[2];
    std::memcpy(&u[0], &l[i], 4);
    std::memcpy(&u[1], &r[i], 4);
    for (int c = 0; c < 2; ++c) {
      for (int b = 0; b < 4; ++b) bytes[8u * i + 4u * c + b] = static_cast<uint8_t>(u[c] >> (8 * b));
    }
  }
}

// 0 -> 1 -> 0 over `period` blocks.
float Tri(uint32_t b, uint32_t period) {
  const uint32_t phase = b % period;
  const uint32_t half  = period / 2u;
  return phase < half ? static_cast<float>(phase) / static_cast<float>(half)
                      : static_cast<float>(period - phase) / static_cast<float>(half);
}

// The "events" configuration: what a player and the pedal's control loop throw at the engine,
// stamped into block b of the engine timeline in (frame, seq) order. Every block: cutoff,
// pitch and grain size sweeps (RebuildDirty and the filter coefficients every block); every
// 250 ms a burst of four footswitch triggers 12 frames apart (manual births); every 1.5 s a
// freeze toggle; every second a Spillover load, alternating two presets in two modes, so
// each is a mode switch: the nominal preset (the default mode) with a FastCut, which fades
// every sounding grain over 128 frames, and the pessimistic one (onset grains at marks) with
// Trails (mode-compiler.md §7.3).
void PushBlockEvents(uint32_t b, uint32_t* seq, Result* r) {
  const int64_t f0   = static_cast<int64_t>(b) * kBlock;
  auto          push = [&](int64_t frame, Engine::EventType type, uint32_t id, float value,
                  const PresetState* preset) {
    Engine::Event e;
    e.frame  = frame;
    e.seq    = (*seq)++;
    e.type   = type;
    e.id     = id;
    e.value  = value;
    e.preset = preset;
    ++r->eventsPushed;
    if (!g_queue.Push(e)) ++r->eventsRefused;
  };
  if (b % 1000u == 500u) {
    const bool nominal = (b / 1000u) % 2u == 0u;
    push(f0, Engine::EventType::SpilloverLoad,
         static_cast<uint32_t>(nominal ? SwitchStyle::FastCut : SwitchStyle::Trails), 0.f,
         nominal ? g_loadB.get() : g_loadA.get());
  }
  if (b % 1500u == 750u) push(f0, Engine::EventType::Freeze, 0, (b / 1500u) % 2u == 0u ? 1.f : 0.f, nullptr);
  const auto set = [&](ParamId id, float v) {
    push(f0, Engine::EventType::SetParam, static_cast<uint32_t>(id), Canonicalize(id, v), nullptr);
  };
  set(ParamId::FilterCutoffHz, 500.f + 7500.f * Tri(b, 2000u));
  set(ParamId::TransposeSt, -12.f + 36.f * Tri(b, 3000u));
  set(ParamId::GrainSizeMs, 5.f + 195.f * Tri(b, 4000u));
  if (b % 250u == 0u) {
    for (uint32_t k = 0; k < 4u; ++k) {
      push(f0 + 12 * static_cast<int64_t>(k), Engine::EventType::Trigger,
           static_cast<uint32_t>(Engine::TriggerSource::Footswitch), 1.f, nullptr);
    }
  }
}

Result Run(const Spec& s) {
  Result r;
  {
    const std::unique_ptr<PresetState> preset = Build(s.preset, s.structure);
    r.loaded = preset != nullptr && g_engine->LoadPreset(*preset, LoadMode::Exact);
  }
  g_queue.Clear();  // the Exact load started a new engine timeline at frame 0
  auto gen = std::make_unique<testsignal::Generator>();
  gen->Start(s.notes.data(), static_cast<uint32_t>(s.notes.size()));
  Sha256  sha;
  int32_t qL[kBlock], qR[kBlock];
  float   inL[kBlock], inR[kBlock], outL[kBlock], outR[kBlock];
  uint8_t bytes[kBlock * 8];
  const float* ins[2]  = {inL, inR};
  float*       outs[2] = {outL, outR};
  uint32_t     seq     = 0;
  r.blocks             = std::min(s.frames / kBlock, kMaxBlocks);
#if defined(BRAINSCAPE_FPENV_TEST_HOOKS)
  detail::fpenv_test::forceFlush = s.forceFlush;
#endif
  for (uint32_t b = 0; b < r.blocks; ++b) {
    gen->RenderQ23(qL, qR, kBlock);
    for (uint32_t i = 0; i < kBlock; ++i) {
      inL[i] = testsignal::Q23ToFloat(qL[i]);
      inR[i] = testsignal::Q23ToFloat(qR[i]);
    }
    Engine::ProcessContext ctx;
    ctx.in        = ins;
    ctx.out       = outs;
    ctx.numFrames = kBlock;
    if (s.events) {
      PushBlockEvents(b, &seq, &r);
      ctx.events    = g_blockEvents;
      ctx.numEvents = g_queue.PopBlock(static_cast<int64_t>(b) * kBlock, kBlock, g_blockEvents,
                                       EventQueue::kCapacity);
    }
#if defined(BRAINSCAPE_FPENV_TEST_HOOKS)
    detail::fpenv_test::flags = 0;
#endif
    __disable_irq();
    if (s.cold) {
      // Inside the interrupts-off region, so no interrupt handler re-warms the caches
      // between the invalidate and the measured call.
      SCB_CleanInvalidateDCache();
      SCB_InvalidateICache();
    }
    __DSB();
    __ISB();
    const uint32_t t0 = DWT->CYCCNT;
    g_engine->Process(ctx);
    const uint32_t t1 = DWT->CYCCNT;
    __enable_irq();
    g_cycles[b] = t1 - t0 > g_overhead ? t1 - t0 - g_overhead : 0;
#if defined(BRAINSCAPE_FPENV_TEST_HOOKS)
    g_flags[b] = static_cast<uint8_t>(detail::fpenv_test::flags & (kFlagIdc | kFlagUfc));
#else
    g_flags[b] = 0;
#endif
    PackFrames(outL, outR, kBlock, bytes);
    sha.Update(bytes, sizeof bytes);
  }
#if defined(BRAINSCAPE_FPENV_TEST_HOOKS)
  detail::fpenv_test::forceFlush = false;
#endif
  g_queue.Clear();
  r.hash  = sha.Hex();
  r.whole = Summarize(0, r.blocks);
  if (s.tailStartFrame != 0) {
    r.segmented      = true;
    r.tailStartBlock = std::min((s.tailStartFrame + kBlock - 1u) / kBlock, r.blocks);
    r.active         = Summarize(0, r.tailStartBlock);
    r.tail           = Summarize(r.tailStartBlock, r.blocks);
  }
  return r;
}

std::string ResultJson(const char* type, const Spec& s, const Result& r) {
  const Stats& w = r.whole;
  JsonObj      o;
  o.Str("type", type)
      .Str("config", s.config)
      .Str("input", s.input)
      .Str("cache", s.cold ? "cold" : "warm")
      .Bool("forceFlush", s.forceFlush)
      .Bool("exactLoad", r.loaded)
      .UInt("blocks", r.blocks)
      .UInt("frames", static_cast<uint64_t>(r.blocks) * kBlock)
      .UInt("sum", w.sum)
      .UInt("mean", r.blocks != 0 ? w.sum / r.blocks : 0)
      .UInt("min", w.min)
      .UInt("p50", w.p50)
      .UInt("p90", w.p90)
      .UInt("p99", w.p99)
      .UInt("p999", w.p999)
      .UInt("max", w.max)
      .UInt("maxBlock", w.maxIndex)
      .Str("hash", r.hash);
  if (kHooks) {
    o.UInt("idcBlocks", w.idcBlocks).UInt("ufcBlocks", w.ufcBlocks).UInt("flaggedBlocks", w.flaggedBlocks);
    o.UInt("worstFlaggedCycles", w.worstFlaggedCycles);
  }
  if (s.events) o.UInt("eventsPushed", r.eventsPushed).UInt("eventsRefused", r.eventsRefused);
  if (r.segmented) {
    o.UInt("tailStartBlock", r.tailStartBlock).Raw("active", StatsJson(r.active)).Raw("tail", StatsJson(r.tail));
  }
  return o.Done();
}

// ---- Configurations ------------------------------------------------------------------------

using P = ParamId;

// grain-engine.md §8's nominal row: 64 voices at unity rate, 20 ms grains, every post stage
// in moderate use, the tamer in the loop.
ParamList Nominal() {
  return {{P::Overlap, 1.0f},  {P::GrainSizeMs, 20.0f}, {P::Feedback, 0.5f},
          {P::ModDepth, 0.3f}, {P::DelayMix, 0.3f},     {P::ReverbMix, 0.3f},
          {P::FilterCutoffHz, 5000.0f}};
}

// The pessimistic configuration: 64 voices at +24 st (r = 4) with full cents spread, reverse,
// spray and jitter, feedback above unity, every post stage on (modulation at its maximum
// rate and depth), and in the kOnsetMarks structure onset-triggered grains positioned at marks.
ParamList Pessimistic(float grainMs) {
  return {{P::Overlap, 1.0f},        {P::GrainSizeMs, grainMs}, {P::TransposeSt, 24.0f},
          {P::SpreadCents, 100.0f},  {P::ReverseProb, 0.5f},    {P::SprayMs, 200.0f},
          {P::Jitter, 1.0f},         {P::PanSpread, 1.0f},      {P::Feedback, 1.05f},
          {P::ModDepth, 1.0f},       {P::ModRateHz, 10.0f},     {P::DelayMix, 0.5f},
          {P::DelayFb, 0.6f},        {P::DelayTimeMs, 350.0f},  {P::ReverbMix, 0.5f},
          {P::ReverbTime, 0.9f},     {P::FilterCutoffHz, 2000.0f}, {P::FilterRes, 0.5f},
          {P::FilterMorph, 1.5f},    {P::TriggerSens, 0.8f}};
}

ParamList With(ParamList base, const ParamList& changes) {
  for (const auto& c : changes) {
    bool found = false;
    for (auto& kv : base) {
      if (kv.first == c.first) {
        kv.second = c.second;
        found     = true;
      }
    }
    if (!found) base.push_back(c);
  }
  return base;
}

Spec Make(const std::string& config, const char* vector, PresetSource preset, uint32_t seconds,
          const char* structure = nullptr) {
  Spec s;
  s.config    = config;
  s.input     = vector;
  s.preset    = std::move(preset);
  s.structure = structure;
  const VectorCase* v = FindVector(vector);
  if (v != nullptr) {
    s.notes  = VectorNotes(*v);
    s.frames = std::min(seconds * 48000u, v->frames);
  }
  return s;
}

// The pessimistic configuration at `grainMs`, in its structure, with `changes` over it.
Spec MakePessimistic(const std::string& config, const char* vector, float grainMs,
                     uint32_t seconds, const ParamList& changes = {}) {
  return Make(config, vector, PresetSource{nullptr, With(Pessimistic(grainMs), changes)}, seconds,
              kOnsetMarks);
}

// A corpus preset as the corpus starts it: its package (leaves, mode, CTRL) and parameters.
PresetSource Corpus(const char* vector, const char* preset) {
  const PresetCase* p = FindPreset(vector, preset);
  return p != nullptr ? PresetSource{p->package, p->params} : PresetSource{};
}

// ---- Suites --------------------------------------------------------------------------------

void SuiteMemory() {
  EngineConfig pedal;
  pedal.maxBlockSize = kBlock;
  const MemoryPlan plan = PlanMemory(pedal);
  Emit(JsonObj()
           .Str("type", "memory")
           .UInt("hotBytes", plan.bytes[0])
           .UInt("warmBytes", plan.bytes[1])
           .UInt("bulkBytes", plan.bytes[2])
           .UInt("engineBytes", sizeof(Engine))
           .UInt("hotArena", fw::kHotArenaBytes)
           .UInt("warmArena", fw::kWarmArenaBytes)
           .UInt("bulkArena", fw::kBulkArenaBytes)
           .UInt("engineSlot", fw::kEngineSlotBytes)
           .Raw("map", fw::MemoryMapJson())
           .Done());
}

struct FpCase {
  const char* name;
  mb::FpOp    op;
  uint32_t    x, a, b;
};

void SuiteMicro() {
  constexpr uint32_t kOne = 0x3F800000u, kOnePointFive = 0x3FC00000u, kZero = 0u;
  constexpr uint32_t kSub = 0x00400000u;      // 2^-127, subnormal
  constexpr uint32_t kSubSmall = 0x00000200u; // 2^-140, subnormal: 2^-127 +- 2^-140 stays subnormal
  constexpr uint32_t kEps = 0x3A800000u;      // 2^-10: 1.5 +- 2^-10 is exact
  constexpr uint32_t kMid = 0x30C00000u;      // 1.5 * 2^-30
  constexpr uint32_t kTiny = 0x0D800000u;     // 2^-100: kMid * kTiny = 1.5 * 2^-130, subnormal
  constexpr uint32_t kHuge = 0x71800000u;     // 2^100
  constexpr uint32_t kSmall = 0x26800000u;    // 2^-50: kMid * kSmall stays normal
  constexpr uint32_t kLarge = 0x58800000u;    // 2^50
  constexpr uint32_t kTwo = 0x40000000u;
  static const FpCase kCases[] = {
      {"vmul normal", mb::FpOp::Mul, kOnePointFive, kOne, 0},
      {"vmul subnormal operand and result", mb::FpOp::Mul, kSub, kOne, 0},
      {"vmul pair, normal throughout", mb::FpOp::MulPair, kMid, kSmall, kLarge},
      {"vmul pair, every other result subnormal (underflow)", mb::FpOp::MulPair, kMid, kTiny, kHuge},
      {"vadd normal (+0)", mb::FpOp::Add, kOnePointFive, kZero, 0},
      {"vadd subnormal operand and result (+0)", mb::FpOp::Add, kSub, kZero, 0},
      {"vadd/vsub pair, normal throughout", mb::FpOp::AddPair, kOnePointFive, kEps, 0},
      {"vadd/vsub pair, subnormal operands and results (nonzero addend)", mb::FpOp::AddPair, kSub, kSubSmall, 0},
      {"vdiv normal", mb::FpOp::Div, kOnePointFive, kOne, 0},
      {"vdiv subnormal dividend and result", mb::FpOp::Div, kSub, kOne, 0},
      {"vsqrt normal", mb::FpOp::Sqrt, 0, kTwo, 0},
      {"vsqrt subnormal operand", mb::FpOp::Sqrt, 0, kSub, 0},
      {"vmul.f64 normal", mb::FpOp::MulF64, kOnePointFive, kOne, 0},
  };
  constexpr uint32_t kIterations = 10000u;
  for (const FpCase& c : kCases) {
    for (const uint32_t fz : {0u, 1u}) {
      const uint32_t cycles = mb::RunFpChain(c.op, c.x, c.a, c.b, fz ? kFpscrFz : 0u, kIterations);
      Emit(JsonObj()
               .Str("type", "micro")
               .Str("name", c.name)
               .UInt("fz", fz)
               .UInt("instructions", 16u * kIterations)
               .UInt("cycles", cycles)
               .Done());
    }
  }
  static const char* const kForms[] = {"none", "bit test (engine, FlushTiny.h)", "two compares"};
  for (const uint32_t chains : {8u, 1u}) {
    for (uint32_t f = 0; f < 3; ++f) {
      const uint32_t cycles = mb::RunFlushLoop(static_cast<mb::FlushForm>(f), kIterations, chains);
      Emit(JsonObj()
               .Str("type", "flush-micro")
               .Str("form", kForms[f])
               .UInt("chains", chains)
               .UInt("updates", chains * kIterations)
               .UInt("cycles", cycles)
               .Done());
    }
  }
}

uint32_t Timed(void (*fn)()) {
  __disable_irq();
  __DSB();
  const uint32_t t0 = DWT->CYCCNT;
  fn();
  const uint32_t t1 = DWT->CYCCNT;
  __enable_irq();
  return t1 - t0;
}

void EmitOp(const char* op, const char* state, uint32_t cycles) {
  Emit(JsonObj()
           .Str("type", "restart")
           .Str("op", op)
           .Str("state", state)
           .UInt("cycles", cycles)
           .Done());
}

constexpr uint32_t kRawClearBytes = 16u * 1024u * 1024u;  // the canonical 2^22 ring's bytes

// Eight registers per store, 32 bytes, in a tight loop: the most the core can push into
// the SDRAM without DMA, for comparing Restart with the hardware (profile §8.3 Q9).
void StmClear(void* p, uint32_t bytes) {
  uint32_t*       q   = static_cast<uint32_t*>(p);
  uint32_t* const end = q + bytes / 4u;
  __asm__ volatile(
      "mov r2, #0\n\t"
      "mov r3, #0\n\t"
      "mov r4, #0\n\t"
      "mov r5, #0\n\t"
      "mov r6, #0\n\t"
      "mov r8, #0\n\t"
      "mov r9, #0\n\t"
      "mov r12, #0\n\t"
      "1:\n\t"
      "stmia %[q]!, {r2-r6, r8, r9, r12}\n\t"
      "cmp %[q], %[end]\n\t"
      "bne 1b\n\t"
      : [q] "+r"(q)
      : [end] "r"(end)
      : "r2", "r3", "r4", "r5", "r6", "r8", "r9", "r12", "cc", "memory");
}

void SuiteRestart() {
  // Dirty buffers first: a Restart after rendering clears the ring, the post delay and the
  // reverb (the canonical config's 16 MiB ring and 0.75 MiB post delay in SDRAM).
  Spec s = MakePessimistic("pess_render", "onset_bursts_6s", 20.0f, 2);
  Run(s);
  EmitOp("Restart", "after 2 s of rendering", Timed([] { g_engine->Restart(); }));
  EmitOp("Restart", "again, nothing rendered since (skips the clears)",
         Timed([] { g_engine->Restart(); }));
  Run(s);
  static std::unique_ptr<PresetState> preset;
  preset = Build(s.preset, s.structure);
  EmitOp("LoadPreset(Exact)", "after 2 s of rendering",
         Timed([] { g_engine->LoadPreset(*preset, LoadMode::Exact); }));
  Run(s);
  EmitOp("Reset", "after 2 s of rendering (real-time subset)", Timed([] { g_engine->Reset(); }));
  EmitOp("ClearHistory", "after rendering", Timed([] { g_engine->ClearHistory(); }));
  // The hardware floor of the clear: 16 MiB of the Bulk arena (the engine's state; Init
  // below rebuilds it), through the firmware's memset (what the engine's clears call) and an
  // 8-register STM loop.
  EmitOp("raw clear, 16 MiB SDRAM", "memset (platform/MemFunctions.c)",
         Timed([] { std::memset(g_place.arenas.base[2], 0, kRawClearBytes); }));
  EmitOp("raw clear, 16 MiB SDRAM", "STM loop, 8 registers (hardware floor)",
         Timed([] { StmClear(g_place.arenas.base[2], kRawClearBytes); }));
  EmitOp("Init", "canonical config, maxBlockSize 48", Timed([] {
           EngineConfig ec;
           ec.maxBlockSize = kBlock;
           g_engine->Init(ec, g_place.arenas);
         }));
}

void SuiteBlocks(bool quick) {
  const uint32_t sec = quick ? 4u : 10u;
  std::vector<Spec> specs;
  specs.push_back(Make("default", "plucks_12s", PresetSource{}, sec));
  specs.push_back(Make("nominal", "strums_16s", PresetSource{nullptr, Nominal()}, sec));
  specs.push_back(MakePessimistic("pess_render", "onset_bursts_6s", 20.0f, 6));
  specs.push_back(MakePessimistic("pess_births", "onset_bursts_6s", 1.0f, 6));
  Spec events = MakePessimistic("pess_events: sweeps every block, triggers, freeze, loads", "strums_16s",
                                20.0f, sec);
  events.events = true;
  specs.push_back(events);
  specs.push_back(Make("corpus:tail_post_fb", "strums_16s", Corpus("strums_tail_123s", "tail_post_fb"), sec));
  specs.push_back(Make("corpus:pitch_reverse_spray", "plucks_12s", Corpus("plucks_12s", "pitch_reverse_spray"), sec));
  specs.push_back(Make("corpus:max_delay_spray_rev_up24", "plucks_12s",
                       Corpus("plucks_12s", "max_delay_spray_rev_up24"), sec));
  specs.push_back(Make("corpus:dense_1ms", "onset_bursts_6s", Corpus("onset_bursts_6s", "dense_1ms"), 6));
  for (Spec& s : specs) {
    for (const bool cold : {false, true}) {
      s.cold = cold;
      Emit(ResultJson("blocks", s, Run(s)));
    }
  }
}

void SuiteStages(bool quick) {
  const uint32_t sec = quick ? 3u : 6u;
  struct Variant {
    const char* name;
    ParamList   changes;
    bool        onsetMarks = true;  // false: the default mode (no onset grains, no marks)
  };
  const Variant variants[] = {
      {"all stages", {}},
      {"-modulation", {{P::ModDepth, 0.0f}}},
      {"-post delay", {{P::DelayMix, 0.0f}}},
      {"-reverb", {{P::ReverbMix, 0.0f}}},
      {"-filter", {{P::FilterCutoffHz, 20000.0f}}},
      {"-feedback", {{P::Feedback, 0.0f}}},
      {"-onset grains and marks", {}, false},
      {"-pitch (r = 1)", {{P::TransposeSt, 0.0f}, {P::SpreadCents, 0.0f}}},
      {"-grains (1 voice)", {{P::Overlap, 0.0f}}},
  };
  for (const Variant& v : variants) {
    Spec s = MakePessimistic(std::string("stages:") + v.name, "onset_bursts_6s", 20.0f, sec, v.changes);
    if (!v.onsetMarks) s.structure = nullptr;
    Emit(ResultJson("stage", s, Run(s)));
  }
}

void SuiteBirths(bool quick) {
  const uint32_t sec = quick ? 4u : 10u;
  // Scheduler births at equal voice counts (target = 64 * overlap^3; overlap values whose
  // binary32 target is >= 48 and 16): 1 ms grains are born every 48/T frames and live 48,
  // 20 ms grains every 960/T frames and live 960, so the voices rendered per block match and
  // the births per block differ by (T - T/20). Per-birth DetMath paths on: pitch with cents
  // spread, reverse, spray, pan; jitter 0 for an exact birth count; post stages off. The
  // difference also carries the short grains' ring-read locality (a new spray position per
  // birth), so it bounds ScheduleGrain's cost from above rather than isolating it.
  struct Birth {
    float    overlap;
    uint32_t voices;
  };
  for (const Birth bt : {Birth{0.90856034f, 48u}, Birth{0.62996054f, 16u}}) {
    for (const float ms : {1.0f, 20.0f}) {
      const ParamList params = {{P::Overlap, bt.overlap}, {P::GrainSizeMs, ms}, {P::Jitter, 0.0f},
                                {P::SprayMs, 50.0f},      {P::TransposeSt, 7.0f}, {P::SpreadCents, 50.0f},
                                {P::ReverseProb, 0.5f},   {P::PanSpread, 1.0f}, {P::Mix, 1.0f}};
      Spec s = Make(std::string("births:") + JsonUInt(bt.voices) + " voices, " +
                        (ms == 1.0f ? "1 ms" : "20 ms"),
                    "plucks_12s", PresetSource{nullptr, params}, sec);
      const Result r     = Run(s);
      // Scheduler births per block x1000: at most one per frame.
      const uint32_t grainFrames = ms == 1.0f ? 48u : 960u;
      const uint32_t perBlock    = std::min<uint32_t>(48000u, bt.voices * 48u * 1000u / grainFrames);
      std::string    line        = ResultJson("births", s, r);
      line.pop_back();
      line += ",\"voices\":" + JsonUInt(bt.voices) + ",\"grainFrames\":" + JsonUInt(grainFrames) +
              ",\"birthsPerBlockX1000\":" + JsonUInt(perBlock) + "}";
      Emit(line);
    }
  }
}

void SuiteTail() {
  // The golden tail vector exactly as the corpus renders it (one Exact load, 48-frame blocks,
  // no events): its hash must equal golden.json's strums_tail_123s/tail_post_fb at FZ = 0,
  // and at FZ = 1 too (profile §6.4's forced-flush control). The silent tail starts after
  // the vector's active frames (3 s of strums).
  const VectorCase* tailVec = FindVector("strums_tail_123s");
  Spec goldenTail;
  goldenTail.config = "golden:strums_tail_123s/tail_post_fb";
  goldenTail.input  = "strums_tail_123s";
  goldenTail.preset = Corpus("strums_tail_123s", "tail_post_fb");
  if (tailVec != nullptr) {
    goldenTail.notes          = VectorNotes(*tailVec);
    goldenTail.frames         = tailVec->frames;
    goldenTail.tailStartFrame = tailVec->activeFrames;
  }
  // 2 s of noise, then 120 s of silence through feedback and every post stage (§4.2 b).
  Spec noise;
  noise.config = "noise-tail:pessimistic, feedback 0.95";
  noise.input  = "2 s noise + 120 s silence";
  noise.preset    = PresetSource{nullptr, With(Pessimistic(20.0f), {{P::Feedback, 0.95f}})};
  noise.structure = kOnsetMarks;
  testsignal::Note n;
  n.kind    = testsignal::Kind::Noise;
  n.start   = 0;
  n.length  = 2u * 48000u;
  n.level   = 1 << 22;  // -6 dBFS
  n.seed    = 0x7A11u;
  n.attack  = 48;
  n.release = 480;  // ends at start + length
  noise.notes.push_back(n);
  noise.frames         = 122u * 48000u;
  noise.tailStartFrame = n.start + n.length;
  for (Spec* s : {&goldenTail, &noise}) {
    for (const bool fz : {false, true}) {
      if (fz && !kHooks) continue;
      s->forceFlush = fz;
      Emit(ResultJson("tail", *s, Run(*s)));
    }
  }
}

std::string Hello() {
  return fw::HelloJson("bench", "\"target\":" + JsonString(BRAINSCAPE_FW_IMAGE) +
                                    ",\"engineCode\":" + JsonString(BRAINSCAPE_FW_ENGINE_CODE) +
                                    ",\"hooks\":" + (kHooks ? "true" : "false"));
}

void Begin(const char* suite) {
  Emit(JsonObj()
           .Str("type", "bench-begin")
           .Str("suite", suite)
           .Str("target", BRAINSCAPE_FW_IMAGE)
           .Str("engineCode", BRAINSCAPE_FW_ENGINE_CODE)
           .Bool("hooks", kHooks)
           .UInt("clockHz", fw::SysClkHz())
           .UInt("blockFrames", kBlock)
           .UInt("budgetCyclesPerBlock", static_cast<uint64_t>(fw::SysClkHz()) / 48000u * kBlock)
           .UInt("measurementOverhead", g_overhead)
           .Str("engineArchiveSha256", kHooks ? fw::Build().hooksArchiveSha256
                                              : fw::Build().engineArchiveSha256)
           .Str("firmwareCommit", fw::Build().commit)
           .Bool("firmwareDirty", fw::Build().dirty)
           .Raw("cpu", fw::CpuStateJson())
           .Done());
}

std::vector<std::string> Words(const std::string& line) {
  std::vector<std::string> out;
  std::string              cur;
  for (const char c : line) {
    if (c == ' ' || c == '\t') {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

void Command(const std::vector<std::string>& w) {
  const std::string& c     = w[0];
  const bool         quick = w.size() >= 2 && w[1] == "quick";
  const uint64_t     t0    = fw::Cycles64();
  fw::SetLedMode(fw::LedMode::Busy);
  if (c == "info") {
    Emit(Hello());
  } else if (c == "run") {
    Begin(quick ? "run quick" : "run");
    SuiteMemory();
    SuiteMicro();
    SuiteRestart();
    SuiteBlocks(quick);
    SuiteStages(quick);
    SuiteBirths(quick);
    if (!quick) SuiteTail();
  } else if (c == "memory") {
    Begin("memory");
    SuiteMemory();
  } else if (c == "micro") {
    Begin("micro");
    SuiteMicro();
  } else if (c == "restart") {
    Begin("restart");
    SuiteRestart();
  } else if (c == "blocks") {
    Begin(quick ? "blocks quick" : "blocks");
    SuiteBlocks(quick);
  } else if (c == "stages") {
    Begin(quick ? "stages quick" : "stages");
    SuiteStages(quick);
  } else if (c == "births") {
    Begin(quick ? "births quick" : "births");
    SuiteBirths(quick);
  } else if (c == "tail") {
    Begin("tail");
    SuiteTail();
  } else if (c == "dfu") {
    fw::RebootToBootloader();
  } else {
    Emit(JsonObj().Str("type", "error").Str("message", "unknown command: " + c).Done());
    fw::SetLedMode(fw::LedMode::Idle);
    return;
  }
  if (c != "info") {
    Emit(JsonObj()
             .Str("type", "bench-end")
             .Str("suite", c)
             .UInt("cycles", fw::Cycles64() - t0)
             .UInt("fpscr", fw::ReadFpscr())
             .UInt("droppedBytes", Serial().DroppedBytes())
             .UInt("droppedLines", Serial().DroppedLines())
             .Done());
  }
  fw::SetLedMode(fw::LedMode::Idle);
}

}  // namespace

int main() {
  fw::BoardInit("bench");
  g_place = fw::Placement();
  EngineConfig ec;
  ec.maxBlockSize = kBlock;  // the pedal's engine
  const char* why = nullptr;
  if (!fw::CheckPlacement(ec, g_place, &why)) fw::Fatal(why);
  g_corpus = new std::vector<VectorCase>(BuildCorpus());
  g_loadA  = Build(PresetSource{nullptr, Pessimistic(20.0f)}, kOnsetMarks);
  g_loadB  = Build(PresetSource{nullptr, Nominal()}, nullptr);
  if (g_loadA == nullptr || g_loadB == nullptr || !CheckPreset(*g_loadA) || !CheckPreset(*g_loadB)) {
    fw::Fatal("the events configuration's presets do not build (a package missing from the image?)");
  }
  g_engine = new (g_place.engine) Engine();
  if (!g_engine->Init(ec, g_place.arenas)) fw::Fatal("Engine::Init refused the placement");

  // The cost of the measurement itself: two CYCCNT reads with interrupts off.
  uint32_t overhead = UINT32_MAX;
  for (int i = 0; i < 16; ++i) {
    __disable_irq();
    __DSB();
    __ISB();
    const uint32_t t0 = DWT->CYCCNT;
    const uint32_t t1 = DWT->CYCCNT;
    __enable_irq();
    overhead = std::min(overhead, t1 - t0);
  }
  g_overhead = overhead;

  Serial().WriteLine(Hello(), UsbSerial::Mode::Drop);
  std::string line;
  for (;;) {
    Serial().Pump();
    if (!Serial().ReadLine(&line)) continue;
    const std::vector<std::string> w = Words(line);
    if (!w.empty()) Command(w);
  }
}
