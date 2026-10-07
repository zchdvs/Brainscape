#include "detail/FpProfilePrivate.h"

#include "brainscape/ModeEval.h"

#include "detail/Canonical.h"
#include "detail/DetMath.h"
#include "detail/FpEnvGuard.h"
#include "detail/GrainMath.h"

// The macro evaluator and the pitch guard (docs/design/mode-compiler.md §3.3, §2.7), as engine
// entry points: each guard owns the FP control word and does no floating-point arithmetic of
// its own; the work sits in BRAINSCAPE_FP_BODY functions (detail/FpEnvGuard.h).
namespace brainscape {

namespace {

// One target at canonical position m (§3.3): one floating-point operation per statement
// (determinism profile §3.9), binary64 intermediates, no contraction (the build profile).
// The branches keep PowF off its domain edges, give exact endpoints, and skip PowF for a
// linear target.
inline float TargetValue(const MacroTarget& t, const ParamDescriptor& row, float m) noexcept {
  float u;
  if (!(m > t.inLo)) {
    u = 0.0f;
  } else if (!(m < t.inHi)) {
    u = 1.0f;
  } else {
    const double num = static_cast<double>(m) - static_cast<double>(t.inLo);
    const double den = static_cast<double>(t.inHi) - static_cast<double>(t.inLo);
    u                = static_cast<float>(num / den);
  }
  float c;
  if (u == 0.0f) {
    c = 0.0f;
  } else if (u == 1.0f) {
    c = 1.0f;
  } else if (t.curve == 1.0f) {
    c = u;
  } else {
    c = detmath::PowF(u, t.curve);
  }
  float v;
  if (c == 0.0f) {
    v = t.lo;  // exact endpoints
  } else if (c == 1.0f) {
    v = t.hi;
  } else {
    const double span = static_cast<double>(t.hi) - static_cast<double>(t.lo);
    const double step = span * static_cast<double>(c);
    v                 = static_cast<float>(static_cast<double>(t.lo) + step);
  }
  return detail::CanonicalValue(row, v);
}

BRAINSCAPE_FP_BODY size_t EvalMacroBody(const ModeBlob& mode, ParamId macro, float position,
                                        PresetLeaf* out, size_t cap) noexcept {
  const ParamDescriptor* row = FindParam(macro);
  if (row == nullptr || row->kind != ParamKind::Macro) return 0;
  const float       m = detail::CanonicalValue(*row, position);
  const MacroTable& t = mode.macros;
  for (uint32_t k = 0; k < t.macroCount && k < kMaxMacros; ++k) {
    const MacroDef& def = t.macros[k];
    if (def.id != static_cast<uint32_t>(macro)) continue;
    size_t n = 0;
    for (uint32_t i = 0; i < def.count && n < cap; ++i) {
      const uint32_t index = static_cast<uint32_t>(def.first) + i;
      if (index >= kMaxTargets) break;
      const MacroTarget&     target = t.targets[index];
      const ParamDescriptor* d      = FindParam(static_cast<ParamId>(target.param));
      if (d == nullptr) continue;
      out[n].id    = target.param;
      out[n].value = TargetValue(target, *d, m);
      ++n;
    }
    return n;
  }
  return 0;
}

BRAINSCAPE_FP_BODY float NearGuardMsBody(float sizeMs, float entrySt, float transposeSt,
                                         float spreadCents) noexcept {
  // As a grain's birth composes its pitch (§7.5), with the largest detune spread draws.
  const float base   = entrySt + transposeSt;
  const float detune = spreadCents * 0.01f;
  float       st     = base + detune;
  if (st > 24.0f) st = 24.0f;
  if (st < -24.0f) st = -24.0f;
  const float ratio = grainmath::SemitonesToRatio(st);
  if (!(ratio > 1.0f)) return 0.0f;
  const double excess = static_cast<double>(ratio) - 1.0;
  const double guard  = static_cast<double>(sizeMs) * excess;
  return static_cast<float>(guard);
}

}  // namespace

size_t EvalMacro(const ModeBlob& mode, ParamId macro, float position, PresetLeaf* out,
                 size_t cap) noexcept {
  if (out == nullptr || cap == 0) return 0;
  const detail::FpEnvGuard guard;
  return EvalMacroBody(mode, macro, position, out, cap);
}

float NearGuardMs(float sizeMs, float entrySt, float transposeSt, float spreadCents) noexcept {
  const detail::FpEnvGuard guard;
  return NearGuardMsBody(sizeMs, entrySt, transposeSt, spreadCents);
}

}  // namespace brainscape
