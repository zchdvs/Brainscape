#pragma once
#include <cstdint>
#include <initializer_list>

#include "brainscape/Engine.h"
#include "brainscape/EventQueue.h"

// The firmware's event contract (docs/design/cpu-budget.md §4.3 step 8 and D12; the stamping rule
// of docs/design/clock.md §4.5): how much event work one 48-frame block may carry, enforced by the
// event queue's single producer, the control loop, before it pushes. The engine is unchanged: it
// plays every event it is given at its stamped frame, so the contract moves events, never
// rewrites what a block does with the events it gets.
//
// A producer stages a control-loop iteration's events, each stamped on the block grid, and
// flushes them into the queue once:
//
// - Coalescing. A SetParam or MacroMove replaces a staged event of the same type and id at the
//   same stamp: the earlier one is removed and the later one keeps its place, so the frame
//   ends in the state the two would have left (the later value wins, and anything staged between
//   them still applies before it). A SpilloverLoad is a barrier: a load reads row 85 (the tempo
//   recall) when it applies, so nothing staged before a load merges with anything after it.
//   ClockTick and Tap are never coalesced (clock.md §9.6), nor is any other type.
// - The max rule. Stamps never fall: each event is pushed at or after the last pushed stamp,
//   and at or after `earliest`, the first block the audio thread has not started, so a late
//   stamp is counted and lands where it would have applied anyway.
// - Per-block caps, by carrying. A block takes at most `events` events of any type, of them at
//   most `loads` SpilloverLoads, `triggers` Triggers and `ticks` ClockTicks (clock.md §4.5: two).
//   An event that does not fit moves to the next block, and with it everything staged after it,
//   so the order of the stream never changes.
// - The trigger rate cap. A Trigger that would be carried more than `triggerCarryBlocks` blocks
//   past its stamp is dropped and counted: a flood of footswitch or MIDI triggers sounds at most
//   `triggers` per block and backs up at most that far. No other event is ever dropped.
//
// The limits are provisional: N (`events`) is set from bench session 2's per-event costs so that
// N times the worst event's cost fits the event reserve of the governor's static reserve S
// (cpu-budget.md §5.3, a 24,000-cycle placeholder; a MacroMove with eight curved targets is
// about 4,300 cycles, compiler §3.3, so N = 5 until it is measured).
namespace brainscape::fw {

struct EventLimits {
  uint32_t blockFrames        = 48;
  uint32_t events             = 5;
  uint32_t loads              = 1;
  uint32_t triggers           = 1;
  uint32_t ticks              = 2;
  uint32_t triggerCarryBlocks = 8;
};

struct EventBudgetCounts {
  uint64_t staged          = 0;  // events staged
  uint64_t coalesced       = 0;  // staged events a later one of the same type and id replaced
  uint64_t pushed          = 0;  // events the queue accepted
  uint64_t carried         = 0;  // events pushed at a later block than their stamp's
  uint64_t late            = 0;  // events stamped before `earliest` (raised to it)
  uint64_t triggersDropped = 0;  // Triggers past the carry limit
  uint64_t refused         = 0;  // events the queue refused (full): the caller's to report
};

class EventBudget {
 public:
  static constexpr uint32_t kMaxStaged = 64;

  // Every limit is at least 1 (a block always takes one event of each type).
  explicit EventBudget(const EventLimits& limits = EventLimits{}) noexcept : limits_(limits) {
    for (uint32_t* l : {&limits_.blockFrames, &limits_.events, &limits_.loads, &limits_.triggers,
                        &limits_.ticks}) {
      if (*l == 0u) *l = 1u;
    }
  }

  // Stages one event, stamped on the block grid. False, and nothing staged, when the batch holds
  // kMaxStaged events: flush first.
  bool Stage(const Engine::Event& e) noexcept;

  // Pushes the staged events into `queue` in staging order under the contract, none before
  // `earliest`, and empties the batch. Returns how many the queue accepted; `index`, when given,
  // receives each staged event's queue index (Retired) by staging order, or UINT32_MAX for one
  // dropped or refused.
  uint32_t Flush(EventQueue* queue, int64_t earliest, uint32_t* index = nullptr) noexcept;

  // A new engine timeline (an Exact load: the queue cleared, stamps from frame 0): forgets the
  // last stamp and the blocks' use. Staged events are dropped.
  void Restart() noexcept;

  uint32_t                 Staged() const noexcept { return count_; }
  const EventBudgetCounts& Counts() const noexcept { return counts_; }
  const EventLimits&       Limits() const noexcept { return limits_; }

 private:
  // Whether the block at the cursor has room for one more event of type t.
  bool Fits(Engine::EventType t) const noexcept;
  void Take(Engine::EventType t) noexcept;

  EventLimits       limits_;
  EventBudgetCounts counts_{};
  Engine::Event     staged_[kMaxStaged]{};
  uint32_t          count_ = 0;
  // The last stamp pushed, the block it is in (its first frame) and what that block holds.
  bool              started_ = false;
  int64_t           last_    = 0;
  int64_t           block_   = 0;
  uint32_t          used_ = 0, usedLoads_ = 0, usedTriggers_ = 0, usedTicks_ = 0;
};

}  // namespace brainscape::fw
