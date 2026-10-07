#include "EventScript.h"

#include <algorithm>

namespace brainscape::golden {

void Script::Add(int64_t frame, EventType type, uint32_t id, float value) {
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

void Script::AddRestart(const RestartPoint& r) {
  const auto at = std::upper_bound(restarts_.begin(), restarts_.end(), r.frame,
                                   [](int64_t f, const RestartPoint& p) { return f < p.frame; });
  restarts_.insert(at, r);
}

std::unique_ptr<PresetState> CompletePreset(const ParamList& params) {
  auto preset = std::make_unique<PresetState>();
  for (uint32_t i = 0; i < kNumLeafParams; ++i) {
    preset->leaves[i] = {static_cast<uint32_t>(LeafId(i)), FindParam(LeafId(i))->def};
  }
  preset->leafCount = static_cast<uint32_t>(kNumLeafParams);
  for (const auto& kv : params) {
    const size_t i = LeafIndex(kv.first);
    if (i < kNumLeafParams) {
      preset->leaves[i].value = kv.second;
    } else if (preset->leafCount < PresetState::kMaxLeaves) {
      // Not a Leaf row: kept, so the load reports it unknown and the harness refuses it.
      preset->leaves[preset->leafCount++] = {static_cast<uint32_t>(kv.first), kv.second};
    }
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
    case EventType::SetParam: engine.SetParam(static_cast<ParamId>(e.id), e.value); break;
    case EventType::Freeze: engine.SetFreeze(e.value != 0.f); break;
    case EventType::Trigger:
      engine.Trigger(static_cast<Engine::TriggerSource>(e.id), e.value);
      break;
    case EventType::SpilloverLoad:
      engine.LoadPreset(*staged[e.id], LoadMode::Spillover);
      break;
  }
}

Event ToEngineEvent(const Event& e, int64_t base, const StagedPresets& staged) {
  Event out = e;
  out.frame = e.frame - base;
  if (e.type == EventType::SpilloverLoad) {
    out.id     = 0;
    out.preset = staged[e.id].get();
  }
  return out;
}

}  // namespace brainscape::golden
