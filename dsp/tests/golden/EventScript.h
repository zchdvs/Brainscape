#pragma once
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "brainscape/Engine.h"
#include "brainscape/EventQueue.h"

// Frame-stamped event scripts (docs/design/determinism-profile.md §5.11): the engine's
// own events, (absolute frame, sequence number, type, id, value). Frames before an
// event's stamp use the old state, the event applies from its frame, and same-frame
// events apply in sequence order. Besides events, a script can restart the engine
// mid-render (§5.8), which Process cannot do: a restart runs with Process stopped.
namespace brainscape::golden {

using ParamList = std::vector<std::pair<ParamId, float>>;
using Event     = Engine::Event;
using EventType = Engine::EventType;

// A restart at `frame`, before the events stamped there: Engine::Restart, which keeps the
// parameter values, or with `load` a LoadPreset(..., Exact) of a staged preset. Either
// begins a new engine timeline, whose frame 0 is `frame`.
struct RestartPoint {
  int64_t  frame  = 0;
  bool     load   = false;
  uint32_t staged = 0;  // load: its preset in Script::Staged()
};

class Script {
 public:
  void Param(int64_t frame, ParamId id, float value) {
    Add(frame, EventType::SetParam, static_cast<uint32_t>(id), value);
  }
  void Freeze(int64_t frame, bool on) { Add(frame, EventType::Freeze, 0, on ? 1.f : 0.f); }
  void Trigger(int64_t frame) {
    Add(frame, EventType::Trigger, static_cast<uint32_t>(Engine::TriggerSource::Footswitch), 1.f);
  }
  // A Spillover load of a complete preset, `params` over the defaults (profile §5.10).
  void Spillover(int64_t frame, ParamList params) {
    Add(frame, EventType::SpilloverLoad, Stage(std::move(params)), 0.f);
  }
  void Restart(int64_t frame) { AddRestart({frame, false, 0}); }
  void ExactLoad(int64_t frame, ParamList params) {
    AddRestart({frame, true, Stage(std::move(params))});
  }

  // Sorted by (frame, seq); frames are the render's. A SpilloverLoad event carries its
  // staged preset's index in `id` and no `preset`: the render binds the decoded preset.
  const std::vector<Event>&        Events() const { return events_; }
  std::vector<Event>&              MutableEvents() { return events_; }
  const std::vector<RestartPoint>& Restarts() const { return restarts_; }
  std::vector<RestartPoint>&       MutableRestarts() { return restarts_; }
  const std::vector<ParamList>&    Staged() const { return staged_; }
  std::vector<ParamList>&          MutableStaged() { return staged_; }

 private:
  void     Add(int64_t frame, EventType type, uint32_t id, float value);
  void     AddRestart(const RestartPoint& r);
  uint32_t Stage(ParamList params) {
    staged_.push_back(std::move(params));
    return static_cast<uint32_t>(staged_.size() - 1u);
  }

  std::vector<Event>        events_;
  std::vector<RestartPoint> restarts_;  // sorted by frame
  std::vector<ParamList>    staged_;
  uint32_t                  seq_ = 0;
};

// A complete preset (companion §6.1): every leaf, `params` over the defaults.
std::unique_ptr<PresetState> CompletePreset(const ParamList& params);

// A script's staged presets, decoded for the render.
using StagedPresets = std::vector<std::unique_ptr<PresetState>>;

// How events reach the engine: through the engine's transport (Engine, the contract's
// delivery: each block's events go into an EventQueue stamped with engine frames, and
// Process takes PopBlock's offsets), or through SetParam, SetFreeze, Trigger and
// LoadPreset with every block split at the events' frames (Split, what a wrapper does
// without engine-side events). Both must render the same bits.
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
// Engine delivery: the event on the engine timeline that began at render frame `base`,
// with a SpilloverLoad's staged preset bound.
Event ToEngineEvent(const Event& e, int64_t base, const StagedPresets& staged);

}  // namespace brainscape::golden
