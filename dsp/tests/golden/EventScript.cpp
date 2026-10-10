#include "EventScript.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "brainscape/ModeEval.h"
#include "brainscape/TestSignal.h"

#if defined(BRAINSCAPE_GOLDEN_EMBEDDED_PACKAGES)
#include "EmbeddedPackages.h"
#endif

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

int64_t ClockTicks(Script& script, int64_t start, uint32_t nsPerQuarter, uint32_t count,
                   TickModel model, uint32_t seed, int64_t dropFrom, int64_t dropTo) {
  // The ideal tick i at start + floor(i·ns·48,000 / (24·10^9)), in exact integer steps.
  const uint64_t num = static_cast<uint64_t>(nsPerQuarter) * 48000u;
  const uint64_t den = 24000000000u;
  const uint64_t q = num / den, rem = num % den;
  auto ideal = [&](uint32_t i) {
    return start + static_cast<int64_t>(q * i + rem * i / den);  // rem·i < 2^64 for i < 2^29
  };
  int64_t last = 0;  // frames never fall below the script's latest tick
  for (const Event& e : script.Events()) {
    if (e.type == EventType::ClockTick && e.frame > last) last = e.frame;
  }
  for (uint32_t i = 0; i < count; ++i) {
    const int64_t at = ideal(i);
    if (at >= dropFrom && at < dropTo) continue;
    int64_t f = at;
    if (model == TickModel::Hardware) {
      f = (at + 47) / 48 * 48;  // at >= 0: the first block boundary at or after it
    } else if (model == TickModel::Computer) {
      int64_t j = 0;
      for (uint32_t k = 0; k < 4; ++k) {
        const uint32_t u = testsignal::SplitMix32(seed, 4u * i + k);
        j += static_cast<int64_t>(u % 283u) - 141;
      }
      f = at + j;
      if (i % 500u == 499u) f = at + 1843;  // a held tick, 38.4 ms
    }
    if (f < last) f = last;
    script.Tick(f);
    last = f;
  }
  return ideal(count);
}

int64_t TapSeries(Script& script, int64_t start, const std::vector<int64_t>& intervals,
                  uint32_t spread, uint32_t seed) {
  int64_t at = start;
  for (size_t k = 0; k <= intervals.size(); ++k) {
    if (k > 0) at += intervals[k - 1];
    int64_t f = at;
    if (spread > 0u) {
      const uint32_t u = testsignal::SplitMix32(seed, static_cast<uint32_t>(k));
      f += static_cast<int64_t>(u % (2u * spread + 1u)) - static_cast<int64_t>(spread);
    }
    script.Tap(f);
    if (k == intervals.size()) return f;
  }
  return at;
}

int64_t TempoSweep(Script& script, int64_t start, uint32_t fromNs, uint32_t toNs, uint32_t steps,
                   int64_t interval) {
  int64_t at = start;
  for (uint32_t i = 1; i <= steps; ++i, at += interval) {
    const int64_t span = static_cast<int64_t>(toNs) - static_cast<int64_t>(fromNs);  // |·| < 2^32
    script.Tempo(at, static_cast<uint32_t>(static_cast<int64_t>(fromNs) +
                                           span * static_cast<int64_t>(i) / static_cast<int64_t>(steps)));
  }
  return at;
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
#if defined(BRAINSCAPE_GOLDEN_EMBEDDED_PACKAGES)
  // The program's copy of presets/NAME.bsp (EmbeddedPackages.h): no file system.
  const std::string      path    = std::string("embedded ") + name + ".bsp";
  size_t                 count   = 0;
  const EmbeddedPackage* all     = EmbeddedPackages(&count);
  const EmbeddedPackage* package = nullptr;
  for (size_t i = 0; i < count && package == nullptr; ++i) {
    if (std::strcmp(all[i].name, name) == 0) package = &all[i];
  }
  if (package == nullptr) {
    std::fprintf(stderr, "the corpus package %s is not embedded (presets/MANIFEST)\n", name);
    return false;
  }
  const uint8_t* const bytes = package->bytes;
  const size_t         n     = package->size;
#else
  const std::string path = std::string(BRAINSCAPE_GOLDEN_PRESETS) + "/" + name + ".bsp";
  FILE*             f    = std::fopen(path.c_str(), "rb");
  if (f == nullptr) {
    std::fprintf(stderr, "cannot open the corpus package %s\n", path.c_str());
    return false;
  }
  std::vector<uint8_t> buffer(kMaxPackageBytes + 1u);
  const size_t         n     = std::fread(buffer.data(), 1, buffer.size(), f);
  const uint8_t* const bytes = buffer.data();
  std::fclose(f);
#endif
  PresetDiagnostic diag;
  if (!DecodePreset(bytes, n, out, &diag, info)) {
    std::fprintf(stderr, "the corpus package %s does not decode: %s (detail %u)\n", path.c_str(),
                 PresetErrorName(diag.error), static_cast<unsigned>(diag.detail));
    return false;
  }
  return true;
}

std::unique_ptr<PresetState> CompletePreset(const PresetSource& source, uint16_t strip,
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
  if ((strip & kStripSources) != 0) {
    preset->mode.schedule.sources = static_cast<uint8_t>(preset->mode.schedule.sources | kDefaultSources);
  }
  if ((strip & kStripPitchSet) != 0) {
    preset->mode.pitch[0]              = kDefaultPitchSet;
    preset->mode.layers[0].pitchSelect = PitchSelect::Cycle;
  }
  if ((strip & kStripPitchSelect) != 0) preset->mode.layers[0].pitchSelect = PitchSelect::Cycle;
  if ((strip & kStripClock) != 0) {
    preset->mode.schedule.sources = static_cast<uint8_t>(preset->mode.schedule.sources & ~kSourceClock);
  }
  if ((strip & kStripSubdiv) != 0) preset->performance.subdiv = Subdivision::Tap;
  if ((strip & kStripSync) != 0) {
    for (ModeLayer& layer : preset->mode.layers) layer.baseSync = 0;
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
    // No unstamped call (docs/design/clock.md §2.6): the render hands these to the block that
    // starts at their frame (IsTempoEvent).
    case EventType::Tap:
    case EventType::Tempo:
    case EventType::ClockTick:
    case EventType::Transport:
    case EventType::Subdivision: break;
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
