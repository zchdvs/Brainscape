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
// 8 (sound revision 3, the Mix law): subnormal_wet at Mix 0.75, where the law scales its dry
// path by 0.5 as the linear crossfade did at 0.5; under the law Mix 0.5 plays the dry at unity.
// 9 (sound revision 4, wave 1's trigger sources, bursts and intermittency, mode-compiler.md
// §7.5 R9): plucks_wave1_12s (onsets alone, footswitch and MIDI triggers gated by the mode's
// sources, bursts at spacing 0 and 120 ms, intermittency on triggers and periodic births); the
// births, burstBirths and skips counters; the sources, burst and intermittency ablations; MIDI
// triggers in scripts.
// 10 (sound revision 5, wave 1's pitch sets, R10): pitch_cycle (a cycled set under transpose
// moves) and pitch_random (a weighted random set, mode switches to and from a cycled one) in
// plucks_wave1_12s; the pitchSet and pitchSelect ablations.
// 11 (sound revision 6, wave 1's repeat and decay, R11): repeat_loops (micro-loops of 4 passes
// with decay over a cycled set, repeat and decay moved alone) and decay_marks (mark positioning
// fading as its mark ages) in plucks_wave1_12s, and repeat_mark_aging in plucks_markage_92s (16
// reverse passes of 500 ms on an aging mark, on the life's far rail from about 79 s); the
// repeatPasses counter; the repeat and decay ablations.
// 12 (sound revision 7, wave 1's voice count, R12): voice_limit (a cloud held to 6 voices, then
// 2, then 64, onset bursts stealing) and mono_stutter (onsets alone, one voice, spaced bursts
// cutting each other) in plucks_wave1_12s; the steals counter; the voiceCount ablation.
// 13 (sound revision 8, the tempo core, docs/design/clock.md §8.3): plucks_clock_30s with
// clock_internal, clock_tap, clock_midi_hw, clock_midi_computer, clock_song, clock_loads and
// tempo_jump, every clock preset at 140 or 137.5 BPM (fractional frames per tick); the script
// verbs Tap, Tempo, Tick, Transport and Subdivision, the ClockTicks and TapSeries generators;
// the tempo counters (Engine::TempoCounts); the clock, tempoEvents and subdiv ablations; a
// restart's tail carries the device settings set before it.
inline constexpr uint32_t kCorpusVersion = 13;

enum class Counter : uint8_t {
  Frames,             // frames rendered
  Events,             // script events applied
  OffGridEvents,      // ... whose frame is not a multiple of 48 (the pedal's block grid)
  Triggers,           // trigger events, of any source (whether the mode lists it or not)
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
  // The grain scheduler's counts over the render (Engine::Stats, from sound revision 4): grains
  // born from every source, the second and later grains of bursts, and the periodic births and
  // triggers intermittency skipped (mode-compiler.md §7.5).
  Births,
  BurstBirths,
  Skips,
  // Passes begun after a voice's first: repeat voices re-reading their region (Engine::Stats,
  // from sound revision 6, mode-compiler.md §7.5 R11).
  RepeatPasses,
  // Triggered grains that took a sounding voice, the oldest, at voice_count or with all 64
  // busy (Engine::Stats, from sound revision 7, mode-compiler.md §7.5 R12).
  Steals,
  // The tempo core's counts over the render (Engine::TempoCounts, from sound revision 8,
  // docs/design/clock.md §2.6, §8.3): valid Tap events, those ignored (under ClockRunning, or a
  // bounce) and the downbeat marks under ClockFree; valid ClockTicks, outliers, re-acquisitions
  // and inferred lost ticks; gaps, losses (gaps under a clock) and resumes; Transport and
  // Subdivision events applied; CLOCK births; commits by the clock rules (acquisition and the
  // deadband), early commits; changes of the committed tempo classed Jump and Drift (slews);
  // the post chain's crossfades and folds, which read 0 until synced times (§11.3); and events
  // 6-10 ignored for an invalid payload.
  Taps,
  TapsIgnored,
  TapPhases,
  ClockTicks,
  TickOutliers,
  Reacquires,
  DropoutTicks,
  ClockGaps,
  ClockLosses,
  ClockResumes,
  TransportEvents,
  SubdivEvents,
  ClockBirths,
  Commits,
  EarlyCommits,
  Jumps,
  Slews,
  Crossfades,
  Folds,
  InvalidEvents,
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
// the kill, to 41 Hz. Wave 1 (sound revision 4): Sources gives every loaded mode the default
// sources back (periodic, footswitch and midi_note, beside its own), Burst sets
// scheduler.burst.count to 1 and Intermittency scheduler.intermittency to 0. Sound revision 5:
// PitchSet gives every loaded mode the default set {0: 1} by `cycle`, PitchSelect makes its
// `random` selection `cycle`. Sound revision 6: Repeat sets layer0.position.repeat to 1 and
// Decay layer0.decay_ms to 0. Sound revision 7: VoiceCount sets layer0.voice_count to 64.
// Sound revision 8 (docs/design/clock.md §8.3): Clock removes the `clock` source from every
// loaded mode, TempoEvents drops events 6-10, and Subdiv plays TAP throughout (every loaded
// preset's stored subdivision TAP, the Subdivision events of field 0 dropped).
enum class Feature : uint8_t {
  MarkPosition, OnsetTrigger, Reverse, Pitch, Spray, Feedback,
  PostMod, PostDelay, PostReverb, PostFilter, Freeze, Triggers, RingLength,
  Spillover, Restart, Mode, Macro, ModeSwitch, FastCut, WetKill,
  Sources, Burst, Intermittency, PitchSet, PitchSelect, Repeat, Decay, VoiceCount,
  Clock, TempoEvents, Subdiv,
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
  uint16_t                               strip   = 0;  // ablations only: Strip bits
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
// restarts moved to that frame, the device settings (Global rows) the script set before it set
// again at its frame 0, as a restart keeps them; *start receives the frame.
PresetCase TailAfterRestart(const PresetCase& p, int64_t* start);

// What AmongEdits renders: the preset with every SetParam event joined by edits that rebuild
// every other domain and change no value.
PresetCase AmongEdits(const PresetCase& p);

}  // namespace brainscape::golden
