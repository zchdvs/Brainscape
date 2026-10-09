#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "Hash.h"
#include "Metrics.h"
#include "Render.h"

// A render's recipe (docs/design/companion-app.md §4.9): what produced it, so it can be made
// again, and its hashes. One JSON document per render, written beside its WAV by the app's
// audition and by `bspc render` alike ("brainscape-audition/2").
namespace bsa {

inline constexpr const char* kRecipeFormat = "brainscape-audition/2";

// Where a preset came from: a package's identity, or nothing for a leaf-only preset.
struct PresetIdentity {
  bool        package = false;  // from a document or package (else leaf-only: the default mode)
  std::string id, name, family, source;
  uint32_t    soundRev = 0;
  std::string soundHash, controlHash, packageHash;  // 64 hex digits each
};

struct RecipeInput {
  std::string                    script, name;  // the audition script and render, or empty
  const brainscape::PresetState* preset = nullptr;  // as loaded: its leaves and positions
  PresetIdentity                 identity;
  // What the script changed in the stored preset before loading it, in words, or empty.
  std::string                    variant;
  // The input, before conditioning.
  std::string                    inputDescription;
  std::string                    inputVector;      // the test-signal vector, or empty
  uint32_t                       inputSignalFrames = 0;
  double                         inputSourceRate   = kRate;
  bool                           inputConverted    = false;  // by platform code: this machine only
  InputMode                      mode              = InputMode::Stereo;
  std::string                    conditionedInputSha256;
  std::vector<uint32_t>          blockPattern{kPedalBlock};
  const std::vector<ScriptEvent>* events = nullptr;
  std::string                    eventsDescription;
  std::vector<PresetIdentity>    staged;            // SpilloverLoad presets, by index
  const RenderResult*            result = nullptr;  // the load report, onsets and output
  RenderHashes                   hashes;
  std::string                    wavPath;           // relative, or empty when not written
  uint16_t                       wavBits = 0;
  std::string                    wavSha256;
  const Metrics*                 metrics = nullptr;
};

std::string RecipeJson(const RecipeInput& in);

// SHA-256 of a script's events in a fixed binary form (frame, type, id, value bits, staged index;
// little-endian), so a recipe can name a long script without listing it.
std::string EventsSha256(const std::vector<ScriptEvent>& events);
const char* EventTypeName(brainscape::Engine::EventType t);

}  // namespace bsa
