#include "EventScript.h"

#include <algorithm>

namespace brainscape::golden {

void Script::Add(int64_t frame, EventType type, ParamId id, float value) {
  Event ev;
  ev.frame = frame;
  ev.seq   = seq_++;
  ev.type  = type;
  ev.id    = id;
  ev.value = value;
  // Sequence numbers rise with insertion, so inserting after every event at or
  // before `frame` keeps the (frame, seq) order.
  const auto at = std::upper_bound(events_.begin(), events_.end(), frame,
                                   [](int64_t f, const Event& e) { return f < e.frame; });
  events_.insert(at, ev);
}

int64_t EventCursor::NextAfter(int64_t frame) const {
  for (size_t i = next_; i < events_.size(); ++i) {
    if (events_[i].frame > frame) return events_[i].frame;
  }
  return INT64_MAX;
}

int64_t EventCursor::NextFreeze(int64_t after, int64_t upTo) const {
  for (size_t i = next_; i < events_.size() && events_[i].frame <= upTo; ++i) {
    if (events_[i].type == EventType::Freeze && events_[i].frame > after) return events_[i].frame;
  }
  return INT64_MAX;
}

uint32_t EventCursor::Take(int64_t end, const Event** first) {
  const size_t begin = next_;
  while (next_ < events_.size() && events_[next_].frame < end) ++next_;
  *first = events_.data() + begin;
  return static_cast<uint32_t>(next_ - begin);
}

void ApplyUnstamped(Engine& engine, const Event& e) {
  switch (e.type) {
    case EventType::SetParam: engine.SetParam(e.id, e.value); break;
    case EventType::Freeze: engine.SetFreeze(e.value != 0.f); break;
    case EventType::Trigger: engine.Trigger(); break;
  }
}

Engine::BlockEvent ToBlockEvent(const Event& e, int64_t blockStart) {
  Engine::BlockEvent b;
  b.offset = static_cast<uint32_t>(e.frame - blockStart);
  b.seq    = e.seq;
  switch (e.type) {
    case EventType::SetParam:
      b.type = Engine::EventType::SetParam;
      b.id   = static_cast<uint32_t>(e.id);
      break;
    case EventType::Freeze: b.type = Engine::EventType::Freeze; break;
    case EventType::Trigger:
      b.type = Engine::EventType::Trigger;
      b.id   = static_cast<uint32_t>(Engine::TriggerSource::Footswitch);
      break;
  }
  b.value = e.type == EventType::Trigger ? 1.f : e.value;
  return b;
}

}  // namespace brainscape::golden
