#include "EventScript.h"

#include <algorithm>
#include <cstdio>
#include <string>

#include "brainscape/ModeEval.h"

#ifndef BRAINSCAPE_GOLDEN_PRESETS
#define BRAINSCAPE_GOLDEN_PRESETS "presets"
#endif

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

bool LoadPackage(const char* name, PresetState* out, PackageInfo* info) {
  const std::string path = std::string(BRAINSCAPE_GOLDEN_PRESETS) + "/" + name + ".bsp";
  FILE*             f    = std::fopen(path.c_str(), "rb");
  if (f == nullptr) {
    std::fprintf(stderr, "cannot open the corpus package %s\n", path.c_str());
    return false;
  }
  std::vector<uint8_t> bytes(kMaxPackageBytes + 1u);
  const size_t         n = std::fread(bytes.data(), 1, bytes.size(), f);
  std::fclose(f);
  PresetDiagnostic diag;
  if (!DecodePreset(bytes.data(), n, out, &diag, info)) {
    std::fprintf(stderr, "the corpus package %s does not decode: %s (detail %u)\n", path.c_str(),
                 PresetErrorName(diag.error), static_cast<unsigned>(diag.detail));
    return false;
  }
  return true;
}

std::unique_ptr<PresetState> CompletePreset(const PresetSource& source, uint8_t strip,
                                            const PresetState* modeFrom, PackageInfo* info) {
  std::unique_ptr<PresetState> preset;
  if (source.package == nullptr) {
    preset = CompletePreset(source.params);
  } else {
    preset = std::make_unique<PresetState>();
    if (!LoadPackage(source.package, preset.get(), info)) return nullptr;
    for (const auto& kv : source.params) {  // over the package's leaves, ascending by id
      bool found = false;
      for (uint32_t i = 0; i < preset->leafCount; ++i) {
        if (preset->leaves[i].id == static_cast<uint32_t>(kv.first)) {
          preset->leaves[i].value = kv.second;
          found                   = true;
        }
      }
      if (!found) return nullptr;  // a package writes every leaf of this build
    }
  }
  // Ablations only (Corpus.h): structure switched off, then the features it now requires.
  if (strip == 0 && modeFrom == nullptr) return preset;
  if ((strip & kStripMode) != 0) {
    preset->mode    = ModeBlob{};
    preset->control = ControlState{};
  } else if (modeFrom != nullptr) {
    preset->mode    = modeFrom->mode;
    preset->control = modeFrom->control;
  }
  if ((strip & kStripOnset) != 0) {
    preset->mode.schedule.sources = static_cast<uint8_t>(preset->mode.schedule.sources & ~kSourceOnset);
  }
  if ((strip & kStripMark) != 0 && preset->mode.layers[0].source == PositionSource::Mark) {
    preset->mode.layers[0].source = PositionSource::Live;
  }
  preset->mode.features = RequiredModeFeatures(preset->mode);
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

void ApplyUnstamped(Engine& engine, const Event& e, const StagedPresets& staged,
                    const PresetState& active) {
  PresetLeaf out[kMaxExpressions * kMaxMacroTargets];
  size_t     n = 0;
  switch (e.type) {
    case EventType::SetParam: engine.SetParam(static_cast<ParamId>(e.id), e.value); break;
    case EventType::Freeze: engine.SetFreeze(e.value != 0.f); break;
    case EventType::Trigger:
      engine.Trigger(static_cast<Engine::TriggerSource>(e.id), e.value);
      break;
    case EventType::SpilloverLoad:
      engine.LoadPreset(*staged[e.id].state, LoadMode::Spillover, nullptr, staged[e.id].style);
      break;
    case EventType::MacroMove:
      n = EvalMacro(active.mode, static_cast<ParamId>(e.id), e.value, out, kMaxMacroTargets);
      break;
    case EventType::Expression:
      n = EvalExpression(active.mode, active.control, e.value, out,
                         kMaxExpressions * kMaxMacroTargets);
      break;
  }
  for (size_t i = 0; i < n; ++i) engine.SetParam(static_cast<ParamId>(out[i].id), out[i].value);
}

Event ToEngineEvent(const Event& e, int64_t base, const StagedPresets& staged) {
  Event out = e;
  out.frame = e.frame - base;
  if (e.type == EventType::SpilloverLoad) {
    out.id     = static_cast<uint32_t>(staged[e.id].style);
    out.preset = staged[e.id].state.get();
  }
  return out;
}

}  // namespace brainscape::golden
