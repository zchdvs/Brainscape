#pragma once
#include <cstddef>
#include <cstdint>

#include "brainscape/FpProfile.h"
#include "brainscape/Mode.h"

namespace brainscape {

// One stored leaf of a preset: a permanent parameter id and its exact binary32 plain value
// (docs/design/companion-app.md §6.3, the STAT section). The id is raw, so a package naming
// ids this build lacks still decodes; the load then reports it.
struct PresetLeaf {
  uint32_t id    = 0;
  float    value = 0.f;
};

// CTRL (docs/design/mode-compiler.md §3.4, §3.5, §6.2): the macro positions, pickup
// references never re-applied at load, and the expression assignments. Outside sound_hash; a
// package may omit it (`present` 0). The default holds one position, 0.5, per default macro.
struct MacroPosition {  // 8 bytes
  uint32_t macroId  = 0;     // a macro MACR defines, ascending
  float    position = 0.0f;  // canonical, 0-1
};
struct ExpressionAssignment {  // 16 bytes
  uint32_t target = 0;     // a Leaf row of a present element, or a Macro row
  float    lo     = 0.0f;  // within the target's range; lo > hi reverses
  float    hi     = 0.0f;
  float    curve  = 0.0f;  // the power exponent, 1/16-16
};
inline constexpr uint32_t kMaxExpressions = 4;
struct ControlState {  // 132 bytes
  uint8_t              present    = 1;  // 0: the package has no CTRL section
  uint8_t              macroCount = 6;  // one position per macro MACR defines
  uint8_t              exprCount  = 0;  // 0-4
  uint8_t              pad        = 0;
  MacroPosition        positions[kMaxMacros] = {{69, 0.5f}, {70, 0.5f}, {71, 0.5f},
                                                {72, 0.5f}, {73, 0.5f}, {74, 0.5f}};
  ExpressionAssignment expressions[kMaxExpressions] = {};
};

// The stored performance state (§2.6; docs/design/clock.md §2.4), STAT's tail: what the knobs
// and switches outside the macros start at. Tempo is integer microseconds per quarter, so no
// floating point is stored. Since sound revision 8 the engine plays the time mode, the
// subdivision and the tempo: they are the active preset's, from which an Exact load and Restart
// start (clock.md §2.5), and a Spillover load applies the time mode and subdivision (and the
// tempo under global.tempo_recall Preset); `reverse` waits for global reverse (W2). Byte 3 was
// `tempo_source`: the tempo source is a device setting (D3), so the byte is reserved and must be
// 0, as DecodePreset requires.
enum class TimeMode : uint8_t { Free, Subdivision, Tempo };
inline constexpr uint8_t kTimeModeCount = 3;
inline constexpr uint32_t kMinUsPerQuarter = 200000;   // 300 BPM
inline constexpr uint32_t kMaxUsPerQuarter = 3000000;  // 20 BPM
struct PerformanceState {  // 8 bytes
  uint8_t     reverse      = 0;  // 0 or 1
  TimeMode    timeMode     = TimeMode::Free;
  Subdivision subdiv       = Subdivision::Tap;  // §5.1's codes (Mode.h)
  uint8_t     reserved     = 0;                 // 0: was tempo_source (D3)
  uint32_t    usPerQuarter = 500000;  // 120 BPM
};

// A decoded preset package (determinism profile §5.10; mode-compiler.md §5.1): the STAT
// section's leaves and performance state, the MODE section's structure and the CTRL section,
// with the revision of the build that compiled it. Fixed-size data with fixed-width members,
// so the firmware decodes into it without allocating; it is never hashed or serialized as a
// struct. About 2.6 KiB: never a stack local in dsp/, where MSVC would probe the frame with
// __chkstk, which the symbol audit rejects (record §2.7).
//
// Default-constructed, it is a leafless state of the default mode, which plays as sound
// revision 1 does; a producer fills the leaves. Freeze is performance state that is never
// stored: every load turns it off.
struct PresetState {
  // The Leaf rows (Params.h kLeafParams), with room for the design's full leaf list.
  static constexpr uint32_t kMaxLeaves = 128;

  uint32_t         soundRev  = 0;  // the package's sound_rev; 0 when not from a package
  uint32_t         leafCount = 0;
  PresetLeaf       leaves[kMaxLeaves];
  ModeBlob         mode;
  ControlState     control;
  PerformanceState performance;
};

static_assert(sizeof(PresetLeaf) == 8 && sizeof(MacroPosition) == 8 &&
                  sizeof(ExpressionAssignment) == 16 && sizeof(ControlState) == 132 &&
                  sizeof(PerformanceState) == 8,
              "PresetState layout");
static_assert(offsetof(PresetState, leaves) == 8 && offsetof(PresetState, mode) == 1032 &&
                  offsetof(PresetState, control) == 2516 &&
                  offsetof(PresetState, performance) == 2648 && sizeof(PresetState) == 2656,
              "PresetState layout");

// Exact: Restart, then the preset, so the engine starts from the exact-restart state; not
// real-time. Spillover: the preset over the running engine, keeping history, grains,
// scheduler phase and smoothers, with the random-number epoch restarted at the load frame
// (determinism profile §5.9, §5.10; companion-app.md §6.7).
enum class LoadMode : uint8_t { Exact = 0, Spillover = 1 };

// What a Spillover load does to the grains already sounding (docs/design/mode-compiler.md
// §7.3), the SpilloverLoad event's id: Trails lets them finish as they were resolved at birth;
// FastCut fades each linearly to zero over kFastCutFrames (Engine.h), so a mode switch cuts
// the old mode without a click. An Exact load restarts, so it has no grains to keep.
enum class SwitchStyle : uint8_t { Trails = 0, FastCut = 1 };

// How faithfully a preset loads (determinism profile §5.10 step 2; mode-compiler.md §7.3). A
// load that is not exact still applies, unless its mode is invalid, but what the engine plays is
// not what the package stores, so the app shows no identity label for it.
struct LoadReport {
  bool     applied       = false;  // false when the engine was not initialized, or the mode
                                   // is invalid
  bool     exact         = false;  // the mode is valid and every count below is 0
  bool     invalidMode   = false;  // ValidateMode's structural and semantic rules on the
                                   // ModeBlob and CTRL failed (Preset.h): nothing is applied
  uint32_t unknownIds    = 0;      // leaves naming an id that is not a Leaf row this build
                                   // plays (mode-compiler.md §4.1, sinceRev), or past
                                   // kMaxLeaves
  uint32_t missingIds    = 0;      // this build's Leaf rows without a leaf, among those that
                                   // existed at the package's revision: they load their default
  uint32_t duplicateIds  = 0;      // repeated ids: the first leaf counts
  uint32_t changedValues = 0;      // values canonicalization changed (NaN, ±inf, -0,
                                   // subnormals, out of range): packages hold canonical values
  uint32_t unsupported   = 0;      // stored performance fields this build cannot play as
                                   // stored: `reverse` away from its default until global
                                   // reverse (W2), and a field outside its range or a nonzero
                                   // reserved byte (never in a decoded package): each loads as
                                   // its default
};

// The checks of a load without loading (the report's `applied` stays false), for a producer
// that stages a Spillover event: the mode's validation and every count of LoadReport. Inside
// the FP environment guard, as canonicalization is. Returns report.exact.
bool CheckPreset(const PresetState& preset, LoadReport* report = nullptr) noexcept;

}  // namespace brainscape
