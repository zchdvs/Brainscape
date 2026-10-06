#pragma once
#include <atomic>
#include <cstdint>

#include "brainscape/Engine.h"
#include "brainscape/FpProfile.h"

namespace brainscape {

// The engine's event transport (docs/design/determinism-profile.md §5.11; grain-engine.md
// §9, threading table): a lock-free single-producer, single-consumer ring of
// frame-stamped events. Each engine has one producer, the pedal's control loop or the
// desktop wrapper's audio-thread side, to which other threads post; the consumer is the
// audio thread, which takes each block's events as ProcessContext::events.
//
// Push refuses and counts an event when the queue is full, and when its (frame, sequence)
// stamp is not above the last one accepted, because it would apply out of its stamped
// order. It never coalesces or drops a queued event to make room, because that would
// rewrite the event stream. A render during which Push refused anything is outside the
// parity contract, so scripted renders size their pushes per block to stay below
// kCapacity.
//
// Stamps are frames of one engine timeline. Restart, which every Exact load runs, starts a
// new one at frame 0, so the queue is cleared with it (Clear) and producers stamp from the
// restarted counter.
class EventQueue {
 public:
  static constexpr uint32_t kCapacity = 256;

  EventQueue() noexcept = default;
  EventQueue(const EventQueue&)            = delete;
  EventQueue& operator=(const EventQueue&) = delete;

  // Producer thread only. False when the queue is full or the event's (frame, seq) is not
  // above the last accepted event's: the event is refused and counted. On success,
  // *index (when given) receives the event's number, for Retired.
  bool Push(const Engine::Event& event, uint32_t* index = nullptr) noexcept;

  // Consumer thread only, once before each Process call. Retires the events the previous
  // call handed out (their Process call has returned), then moves the queued events
  // stamped before blockStart + numFrames, at most `capacity` of them, into `out` as block
  // events and returns how many. An event stamped before blockStart (a producer's "now",
  // or a late stamp) gets offset 0. A zero-frame block takes none. Events that do not fit
  // stay queued for the next block.
  uint32_t PopBlock(int64_t blockStart, uint32_t numFrames, Engine::BlockEvent* out,
                    uint32_t capacity) noexcept;

  // Any thread: whether event number `index` (from Push, within 2^31 events) is retired,
  // applied by a Process call that has returned or dropped by Clear. The PresetState of a
  // SpilloverLoad event may be reused once the event is retired (companion-app.md §6.1:
  // without a free staging slot, the producer delays the stamp).
  bool Retired(uint32_t index) const noexcept;

  // With no Push, PopBlock or Process running, as Restart requires: drops every queued
  // event, retires every event and forgets the last stamp, so stamps start again from
  // frame 0. Returns how many queued events it dropped.
  uint32_t Clear() noexcept;

  // Any thread; atomic exchange(0). The events Push refused since the last call.
  uint32_t ConsumeRefused() noexcept;

 private:
  Engine::Event         ring_[kCapacity];
  std::atomic<uint32_t> head_{0};     // free-running retire count; written by the consumer
  std::atomic<uint32_t> tail_{0};     // free-running push count; written by the producer
  std::atomic<uint32_t> refused_{0};  // written by the producer
  uint32_t              taken_     = 0;  // consumer: free-running count handed out
  int64_t               lastFrame_ = 0;  // producer: the last accepted stamp
  uint32_t              lastSeq_   = 0;
  bool                  stamped_   = false;
};

}  // namespace brainscape
