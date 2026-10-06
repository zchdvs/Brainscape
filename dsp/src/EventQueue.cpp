#include "detail/FpProfilePrivate.h"

#include "brainscape/EventQueue.h"

// No floating-point arithmetic here: events are copied, never computed, so the queue
// needs no FP environment guard.
namespace brainscape {

static_assert((EventQueue::kCapacity & (EventQueue::kCapacity - 1u)) == 0,
              "the free-running indices wrap at 2^32, so the capacity must divide it");

bool EventQueue::Push(const Engine::Event& event, uint32_t* index) noexcept {
  const uint32_t tail    = tail_.load(std::memory_order_relaxed);
  const bool     inOrder = !stamped_ || event.frame > lastFrame_ ||
                       (event.frame == lastFrame_ && event.seq > lastSeq_);
  if (!inOrder || tail - head_.load(std::memory_order_acquire) == kCapacity) {
    refused_.fetch_add(1u, std::memory_order_relaxed);
    return false;
  }
  ring_[tail & (kCapacity - 1u)] = event;
  tail_.store(tail + 1u, std::memory_order_release);
  lastFrame_ = event.frame;
  lastSeq_   = event.seq;
  stamped_   = true;
  if (index != nullptr) *index = tail;
  return true;
}

uint32_t EventQueue::PopBlock(int64_t blockStart, uint32_t numFrames, Engine::BlockEvent* out,
                              uint32_t capacity) noexcept {
  head_.store(taken_, std::memory_order_release);
  if (numFrames == 0) return 0;
  const uint32_t tail = tail_.load(std::memory_order_acquire);
  const int64_t  end  = blockStart + numFrames;
  uint32_t       n    = 0;
  // Push keeps the frames from falling, so the offsets never decrease either.
  while (taken_ != tail && n < capacity) {
    const Engine::Event& e = ring_[taken_ & (kCapacity - 1u)];
    if (e.frame >= end) break;
    Engine::BlockEvent& b = out[n++];
    b.offset = e.frame > blockStart ? static_cast<uint32_t>(e.frame - blockStart) : 0u;
    b.seq    = e.seq;
    b.type   = e.type;
    b.id     = e.id;
    b.value  = e.value;
    b.preset = e.preset;
    ++taken_;
  }
  return n;
}

bool EventQueue::Retired(uint32_t index) const noexcept {
  return static_cast<int32_t>(head_.load(std::memory_order_acquire) - index) > 0;
}

uint32_t EventQueue::Clear() noexcept {
  const uint32_t tail    = tail_.load(std::memory_order_relaxed);
  const uint32_t dropped = tail - taken_;
  taken_                 = tail;
  head_.store(tail, std::memory_order_release);
  stamped_ = false;
  return dropped;
}

uint32_t EventQueue::ConsumeRefused() noexcept {
  return refused_.exchange(0u, std::memory_order_relaxed);
}

}  // namespace brainscape
