#pragma once
#include "detail/FpProfilePrivate.h"

#include <cstddef>

#include "brainscape/Mode.h"
#include "brainscape/Params.h"
#include "brainscape/PresetState.h"
#include "detail/FpEnvGuard.h"

// The bodies of EvalMacro and EvalExpression (brainscape/ModeEval.h), for a caller already
// inside the FP environment guard: the engine's MacroMove and Expression events (§7.4), which
// so run the one evaluator the compiler's lint and derive passes and the app call
// (docs/design/mode-compiler.md §3.3). Non-inline FP bodies (detail/FpEnvGuard.h).
namespace brainscape::detail {

BRAINSCAPE_FP_BODY size_t EvalMacroBody(const ModeBlob& mode, ParamId macro, float position,
                                        PresetLeaf* out, size_t cap) noexcept;

BRAINSCAPE_FP_BODY size_t EvalExpressionBody(const ModeBlob& mode, const ControlState& control,
                                             float position, PresetLeaf* out,
                                             size_t cap) noexcept;

}  // namespace brainscape::detail
