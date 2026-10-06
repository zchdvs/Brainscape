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

uint32_t EventCursor::Apply(Engine& engine, int64_t frame, const Event** first) {
  const size_t begin = next_;
  while (next_ < events_.size() && events_[next_].frame == frame) {
    const Event& ev = events_[next_++];
    switch (ev.type) {
      case EventType::SetParam: engine.SetParam(ev.id, ev.value); break;
      case EventType::Freeze: engine.SetFreeze(ev.value != 0.f); break;
      case EventType::Trigger: engine.Trigger(); break;
    }
  }
  *first = events_.data() + begin;
  return static_cast<uint32_t>(next_ - begin);
}

int64_t EventCursor::BlockEnd(int64_t from, int64_t to) const {
  if (next_ < events_.size() && events_[next_].frame > from && events_[next_].frame < to) {
    return events_[next_].frame;
  }
  return to;
}

}  // namespace brainscape::golden
