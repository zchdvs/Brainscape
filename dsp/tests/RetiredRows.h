#pragma once
#include <memory>
#include <utility>
#include <vector>

#include "brainscape/Engine.h"
#include "brainscape/Mode.h"
#include "brainscape/Params.h"
#include "brainscape/PresetState.h"

// Sound revision 1's parameters 27 (the onset trigger) and 28 (mark positioning), retired at
// sound revision 2 into mode structure: `onset` in scheduler.sources and layer 0's
// position.source (docs/design/mode-compiler.md §4.2, §7.3). A SetParam on either is now a no-op
// and a preset leaf naming either is unknown. The unit tests' parameter lists still name them,
// at 0.5 or above for on, meaning that structure: these helpers put it in a preset's mode, or load
// it into an engine before the list's SetParams. (The golden corpus takes structure only from
// compiled packages, §10.3.)
namespace brainscape::testing {

inline bool IsRetiredStructure(ParamId id) {
  return id == ParamId::OnsetTrigger || id == ParamId::PositionSource;
}

// The structure a retired row's value stands for, set in `mode` (features recomputed).
inline void SetRetiredStructure(ModeBlob* mode, ParamId id, float value) {
  const bool on = value >= 0.5f;
  if (id == ParamId::OnsetTrigger) {
    mode->schedule.sources = static_cast<uint8_t>(
        on ? mode->schedule.sources | kSourceOnset : mode->schedule.sources & ~kSourceOnset);
  } else if (id == ParamId::PositionSource) {
    mode->layers[0].source = on ? PositionSource::Mark : PositionSource::Live;
  }
  mode->features = RequiredModeFeatures(*mode);
}

// The default leaves with the structure the list's retired rows stand for, or null when they
// stand for the default structure.
inline std::unique_ptr<PresetState> RetiredStructurePreset(
    const std::vector<std::pair<ParamId, float>>& params) {
  auto preset = std::make_unique<PresetState>();
  bool any    = false;
  for (const auto& p : params) {
    if (!IsRetiredStructure(p.first)) continue;
    SetRetiredStructure(&preset->mode, p.first, p.second);
    any = true;
  }
  if (!any || RequiredModeFeatures(preset->mode) == 0u) return nullptr;
  for (size_t i = 0; i < kNumLeafParams; ++i) {
    preset->leaves[i] = {static_cast<uint32_t>(LeafId(i)), FindParam(LeafId(i))->def};
  }
  preset->leafCount = static_cast<uint32_t>(kNumLeafParams);
  return preset;
}

// On an engine fresh from Init: the structure the list's retired rows stand for, by an Exact
// load of the default leaves with that mode, which changes nothing else there. A harness then
// sets the list's values as it did at sound revision 1 (SetParam on a retired row is a no-op).
// False when the load was not exact.
inline bool LoadRetiredStructure(Engine& engine,
                                 const std::vector<std::pair<ParamId, float>>& params) {
  const std::unique_ptr<PresetState> preset = RetiredStructurePreset(params);
  return preset == nullptr || engine.LoadPreset(*preset, LoadMode::Exact);
}

}  // namespace brainscape::testing
