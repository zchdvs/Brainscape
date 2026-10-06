#pragma once
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "brainscape/Engine.h"

// Frame-stamped event scripts (docs/design/determinism-profile.md §5.11): an event
// is (absolute frame, sequence number, type, id, value). Frames before its stamp use
// the old state, the event applies from its frame, and same-frame events apply in
// sequence order.
namespace brainscape::golden {

using ParamList = std::vector<std::pair<ParamId, float>>;

enum class EventType : uint8_t { SetParam, Freeze, Trigger, SpilloverLoad };

struct Event {
  int64_t   frame  = 0;
  uint32_t  seq    = 0;
  EventType type   = EventType::SetParam;
  ParamId   id     = ParamId::Mix;  // SetParam only
  float     value  = 0.f;           // SetParam: exact binary32 plain value; Freeze: 0 or 1
  uint32_t  staged = 0;             // SpilloverLoad: its preset in Script::Staged()
};

class Script {
 public:
  void Param(int64_t frame, ParamId id, float value) { Add(frame, EventType::SetParam, id, value); }
  void Freeze(int64_t frame, bool on) {
    Add(frame, EventType::Freeze, ParamId::Mix, on ? 1.f : 0.f);
  }
  void Trigger(int64_t frame) { Add(frame, EventType::Trigger, ParamId::Mix, 0.f); }
  // A Spillover load of a complete preset, `params` over the defaults (profile §5.10).
  void Spillover(int64_t frame, ParamList params) {
    staged_.push_back(std::move(params));
    Add(frame, EventType::SpilloverLoad, ParamId::Mix, 0.f,
        static_cast<uint32_t>(staged_.size() - 1u));
  }

  // Sorted by (frame, seq).
  const std::vector<Event>& Events() const { return events_; }
  std::vector<Event>&       MutableEvents() { return events_; }
  const std::vector<ParamList>& Staged() const { return staged_; }
  std::vector<ParamList>&       MutableStaged() { return staged_; }

 private:
  void Add(int64_t frame, EventType type, ParamId id, float value, uint32_t staged = 0);

  std::vector<Event>     events_;
  std::vector<ParamList> staged_;
  uint32_t               seq_ = 0;
};

// A complete preset (companion §6.1): every leaf, `params` over the defaults.
std::unique_ptr<PresetState> CompletePreset(const ParamList& params);

// A script's Spillover presets, decoded for the render.
using StagedPresets = std::vector<std::unique_ptr<PresetState>>;

// How events reach the engine: as stamped events in ProcessContext (Engine, the
// contract's delivery), or through SetParam, SetFreeze, Trigger and LoadPreset with every
// block split at the events' frames (Split, what a wrapper does without engine-side
// events). Both must render the same bits.
enum class Delivery : uint8_t { Engine, Split };

// Walks a script block by block.
class EventCursor {
 public:
  explicit EventCursor(const std::vector<Event>& events) : events_(events) {}

  // The frame of the first event not yet taken that is stamped after `frame`, or
  // INT64_MAX.
  int64_t NextAfter(int64_t frame) const;
  // The first freeze or Spillover event not yet taken with a frame in (after, upTo], or
  // INT64_MAX.
  int64_t NextFreeze(int64_t after, int64_t upTo) const;
  // Takes the events stamped before `end` as [*first, *first + count) of the script.
  uint32_t Take(int64_t end, const Event** first);

 private:
  const std::vector<Event>& events_;
  size_t                    next_ = 0;
};

// Split delivery: applies one event through the unstamped API, which the next Process
// call applies at its first frame.
void ApplyUnstamped(Engine& engine, const Event& e, const StagedPresets& staged);
// Engine delivery: the event stamped with its offset in the block starting at blockStart.
Engine::BlockEvent ToBlockEvent(const Event& e, int64_t blockStart, const StagedPresets& staged);

}  // namespace brainscape::golden
