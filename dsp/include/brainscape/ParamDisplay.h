#pragma once
#include <cstddef>
#include <cstdint>

#include "brainscape/Params.h"
#include "brainscape/Tempo.h"

namespace brainscape {

// Taper and display metadata for the parameter table (docs/design/companion-app.md §5.4),
// keyed by ParamId beside the descriptors, which stay unchanged. Everything that maps a
// knob position to the plain value the engine receives lives here, in dsp/, so a pedal
// pot and a plugin knob at the same normalised position yield the same plain bits.
// The functions are exported and non-inline: this header carries no floating-point code
// (determinism profile §3.5). Those that compute in floating point (PlainFromNormalized,
// NormalizedFromPlain, FormatPlain) are engine entry points: they own the complete FP
// control word for their duration (profile §4.1, companion §4.6), so the host thread's
// FTZ/DAZ or rounding mode cannot change a result. Canonicalization is
// brainscape::Canonicalize (Params.h), the rule SetParam applies.
//
// Every row of the table has a display row, Reserved rows included (docs/design/
// mode-compiler.md §4.3): a feature that lands changes its row's kind, never its display.
// Not final: tapers, titles, groups and flags are frozen with the parameter IDs at the first
// public release (companion §5.7, mode-compiler.md §4.5).

// Power tapers need only IEEE basic operations and sqrt, so they are bit-identical on
// every conforming target without the in-tree transcendental kernels (profile §3.9).
enum class Taper : uint8_t {
  Linear,   // plain = min + (max - min) * n
  Square,   // plain = min + (max - min) * n^2
  Quartic,  // plain = min + (max - min) * n^4
};

enum class DisplayKind : uint8_t {
  Milliseconds,
  Hertz,
  Percent,       // 0..1 shown as 0-100 %
  Balance,       // 0..1 shown as -100..+100 %, 0.5 = 0 % (centred)
  Amount,        // 0..1 shown as 0-100, no unit (a scale, not a share)
  Decibels,
  Semitones,
  Cents,
  FilterCutoff,  // Hertz, "Off" at the maximum (the stage's exact bypass, Params.h)
  FilterMorph,   // 0..3: LP -> BP -> HP -> Notch
  OffOn,         // >= 0.5 is on (the engine's threshold)
  LiveMark,      // PositionSource: >= 0.5 is POS_MARK
  Count,         // an integer leaf, shown as the integer the engine reads:
                 // RoundHalfAwayI32 of the canonical value (mode-compiler.md §3.7)
  MsOrOff,       // milliseconds, "Off" at 0 (decay_ms)
  Signed,        // -1..1 shown as -100..+100 %
  ReverbMode,    // 0..3: Bright room, Dark medium, Large hall, Ambient
  Division,      // post.delay.sync: "Off" at 0, then the note value (docs/design/clock.md §5.2,
                 // §5.4: "1/8D", "1/16T")
  SubdivPosition,  // perf.subdiv: the knob's six positions as rates (×1/4, ×1/2, TAP, ×2, ×4,
                   // ×8; the × is UTF-8), clock.md §5.1
  TimeMode,      // perf.time_mode: Free, Subdiv, Tempo
  TempoRecall,   // global.tempo_recall: Keep, Preset
};

// The post-chain groups follow its signal order (grain-engine.md §2): mod -> delay ->
// reverb -> filter. The groups after Triggers hold rows added for the mode system
// (mode-compiler.md §4.3); Layer2 is layer index 1, which hosts show as "Layer 2".
enum class ParamGroup : uint8_t {
  GrainDelay, Grains, Pitch, Window, Mod, PostDelay, Reverb, Filter, Triggers,
  Scheduler, Layer2, Modifiers, Modulation, Macros, Performance, Device,
};
inline constexpr size_t kNumParamGroups = 16;

// kParamAutomatable follows the host model (mode-compiler.md §3.6, Q12): option (b), the
// recommended one, provisionally until the owner decides: macros, global.mix, the effect
// volume and the performance rows are automatable; every other leaf is registered but not
// automatable, so a host records the knobs a player turns, not the leaves they fan out to.
// The plugin follows it since lane D's curation slice registered the macro parameters.
enum ParamFlag : uint16_t {
  kParamAutomatable = 1u << 0,
  kParamDiscrete    = 1u << 1,
  kParamReadOnly    = 1u << 2,
};

struct ParamDisplay {
  ParamId     id;
  ParamGroup  group;
  const char* title;       // unique, host-facing ("Post delay mix")
  const char* shortTitle;  // knob caption inside its group ("Mix")
  Taper       taper;
  DisplayKind kind;
  uint16_t    steps;       // 0 = continuous; otherwise the number of discrete positions
                           // (an integer leaf: max - min + 1)
  uint16_t    flags;       // ParamFlag bits
};

// nullptr for an unknown id. Same order as kParamTable.
const ParamDisplay* FindParamDisplay(ParamId id) noexcept;
const char*         GroupTitle(ParamGroup group) noexcept;

// Knob/pot position in [0, 1] (NaN -> 0, clamped) to a canonical plain value. Computed
// in binary64 and rounded once; the endpoints map exactly to min and max. Values are
// never quantized to a grid (companion §5.5).
float PlainFromNormalized(ParamId id, float normalized) noexcept;
// The host-facing view of a plain value. Not an exact inverse: the plain value is the
// source of truth, the normalised value only a view (companion §5.1).
float NormalizedFromPlain(ParamId id, float plain) noexcept;

// Display text with unit ("250 ms", "+7.00 st", "Off"), NUL-terminated and truncated to
// fit. Returns the length written. Display only; never parsed back by the pedal. The
// digits are formatted in integers here, not by the C library, so the text is the same
// on every target and in any locale: each number is the canonical value rounded to the
// shown decimals, ties to even, as a correctly rounding printf("%.Nf") gives it.
size_t FormatPlain(ParamId id, float plain, char* out, size_t outSize) noexcept;

// The effective value of a synced field (docs/design/clock.md §5.3, §5.4): what it plays at the
// committed tempo, for the editor's and the plugin's displays beside the field; the host's value
// text for row 63 stays FormatPlain's, the code's name alone, since a host caches value text as
// a function of the value. `code` is §5.2's (row 63's value read as RoundHalfAwayI32, or a
// layer's base_sync), `nsPerQuarter`, `subdiv` and `timeMode` as TempoInfo reports them (the
// committed tempo, the stored or live subdivision and time mode), `rate` the engine's integer
// rate; the duration is tempo::SyncedDuration's. "Off" for code 0; otherwise the note value's
// name, " · Subdiv ×1/2" when the effective subdivision is not TAP, " → " and the note value
// that plays when the subdivision or a fold changes it, then the duration: "1/4 · 500 ms",
// "2/1 → 1/1 · 2.02 s" (a fold, at 119 BPM), "1/4 · Subdiv ×1/2 → 1/2 · 1.00 s". The × and →
// are UTF-8. NUL-terminated and truncated to fit; returns the length written, as FormatPlain.
size_t FormatSyncedTime(tempo::SyncTarget target, uint8_t code, uint32_t nsPerQuarter,
                        uint8_t subdiv, uint8_t timeMode, uint32_t rate, char* out,
                        size_t outSize) noexcept;

}  // namespace brainscape
