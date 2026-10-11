// ctest firmware_event_budget: the firmware's event contract (firmware/platform/EventBudget.h,
// docs/design/cpu-budget.md §4.3 step 8, D12) on the host. The rules one by one: coalescing and
// its load barrier, the max rule, the per-block caps by carrying, the trigger rate cap, late
// stamps and Restart. Then the claim that coalescing moves no output bit: a random stream of
// same-frame parameter, macro, expression, freeze, trigger and load events, rendered once as
// staged and once coalesced through the budget, must give the same output sample for sample.
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "EventScript.h"
#include "brainscape/Engine.h"
#include "brainscape/EventQueue.h"
#include "brainscape/HostArenas.h"
#include "brainscape/Params.h"
#include "platform/EventBudget.h"

using namespace brainscape;
using fw::EventBudget;
using fw::EventLimits;
using Type = Engine::EventType;

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    ++g_failures;
  }
}

Engine::Event Ev(int64_t frame, uint32_t seq, Type type, uint32_t id = 0, float value = 0.f,
                 const PresetState* preset = nullptr) {
  Engine::Event e;
  e.frame  = frame;
  e.seq    = seq;
  e.type   = type;
  e.id     = id;
  e.value  = value;
  e.preset = preset;
  return e;
}

// Everything the queue holds, as (frame, type, id, value), by popping far ahead.
struct Popped {
  int64_t  frame;
  Type     type;
  uint32_t id;
  float    value;
};
std::vector<Popped> Drain(EventQueue* q) {
  std::vector<Popped> out;
  Engine::BlockEvent  be[EventQueue::kCapacity];
  // One giant block from frame 0: every event's offset is its frame.
  const uint32_t n = q->PopBlock(0, 0x7FFFFFFFu, be, EventQueue::kCapacity);
  for (uint32_t i = 0; i < n; ++i) out.push_back({be[i].offset, be[i].type, be[i].id, be[i].value});
  q->PopBlock(0, 0, be, 0);  // retire them
  return out;
}

