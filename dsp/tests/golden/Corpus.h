#pragma once
#include <cstdint>
#include <utility>
#include <vector>

#include "EventScript.h"
#include "brainscape/Params.h"
#include "brainscape/TestSignal.h"

// The golden corpus (docs/design/determinism-profile.md §6.1): generator vectors,
// each rendered through presets with event scripts. Every preset states the
// coverage it must show: counter floors and ceilings, and ablations (the feature
// switched off must change the output). A preset whose render does not exercise its
// feature fails the corpus, whatever its hash.
namespace brainscape::golden {

// Bump when any vector, preset, script or requirement changes.
inline constexpr uint32_t kCorpusVersion = 1;

enum class Counter : uint8_t {
  Frames,             // frames rendered
  Events,             // script events applied
  OffGridEvents,      // ... whose frame is not a multiple of 48 (the pedal's block grid)
  Triggers,           // footswitch triggers
  Onsets,             // ConsumeOnsetCount() total
  FrozenOnsets,       // onsets detected in blocks rendered while frozen
  FrozenFrames,       // frames rendered while frozen
  FreezeEngages,
  Reanchors,          // frozen holds crossing 3/4 of the ring past the pin (from the script)
  MarksAgedPastRing,  // 1 if frames after the last onset's block exceed the ring
  FbAbove1Frames,     // frames rendered with the Feedback target above 1
  InClipFrames,       // input frames at full scale on either channel
  SilentInFrames,     // input frames exactly zero on both channels
  OutActiveFrames,    // output frames with |out| > 2^-16 on either channel
  TailActiveFrames,   // ... among the silent-input frames
  LastActiveFrame,    // last output frame with |out| > 2^-16, or -1
  LastNonzeroFrame,   // last output frame with a nonzero sample, or -1
  kCount
};
const char* CounterName(Counter) noexcept;

// A feature an ablation switches off: a parameter forced to its neutral value
// (and its script events dropped), or the freeze / trigger events dropped.
enum class Feature : uint8_t {
  MarkPosition, OnsetTrigger, Reverse, Pitch, Spray, Feedback,
  PostMod, PostDelay, PostReverb, PostFilter, Freeze, Triggers,
};
const char* FeatureName(Feature) noexcept;

struct Requirement {
  Counter counter;
  int64_t min;
  int64_t max = INT64_MAX;
};

struct PresetCase {
  const char*                            name;
  std::vector<std::pair<ParamId, float>> params;  // the Exact load: set, then Reset()
  Script                                 script;  // events during the render
  std::vector<Requirement>               require;
  std::vector<Feature>                   ablate;
};

struct VectorCase {
  const char*        name;
  testsignal::Vector source;
  uint32_t           activeFrames;  // notes start and end inside [0, activeFrames)
  uint32_t           frames;        // render length; later frames are silent input
  bool               longRender;    // skipped by --quick
  std::vector<PresetCase> presets;
};

std::vector<VectorCase> BuildCorpus();

// The preset with `f` switched off.
PresetCase Ablate(const PresetCase&, Feature f);

}  // namespace brainscape::golden
