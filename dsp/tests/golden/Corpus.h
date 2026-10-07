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
// 5: plucks_subnormal_6s (subnormal input) and the subnormalOutFrames counter.
// 6 (sound revision 2, mode-compiler.md §10.3): structure only from committed packages (the
// onset and mark presets; the automation preset's toggles of the retired rows 27 and 28 are
// Spillover loads between two modes, in both switch styles); post_max at 41 Hz; plucks_modes_14s
// (macro moves, expression, mode switches, the wet kill, a lone change per leaf); the macroMoves,
// expressionEvents, modeSwitches and killedFrames counters; the mode, macro, modeSwitch, fastCut
// and wetKill ablations and the amongEdits invariance.
// 7 (lane C's review): mode_switch's macro and expression moves after its loads, on switch
// packages each with its own macro table and CTRL (and switch_marks_ctrl, the same mode with
// another CTRL), its FastCuts on pitched grains; automation_offgrid holds each freeze, no load
// cutting it short; wet_kill's kills at mix 1 and the mutedFrames counter, the kill measured
// on the output.
inline constexpr uint32_t kCorpusVersion = 7;

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
  // Output frames with a subnormal sample on either channel: arithmetic under any flush
  // mode (x86 FTZ, Arm FZ) cannot produce one.
  SubnormalOutFrames,
  LastActiveFrame,    // last output frame with |out| > 2^-16, or -1
  LastNonzeroFrame,   // last output frame with a nonzero sample, or -1
  MacroMoves,         // MacroMove events applied
  ExpressionEvents,   // Expression events applied
  ModeSwitches,       // loads whose mode differed by content (Engine::ModeSwitches), after
                      // the render's first load
  KilledFrames,       // frames rendered with the cutoff target at its minimum, the wet kill,
                      // as the script sets it (the harness's model, not the output)
  // Output frames exactly ±0 on both channels while the input is nonzero on either: at mix
  // below 1 the dry signal shows, so only mix 1 over a killed wet path (or a wet path not yet
  // sounding) gives one. Bits only, so no FP mode can change it.
  MutedFrames,
  kCount
};
const char* CounterName(Counter) noexcept;

// A feature an ablation switches off: a parameter forced to its neutral value
// (and its script events dropped), or the freeze / trigger events dropped.
// RingLength keeps the preset and renders it on a ring twice as long: the ring
// reaches the output only through the re-anchor, mark staleness and the far guard
// (profile §6.4), so a change proves the render reached one of them. Spillover and
// Restart drop the script's Spillover loads or its restarts.
// Since sound revision 2 the onset trigger and mark positioning are structure (rows 27 and 28
// retired), so MarkPosition and OnsetTrigger switch structure off in every preset the render
// loads (Strip, EventScript.h); Mode loads the default mode and CTRL instead of each preset's;
// Macro drops the macro and expression moves; ModeSwitch makes every load keep the starting
// preset's mode; FastCut makes every FastCut load Trails; WetKill moves every cutoff at 40 Hz,
// the kill, to 41 Hz.
enum class Feature : uint8_t {
  MarkPosition, OnsetTrigger, Reverse, Pitch, Spray, Feedback,
  PostMod, PostDelay, PostReverb, PostFilter, Freeze, Triggers, RingLength,
  Spillover, Restart, Mode, Macro, ModeSwitch, FastCut, WetKill,
};
const char* FeatureName(Feature) noexcept;

// A perturbation that must NOT change the output (profile §6.4), checked by a second render.
//   HostileFpEnv  the render repeated on a fresh engine with the caller's control word
//                 hostile (x86 FTZ|DAZ, Arm FZ|DN, each with round-toward-zero) around
//                 every entry point, Init included, or clean if the run is hostile;
//   RestartTail   the output from the script's last restart on equals a render from the
//                 exact-restart state of the rest: the input from that frame, the
//                 parameter values and mode the restart kept or loaded, the later events;
//   AmongEdits    every SetParam event of the script among edits that rebuild every other
//                 domain without changing a value (another leaf of each domain set to
//                 another value and back at its frame) renders the same bits: a lone change
//                 reaches its rebuild (mode-compiler.md §7.2, R1).
enum class Invariance : uint8_t { HostileFpEnv, RestartTail, AmongEdits };
const char* InvarianceName(Invariance) noexcept;

struct Requirement {
  Counter counter;
  int64_t min;
  int64_t max = INT64_MAX;
};

struct PresetCase {
  const char*                            name;
  std::vector<std::pair<ParamId, float>> params;  // over the defaults (or the package's
                                                  // leaves): the Exact load
  Script                                 script;  // events and restarts during the render
  std::vector<Requirement>               require;
  std::vector<Feature>                   ablate;
  std::vector<Invariance>                invariant;
  // A committed package of the corpus (presets/NAME.json and NAME.bsp) the preset starts from:
  // its leaves, mode and CTRL (mode-compiler.md §10.3). Its sound and control hashes go into
  // the golden file, for the sound-revision gate's package rule (§8.3).
  const char*                            package = nullptr;
  uint8_t                                strip   = 0;  // ablations only: Strip bits
};

struct VectorCase {
  const char*        name;
  testsignal::Vector source;
  uint32_t           activeFrames;  // notes start and end inside [0, activeFrames)
  uint32_t           frames;        // render length; later frames are silent input
  bool               longRender;    // skipped by --quick
  std::vector<PresetCase> presets;
  // Every input sample the generator leaves at exactly zero is a subnormal instead,
  // ±k·2^-149 keyed on its frame and channel (profile §2.3 #4 allows them). Such a vector
  // is the one a flushing FP environment changes: it gives the hostile-environment check
  // and the forced-flush control something to see (profile §6.4).
  bool               subnormalInput = false;
};

std::vector<VectorCase> BuildCorpus();

// The preset with `f` switched off.
PresetCase Ablate(const PresetCase&, Feature f);

// The preset in force from the script's frame `frame` on, before the events stamped there: the
// package of the last load (whose mode the engine plays) with every leaf's value after the Exact
// load and every parameter event, macro or expression move, Spillover load and Exact load
// before it. Empty params when a package cannot be loaded.
PresetSource StateAt(const PresetCase& p, int64_t frame);

// What RestartTail renders: the preset from the script's last restart on, its events and
// restarts moved to that frame; *start receives the frame.
PresetCase TailAfterRestart(const PresetCase& p, int64_t* start);

// What AmongEdits renders: the preset with every SetParam event joined by edits that rebuild
// every other domain and change no value.
PresetCase AmongEdits(const PresetCase& p);

}  // namespace brainscape::golden