void Rules() {
  const auto set = [](int64_t f, uint32_t seq, uint32_t id, float v) {
    return Ev(f, seq, Type::SetParam, id, v);
  };
  {  // Coalescing: the later value at the later place; other ids and other stamps untouched.
    EventQueue  q;
    EventBudget b;
    b.Stage(set(96, 1, 5, 1.f));
    b.Stage(set(96, 2, 6, 2.f));
    b.Stage(set(96, 3, 5, 3.f));
    b.Stage(Ev(96, 4, Type::MacroMove, 69, 0.25f));
    b.Stage(Ev(96, 5, Type::MacroMove, 69, 0.5f));
    b.Stage(Ev(96, 6, Type::MacroMove, 70, 0.5f));
    b.Stage(set(144, 7, 5, 4.f));  // the same id at another stamp: kept
    Check(b.Staged() == 5 && b.Counts().coalesced == 2, "coalescing removes the earlier SetParam and MacroMove");
    Check(b.Flush(&q, 0) == 5, "the coalesced batch pushes");
    const auto p = Drain(&q);
    Check(p.size() == 5 && p[0].id == 6 && p[1].id == 5 && p[1].value == 3.f && p[1].frame == 96 &&
              p[2].type == Type::MacroMove && p[2].value == 0.5f && p[3].id == 70 && p[4].frame == 144 &&
              p[4].value == 4.f,
          "coalescing keeps the later event's place and value");
  }
  {  // A load is a barrier; ticks, taps and triggers never coalesce.
    EventQueue  q;
    EventLimits lim;
    lim.events = lim.loads = lim.triggers = lim.ticks = 100;
    EventBudget b(lim);
    PresetState s;
    b.Stage(set(0, 1, 5, 1.f));
    b.Stage(Ev(0, 2, Type::SpilloverLoad, 0, 0.f, &s));
    b.Stage(set(0, 3, 5, 2.f));
    b.Stage(Ev(0, 4, Type::ClockTick));
    b.Stage(Ev(0, 5, Type::ClockTick));
    b.Stage(Ev(0, 6, Type::Tap));
    b.Stage(Ev(0, 7, Type::Tap));
    b.Stage(Ev(0, 8, Type::Trigger));
    b.Stage(Ev(0, 9, Type::Trigger));
    Check(b.Counts().coalesced == 0 && b.Staged() == 9, "nothing merges across a load, and ticks, taps and triggers never do");
    b.Flush(&q, 0);
  }
  {  // N per block: the overflow moves to the next block, the rest of the stream with it.
    EventQueue  q;
    EventBudget b;  // N = 5
    for (uint32_t i = 0; i < 7; ++i) b.Stage(set(48, 1 + i, 1 + i, 0.f));
    Check(b.Flush(&q, 0) == 7, "seven events push");
    const auto p = Drain(&q);
    Check(p.size() == 7 && p[4].frame == 48 && p[5].frame == 96 && p[6].frame == 96 &&
              b.Counts().carried == 2,
          "five events a block, the rest carried");
    b.Stage(set(48, 20, 1, 0.f));  // a later batch stamped behind the cursor: the max rule
    b.Flush(&q, 0);
    const auto p2 = Drain(&q);
    Check(p2.size() == 1 && p2[0].frame == 96, "stamps never fall below the last pushed");
  }
  {  // One load a block, two ticks a block.
    EventQueue  q;
    EventBudget b;
    PresetState s;
    b.Stage(Ev(0, 1, Type::SpilloverLoad, 0, 0.f, &s));
    b.Stage(Ev(0, 2, Type::SpilloverLoad, 1, 0.f, &s));
    b.Stage(set(0, 3, 9, 0.f));
    b.Stage(Ev(0, 4, Type::ClockTick));
    b.Stage(Ev(0, 5, Type::ClockTick));
    b.Stage(Ev(0, 6, Type::ClockTick));
    b.Flush(&q, 0);
    const auto p = Drain(&q);
    Check(p.size() == 6 && p[0].frame == 0 && p[1].frame == 48 && p[2].frame == 48 && p[3].frame == 48 &&
              p[4].frame == 48 && p[5].frame == 96,
          "a second load moves a block, with what follows it; a third tick moves a block");
  }
  {  // The trigger rate cap: one a block, carried at most 8 blocks, then dropped.
    EventQueue  q;
    EventBudget b;
    for (uint32_t i = 0; i < 12; ++i) b.Stage(Ev(0, 1 + i, Type::Trigger));
    Check(b.Flush(&q, 0) == 9, "nine triggers push");
    const auto p = Drain(&q);
    Check(p.size() == 9 && p[8].frame == 8 * 48 && b.Counts().triggersDropped == 3,
          "one trigger a block, the ninth eight blocks late, the rest dropped");
  }
  {  // Late stamps rise to `earliest`; Restart starts a new timeline.
    EventQueue  q;
    EventBudget b;
    b.Stage(set(48, 1, 3, 0.f));
    b.Flush(&q, 480);
    Check(Drain(&q)[0].frame == 480 && b.Counts().late == 1, "a late stamp rises to the first block not started");
    q.Clear();
    b.Restart();
    b.Stage(set(0, 1, 3, 0.f));
    b.Flush(&q, 0);
    Check(Drain(&q)[0].frame == 0, "after Restart stamps start again from 0");
  }
}

// A deterministic stream: at each block's first frame, 0-11 events among SetParam (a few leaves
// and row 85, the tempo recall a load reads), MacroMove, Expression, Freeze, Trigger and
// SpilloverLoad. Rendered as staged and coalesced, the outputs must match.
uint32_t g_rng = 0x2545F491u;
uint32_t Rand(uint32_t n) {
  g_rng = g_rng * 1664525u + 1013904223u;
  return (g_rng >> 8) % n;
}

