#include "detail/FpProfilePrivate.h"

#include "brainscape/EventQueue.h"

// No floating-point arithmetic here: events are copied, never computed, so the queue
// needs no FP environment guard.
namespace brainscape {

static_assert((EventQueue::kCapacity & (EventQueue::kCapacity - 1u)) == 0,
              "the free-running indices wrap at 2^32, so the capacity must divide it");

bool EventQueue::Push(const Engine::Event& event) noexcept {
  const uint32_t tail = tail_.load(std::memory_order_relaxed);
  if (tail - head_.load(std::memory_order_acquire) == kCapacity) {
    overflows_.fetch_add(1u, std::memory_order_relaxed);
    return false;
  }
  ring_[tail & (kCapacity - 1u)] = event;
  tail_.store(tail + 1u, std::memory_order_release);
  return true;
}

uint32_t EventQueue::PopBlock(int64_t blockStart, uint32_t numFrames, Engine::BlockEvent* out,
                              uint32_t capacity) noexcept {
  uint32_t       head   = head_.load(std::memory_order_relaxed);
  const uint32_t tail   = tail_.load(std::memory_order_acquire);
  const int64_t  end    = blockStart + numFrames;
  uint32_t       n      = 0;
  uint32_t       offset = 0;
  while (head != tail && n < capacity) {
    const Engine::Event& e = ring_[head & (kCapacity - 1u)];
    if (e.frame >= end) break;
    if (e.frame > blockStart && static_cast<uint32_t>(e.frame - blockStart) > offset) {
      offset = static_cast<uint32_t>(e.frame - blockStart);
    }
    Engine::BlockEvent& b = out[n++];
    b.offset = offset;
    b.seq    = e.seq;
    b.type   = e.type;
    b.id     = e.id;
    b.value  = e.value;
    b.preset = e.preset;
    ++head;
  }
  head_.store(head, std::memory_order_release);
  return n;
}

uint32_t EventQueue::Overflows() const noexcept {
  return overflows_.load(std::memory_order_relaxed);
}

}  // namespace brainscape
