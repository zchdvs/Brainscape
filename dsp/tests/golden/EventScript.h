#pragma once
#include <cstdint>
#include <vector>

#include "brainscape/Engine.h"

// Frame-stamped event scripts (docs/design/determinism-profile.md §5.11): an event
// is (absolute frame, sequence number, type, id, value). Frames before its stamp use
// the old state, the event applies from its frame, and same-frame events apply in
// sequence order.
namespace brainscape::golden {

enum class EventType : uint8_t { SetParam, Freeze, Trigger };

struct Event {
  int64_t   frame = 0;
  uint32_t  seq   = 0;
  EventType type  = EventType::SetParam;
  ParamId   id    = ParamId::Mix;  // SetParam only
  float     value = 0.f;           // SetParam: exact binary32 plain value; Freeze: 0 or 1
};

class Script {
 public:
  void Param(int64_t frame, ParamId id, float value) { Add(frame, EventType::SetParam, id, value); }
  void Freeze(int64_t frame, bool on) {
    Add(frame, EventType::Freeze, ParamId::Mix, on ? 1.f : 0.f);
  }
  void Trigger(int64_t frame) { Add(frame, EventType::Trigger, ParamId::Mix, 0.f); }

  // Sorted by (frame, seq).
  const std::vector<Event>& Events() const { return events_; }
  std::vector<Event>&       MutableEvents() { return events_; }

 private:
  void Add(int64_t frame, EventType type, ParamId id, float value);

  std::vector<Event> events_;
  uint32_t           seq_ = 0;
};

// The one place that knows how events reach the engine. Today's public API applies
// SetParam, SetFreeze and Trigger at the start of the next Process call, so blocks
// are split at every event frame and the frame's events are applied, in sequence
// order, just before the block that starts there: exactly what an engine-side
// stamped event at in-block offset 0 means (§5.11). When Process takes stamped
// events, Apply hands each block's events over with their offsets and BlockEnd
// stops splitting; nothing else in the harness changes.
class EventCursor {
 public:
  explicit EventCursor(const std::vector<Event>& events) : events_(events) {}

  // Applies the events stamped at `frame`; every earlier event must already be
  // applied. Returns them as [*first, *first + count) of the script.
  uint32_t Apply(Engine& engine, int64_t frame, const Event** first);

  // Where a block that starts at `from` and wants to end at `to` must end instead.
  int64_t BlockEnd(int64_t from, int64_t to) const;

 private:
  const std::vector<Event>& events_;
  size_t                    next_ = 0;
};

}  // namespace brainscape::golden
