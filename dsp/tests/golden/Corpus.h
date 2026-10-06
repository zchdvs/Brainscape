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
// feature fails the corpus, whatever its hash; so does one whose output an invariance
// check changes.
namespace brainscape::golden {

// Bump when any vector, preset, script or requirement changes. 2: automation_offgrid's
// DelayFb events hold the canonical 0.9f, not 9 * 0.1f (SetParam stored 0.9f either way).
// 3: strums_16s/freeze_retoggle_spill (same-frame freeze events, Spillover loads).
// 4: plucks_state_14s (Spillover loads, a Restart and an Exact load mid-render), the
// loads and restarts counters, and the invariance checks.
inline constexpr uint32_t kCorpusVersion = 4;

enum class Counter : uint8_t {
  Frames,             // frames rendered
  Events,             // script events applied
  OffGridEvents,      // ... whose frame is not a multiple of 48 (the pedal's block grid)
  Triggers,           // footswitch triggers
  Loads,              // Spillover loads
  Restarts,           // restarts mid-render (Restart or an Exact load)
  Onsets,             // ConsumeOnsetCount() total
  FrozenOnsets,       // onsets detected in blocks rendered while frozen
  FrozenFrames,       // frames rendered while frozen
  FreezeEngages,
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
// RingLength keeps the preset and renders it on a ring twice as long: the ring
// reaches the output only through the re-anchor, mark staleness and the far guard
// (profile §6.4), so a change proves the render reached one of them. Spillover and
// Restart drop the script's Spillover loads or its restarts.
enum class Feature : uint8_t {
  MarkPosition, OnsetTrigger, Reverse, Pitch, Spray, Feedback,
  PostMod, PostDelay, PostReverb, PostFilter, Freeze, Triggers, RingLength,
  Spillover, Restart,
};
const char* FeatureName(Feature) noexcept;

// A perturbation that must NOT change the output (profile §6.4), checked by a second render.
//   HostileFpEnv  the render repeated on a fresh engine with the caller's control word
//                 hostile (x86 FTZ|DAZ, Arm FZ|DN, each with round-toward-zero) around
//                 every entry point, Init included, or clean if the run is hostile;
//   RestartTail   the output from the script's last restart on equals a render from the
//                 exact-restart state of the rest: the input from that frame, the
//                 parameter values the restart kept or loaded, the later events.
enum class Invariance : uint8_t { HostileFpEnv, RestartTail };
const char* InvarianceName(Invariance) noexcept;

struct Requirement {
  Counter counter;
  int64_t min;
  int64_t max = INT64_MAX;
};

struct PresetCase {
  const char*                            name;
  std::vector<std::pair<ParamId, float>> params;  // over the defaults: the Exact load
  Script                                 script;  // events and restarts during the render
  std::vector<Requirement>               require;
  std::vector<Feature>                   ablate;
  std::vector<Invariance>                invariant;
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

// The parameter values of `p` from the script's frame `frame` on, before the events stamped
// there: the Exact load, then every parameter event, Spillover load and Exact load before it.
ParamList ParamsAt(const PresetCase& p, int64_t frame);

// What RestartTail renders: the preset from the script's last restart on, its events and
// restarts moved to that frame; *start receives the frame.
PresetCase TailAfterRestart(const PresetCase& p, int64_t* start);

}  // namespace brainscape::golden
