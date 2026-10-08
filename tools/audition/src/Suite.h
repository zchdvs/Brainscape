#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Hash.h"
#include "Inputs.h"
#include "Metrics.h"
#include "Recipe.h"
#include "Render.h"

// The audition scripts and the objective pre-screen (docs/design/mode-compiler.md §11.3).
//
//   S0      stored positions: the preset on every test-signal vector (10 s, then 10 s of
//           silence), and at Mix 1 (the wet alone) on Plucks, Strums and SoftNotes
//   S1-S6   Activity, Repeats, Shape, Time, Space, Filter: each moved from its stored position to
//           0, to 1 and back over 16 s, one MacroMove per 48-frame block, the class input looped
//           for the whole script; beside them the stored preset on the same looped input, the
//           sweeps' reference (in S1). An attack mode's sweeps and reference are rendered on
//           SoftNotes too, for the Clicks check (kClickVector)
//   S7      Repeats at 0, 0.25, 0.5, 0.75 and 1 (the design's "at maximum"), each over the class
//           input and then 20 s of silence: the Repeats rule's level and tail
//   S8      freeze engaged at 4 s, released at 14 s, over the class input
//   S9      footswitch triggers every 0.5 s from 1.25 s to 8.75 s, over the class input
//   S10     a Spillover load at 5 s from the previous mode of the set (loaded Exact at frame 0),
//           with Trails and with FastCut
//   S11     the preset at every other set member's stored positions, and at the 16 corners of
//           Activity x Repeats x Shape x Time (the others stored), over the class input, and for
//           an attack mode over SoftNotes too (the Clicks part of Combinations)
//
// A script's positions are stored ones: each targeted leaf is EvalMacro at its position (the
// derive rule, §3.5), so a combination plays as if saved there. The class input is Plucks for an
// attack mode and SoftNotes for a pad mode (§11.3's input classes). A tail the class renders of
// S0, S7 and S11 do not see end is measured on a probe, the same request with 60 s of silence
// (Metrics.h, MeasureTail).
namespace bsa {

enum class InputClass : uint8_t { Attack, Pad };

// What a mode declares for its audition (§11.3): its input class, whether it is meant to
// self-oscillate (the Tail check), and whether it is documented as needing attacks (the Fallback
// check). Documents cannot carry them (an unknown key is an error), so the ratings log does.
struct Declarations {
  InputClass inputClass      = InputClass::Attack;
  bool       selfOscillating = false;
  bool       needsAttacks    = false;
};

struct Preset {
  PresetIdentity                           identity;  // identity.package false: leaf-only
  std::shared_ptr<const brainscape::PresetState> state;
  Declarations                             declare;
};

inline constexpr const char* kScriptNames[] = {"S0", "S1", "S2", "S3", "S4",  "S5",
                                               "S6", "S7", "S8", "S9", "S10", "S11"};
inline constexpr size_t      kScriptCount   = 12;
// The macro each sweep moves: S1 Activity ... S6 Filter.
inline constexpr brainscape::ParamId kSweepMacros[6] = {
    brainscape::ParamId::MacroActivity, brainscape::ParamId::MacroRepeats,
    brainscape::ParamId::MacroShape,    brainscape::ParamId::MacroTime,
    brainscape::ParamId::MacroSpace,    brainscape::ParamId::MacroFilter};

// Clicks are judged on SoftNotes, the smooth vector, whatever the class: a sample step shows
// there, while on Plucks the dry's own attacks and the wet's replays of them hide one (the dry
// plays at unity under the Mix law).
inline constexpr Vector   kClickVector      = Vector::SoftNotes;
inline constexpr uint32_t kSweepFrames      = 16 * kRate;
inline constexpr uint32_t kRepeatsTail      = 20 * kRate;
inline constexpr float    kRepeatsRungs[5]  = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
inline constexpr uint32_t kFreezeOn         = 4 * kRate, kFreezeOff = 14 * kRate;
inline constexpr uint32_t kLoadFrame        = 5 * kRate;
// The determinism check's second block pattern (any pattern must render the same bits).
inline const std::vector<uint32_t> kMixedPattern = {1, 37, 48, 441, 512, 96, 7};

enum class Role : uint8_t {
  Engaged,      // S0: the stored preset
  Wet,          // S0: the stored preset at Mix 1
  SweepRef,     // S1: the stored preset on the looped input, no events
  Sweep,        // S1-S6
  RepeatsRung,  // S7
  Freeze,       // S8
  Triggers,     // S9
  Load,         // S10
  Combination,  // S11
  Determinism,  // a repeat of another render: same request, or another block pattern
};

// One planned render.
struct Planned {
  std::string                     script, name;
  Role                            role = Role::Engaged;
  Vector                          vector = Vector::Plucks;
  bool                            looped = false;
  uint32_t                        signalFrames = kSignalFrames, tailFrames = kTailFrames;
  // Stored macro positions applied before loading, then leaf overrides.
  std::vector<std::pair<brainscape::ParamId, float>> positions;
  std::vector<std::pair<brainscape::ParamId, float>> leaves;
  std::vector<ScriptEvent>        events;
  std::string                     eventsDescription;
  std::string                     variant;  // what was changed, in words
  const Preset*                   from = nullptr;  // S10: loaded Exact at frame 0
  std::vector<uint32_t>           blockPattern{kPedalBlock};
  brainscape::ParamId             macro    = brainscape::ParamId::MacroActivity;
  float                           stored   = 0;  // the macro's stored position (sweeps)
  float                           position = 0;  // S7's rung
  std::string                     repeatOf;      // Determinism: the render it repeats
  bool                            writeWav = true;
};

// The renders of the scripts asked for (names from kScriptNames). `set` holds every preset of
// the set in order (this one included): S10 loads from the one before it (the last for the
// first; the default preset for a set of one) and S11 visits the others' positions. Macros the
// mode leaves undefined are skipped and listed in *skipped.
std::vector<Planned> Plan(const Preset& preset, const std::vector<const Preset*>& set,
                          const std::vector<std::string>& scripts,
                          std::vector<std::string>* skipped = nullptr);

// Activity's and Shape's response, measured by RunSuite from a class sweep's audio against the
// reference's at the same time (the pre-screen judges it): the brightness and the envelope's
// variation from position 0 to 1, and for Activity the onsets the engine's own detector hears per
// second of the rising leg, the sweep's and the reference's.
struct Response {
  bool                  measured      = false;
  double                brightnessPct = 0;
  double                envelopeDb    = 0;
  std::vector<uint32_t> heard, refHeard;
};

struct Rendered {
  Planned      plan;
  RenderHashes hashes;
  Metrics      metrics;
  Response     response;
  bool         ok = false;
  bool         exact = false;
  uint64_t     onsets = 0;
  std::string  error;
  std::string  wavPath, recipePath;  // relative to the preset's directory, or empty
};

struct Check {
  std::string name;
  std::string verdict;  // "pass", "FAIL", "info", "n/a", "declared"
  std::string detail;
};

struct SuiteResult {
  std::vector<Rendered>    renders;
  std::vector<Check>       checks;
  std::vector<std::string> skipped;
  bool                     ok = false;  // every render rendered
  bool                     cancelled = false;  // SuiteOptions::cancel stopped it
  bool                     Failed() const;  // a render failed or a check says FAIL
};

struct SuiteOptions {
  std::vector<std::string> scripts{"S0"};
  bool                     metrics   = false;  // the pre-screen
  bool                     wav       = true;   // 16-bit WAVs (S11: only the worst cases)
  size_t                   worstWavs = 3;      // S11's worst cases written per measure
  std::string              outDir;             // the preset's renders go to outDir/<id>/
  // Read between renders and inside each (RenderRequest::cancel): once true, the suite stops,
  // writes nothing more and returns cancelled.
  const std::atomic<bool>* cancel = nullptr;
};

// Renders the plan, hashes and measures every render, runs the pre-screen when asked, and writes
// the WAVs, a recipe per render and the preset's index, outDir/<id>/audition.json.
SuiteResult RunSuite(Renderer& renderer, const Preset& preset,
                     const std::vector<const Preset*>& set, const SuiteOptions& options);

// The pre-screen over a suite's renders (exposed for tests).
std::vector<Check> PreScreen(const Preset& preset, const std::vector<Rendered>& renders);

// The preset's directory name under the output directory.
std::string PresetDir(const Preset& preset);
// A one-screen text summary of a suite.
std::string Summary(const Preset& preset, const SuiteResult& r);
// The dry input's integrated loudness over its 10 s, what bypass plays: the levels' reference.
double DryLevel(Vector v);
// Whether the mode fires on onsets or reads marks (the Fallback check's onset modes).
bool OnsetMode(const brainscape::PresetState& state);
const char* InputClassName(InputClass c);
Vector      ClassVector(InputClass c);

}  // namespace bsa
