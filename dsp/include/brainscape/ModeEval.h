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
// Nothing in Engine::Process calls them before sound revision 2, which sends MacroMove events
// through EvalMacro (§7.1 R6).

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

// The near write-head guard of a forward grain, in milliseconds without the guard margin
// (engine §3; mode-compiler.md §2.7 lint L2): size_ms * (r - 1), where r is the ratio of the
// highest pitch a pitch entry reaches, clamp((entry + transpose) + spread / 100, -24, 24)
// semitones as a grain's birth composes it (§7.5), through the engine's own semitone-to-ratio
// conversion. 0 when r <= 1. A grain whose base delay is below it is moved back by the guard.
float NearGuardMs(float sizeMs, float entrySt, float transposeSt, float spreadCents) noexcept;

}  // namespace brainscape
