#include "platform/EventBudget.h"

namespace brainscape::fw {

namespace {

using Type = Engine::EventType;

bool Coalesces(Type t) noexcept { return t == Type::SetParam || t == Type::MacroMove; }

}  // namespace

bool EventBudget::Stage(const Engine::Event& e) noexcept {
  if (count_ == kMaxStaged) return false;
  ++counts_.staged;
  if (Coalesces(e.type)) {
    // The latest staged event of the same type and id at the same stamp, unless a load lies
    // between: removed, and the new one appended, so it applies where the later one did.
    for (uint32_t i = count_; i-- > 0;) {
      const Engine::Event& s = staged_[i];
      if (s.type == Type::SpilloverLoad) break;
      if (s.type == e.type && s.id == e.id && s.frame == e.frame) {
        for (uint32_t k = i + 1u; k < count_; ++k) staged_[k - 1u] = staged_[k];
        --count_;
        ++counts_.coalesced;
        break;
      }
    }
  }
  staged_[count_++] = e;
  return true;
}

bool EventBudget::Fits(Type t) const noexcept {
  if (used_ >= limits_.events) return false;
  switch (t) {
    case Type::SpilloverLoad: return usedLoads_ < limits_.loads;
    case Type::Trigger: return usedTriggers_ < limits_.triggers;
    case Type::ClockTick: return usedTicks_ < limits_.ticks;
    default: return true;
  }
}

void EventBudget::Take(Type t) noexcept {
  ++used_;
  if (t == Type::SpilloverLoad) ++usedLoads_;
  if (t == Type::Trigger) ++usedTriggers_;
  if (t == Type::ClockTick) ++usedTicks_;
}

uint32_t EventBudget::Flush(EventQueue* queue, int64_t earliest, uint32_t* index) noexcept {
  const int64_t frames = limits_.blockFrames != 0u ? static_cast<int64_t>(limits_.blockFrames) : 1;
  const auto    blockOf = [frames](int64_t f) { return f - ((f % frames) + frames) % frames; };
  uint32_t      accepted = 0;
  for (uint32_t i = 0; i < count_; ++i) {
    const Engine::Event& e = staged_[i];
    if (index != nullptr) index[i] = UINT32_MAX;
    int64_t want = e.frame;
    if (want < earliest) {
      want = earliest;
      ++counts_.late;
    }
    // Never below the last stamp pushed, and into the next block when this one is full: a
    // fresh block has room for one event of every type (each limit is at least 1).
    int64_t stamp = started_ && want < last_ ? last_ : want;
    int64_t block = blockOf(stamp);
    bool    fresh = !started_ || block != block_;
    if (!fresh && !Fits(e.type)) {
      block = block_ + frames;
      stamp = block;
      fresh = true;
    }
    if (e.type == Type::Trigger &&
        block - blockOf(want) > static_cast<int64_t>(limits_.triggerCarryBlocks) * frames) {
      ++counts_.triggersDropped;
      continue;
    }
    if (block != blockOf(want)) ++counts_.carried;
    if (fresh) {
      block_   = block;
      started_ = true;
      used_ = usedLoads_ = usedTriggers_ = usedTicks_ = 0;
    }
    last_            = stamp;
    Engine::Event out = e;
    out.frame        = stamp;
    uint32_t at      = 0;
    if (!queue->Push(out, &at)) {
      ++counts_.refused;
      continue;
    }
    Take(e.type);
    ++counts_.pushed;
    ++accepted;
    if (index != nullptr) index[i] = at;
  }
  count_ = 0;
  return accepted;
}

void EventBudget::Restart() noexcept {
  count_   = 0;
  started_ = false;
  last_    = 0;
  block_   = 0;
  used_ = usedLoads_ = usedTriggers_ = usedTicks_ = 0;
}

}  // namespace brainscape::fw
