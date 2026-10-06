#pragma once
#include <cstddef>
#include <cstdint>

#include "brainscape/Params.h"

namespace brainscape {

// Taper and display metadata for the parameter table (docs/design/companion-app.md §5.4),
// keyed by ParamId beside the descriptors, which stay unchanged. Everything that maps a
// knob position to the plain value the engine receives lives here, in dsp/, so a pedal
// pot and a plugin knob at the same normalised position yield the same plain bits.
// The functions are exported and non-inline: this header carries no floating-point code
// (determinism profile §3.5).
//
// Not final: tapers, titles and groups are frozen with the parameter IDs at the first
// public release (companion §5.7).

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
  Decibels,
  Semitones,
  Cents,
  FilterCutoff,  // Hertz, "Off" at the maximum (the stage's exact bypass, Params.h)
  FilterMorph,   // 0..3: LP -> BP -> HP -> Notch
  OffOn,         // >= 0.5 is on (the engine's threshold)
  LiveMark,      // PositionSource: >= 0.5 is POS_MARK
};

enum class ParamGroup : uint8_t {
  GrainDelay, Grains, Pitch, Window, PanMod, PostDelay, Reverb, Filter, Triggers,
};
inline constexpr size_t kNumParamGroups = 9;

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
  uint16_t    flags;       // ParamFlag bits
};

// nullptr for an unknown id. Same order as kParamTable.
const ParamDisplay* FindParamDisplay(ParamId id) noexcept;
const char*         GroupTitle(ParamGroup group) noexcept;

// Integer tests on the bit pattern, so the result does not depend on DAZ: NaN and ±inf
// map to the descriptor minimum, ±0 and subnormals to +0, then the clamp (profile §3.7).
// Values are never quantized to a grid (companion §5.5).
float CanonicalizePlain(ParamId id, float plain) noexcept;

// Knob/pot position in [0, 1] (NaN -> 0, clamped) to a canonical plain value. Computed
// in binary64 and rounded once; the endpoints map exactly to min and max.
float PlainFromNormalized(ParamId id, float normalized) noexcept;
// The host-facing view of a plain value. Not an exact inverse: the plain value is the
// source of truth, the normalised value only a view (companion §5.1).
float NormalizedFromPlain(ParamId id, float plain) noexcept;

// Display text with unit ("250 ms", "+7.00 st", "Off"), NUL-terminated and truncated to
// fit. Returns the length written. Display only; never parsed back by the pedal.
size_t FormatPlain(ParamId id, float plain, char* out, size_t outSize) noexcept;

}  // namespace brainscape
