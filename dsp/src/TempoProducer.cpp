#include "detail/FpProfilePrivate.h"

#include <cstdint>
#include <cstring>

#include "brainscape/ModeEval.h"
#include "brainscape/Params.h"
#include "brainscape/Tempo.h"
#include "detail/Canonical.h"
#include "detail/DetMath.h"
#include "detail/FpEnvGuard.h"
#include "detail/IntMath.h"
#include "detail/Tempo.h"

// The tempo core's producer functions (docs/design/clock.md §4.4, §5.3, §6.4, §6.6): what the
// pedal's control loop, the plugin and the editor call, never Process. Out of the pedal's ITCM
// with the rest of the tempo code (§9.6, firmware/CMakeLists.txt). TempoNsFromKnob and the host
// conversions NsPerQuarterFromBpm, HostTempoFromBpm and HostAnchor are engine entry points, so
// they own the FP control word (detail/FpEnvGuard.h); UsesTempo and SyncedDuration are
// integer-only.
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

// A binary64's bits say it is finite: no comparison, so no FP environment changes it.
bool FiniteD(double x) noexcept {
  uint64_t b = 0;
  std::memcpy(&b, &x, sizeof b);
  return (b & 0x7FF0000000000000ull) != 0x7FF0000000000000ull;
}

BRAINSCAPE_FP_BODY uint32_t NsPerQuarterFromBpmBody(double bpm) noexcept {
  if (!FiniteD(bpm) || !(bpm > 0.0)) return 0;
  const double q = 6.0e10 / bpm;  // +inf for a subnormal bpm, which the clamp takes
  if (q >= static_cast<double>(tempo::kMaxNsPerQuarter)) return tempo::kMaxNsPerQuarter;
  if (q <= static_cast<double>(tempo::kMinNsPerQuarter)) return tempo::kMinNsPerQuarter;
  return static_cast<uint32_t>(detmath::RoundHalfAwayI64(q));
}

BRAINSCAPE_FP_BODY tempo::HostTempo HostTempoFromBpmBody(double bpm) noexcept {
  tempo::HostTempo h;
  if (!FiniteD(bpm) || !(bpm > 0.0)) return h;
  const auto lo = static_cast<double>(tempo::kMinNsPerQuarter);
  const auto hi = static_cast<double>(tempo::kMaxNsPerQuarter);
  double     q  = 6.0e10 / bpm;  // +inf for a subnormal bpm: no fold reaches the range
  // Doubling and halving are exact: q is §4.4's quotient at the folded tempo.
  while (q < lo && h.octaves < tempo::kMaxHostOctaves) {
    q *= 2.0;
    ++h.octaves;
  }
  while (q > hi && h.octaves > -tempo::kMaxHostOctaves) {
    q *= 0.5;
    --h.octaves;
  }
  h.clamped      = q < lo || q > hi;
  h.nsPerQuarter = q >= hi   ? tempo::kMaxNsPerQuarter
                   : q <= lo ? tempo::kMinNsPerQuarter
                             : static_cast<uint32_t>(detmath::RoundHalfAwayI64(q));
  return h;
}

// |ppq| below 2^40 quarters keeps x = 24·ppq below 2^45, inside RoundHalfAwayI64's domain, and k
// inside int64_t; about 2,900 years at 300 BPM.
constexpr double kMaxHostPpq = 1099511627776.0;  // 2^40
// The tolerance below which x counts as the integer nearest it (§4.4).
constexpr double kOnTick = 1.0e-9;

BRAINSCAPE_FP_BODY bool HostAnchorBody(double ppq, uint32_t ns, uint32_t rate, uint32_t* position,
                                       uint32_t* offset) noexcept {
  if (!FiniteD(ppq) || !(ppq > -kMaxHostPpq && ppq < kMaxHostPpq)) return false;
  const double  x = 24.0 * ppq;
  const int64_t n = detmath::RoundHalfAwayI64(x);
  // Exact (Sterbenz): n is the integer nearest x, so x and n lie within a factor of two of each
  // other, or n is 0 and d is x itself.
  const double d    = x - static_cast<double>(n);
  int64_t      k    = n;    // the first tick at or after the block's first frame
  double       frac = 0.0;  // k − x, in ticks
  if (d > kOnTick) {
    k    = n + 1;
    frac = 1.0 - d;
  } else if (d < -kOnTick) {
    frac = -d;
  }
  const double  frames = frac * static_cast<double>(ns) * static_cast<double>(rate) / 24.0e9;
  const int64_t o      = detmath::RoundHalfAwayI64(frames);  // at most one tick: 48,000 frames
  *position = static_cast<uint32_t>(intmath::FloorModI64(k, static_cast<int64_t>(tempo::kPositionModulus)));
  *offset   = static_cast<uint32_t>(o);
  return true;
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

tempo::SyncedTime tempo::SyncedDuration(SyncTarget target, uint8_t code, uint32_t nsPerQuarter,
                                        uint8_t subdiv, uint8_t timeMode, uint32_t rate) noexcept {
  SyncedTime out;
  if (code == 0u || code >= kSyncCodes || nsPerQuarter < kMinNsPerQuarter ||
      nsPerQuarter > kMaxNsPerQuarter || subdiv >= kSubdivCodes || timeMode >= kTimeModeCodes ||
      rate < TempoCore::kMinRate || rate > TempoCore::kMaxRate) {
    return out;
  }
  out.subdiv           = timeMode == kTimeModeTempo ? kSubdivTap : subdiv;
  const bool     post  = target == SyncTarget::PostDelay;
  const uint32_t lo    = post ? PostSyncMinFrames(rate) : BaseSyncMinFrames(rate);
  const uint32_t hi    = post ? PostSyncMaxFrames(rate) : BaseSyncMaxFrames(rate);
  out.frames = FoldedFrames(PFromNs(nsPerQuarter, rate), NoteTicks(code), SubdivTicks(out.subdiv),
                            lo, hi, &out.octaves);
  return out;
}

uint32_t tempo::NsPerQuarterFromBpm(double bpm) noexcept {
  const detail::FpEnvGuard guard;
  return NsPerQuarterFromBpmBody(bpm);
}

tempo::HostTempo tempo::HostTempoFromBpm(double bpm) noexcept {
  const detail::FpEnvGuard guard;
  return HostTempoFromBpmBody(bpm);
}

bool tempo::HostAnchor(double ppq, uint32_t ns, uint32_t rate, uint32_t* position,
                       uint32_t* offset) noexcept {
  if (ns < kMinNsPerQuarter || ns > kMaxNsPerQuarter || rate < 8000u || rate > 384000u ||
      position == nullptr || offset == nullptr) {
    return false;
  }
  const detail::FpEnvGuard guard;
  return HostAnchorBody(ppq, ns, rate, position, offset);
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
