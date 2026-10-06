#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace brainscape::plugin {

// One control event for the engine (companion §4.7). Producers: host automation (audio or
// host thread), the GUI (message thread) and scripted producers. MIDI arrives with the
// audio block and state restores travel as a unit, so neither passes through here.
struct WrapperEvent {
  enum class Type : uint8_t { Param, Freeze, Trigger };
  // Same-frame order is state load, host automation, MIDI, UI; Source is that rank.
  enum class Source : uint8_t { Host = 1, Ui = 3 };
  Type     type   = Type::Param;
  Source   source = Source::Ui;
  uint32_t id     = 0;    // ParamId for Type::Param
  float    value  = 0.f;  // canonical plain value; 0/1 for Freeze
  // Set by EventSink::Post: the restore generation the event was posted in.
  uint32_t generation = 0;
  // Absolute engine frame (frames since the last Init) the event applies at (companion
  // §4.10). Live producers leave 0: a stamp at or before a block's first frame applies there.
  uint64_t frame = 0;
};

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)  // padded for alignas: the point of the alignas
#endif

// Bounded multi-producer, single-consumer queue (D. Vyukov's bounded MPMC design with
// one consumer). Lock-free and allocation-free on both sides, so the audio thread may
// also produce. A producer preempted mid-push delays later cells by a block; nothing
// is lost unless the queue is full, which Push reports.
template <typename T, size_t Capacity>
class MpscQueue {
  static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0, "power of two");
  static_assert(std::is_trivially_copyable<T>::value, "cells are copied, not constructed");

 public:
  MpscQueue() noexcept {
    for (size_t i = 0; i < Capacity; ++i) cells_[i].seq.store(i, std::memory_order_relaxed);
  }
  MpscQueue(const MpscQueue&) = delete;
  MpscQueue& operator=(const MpscQueue&) = delete;

  // Any thread.
  bool Push(const T& value) noexcept {
    size_t pos = enqueuePos_.load(std::memory_order_relaxed);
    Cell*  cell;
    for (;;) {
      cell             = &cells_[pos & (Capacity - 1)];
      const size_t seq = cell->seq.load(std::memory_order_acquire);
      const auto   dif = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
      if (dif == 0) {
        if (enqueuePos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) break;
      } else if (dif < 0) {
        return false;  // full
      } else {
        pos = enqueuePos_.load(std::memory_order_relaxed);
      }
    }
    cell->value = value;
    cell->seq.store(pos + 1, std::memory_order_release);
    return true;
  }

  // Consumer thread only.
  bool Pop(T& out) noexcept {
    Cell&        cell = cells_[dequeuePos_ & (Capacity - 1)];
    const size_t seq  = cell.seq.load(std::memory_order_acquire);
    if (static_cast<intptr_t>(seq) - static_cast<intptr_t>(dequeuePos_ + 1) < 0) return false;
    out = cell.value;
    cell.seq.store(dequeuePos_ + Capacity, std::memory_order_release);
    ++dequeuePos_;
    return true;
  }

  static constexpr size_t capacity() noexcept { return Capacity; }

 private:
  struct Cell {
    std::atomic<size_t> seq;
    T                   value;
  };
  Cell                            cells_[Capacity];
  alignas(64) std::atomic<size_t> enqueuePos_{0};
  alignas(64) size_t              dequeuePos_ = 0;
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

using WrapperQueue = MpscQueue<WrapperEvent, 2048>;

// The producers' side of the wrapper queue. On overflow of a live event the consumer
// re-sends every mirror at the next block's first frame, so the live engine cannot stay
// out of step with what the host and GUI show. That is exact for a parameter or freeze
// event, which would have applied at that same frame with the mirror's value; a trigger
// or a scripted event cannot be re-sent, so those are counted as lost, never coalesced
// (profile §5.11).
class EventSink {
 public:
  // Any thread. Live producers: a parameter's or freeze's mirror is stored first.
  void Post(WrapperEvent e) noexcept {
    if (Push(e)) return;
    resync_.store(true, std::memory_order_release);
    if (e.type == WrapperEvent::Type::Trigger) CountLost();
  }
  // Any thread. Scripted producers: no mirror holds the event, so an overflow loses it.
  void PostScripted(WrapperEvent e) noexcept {
    if (!Push(e)) CountLost();
  }

  // Restore generations (companion §4.7): an event posted before a state restore or an
  // Init is older than the state that restore applies. Advanced only under the
  // processor's control mutex, after the restore's state slot is complete.
  uint32_t Generation() const noexcept { return generation_.load(std::memory_order_acquire); }
  void     SetGeneration(uint32_t g) noexcept {
    generation_.store(g, std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_seq_cst);
  }

  WrapperQueue& Queue() noexcept { return queue_; }
  uint32_t      Lost() const noexcept { return lost_.load(std::memory_order_relaxed); }
  void          CountLost() noexcept { lost_.fetch_add(1u, std::memory_order_relaxed); }
  bool          TakeResync() noexcept { return resync_.exchange(false, std::memory_order_acq_rel); }

 private:
  bool Push(WrapperEvent& e) noexcept {
    // Pairs with the fence in SetGeneration: either this event carries the new
    // generation, or the restore's mirror stores land after this producer's.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    e.generation = generation_.load(std::memory_order_acquire);
    return queue_.Push(e);
  }

  WrapperQueue          queue_;
  std::atomic<uint32_t> generation_{0};
  std::atomic<uint32_t> lost_{0};
  std::atomic<bool>     resync_{false};
};

}  // namespace brainscape::plugin
