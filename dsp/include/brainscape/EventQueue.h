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
// A full queue refuses the event and counts it. It never coalesces or drops a queued
// event to make room, because that would rewrite the event stream; a render with a
// nonzero Overflows() is outside the parity contract, so scripted renders size their
// pushes per block to stay below kCapacity.
class EventQueue {
 public:
  static constexpr uint32_t kCapacity = 256;

  EventQueue() noexcept = default;
  EventQueue(const EventQueue&)            = delete;
  EventQueue& operator=(const EventQueue&) = delete;

  // Producer thread only, in (frame, sequence) order. False when the queue is full: the
  // event is refused and counted.
  bool Push(const Engine::Event& event) noexcept;

  // Consumer thread only. Moves the queued events stamped before blockStart + numFrames,
  // at most `capacity` of them, into `out` as block events and returns how many. An event
  // stamped before blockStart (a producer's "now", or a late stamp) gets offset 0, and the
  // offsets never decrease, so the span is valid for Process. Events that do not fit stay
  // queued for the next block.
  uint32_t PopBlock(int64_t blockStart, uint32_t numFrames, Engine::BlockEvent* out,
                    uint32_t capacity) noexcept;

  // Any thread: the events Push refused since construction.
  uint32_t Overflows() const noexcept;

 private:
  Engine::Event         ring_[kCapacity];
  std::atomic<uint32_t> head_{0};       // free-running pop count; written by the consumer
  std::atomic<uint32_t> tail_{0};       // free-running push count; written by the producer
  std::atomic<uint32_t> overflows_{0};  // written by the producer
};

}  // namespace brainscape
