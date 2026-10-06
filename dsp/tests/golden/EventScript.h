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

// How events reach the engine: as stamped events in ProcessContext (Engine, the
// contract's delivery), or through SetParam, SetFreeze and Trigger with every block
// split at the events' frames (Split, what a wrapper does without engine-side events).
// Both must render the same bits.
enum class Delivery : uint8_t { Engine, Split };

// Walks a script block by block.
class EventCursor {
 public:
  explicit EventCursor(const std::vector<Event>& events) : events_(events) {}

  // The frame of the first event not yet taken that is stamped after `frame`, or
  // INT64_MAX.
  int64_t NextAfter(int64_t frame) const;
  // The first freeze event not yet taken with a frame in (after, upTo], or INT64_MAX.
  int64_t NextFreeze(int64_t after, int64_t upTo) const;
  // Takes the events stamped before `end` as [*first, *first + count) of the script.
  uint32_t Take(int64_t end, const Event** first);

 private:
  const std::vector<Event>& events_;
  size_t                    next_ = 0;
};

// Split delivery: applies one event through the unstamped API, which the next Process
// call applies at its first frame.
void ApplyUnstamped(Engine& engine, const Event& e);
// Engine delivery: the event stamped with its offset in the block starting at blockStart.
Engine::BlockEvent ToBlockEvent(const Event& e, int64_t blockStart);

}  // namespace brainscape::golden
