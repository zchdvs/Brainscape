#include "detail/FpProfilePrivate.h"

#include <cstdint>
#include <cstring>

#include "brainscape/ModeEval.h"
#include "brainscape/Params.h"
#include "brainscape/Tempo.h"
#include "detail/Canonical.h"
#include "detail/DetMath.h"
#include "detail/FpEnvGuard.h"

// The tempo core's producer functions (docs/design/clock.md §6.4, §6.6): what the pedal's
// control loop, the plugin and the editor call, never Process. Out of the pedal's ITCM with the
// rest of the tempo code (§9.6, firmware/CMakeLists.txt). TempoNsFromKnob is an engine entry
// point, so it owns the FP control word (detail/FpEnvGuard.h); UsesTempo is integer-only.
namespace brainscape {

namespace {

// log2(15): the Tempo knob spans 3·10^9 down to 2·10^8 ns per quarter, a ratio of 15.
constexpr double kLog2Of15 = 3.9068905956085187;

BRAINSCAPE_FP_BODY uint32_t TempoNsFromKnobBody(float m) noexcept {
  const float  pos = detail::CanonicalValue(*FindParam(ParamId::MacroTime), m);  // [0, 1]
  const double e   = static_cast<double>(pos) * -kLog2Of15;
  const double x   = detmath::Exp2D(e);  // (1/15, 1]
  const double ns  = x * 3.0e9;
  const int64_t r  = detmath::RoundHalfAwayI64(ns);  // ns in about [2e8, 3e9]: in range
  if (r < static_cast<int64_t>(tempo::kMinNsPerQuarter)) return tempo::kMinNsPerQuarter;
  if (r > static_cast<int64_t>(tempo::kMaxNsPerQuarter)) return tempo::kMaxNsPerQuarter;
  return static_cast<uint32_t>(r);
}

// Whether a row-63 value reads as a nonzero code: its canonical value (detail/Canonical.h: NaN
// and ±inf to the minimum 0, negatives clamped to 0) is at least 0.5, so it rounds to a code of 1
// or more (mode-compiler.md §3.7). On the bits, which order like the values for positive
// floats, so no FP environment can change it.
bool SyncCodeNonzero(float v) noexcept {
  uint32_t b = 0;
  std::memcpy(&b, &v, sizeof b);
  if ((b >> 31) != 0u) return false;                       // negative or -0
  if ((b & 0x7F800000u) == 0x7F800000u) return false;      // NaN, +inf: canonically 0
  return b >= 0x3F000000u;                                 // 0.5f
}

}  // namespace

uint32_t tempo::TempoNsFromKnob(float m) noexcept {
  const detail::FpEnvGuard guard;
  return TempoNsFromKnobBody(m);
}

bool UsesTempo(const PresetState& preset) noexcept {
  const ModeBlob& mode = preset.mode;
  if ((mode.schedule.sources & kSourceClock) != 0u) return true;
  const uint32_t layers =
      mode.schedule.layerCount <= kMaxModeLayers ? mode.schedule.layerCount : kMaxModeLayers;
  for (uint32_t l = 0; l < layers; ++l) {
    if (mode.layers[l].baseSync != 0u) return true;
  }
  const auto sync = static_cast<uint32_t>(ParamId::DelaySync);
  const uint32_t leaves =
      preset.leafCount <= PresetState::kMaxLeaves ? preset.leafCount : PresetState::kMaxLeaves;
  for (uint32_t i = 0; i < leaves; ++i) {
    if (preset.leaves[i].id == sync && SyncCodeNonzero(preset.leaves[i].value)) return true;
  }
  // A macro's targets reach their range's ends (lo at one end of in_range, hi at the other), and
  // the expression pedal's assignments theirs; a macro moved by the pedal is one of the first.
  const uint32_t targets =
      mode.macros.targetCount <= kMaxTargets ? mode.macros.targetCount : kMaxTargets;
  for (uint32_t t = 0; t < targets; ++t) {
    const MacroTarget& m = mode.macros.targets[t];
    if (m.param == sync && (SyncCodeNonzero(m.lo) || SyncCodeNonzero(m.hi))) return true;
  }
  const uint32_t exprs = preset.control.exprCount <= kMaxExpressions ? preset.control.exprCount
                                                                     : kMaxExpressions;
  for (uint32_t e = 0; e < exprs; ++e) {
    const ExpressionAssignment& a = preset.control.expressions[e];
    if (a.target == sync && (SyncCodeNonzero(a.lo) || SyncCodeNonzero(a.hi))) return true;
  }
  return false;
}

}  // namespace brainscape
