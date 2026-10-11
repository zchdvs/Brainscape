// Image B, BENCH (firmware/README.md): the DWT measurement pass of determinism-profile.md
// §4.2, §7 and §8.4 step 13, grain-engine.md §8 and the CPU plan's bench sessions
// (docs/design/cpu-budget.md §8). Every number is DWT cycles of the 480 MHz core with
// interrupts off around the measured call; results stream as JSON lines for
// tools/hil/bench_report.py, which turns them into budget percentages (10,000 cycles per
// sample, 480,000 per 48-frame block) and the profile's and the CPU plan's checks.
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
//           | births [quick] | tail | stress [quick] | post [quick] | events [quick]
//           | speed | pld | ledger [quick] | hop (brainscape_bench_hop only) | dfu
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
//   births   an upper bound on the per-birth cost at the maximum birth rate (one per
//            sample): equal voice counts, 48 vs 2.4 births per block
//   tail     §4.2 test b: 123 s of the golden tail vector and 2 s of noise then 120 s of
//            silence, at FZ = 0 and (hooks build) FZ = 1, with statistics for the active
//            part and the silent tail separately and (hooks build) the blocks raising IDC or
//            UFC; output hashes for the golden comparison
//   stress   the CPU plan's session-2 configurations (cpu-budget.md §8.2): the legal corners,
//            wave 1's stressors, CLOCK, the macro sweep and 50 seeded fuzz sets
//   post     each post stage alone, its mix settled and moving, the delay's head gliding and
//            the filter's coefficients moving, against none (costs by difference)
//   events   each event type once every 16 blocks: the event blocks against the blocks before
//   speed    DWT per function on a grain core of the bench's own (bench/EngineMicro.h): RenderRun
//            per voice-frame by path, rate class, envelope leg and direction, ring warm and cold;
//            one RenderRun call; the worst trigger birth at 0, 32 and 64 voices; the voice heap's
//            push and pop; a pass boundary; the old 64-slot sweep; the DetMath calls of a birth
//   pld      the PLD micro-test: SDRAM line walks with no PLD and PLD 1, 2 and 4 lines ahead, and
//            a PLD of the line about to be read (does a demand miss wait on it?)
//   hop      (the brainscape_bench_hop image) the onset hop's analysis three ways on 64 hops, each
//            from the same state: the engine's AnalyzeHop, the textbook form and P1's v2, all in
//            ITCM, caches warm and cleaned, each form's bits against the engine's (D4)
//   ledger   the blocks and stress configurations (fuzz aside) with every block logged: DWT
//            next to the shadow ledger's charge and the engine's counts (block-log lines)
// Every result line carries the shadow ledger's summary (bench/ShadowLedger.h): the blocks DWT
// exceeded it (G3 asks none) and the worst and median DWT-to-ledger ratios.
//
// The configurations themselves are bench/Configs.h's, which the host tool
// brainscape_bench_configs renders too: its hashes are what every line's "hash" must equal.
#include <algorithm>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "JsonLine.h"
#include "bench/Configs.h"
#include "bench/EngineMicro.h"
#include "bench/Microbench.h"
#if defined(BRAINSCAPE_BENCH_HOP)
#include "bench/HopMicro.h"
#endif
#include "bench/ShadowLedger.h"
#include "brainscape/Engine.h"
#include "brainscape/Params.h"
#include "platform/Placement.h"
#include "platform/Platform.h"
#include "platform/UsbSerial.h"
#include "stm32h7xx.h"
#if defined(BRAINSCAPE_FPENV_TEST_HOOKS)
#include "detail/FpEnvGuard.h"
#endif

using namespace brainscape;
using namespace brainscape::golden;
using namespace brainscape::fw::bench;
using brainscape::fw::Serial;
using brainscape::fw::UsbSerial;
namespace mb = brainscape::fw::bench;

