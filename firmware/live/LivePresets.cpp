#include "live/LivePresets.h"

#include <cstring>
#include <memory>
#include <vector>

#include "Corpus.h"
#include "EventScript.h"

namespace brainscape::fw::live {

namespace {

// Whether `mode` is the default mode but for the onset source, layer 0's position source and
// the feature bits they require (and modeHash, which the decoder sets). A comparison only:
// what the image plays comes from the packages as decoded.
bool DefaultButOnsetAndMarks(const ModeBlob& mode) noexcept {
  const ModeBlob def{};
  ModeBlob       m      = mode;
  m.features            = def.features;
  m.schedule.sources    = static_cast<uint8_t>((m.schedule.sources & ~kSourceOnset) |
                                               (def.schedule.sources & kSourceOnset));
  m.layers[0].source    = def.layers[0].source;
  m.modeHash            = def.modeHash;
  return std::memcmp(&m, &def, sizeof m) == 0;
}

}  // namespace

bool PlaysOnset(const ModeBlob& mode) noexcept { return (mode.schedule.sources & kSourceOnset) != 0; }

bool PlaysMarks(const ModeBlob& mode) noexcept {
  return mode.layers[0].source == PositionSource::Mark;
}

bool BuildSlots(PresetState* out, const char** why) {
  const std::vector<golden::VectorCase> corpus = golden::BuildCorpus();
  for (uint32_t s = 0; s < kNumSlots; ++s) {
    const golden::PresetCase* found = nullptr;
    for (const golden::VectorCase& v : corpus) {
      if (std::strcmp(v.name, kSlots[s].vector) != 0) continue;
      for (const golden::PresetCase& p : v.presets) {
        if (std::strcmp(p.name, kSlots[s].preset) == 0) found = &p;
      }
    }
    if (found == nullptr) {
      *why = "a live preset is missing from the golden corpus";
      return false;
    }
    const std::unique_ptr<PresetState> preset =
        golden::CompletePreset(golden::PresetSource{found->package, found->params});
    if (preset == nullptr) {
      *why = "a live preset's package is not in the image (presets/MANIFEST)";
      return false;
    }
    out[s] = *preset;
    if (!CheckPreset(out[s])) {
      *why = "a live preset does not pass CheckPreset";
      return false;
    }
  }
  return true;
}

bool BuildStructures(PresetState* out, const char** why) {
  const ControlState defaultControl{};
  for (uint32_t i = 0; i < kNumStructures; ++i) {
    const Structure& s = kStructures[i];
    out[i]             = PresetState{};  // the default mode and CTRL
    if (s.package != nullptr && !golden::LoadPackage(s.package, &out[i])) {
      *why = "a structure package is not in the image (presets/MANIFEST)";
      return false;
    }
    if (StructureIndex(s.onset, s.marks) != i || PlaysOnset(out[i].mode) != s.onset ||
        PlaysMarks(out[i].mode) != s.marks) {
      *why = "a structure package does not have the onset and mark structure its row names";
      return false;
    }
    if (!DefaultButOnsetAndMarks(out[i].mode) ||
        std::memcmp(&out[i].control, &defaultControl, sizeof defaultControl) != 0) {
      *why = "a structure package differs from the default mode or CTRL in more than onset and marks";
      return false;
    }
  }
  return true;
}

void WithStructure(PresetState* state, const PresetState& structure) noexcept {
  state->mode    = structure.mode;
  state->control = structure.control;
}

}  // namespace brainscape::fw::live
