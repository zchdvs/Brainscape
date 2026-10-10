#pragma once
#include <cstddef>
#include <cstdint>

#include "brainscape/FpProfile.h"
#include "brainscape/Mode.h"
#include "brainscape/Params.h"
#include "brainscape/PresetState.h"

namespace brainscape {

// Floating-point functions over a compiled mode (docs/design/mode-compiler.md §3.3, §2.7).
// Exported, non-inline engine entry points that own the complete FP control word for their
// duration (determinism profile §4.1), so a result never depends on the caller's environment
// and every conforming build gives the same bits. The compiler does no floating-point
// arithmetic of its own (§1.4 principle 3): its lint and derive passes call these instead.
// The engine's MacroMove and Expression events (§7.4, R6) run the same evaluators inside its
// own guard, so a fan-out the app or the compiler derives is the one the engine plays.

// The macro evaluator (§3.3): the value of each target of `macro` (a Macro row, 69-76) that
// `mode` defines, at `position`, written to out[0..n) as {param, value} in the targets' list
// order. Returns n, the number written: the macro's target count, at most `cap`, and 0 for a
// macro the mode leaves undefined or an id that is not a Macro row. `position` is first
// canonicalized as the macro row's value (NaN to 0, -0 and subnormals to +0, clamped to
// [0, 1]). Each target maps it through its in_range window to u in [0, 1], shapes it as
// c = u^curve (DetMath's PowF, skipped for curve 1 and at u = 0 and 1) and returns lo + (hi - lo)
// * c, exactly lo at c = 0 and hi at c = 1, in binary64 one operation per statement, rounded
// once and canonicalized for the target's row. Monotonic in the position (record §2.6). For a
// mode that passes ValidateMode; a target on an unknown row is skipped.
size_t EvalMacro(const ModeBlob& mode, ParamId macro, float position, PresetLeaf* out,
                 size_t cap) noexcept;

// The expression pedal's fan-out (§3.4): `position` canonicalized as perf.expression's value
// (to [0, 1]), then each of CTRL's assignments in order, mapped as a macro target with in_range
// [0, 1] through the assignment's lo, hi and curve: a Leaf target gets that value, a Macro target
// that position, and its targets are written as EvalMacro writes them. Writes {param, value} to
// out[0..n) in application order and returns n, at most `cap` (4 assignments of at most 8
// targets each fit in 32). Nothing without CTRL or assignments. For a state that passes
// ValidateMode.
size_t EvalExpression(const ModeBlob& mode, const ControlState& control, float position,
                      PresetLeaf* out, size_t cap) noexcept;

// The near write-head guard of a forward grain, in milliseconds without the guard margin
// (engine §3; mode-compiler.md §2.7 lint L2): size_ms * (r - 1), where r is the ratio of the
// highest pitch a pitch entry reaches, clamp((entry + transpose) + spread / 100, -24, 24)
// semitones as a grain's birth composes it (§7.5), through the engine's own semitone-to-ratio
// conversion. 0 when r <= 1. A grain whose base delay is below it is moved back by the guard.
float NearGuardMs(float sizeMs, float entrySt, float transposeSt, float spreadCents) noexcept;

// Whether a preset reads tempo (docs/design/clock.md §6.6, D21): its mode lists `clock`, a
// layer's base_sync is not off, or post.delay.sync (row 63) reads as a nonzero code at its stored
// value or at an end of a macro target or an expression assignment on it. Derived from the
// package, never stored, so no byte and no hash depends on it. Producers read it at each load:
// in a preset without it the pedal ignores the time-mode gesture and shows the tap LED steady,
// and the editor dims rows 83-84; tap still sets the (global) tempo. Integer-only (the values
// are compared on their bits), outside the FP guard; bspc reports it beside the lint.
bool UsesTempo(const PresetState& preset) noexcept;

}  // namespace brainscape
