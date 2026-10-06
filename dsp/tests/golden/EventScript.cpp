#include "EventScript.h"

#include <algorithm>

namespace brainscape::golden {

void Script::Add(int64_t frame, EventType type, ParamId id, float value, uint32_t staged) {
  Event ev;
  ev.frame  = frame;
  ev.seq    = seq_++;
  ev.type   = type;
  ev.id     = id;
  ev.value  = value;
  ev.staged = staged;
  // Sequence numbers rise with insertion, so inserting after every event at or
  // before `frame` keeps the (frame, seq) order.
  const auto at = std::upper_bound(events_.begin(), events_.end(), frame,
                                   [](int64_t f, const Event& e) { return f < e.frame; });
  events_.insert(at, ev);
}

std::unique_ptr<PresetState> CompletePreset(const ParamList& params) {
  auto preset = std::make_unique<PresetState>();
  for (uint32_t i = 0; i < kNumParams; ++i) {
    preset->leaves[i] = {static_cast<uint32_t>(kParamTable[i].id), kParamTable[i].def};
  }
  preset->leafCount = static_cast<uint32_t>(kNumParams);
  for (const auto& kv : params) {
    preset->leaves[static_cast<uint32_t>(kv.first) - 1u].value = kv.second;
  }
  return preset;
}

int64_t EventCursor::NextAfter(int64_t frame) const {
  for (size_t i = next_; i < events_.size(); ++i) {
    if (events_[i].frame > frame) return events_[i].frame;
  }
  return INT64_MAX;
}

int64_t EventCursor::NextFreeze(int64_t after, int64_t upTo) const {
  for (size_t i = next_; i < events_.size() && events_[i].frame <= upTo; ++i) {
    const bool toggles =
        events_[i].type == EventType::Freeze || events_[i].type == EventType::SpilloverLoad;
    if (toggles && events_[i].frame > after) return events_[i].frame;
  }
  return INT64_MAX;
}

uint32_t EventCursor::Take(int64_t end, const Event** first) {
  const size_t begin = next_;
  while (next_ < events_.size() && events_[next_].frame < end) ++next_;
  *first = events_.data() + begin;
  return static_cast<uint32_t>(next_ - begin);
}

void ApplyUnstamped(Engine& engine, const Event& e, const StagedPresets& staged) {
  switch (e.type) {
    case EventType::SetParam: engine.SetParam(e.id, e.value); break;
    case EventType::Freeze: engine.SetFreeze(e.value != 0.f); break;
    case EventType::Trigger: engine.Trigger(); break;
    case EventType::SpilloverLoad:
      engine.LoadPreset(*staged[e.staged], LoadMode::Spillover);
      break;
  }
}

Engine::BlockEvent ToBlockEvent(const Event& e, int64_t blockStart, const StagedPresets& staged) {
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
    case EventType::SpilloverLoad:
      b.type   = Engine::EventType::SpilloverLoad;
      b.preset = staged[e.staged].get();
      break;
  }
  b.value = e.type == EventType::Trigger ? 1.f : e.value;
  return b;
}

}  // namespace brainscape::golden
