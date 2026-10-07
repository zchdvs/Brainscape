#pragma once
#include <cstddef>
#include <cstdint>

#include "brainscape/Mode.h"
#include "brainscape/PresetState.h"

// The live image's presets and mode structures (firmware/README.md, image C), apart from
// libDaisy, so the host test firmware_live_presets builds exactly what the image builds at
// boot. Both come from the golden corpus (dsp/tests/golden), whose structure since sound
// revision 2 lives in its committed packages, compiled into the image (EmbeddedPackages.h):
// the onset trigger and mark positioning are a mode's (mode-compiler.md §4.2, rows 27 and 28
// retired), so the image selects them by loading a PresetState with another package's mode,
// never by setting a parameter or assigning ModeBlob fields (§4.4).
namespace brainscape::fw::live {

// Musically useful corpus presets (dsp/tests/golden/Corpus.cpp): their parameters, and their
// package's leaves, mode and CTRL when they start from one; the corpus scripts drive renders,
// not the live engine.
struct Slot {
  const char* label;
  const char* vector;
  const char* preset;
};
inline constexpr Slot kSlots[] = {
    {"default", "plucks_12s", "default"},
    {"clean delay", "plucks_12s", "clean_delay"},
    {"strum marks (onset grains at marks)", "plucks_12s", "strum_marks"},
    {"pitch, reverse, spray", "plucks_12s", "pitch_reverse_spray"},
    {"freeze marks (all wet)", "strums_16s", "freeze_marks"},
    {"ambient tail (feedback, delay, reverb, filter)", "strums_tail_123s", "tail_post_fb"},
    {"octave shimmer (onset grains at marks)", "strums_freeze_70s", "freeze_long"},
    {"glitch (1 ms onset grains)", "onset_bursts_6s", "dense_1ms"},
};
inline constexpr uint32_t kNumSlots = sizeof kSlots / sizeof kSlots[0];

// The structures the `onset` and `marks` commands choose between, by StructureIndex: the
// default mode (sound revision 1's structure) and three corpus packages whose modes differ
// from it only in the onset source and layer 0's position source, with the default macros
// and CTRL (BuildStructures checks both).
struct Structure {
  const char* package;  // null: the default mode and CTRL
  bool        onset;    // `onset` in scheduler.sources
  bool        marks;    // layer 0 positioned at marks
};
inline constexpr Structure kStructures[] = {
    {nullptr, false, false},
    {"lone_busy", true, false},
    {"reverse_mark_aging", false, true},
    {"strum_marks", true, true},
};
inline constexpr uint32_t kNumStructures = sizeof kStructures / sizeof kStructures[0];
constexpr uint32_t StructureIndex(bool onset, bool marks) {
  return (onset ? 1u : 0u) | (marks ? 2u : 0u);
}

// What a mode plays.
bool PlaysOnset(const ModeBlob& mode) noexcept;
bool PlaysMarks(const ModeBlob& mode) noexcept;

// Every slot's complete preset (out[kNumSlots]), each passing CheckPreset; false with *why on
// the first that cannot be built.
bool BuildSlots(PresetState* out, const char** why);

// Every structure's mode and CTRL (out[kNumStructures], decoded from its package), each with
// the onset and mark structure its row names and otherwise the default mode and CTRL; false
// with *why on the first that is not.
bool BuildStructures(PresetState* out, const char** why);

// `state` with the mode and CTRL of `structure`, its leaves kept: what `onset` and `marks`
// load (a Spillover load; the engine sees a mode switch).
void WithStructure(PresetState* state, const PresetState& structure) noexcept;

}  // namespace brainscape::fw::live