void Equivalence() {
  using golden::CompletePreset;
  using golden::PresetSource;
  const std::unique_ptr<PresetState> a = CompletePreset(PresetSource{"macro_sweep", {}});
  const std::unique_ptr<PresetState> c =
      CompletePreset(PresetSource{nullptr, {{ParamId::Overlap, 0.8f}, {ParamId::GrainSizeMs, 30.f}}});
  if (a == nullptr || c == nullptr) {
    Check(false, "the equivalence presets build");
    return;
  }
  constexpr uint32_t kBlocks = 3000, kB = 48;
  const ParamId      leaves[] = {ParamId::GrainSizeMs, ParamId::Overlap, ParamId::TransposeSt,
                                 ParamId::Mix,         ParamId::DelayMix, ParamId::TempoRecall};
  // The stream, by block: what a control loop would stage.
  std::vector<std::vector<Engine::Event>> stream(kBlocks);
  uint32_t                                seq = 0;
  for (uint32_t blk = 0; blk < kBlocks; ++blk) {
    const int64_t  f = static_cast<int64_t>(blk) * kB;
    const uint32_t n = Rand(12);
    for (uint32_t i = 0; i < n; ++i) {
      const uint32_t k = Rand(100);
      if (k < 45) {
        const ParamId id = leaves[Rand(6)];
        const auto*   d  = FindParam(id);
        const float   v  = d->min + (d->max - d->min) * static_cast<float>(Rand(1001)) / 1000.f;
        stream[blk].push_back(Ev(f, seq++, Type::SetParam, static_cast<uint32_t>(id), Canonicalize(id, v)));
      } else if (k < 80) {
        const auto id = static_cast<ParamId>(static_cast<uint32_t>(ParamId::MacroActivity) + Rand(8));
        stream[blk].push_back(Ev(f, seq++, Type::MacroMove, static_cast<uint32_t>(id),
                                 Canonicalize(id, static_cast<float>(Rand(1001)) / 1000.f)));
      } else if (k < 86) {
        stream[blk].push_back(Ev(f, seq++, Type::Expression, 0,
                                 Canonicalize(ParamId::PerfExpression, static_cast<float>(Rand(1001)) / 1000.f)));
      } else if (k < 90) {
        stream[blk].push_back(Ev(f, seq++, Type::Freeze, 0, Rand(2) ? 1.f : 0.f));
      } else if (k < 96) {
        stream[blk].push_back(Ev(f, seq++, Type::Trigger, Rand(2)));
      } else {
        stream[blk].push_back(Ev(f, seq++, Type::SpilloverLoad, Rand(2), 0.f, Rand(2) ? a.get() : c.get()));
      }
    }
  }
  EngineConfig ec;
  ec.maxBlockSize = kB;
  host::HeapArenas arenas(PlanMemory(ec));
  auto             engine = std::make_unique<Engine>();
  if (!arenas.ok() || !engine->Init(ec, arenas.get())) {
    Check(false, "Engine::Init");
    return;
  }
  EventLimits wide;  // no carrying: only the coalescing differs between the two renders
  wide.events = wide.loads = wide.triggers = wide.ticks = 1000;
  uint64_t coalesced = 0;
  std::vector<float> out[2];
  for (int pass = 0; pass < 2; ++pass) {
    engine->LoadPreset(*a, LoadMode::Exact);
    EventQueue  q;
    EventBudget b(wide);
    float       inL[kB], inR[kB], oL[kB], oR[kB];
    const float* ins[2]  = {inL, inR};
    float*       outs[2] = {oL, oR};
    Engine::BlockEvent be[EventQueue::kCapacity];
    uint32_t     noise = 12345u;
    for (uint32_t blk = 0; blk < kBlocks; ++blk) {
      for (uint32_t i = 0; i < kB; ++i) {
        noise = noise * 1103515245u + 12345u;
        inL[i] = static_cast<float>(static_cast<int32_t>(noise >> 8) - (1 << 23)) / static_cast<float>(1 << 24);
        inR[i] = inL[i];
      }
      for (const Engine::Event& e : stream[blk]) {
        if (pass == 0) {
          q.Push(e);
        } else {
          b.Stage(e);
        }
      }
      if (pass == 1) b.Flush(&q, static_cast<int64_t>(blk) * kB);
      Engine::ProcessContext ctx;
      ctx.in        = ins;
      ctx.out       = outs;
      ctx.numFrames = kB;
      ctx.events    = be;
      ctx.numEvents = q.PopBlock(static_cast<int64_t>(blk) * kB, kB, be, EventQueue::kCapacity);
      engine->Process(ctx);
      out[pass].insert(out[pass].end(), oL, oL + kB);
      out[pass].insert(out[pass].end(), oR, oR + kB);
    }
    if (pass == 1) coalesced = b.Counts().coalesced;
  }
  Check(coalesced > 500, "the stream coalesces");
  Check(out[0].size() == out[1].size() &&
            std::memcmp(out[0].data(), out[1].data(), out[0].size() * sizeof(float)) == 0,
        "the coalesced stream renders the same output, bit for bit");
  std::printf("equivalence: %u blocks, %llu events coalesced\n", kBlocks,
              static_cast<unsigned long long>(coalesced));
}

}  // namespace

int main() {
  Rules();
  Equivalence();
  std::printf("%s\n", g_failures == 0 ? "event budget: ok" : "event budget: FAILED");
  return g_failures == 0 ? 0 : 1;
}