namespace {

#if defined(BRAINSCAPE_FPENV_TEST_HOOKS)
constexpr bool kHooks = true;
#else
constexpr bool kHooks = false;
#endif

constexpr uint32_t kFpscrFz     = 1u << 24;
constexpr uint32_t kFlagIdc     = 0x80u;  // input denormal
constexpr uint32_t kFlagUfc     = 0x08u;  // underflow

// What the engine did in one block (the differences of Engine::Stats over it), as the ledger
// suite logs it; saturated at the field's top.
struct BlockCounts {
  uint16_t voiceFrames[Engine::kCostClasses];
  uint16_t births, passes, runs, fade, events;
  uint8_t  post[Engine::kPostStages];
};

// Per-block cycles, ledger charges, FPSCR flags and counts of the current run, and a scratch
// copy for percentiles, in SDRAM (written between measurements only).
__attribute__((section(".bss.brainscape_sdram_cycles"))) uint32_t g_cycles[kMaxBlocks];
__attribute__((section(".bss.brainscape_sdram_sorted"))) uint32_t g_sorted[kMaxBlocks];
__attribute__((section(".bss.brainscape_sdram_ledger"))) uint32_t g_ledger[kMaxBlocks];
__attribute__((section(".bss.brainscape_sdram_flags"))) uint8_t   g_flags[kMaxBlocks];
__attribute__((section(".bss.brainscape_sdram_counts"))) BlockCounts g_counts[kMaxBlocks];

// The speed suite's grain core, window table and wet buffers in DTCM beside the engine's, its
// ring in SDRAM; the PLD walk's buffer in SDRAM.
constexpr uint32_t kMicroRingFrames = 1u << 16;
alignas(16) __attribute__((section(".bss.brainscape_dtcm_micro_core"))) unsigned char g_microCore[kMicroCoreBytes];
__attribute__((section(".bss.brainscape_dtcm_micro_lut"))) float g_microLut[4096];
__attribute__((section(".bss.brainscape_dtcm_micro_wet"))) float g_microWet[2][kMicroMaxFrames];
__attribute__((section(".bss.brainscape_sdram_micro_ring"))) int16_t g_microRing[2u * kMicroRingFrames];
constexpr uint32_t kPldWords = 256u * 1024u;  // 1 MiB
__attribute__((section(".bss.brainscape_sdram_pld"))) uint32_t g_pldBuffer[kPldWords];

Engine*             g_engine = nullptr;
fw::EnginePlacement g_place;
Bench*              g_bench    = nullptr;
uint32_t            g_overhead = 0;  // DWT cycles of an empty measurement

void Emit(const std::string& json) {
  Serial().WriteLine(json, UsbSerial::Mode::Block);
  Serial().Flush(2000);
}

// ---- One measured render -----------------------------------------------------------------

// Statistics of blocks [first, first + blocks) of the run.
struct Stats {
  uint32_t first = 0, blocks = 0;
  uint64_t sum = 0;
  uint32_t min = 0, max = 0, maxIndex = 0, p50 = 0, p90 = 0, p99 = 0, p999 = 0;
  uint32_t idcBlocks = 0, ufcBlocks = 0, flaggedBlocks = 0, worstFlaggedCycles = 0;
  // The shadow ledger (G3): blocks whose DWT exceeded its charge, the largest DWT / ledger and
  // the median ledger / DWT, in permille, and its largest charge.
  uint32_t overLedger = 0, worstPermille = 0, medianLedgerPermille = 0, ledgerMax = 0;
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
    const uint32_t l = g_ledger[b];
    if (c > l) ++s.overLedger;
    if (l > s.ledgerMax) s.ledgerMax = l;
    const uint64_t worst = l != 0 ? static_cast<uint64_t>(c) * 1000u / l : 0xFFFFFFFFu;
    if (worst > s.worstPermille) s.worstPermille = static_cast<uint32_t>(std::min<uint64_t>(worst, 0xFFFFFFFFu));
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
  for (uint32_t b = first; b < end; ++b) {
    const uint64_t r = g_cycles[b] != 0 ? static_cast<uint64_t>(g_ledger[b]) * 1000u / g_cycles[b] : 0xFFFFFFFFu;
    g_sorted[b - first] = static_cast<uint32_t>(std::min<uint64_t>(r, 0xFFFFFFFFu));
  }
  std::sort(g_sorted, g_sorted + s.blocks);
  s.medianLedgerPermille = pct(500);
  return s;
}

std::string LedgerJson(const Stats& s) {
  return JsonObj()
      .UInt("overLedger", s.overLedger)
      .UInt("worstPermille", s.worstPermille)
      .UInt("medianLedgerPermille", s.medianLedgerPermille)
      .UInt("ledgerMax", s.ledgerMax)
      .Done();
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
  o.Raw("ledger", LedgerJson(s));
  return o.Done();
}

uint16_t Sat16(uint64_t v) { return static_cast<uint16_t>(v < 0xFFFFu ? v : 0xFFFFu); }

// The measured block: DWT around Process with interrupts off, then the engine's counts over it.
struct RunState {
  bool               cold = false;  // clean and invalidate the D and I caches before each block
  Engine::GrainStats before{};
};

void TimedBlock(void* user, uint32_t b, const Engine::ProcessContext& ctx) {
  auto* rs = static_cast<RunState*>(user);
#if defined(BRAINSCAPE_FPENV_TEST_HOOKS)
  detail::fpenv_test::flags = 0;
#endif
  __disable_irq();
  if (rs->cold) {
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
  const Engine::GrainStats now = g_engine->Stats();
  const Engine::GrainStats d   = StatsDelta(rs->before, now);
  rs->before                   = now;
  const uint64_t ledger        = LedgerCycles(d, ctx.numFrames);
  g_ledger[b]                  = static_cast<uint32_t>(std::min<uint64_t>(ledger, 0xFFFFFFFFu));
  BlockCounts& c               = g_counts[b];
  for (uint32_t k = 0; k < Engine::kCostClasses; ++k) c.voiceFrames[k] = Sat16(d.voiceFrames[k]);
  c.births = Sat16(d.births);
  c.passes = Sat16(d.repeatPasses);
  c.runs   = Sat16(d.renderRuns);
  c.fade   = Sat16(d.fadeFrames);
  c.events = Sat16(ctx.numEvents);
  for (uint32_t k = 0; k < Engine::kPostStages; ++k) c.post[k] = static_cast<uint8_t>(std::min<uint64_t>(d.postFrames[k], 255u));
}

Result Run(const Spec& s) {
  Result r;
#if defined(BRAINSCAPE_FPENV_TEST_HOOKS)
  detail::fpenv_test::forceFlush = s.forceFlush;
#endif
  RunState rs;
  rs.cold                   = s.cold;
  rs.before                 = g_engine->Stats();
  const Bench::Rendered out = g_bench->Render(s, TimedBlock, &rs);
#if defined(BRAINSCAPE_FPENV_TEST_HOOKS)
  detail::fpenv_test::forceFlush = false;
#endif
  r.loaded        = out.loaded;
  r.blocks        = out.blocks;
  r.eventsPushed  = out.eventsPushed;
  r.eventsRefused = out.eventsRefused;
  r.hash          = out.hash;
  r.whole         = Summarize(0, r.blocks);
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
  if (s.events.kind != Events::None) o.UInt("eventsPushed", r.eventsPushed).UInt("eventsRefused", r.eventsRefused);
  o.Raw("ledger", LedgerJson(w));
  if (r.segmented) {
    o.UInt("tailStartBlock", r.tailStartBlock).Raw("active", StatsJson(r.active)).Raw("tail", StatsJson(r.tail));
  }
  return o.Done();
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
  const Spec s = g_bench->RestartRender();
  Run(s);
  EmitOp("Restart", "after 2 s of rendering", Timed([] { g_engine->Restart(); }));
  EmitOp("Restart", "again, nothing rendered since (skips the clears)",
         Timed([] { g_engine->Restart(); }));
  Run(s);
  static std::unique_ptr<PresetState> preset;
  preset = g_bench->Build(s);
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
  for (Spec& s : g_bench->Blocks(quick)) {
    for (const bool cold : {false, true}) {
      s.cold = cold;
      Emit(ResultJson("blocks", s, Run(s)));
    }
  }
}

void SuiteStages(bool quick) {
  for (const Spec& s : g_bench->Stages(quick)) Emit(ResultJson("stage", s, Run(s)));
}

void SuiteBirths(bool quick) {
  for (const Spec& s : g_bench->Births(quick)) {
    const Result r = Run(s);
    // Scheduler births per block x1000: at most one per frame.
    const uint32_t perBlock = std::min<uint32_t>(48000u, s.voices * 48u * 1000u / s.grainFrames);
    std::string    line     = ResultJson("births", s, r);
    line.pop_back();
    line += ",\"voices\":" + JsonUInt(s.voices) + ",\"grainFrames\":" + JsonUInt(s.grainFrames) +
            ",\"birthsPerBlockX1000\":" + JsonUInt(perBlock) + "}";
    Emit(line);
  }
}

void SuiteTail() {
  for (Spec& s : g_bench->Tail()) {
    for (const bool fz : {false, true}) {
      if (fz && !kHooks) continue;
      s.forceFlush = fz;
      Emit(ResultJson("tail", s, Run(s)));
    }
  }
}

// §8.2's configurations, warm (a cold pass of the corners is `stress` run twice by the report).
void SuiteStress(bool quick) {
  for (const Spec& s : g_bench->Stress(quick)) Emit(ResultJson("stress", s, Run(s)));
}

void SuitePost(bool quick) {
  for (const Spec& s : g_bench->Post(quick)) Emit(ResultJson("post", s, Run(s)));
}

// Each event type at block 16k + 8: the event blocks' mean and maximum, and the mean of the
// blocks just before them (no event), for the event's cost by difference.
void SuiteEvents(bool quick) {
  for (const Spec& s : g_bench->EventCosts(quick)) {
    const Result r      = Run(s);
    uint64_t     sumEv = 0, sumBefore = 0;
    uint32_t     n = 0, maxEv = 0;
    for (uint32_t b = 8; b < r.blocks; b += 16u) {
      sumEv += g_cycles[b];
      sumBefore += g_cycles[b - 1u];
      if (g_cycles[b] > maxEv) maxEv = g_cycles[b];
      ++n;
    }
    std::string line = ResultJson("event", s, r);
    line.pop_back();
    line += ",\"eventBlocks\":" + JsonUInt(n) + ",\"eventMean\":" + JsonUInt(n != 0 ? sumEv / n : 0) +
            ",\"beforeMean\":" + JsonUInt(n != 0 ? sumBefore / n : 0) + ",\"eventMax\":" + JsonUInt(maxEv) + "}";
    Emit(line);
  }
}

// ---- The speed and PLD micros ---------------------------------------------------------------

void MicroBegin(bool cold) {
  __disable_irq();
  if (cold) SCB_CleanInvalidateDCache();  // the ring's lines; the core and tables are in DTCM
  __DSB();
  __ISB();
}
uint32_t MicroNow() { return DWT->CYCCNT; }
void     MicroEnd() { __enable_irq(); }

void EmitMicro(const MicroLine& l) {
  JsonObj o;
  o.Str("type", "speed-micro")
      .Str("group", l.group)
      .Str("name", l.name)
      .UInt("samples", l.samples)
      .UInt("units", l.units)
      .UInt("min", l.min)
      .UInt("max", l.max)
      .UInt("mean", l.samples != 0 ? l.sum / l.samples : 0);
  std::string line = o.Done();
  if (!l.detail.empty()) {
    line.pop_back();
    line += "," + l.detail + "}";
  }
  Emit(line);
}

void SuiteSpeed() {
  static_assert(sizeof(g_microLut) / sizeof(float) >= 4096u, "the window table's size (detail::kWindowLutSize)");
  MicroMemory m;
  m.core       = g_microCore;
  m.ring       = g_microRing;
  m.ringFrames = kMicroRingFrames;
  m.lut        = g_microLut;
  m.wetL       = g_microWet[0];
  m.wetR       = g_microWet[1];
  MicroClock clock;
  clock.begin = MicroBegin;
  clock.now   = MicroNow;
  clock.end   = MicroEnd;
  if (!RunEngineMicros(clock, m, EmitMicro)) {
    Emit(JsonObj().Str("type", "error").Str("message", "a speed micro's setup failed (see its line)").Done());
  }
}

void SuitePld() {
  constexpr uint32_t kLines = 4096;  // 128 KiB: eight times the D-cache, every line cold
  for (uint32_t i = 0; i < kPldWords; ++i) g_pldBuffer[i] = i * 2654435761u;
  struct Walk {
    uint32_t ahead, work;
    bool     same;
  };
  std::vector<Walk> walks;
  for (const uint32_t work : {0u, 16u, 64u}) {
    for (const uint32_t ahead : {0u, 1u, 2u, 4u}) walks.push_back({ahead, work, false});
  }
  walks.push_back({0, 0, true});
  for (const Walk& w : walks) {
    uint32_t best = UINT32_MAX, worst = 0;
    for (int k = 0; k < 4; ++k) {
      const uint32_t c = mb::RunLineWalk(g_pldBuffer, kLines, w.ahead, w.work, w.same);
      best             = std::min(best, c);
      worst            = std::max(worst, c);
    }
    Emit(JsonObj()
             .Str("type", "pld-micro")
             .Str("pld", w.same ? "the line about to be read" : (w.ahead == 0 ? "none" : "ahead"))
             .UInt("ahead", w.ahead)
             .UInt("work", w.work)
             .UInt("lines", kLines)
             .UInt("cycles", best)
             .UInt("worstCycles", worst)
             .UInt("cyclesPerLineX100", static_cast<uint64_t>(best) * 100u / kLines)
             .Done());
  }
}

#if defined(BRAINSCAPE_BENCH_HOP)
// The detector's tables and state in AXI SRAM, as the engine's Warm arena places its own.
float g_hopWarm[kHopWarmFloats];

void SuiteHop() {
  MicroClock clock;
  clock.begin = MicroBegin;
  clock.now   = MicroNow;
  clock.end   = MicroEnd;
  if (!RunHopMicro(clock, g_hopWarm, EmitMicro)) {
    Emit(JsonObj().Str("type", "error").Str("message", "a hop form's bits differ from AnalyzeHop's").Done());
  }
}
#endif

// Every block of the run: DWT, the ledger's charge and the engine's counts, 50 blocks a line.
void EmitBlockLog(const Spec& s, const Result& r) {
  constexpr uint32_t kPerLine = 50;
  for (uint32_t b0 = 0; b0 < r.blocks; b0 += kPerLine) {
    const uint32_t end = std::min(b0 + kPerLine, r.blocks);
    const auto     arr = [&](auto field) {
      std::string a = "[";
      for (uint32_t b = b0; b < end; ++b) a += (b != b0 ? "," : "") + JsonUInt(field(b));
      return a + "]";
    };
    JsonObj o;
    o.Str("type", "block-log").Str("config", s.config).UInt("first", b0);
    o.Raw("cycles", arr([](uint32_t b) { return static_cast<uint64_t>(g_cycles[b]); }));
    o.Raw("ledger", arr([](uint32_t b) { return static_cast<uint64_t>(g_ledger[b]); }));
    o.Raw("births", arr([](uint32_t b) { return static_cast<uint64_t>(g_counts[b].births); }));
    o.Raw("passes", arr([](uint32_t b) { return static_cast<uint64_t>(g_counts[b].passes); }));
    o.Raw("runs", arr([](uint32_t b) { return static_cast<uint64_t>(g_counts[b].runs); }));
    o.Raw("fade", arr([](uint32_t b) { return static_cast<uint64_t>(g_counts[b].fade); }));
    o.Raw("events", arr([](uint32_t b) { return static_cast<uint64_t>(g_counts[b].events); }));
    for (uint32_t k = 0; k < Engine::kCostClasses; ++k) {
      static const char* const kNames[Engine::kCostClasses] = {"vfUnity", "vfHermite0", "vfHermite1", "vfHermite2",
                                                               "vfLinear0", "vfLinear1", "vfLinear2"};
      std::string a = "[";
      for (uint32_t b = b0; b < end; ++b) a += (b != b0 ? "," : "") + JsonUInt(g_counts[b].voiceFrames[k]);
      o.Raw(kNames[k], a + "]");
    }
    for (uint32_t k = 0; k < Engine::kPostStages; ++k) {
      static const char* const kNames[Engine::kPostStages] = {"postMod", "postDelay", "postReverb", "postFilter"};
      std::string a = "[";
      for (uint32_t b = b0; b < end; ++b) a += (b != b0 ? "," : "") + JsonUInt(g_counts[b].post[k]);
      o.Raw(kNames[k], a + "]");
    }
    Emit(o.Done());
  }
}

void SuiteLedger(bool quick) {
  const LedgerConstants& k = kProvisionalLedger;
  std::string            w = "[";
  for (uint32_t i = 0; i < Engine::kCostClasses; ++i) w += (i != 0 ? "," : "") + JsonUInt(k.weight[i]);
  std::string p = "[";
  for (uint32_t i = 0; i < Engine::kPostStages; ++i) p += (i != 0 ? "," : "") + JsonUInt(k.post[i]);
  Emit(JsonObj()
           .Str("type", "ledger-constants")
           .Raw("weight", w + "]")
           .UInt("birth", k.birth)
           .UInt("pass", k.pass)
           .UInt("fade", k.fade)
           .Raw("post", p + "]")
           .UInt("reserve", k.reserve)
           .Done());
  for (const Spec& s : g_bench->LedgerSet(quick)) {
    const Result r = Run(s);
    Emit(ResultJson("ledger", s, r));
    EmitBlockLog(s, r);
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
    SuiteStress(quick);
    SuitePost(quick);
    SuiteEvents(quick);
    SuiteSpeed();
    SuitePld();
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
  } else if (c == "stress") {
    Begin(quick ? "stress quick" : "stress");
    SuiteStress(quick);
  } else if (c == "post") {
    Begin(quick ? "post quick" : "post");
    SuitePost(quick);
  } else if (c == "events") {
    Begin(quick ? "events quick" : "events");
    SuiteEvents(quick);
  } else if (c == "speed") {
    Begin("speed");
    SuiteSpeed();
  } else if (c == "pld") {
    Begin("pld");
    SuitePld();
  } else if (c == "hop") {
#if defined(BRAINSCAPE_BENCH_HOP)
    Begin("hop");
    SuiteHop();
#else
    Emit(JsonObj().Str("type", "error").Str("message", "no hop forms in this image: flash brainscape_bench_hop").Done());
    fw::SetLedMode(fw::LedMode::Idle);
    return;
#endif
  } else if (c == "ledger") {
    Begin(quick ? "ledger quick" : "ledger");
    SuiteLedger(quick);
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
  g_engine = new (g_place.engine) Engine();
  if (!g_engine->Init(ec, g_place.arenas)) fw::Fatal("Engine::Init refused the placement");
  g_bench = new Bench();
  if (!g_bench->Setup(g_engine, &why)) fw::Fatal(why);

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
