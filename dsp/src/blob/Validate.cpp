#include "detail/FpProfilePrivate.h"

#include "brainscape/Params.h"
#include "brainscape/Preset.h"

#include "Blob.h"

// The validator (docs/design/mode-compiler.md §5.3): the structural rules DecodePreset applies
// to what it decoded, held to a state built in memory too, and ValidateMode's semantic rules
// (E8-E11, absent elements' leaves). Integer-only, no allocation, no library call.
namespace brainscape {

namespace blob {

namespace {

constexpr uint32_t kFirstMacroId = static_cast<uint32_t>(ParamId::MacroActivity);  // 69
constexpr uint32_t kLastMacroId  = static_cast<uint32_t>(ParamId::MacroAux2);      // 76

// A float field: canonical (else `valueError`) and within [lo, hi] (else ModeRange).
bool CheckFloat(const float& f, uint32_t lo, uint32_t hi, PresetDiagnostic* d,
                PresetError valueError = PresetError::ModeValue,
                PresetError rangeError = PresetError::ModeRange, uint32_t detail = 0) noexcept {
  const uint32_t b = BitsOf(f);
  if (!Canonical(b)) return Fail(d, valueError, detail);
  if (!InRange(b, lo, hi)) return Fail(d, rangeError, detail);
  return true;
}

bool CheckSchedule(const ModeSchedule& s, PresetDiagnostic* d) noexcept {
  if ((s.sources & ~kSourceAll) != 0u) return Fail(d, PresetError::ModeEnum, kChunkSchd);
  if (s.layerCount < 1u || s.layerCount > kMaxModeLayers) {
    return Fail(d, PresetError::ModeCount, kChunkSchd);
  }
  if (static_cast<uint8_t>(s.subdiv) >= kSubdivisionCount ||
      static_cast<uint8_t>(s.stepOrder) >= kStepOrderCount) {
    return Fail(d, PresetError::ModeEnum, kChunkSchd);
  }
  if (!AllZero(s.pad, sizeof s.pad)) return Fail(d, PresetError::ModePadding, kChunkSchd);
  return true;
}

bool CheckLayer(const ModeLayer& l, PresetDiagnostic* d) noexcept {
  const uint32_t at = kChunkLayr;
  if (static_cast<uint8_t>(l.source) >= kPositionSourceCount ||
      static_cast<uint8_t>(l.sprayLaw) >= kSprayLawCount ||
      static_cast<uint8_t>(l.markWalk) >= kMarkWalkCount ||
      static_cast<uint8_t>(l.pinRearm) >= kPinRearmCount ||
      static_cast<uint8_t>(l.pitchSelect) >= kPitchSelectCount ||
      static_cast<uint8_t>(l.quantize) >= kQuantizeModeCount ||
      static_cast<uint8_t>(l.modifier[0]) >= kModifierOpCount ||
      static_cast<uint8_t>(l.modifier[1]) >= kModifierOpCount ||
      static_cast<uint8_t>(l.svfBand) >= kSvfBandCount ||
      static_cast<uint8_t>(l.svfCutoffSource) >= kCutoffSourceCount) {
    return Fail(d, PresetError::ModeEnum, at);
  }
  if (l.baseSync > kMaxSyncDivision || l.markIndex > kMaxMarkIndex || l.quantizeRoot > 11u ||
      (l.scaleMask & ~kScaleMaskAll) != 0u) {
    return Fail(d, PresetError::ModeRange, at);
  }
  // Modifiers: at most two distinct ops, packed (an op never follows None).
  if (l.modifier[0] == ModifierOp::None && l.modifier[1] != ModifierOp::None) {
    return Fail(d, PresetError::ModePadding, at);
  }
  if (l.modifier[0] != ModifierOp::None && l.modifier[0] == l.modifier[1]) {
    return Fail(d, PresetError::ModeEnum, at);
  }
  // The fields of an element the layer lacks hold their defaults: the SVF's band and cutoff
  // source without an SVF, the root and the scale without quantization (which needs a note).
  const bool svf = l.modifier[0] == ModifierOp::Svf || l.modifier[1] == ModifierOp::Svf;
  if (!svf && (l.svfBand != SvfBand::Lowpass || l.svfCutoffSource != CutoffSource::Fixed)) {
    return Fail(d, PresetError::ModeRange, at);
  }
  if (l.quantize == QuantizeMode::Off ? (l.quantizeRoot != 0u || l.scaleMask != 0u)
                                      : l.scaleMask == 0u) {
    return Fail(d, PresetError::ModeRange, at);
  }
  if (l.pad != 0u) return Fail(d, PresetError::ModePadding, at);
  const uint32_t share = BitsOf(l.slotShare);
  if (!Canonical(share)) return Fail(d, PresetError::ModeValue, at);
  if (share == kF0 || !InRange(share, kF0, kFOne)) return Fail(d, PresetError::ModeRange, at);
  return CheckFloat(l.markJitter, kF0, kFOne, d, PresetError::ModeValue, PresetError::ModeRange,
                    at) &&
         CheckFloat(l.pinRearmMs, kFRearmMin, kFRearmMax, d, PresetError::ModeValue,
                    PresetError::ModeRange, at) &&
         CheckFloat(l.glideStStart, kFMinusSt, kFSt, d, PresetError::ModeValue,
                    PresetError::ModeRange, at) &&
         CheckFloat(l.glideStEnd, kFMinusSt, kFSt, d, PresetError::ModeValue,
                    PresetError::ModeRange, at);
}

bool CheckPitchSet(const PitchSet& s, PresetDiagnostic* d) noexcept {
  const uint32_t at = kChunkPset;
  if (s.count < 1u || s.count > kMaxPitchEntries) return Fail(d, PresetError::ModeCount, at);
  if (!AllZero(s.pad, sizeof s.pad)) return Fail(d, PresetError::ModePadding, at);
  for (uint32_t i = 0; i < kMaxPitchEntries; ++i) {
    const PitchEntry& e = s.entries[i];
    if (i >= s.count) {
      if (!AllZero(&e, sizeof e)) return Fail(d, PresetError::ModePadding, at);
      continue;
    }
    if (!CheckFloat(e.st, kFMinusSt, kFSt, d, PresetError::ModeValue, PresetError::ModeRange,
                    at)) {
      return false;
    }
    if (e.weight < 1u || e.weight > 16u) return Fail(d, PresetError::ModeRange, at);
    if (e.pad != 0u) return Fail(d, PresetError::ModePadding, at);
  }
  return true;
}

bool CheckSteps(const StepTable& t, PresetDiagnostic* d) noexcept {
  const uint32_t at = kChunkStep;
  if (t.countMax > kMaxSteps) return Fail(d, PresetError::ModeCount, at);
  if (!AllZero(t.pad, sizeof t.pad)) return Fail(d, PresetError::ModePadding, at);
  for (uint32_t i = 0; i < kMaxSteps; ++i) {
    const StepEntry& e = t.entries[i];
    if (i >= t.countMax) {
      if (!AllZero(&e, sizeof e)) return Fail(d, PresetError::ModePadding, at);
      continue;
    }
    if (e.slot >= kMaxSteps || e.ratioIdx >= kMaxPitchEntries) {
      return Fail(d, PresetError::ModeRange, at);
    }
    if ((e.flags & ~kStepFlagReverse) != 0u) return Fail(d, PresetError::ModeEnum, at);
    if (e.pad != 0u) return Fail(d, PresetError::ModePadding, at);
    if (!CheckFloat(e.posSel, kF0, kFPosSelMax, d, PresetError::ModeValue,
                    PresetError::ModeRange, at) ||
        !CheckFloat(e.gain, kF0, kFOne, d, PresetError::ModeValue, PresetError::ModeRange, at) ||
        !CheckFloat(e.prob, kF0, kFOne, d, PresetError::ModeValue, PresetError::ModeRange, at)) {
      return false;
    }
  }
  return true;
}

bool CheckModulators(const Modulator* m, PresetDiagnostic* d) noexcept {
  const uint32_t at = kChunkMods;
  for (uint32_t k = 0; k < kMaxModulators; ++k) {
    const Modulator& e = m[k];
    if (static_cast<uint8_t>(e.type) >= kModulatorTypeCount) {
      return Fail(d, PresetError::ModeEnum, at);
    }
    if (e.type == ModulatorType::None) {
      if (!AllZero(&e, sizeof e)) return Fail(d, PresetError::ModePadding, at);
      continue;
    }
    // Packed: the second modulator needs the first.
    if (k > 0 && m[k - 1].type == ModulatorType::None) {
      return Fail(d, PresetError::ModePadding, at);
    }
    if (e.shape >= kModulatorShapeCount) return Fail(d, PresetError::ModeEnum, at);
    if (e.sync > kMaxSyncDivision) return Fail(d, PresetError::ModeRange, at);
    if (e.pad != 0u) return Fail(d, PresetError::ModePadding, at);
    if (!CheckFloat(e.attackMs, kF0, kFAttackMax, d, PresetError::ModeValue,
                    PresetError::ModeRange, at) ||
        !CheckFloat(e.releaseMs, kF0, kFReleaseMax, d, PresetError::ModeValue,
                    PresetError::ModeRange, at)) {
      return false;
    }
  }
  return true;
}

bool CheckRoutes(const Route* entries, uint8_t count, uint8_t cap, const uint8_t* pad,
                 uint8_t fromCount, uint8_t toCount, uint32_t at, PresetDiagnostic* d) noexcept {
  if (count > cap) return Fail(d, PresetError::ModeCount, at);
  if (!AllZero(pad, 3)) return Fail(d, PresetError::ModePadding, at);
  for (uint32_t i = 0; i < cap; ++i) {
    const Route& e = entries[i];
    if (i >= count) {
      if (!AllZero(&e, sizeof e)) return Fail(d, PresetError::ModePadding, at);
      continue;
    }
    if (e.from >= fromCount || e.to >= toCount) return Fail(d, PresetError::ModeEnum, at);
    if (e.layer >= kMaxModeLayers) return Fail(d, PresetError::ModeRange, at);
    if (e.pad != 0u) return Fail(d, PresetError::ModePadding, at);
    if (!CheckFloat(e.amount, kFMinusOne, kFOne, d, PresetError::ModeValue,
                    PresetError::ModeRange, at)) {
      return false;
    }
  }
  return true;
}

bool CheckMacros(const MacroTable& t, PresetDiagnostic* d) noexcept {
  const uint32_t at = kChunkMacr;
  if (t.macroCount > kMaxMacros || t.targetCount > kMaxTargets) {
    return Fail(d, PresetError::ModeCount, at);
  }
  if (t.pad != 0u) return Fail(d, PresetError::ModePadding, at);
  uint32_t first = 0, previous = 0;
  for (uint32_t m = 0; m < kMaxMacros; ++m) {
    const MacroDef& e = t.macros[m];
    if (m >= t.macroCount) {
      if (!AllZero(&e, sizeof e)) return Fail(d, PresetError::ModePadding, at);
      continue;
    }
    // Macro rows 69-76, strictly ascending (unique by ID, E8), each `first` the running sum.
    if (e.id < kFirstMacroId || e.id > kLastMacroId || (m > 0 && e.id <= previous)) {
      return Fail(d, PresetError::MacroLayout, e.id);
    }
    if (e.count > kMaxMacroTargets) return Fail(d, PresetError::ModeCount, e.id);
    if (e.first != first) return Fail(d, PresetError::MacroLayout, e.id);
    if (e.pad != 0u) return Fail(d, PresetError::ModePadding, at);
    previous = e.id;
    first += e.count;
  }
  if (first != t.targetCount) return Fail(d, PresetError::MacroLayout, at);
  for (uint32_t i = 0; i < kMaxTargets; ++i) {
    const MacroTarget& e = t.targets[i];
    if (i >= t.targetCount) {
      if (!AllZero(&e, sizeof e)) return Fail(d, PresetError::ModePadding, at);
      continue;
    }
    const uint32_t lo = BitsOf(e.lo), hi = BitsOf(e.hi), inLo = BitsOf(e.inLo),
                   inHi = BitsOf(e.inHi), curve = BitsOf(e.curve);
    if (!Canonical(lo) || !Canonical(hi) || !Canonical(inLo) || !Canonical(inHi) ||
        !Canonical(curve)) {
      return Fail(d, PresetError::ModeValue, e.param);
    }
    // E9's table-free half: 0 <= in_lo < in_hi <= 1 and the curve in [1/16, 16].
    if (!InRange(inLo, kF0, kFOne) || !InRange(inHi, kF0, kFOne) || !Less(inLo, inHi) ||
        !InRange(curve, kFCurveMin, kFCurveMax)) {
      return Fail(d, PresetError::ModeRange, e.param);
    }
  }
  return true;
}

// Round half away from zero of a canonical value clamped to [1, 64] (§3.7), from its bits.
uint32_t RoundedVoiceCount(uint32_t b) noexcept {
  if ((b & 0x80000000u) != 0u || OrderKey(b) < OrderKey(kFOne)) return 1;
  const uint32_t exponent = (b >> 23) & 0xFFu;
  if (exponent >= 127u + 6u) return 64;  // 64 and above
  const uint32_t e = exponent - 127u;  // 0-5
  const uint32_t m = (b & 0x7FFFFFu) | 0x800000u;
  return (m >> (23u - e)) + ((m >> (22u - e)) & 1u);
}

// a + b <= 1 for two canonical values in (0, 1], exactly: each as an integer scaled by 2^149.
bool SharesFit(uint32_t a, uint32_t b) noexcept {
  uint64_t       sum[3]    = {0, 0, 0};
  const uint32_t values[2] = {a, b};
  for (const uint32_t v : values) {
    const uint32_t exponent = (v >> 23) & 0xFFu;  // >= 1: canonical, nonzero
    uint64_t       part[3]  = {(v & 0x7FFFFFu) | 0x800000u, 0, 0};
    for (uint32_t s = exponent - 1u; s > 0;) {  // shift left by exponent - 1 (<= 126)
      const uint32_t k = s > 63u ? 63u : s;
      part[2]          = (part[2] << k) | (part[1] >> (64u - k));
      part[1]          = (part[1] << k) | (part[0] >> (64u - k));
      part[0] <<= k;
      s -= k;
    }
    uint64_t carry = 0;
    for (int i = 0; i < 3; ++i) {
      const uint64_t t = sum[i] + part[i];
      const uint64_t c = t < sum[i] ? 1u : 0u;
      sum[i]           = t + carry;
      carry            = c | (sum[i] < t ? 1u : 0u);
    }
  }
  // 1 scaled by 2^149: bit 149 = word 2, bit 21.
  const uint64_t one2 = uint64_t{1} << 21;
  return sum[2] < one2 || (sum[2] == one2 && sum[1] == 0u && sum[0] == 0u);
}

const ParamDescriptor* Row(uint32_t id) noexcept { return FindParam(static_cast<ParamId>(id)); }

// A Reserved row is a later wave's ID: a target on it is newer content than this build plays,
// not a corrupt one (§5.2), so it is named as unsupported, as an unknown chunk is.
bool Reserved(const ParamDescriptor* r) noexcept {
  return r != nullptr && r->kind == ParamKind::Reserved;
}

bool WithinRow(const ParamDescriptor& r, const float& v) noexcept {
  return InRange(BitsOf(v), BitsOf(r.min), BitsOf(r.max));
}

// The largest rounded value voice_count leaf `id` can take from what the state stores: its
// leaf (or default) and every macro and expression end that targets it.
uint32_t VoiceCountMax(const PresetState& s, uint32_t id) noexcept {
  const ParamDescriptor* r = Row(id);
  uint32_t               v = r != nullptr ? BitsOf(r->def) : kFOne;
  for (uint32_t i = 0; i < s.leafCount && i < PresetState::kMaxLeaves; ++i) {
    if (s.leaves[i].id == id) v = BitsOf(s.leaves[i].value);
  }
  uint32_t best = RoundedVoiceCount(v);
  const MacroTable& t = s.mode.macros;
  for (uint32_t i = 0; i < t.targetCount && i < kMaxTargets; ++i) {
    if (t.targets[i].param != id) continue;
    const uint32_t lo = RoundedVoiceCount(BitsOf(t.targets[i].lo));
    const uint32_t hi = RoundedVoiceCount(BitsOf(t.targets[i].hi));
    if (lo > best) best = lo;
    if (hi > best) best = hi;
  }
  for (uint32_t i = 0; i < s.control.exprCount && i < kMaxExpressions; ++i) {
    const ExpressionAssignment& e = s.control.expressions[i];
    if (e.target != id) continue;
    const uint32_t lo = RoundedVoiceCount(BitsOf(e.lo)), hi = RoundedVoiceCount(BitsOf(e.hi));
    if (lo > best) best = lo;
    if (hi > best) best = hi;
  }
  return best;
}

bool CheckSemantics(const PresetState& s, PresetDiagnostic* d) noexcept {
  const ModeBlob&   mode = s.mode;
  const MacroTable& t    = mode.macros;
  // E8, E9: each macro's targets are distinct Leaf rows of present elements, not global.mix,
  // with both range ends inside the target's range. A later wave's leaf (a Reserved row) is
  // unsupported rather than wrong.
  for (uint32_t m = 0; m < t.macroCount; ++m) {
    const MacroDef& def = t.macros[m];
    for (uint32_t i = def.first; i < static_cast<uint32_t>(def.first) + def.count; ++i) {
      const MacroTarget&     target = t.targets[i];
      const ParamDescriptor* r      = Row(target.param);
      if (target.param == static_cast<uint32_t>(ParamId::Mix)) {
        return Fail(d, PresetError::TargetMix, target.param);
      }
      if (Reserved(r)) return Fail(d, PresetError::UnsupportedTarget, target.param);
      if (r == nullptr || r->kind != ParamKind::Leaf) {
        return Fail(d, PresetError::TargetNotLeaf, target.param);
      }
      if (!ElementPresent(mode, target.param)) {
        return Fail(d, PresetError::TargetAbsent, target.param);
      }
      for (uint32_t j = def.first; j < i; ++j) {
        if (t.targets[j].param == target.param) {
          return Fail(d, PresetError::TargetDuplicate, target.param);
        }
      }
      if (!WithinRow(*r, target.lo) || !WithinRow(*r, target.hi)) {
        return Fail(d, PresetError::TargetRange, target.param);
      }
    }
    // E10: a written shape macro has a target.
    if (def.id == static_cast<uint32_t>(ParamId::MacroShape) && def.count == 0u) {
      return Fail(d, PresetError::ShapeEmpty, def.id);
    }
  }
  // E8: a step's ratio index names an entry of the pitch set (layer 0's, until W2 decides
  // otherwise), and route and link endpoints exist.
  for (uint32_t i = 0; i < mode.steps.countMax; ++i) {
    if (mode.steps.entries[i].ratioIdx >= mode.pitch[0].count) {
      return Fail(d, PresetError::StepRatioIndex, i);
    }
  }
  uint32_t modulators = 0;
  while (modulators < kMaxModulators && mode.modulators[modulators].type != ModulatorType::None) {
    ++modulators;
  }
  for (uint32_t i = 0; i < mode.routes.count; ++i) {
    const Route& r = mode.routes.entries[i];
    if (r.from >= modulators || r.layer >= mode.schedule.layerCount) {
      return Fail(d, PresetError::RouteEndpoint, i);
    }
  }
  for (uint32_t i = 0; i < mode.links.count; ++i) {
    const Route& r = mode.links.entries[i];
    if (r.from == r.to || r.layer >= mode.schedule.layerCount) {
      return Fail(d, PresetError::RouteEndpoint, 0x100u + i);
    }
  }
  // E11: two layers share the voices and the slots. (Their fit in the memory tiers is W3's.)
  if (mode.schedule.layerCount == 2u) {
    if (!SharesFit(BitsOf(mode.layers[0].slotShare), BitsOf(mode.layers[1].slotShare))) {
      return Fail(d, PresetError::LayerBudget, 0);
    }
    if (VoiceCountMax(s, static_cast<uint32_t>(ParamId::VoiceCount)) +
            VoiceCountMax(s, static_cast<uint32_t>(ParamId::L1VoiceCount)) >
        64u) {
      return Fail(d, PresetError::LayerBudget, 1);
    }
  }
  // The leaves of absent elements hold their defaults (§2.2). Bounded by the array too: a
  // load's step 0 checks the structure of a state whose STAT CheckStat has not seen.
  for (uint32_t i = 0; i < s.leafCount && i < PresetState::kMaxLeaves; ++i) {
    const PresetLeaf& leaf = s.leaves[i];
    if (ElementPresent(mode, leaf.id)) continue;
    const ParamDescriptor* r = Row(leaf.id);
    if (r != nullptr && BitsOf(leaf.value) != BitsOf(r->def)) {
      return Fail(d, PresetError::AbsentLeaf, leaf.id);
    }
  }
  // An expression's leaf target must be a leaf of a present element, as a macro's (E8): an
  // absent element's leaves hold their defaults.
  for (uint32_t i = 0; i < s.control.exprCount; ++i) {
    const ExpressionAssignment& e = s.control.expressions[i];
    const ParamDescriptor*      r = Row(e.target);
    if (r != nullptr && r->kind == ParamKind::Leaf && !ElementPresent(mode, e.target)) {
      return Fail(d, PresetError::ExpressionTarget, e.target);
    }
  }
  return true;
}

}  // namespace

bool CheckStat(const PresetState& s, PresetDiagnostic* d) noexcept {
  if (s.leafCount > PresetState::kMaxLeaves) return Fail(d, PresetError::StatCount);
  for (uint32_t i = 0; i < PresetState::kMaxLeaves; ++i) {
    const PresetLeaf& leaf = s.leaves[i];
    // Nothing past the count, as in CTRL and MACR: one state, one encoding (§5.2).
    if (i >= s.leafCount) {
      if (!AllZero(&leaf, sizeof leaf)) return Fail(d, PresetError::StatPadding, i);
      continue;
    }
    if (i > 0 && leaf.id <= s.leaves[i - 1].id) return Fail(d, PresetError::StatOrder, leaf.id);
    if (!Canonical(BitsOf(leaf.value))) return Fail(d, PresetError::StatValue, leaf.id);
  }
  const PerformanceState& p = s.performance;
  if (p.reverse > 1u || static_cast<uint8_t>(p.timeMode) >= kTimeModeCount ||
      static_cast<uint8_t>(p.subdiv) >= kSubdivisionCount ||
      static_cast<uint8_t>(p.tempoSource) >= kTempoSourceCount ||
      p.usPerQuarter < kMinUsPerQuarter || p.usPerQuarter > kMaxUsPerQuarter) {
    return Fail(d, PresetError::Performance);
  }
  return true;
}

bool CheckMode(const ModeBlob& mode, uint32_t supported, PresetDiagnostic* d) noexcept {
  // Declared features first: a package from a build with more is UnsupportedFeature, named,
  // whatever else its content holds.
  const uint32_t lacking = mode.features & ~(supported & kModeFeatureAll);
  if (lacking != 0u) return Fail(d, PresetError::UnsupportedFeature, lacking);
  if (!CheckSchedule(mode.schedule, d)) return false;
  const uint32_t layers = mode.schedule.layerCount;
  for (uint32_t l = 0; l < kMaxModeLayers; ++l) {
    if (l >= layers) {
      if (!SameBytes(&mode.layers[l], &kAbsentModeLayer, sizeof(ModeLayer)) ||
          !AllZero(&mode.pitch[l], sizeof(PitchSet))) {
        return Fail(d, PresetError::ModePadding, kChunkLayr);
      }
      continue;
    }
    if (!CheckLayer(mode.layers[l], d) || !CheckPitchSet(mode.pitch[l], d)) return false;
  }
  if (!CheckSteps(mode.steps, d) || !CheckModulators(mode.modulators, d) ||
      !CheckRoutes(mode.routes.entries, mode.routes.count, kMaxRoutes, mode.routes.pad,
                   kRouteSourceCount, kRouteDestinationCount, kChunkRout, d) ||
      !CheckRoutes(mode.links.entries, mode.links.count, kMaxLinks, mode.links.pad,
                   kLinkDrawCount, kLinkDrawCount, kChunkLink, d)) {
    return false;
  }
  if (!CheckFloat(mode.dryDuck.attackMs, kFDuckAtkMin, kFDuckAtkMax, d, PresetError::ModeValue,
                  PresetError::ModeRange, kChunkDuck) ||
      !CheckFloat(mode.dryDuck.releaseMs, kFDuckRelMin, kFDuckRelMax, d, PresetError::ModeValue,
                  PresetError::ModeRange, kChunkDuck)) {
    return false;
  }
  if (!CheckMacros(mode.macros, d)) return false;
  const uint32_t required = RequiredModeFeatures(mode);
  if (required != mode.features) {
    return Fail(d, PresetError::FeatureMismatch, required ^ mode.features);
  }
  return true;
}

bool CheckControl(const ModeBlob& mode, const ControlState& c, PresetDiagnostic* d) noexcept {
  if (c.present > 1u) return Fail(d, PresetError::CtrlValue);
  if (c.present == 0u) {
    if (!AllZero(&c, sizeof c)) return Fail(d, PresetError::CtrlPadding);
    return true;
  }
  if (c.pad != 0u) return Fail(d, PresetError::CtrlPadding);
  if (c.exprCount > kMaxExpressions) return Fail(d, PresetError::CtrlCount);
  // One position per macro MACR defines, by ascending id, each canonical in [0, 1].
  if (c.macroCount != mode.macros.macroCount) return Fail(d, PresetError::CtrlPositions);
  for (uint32_t m = 0; m < kMaxMacros; ++m) {
    const MacroPosition& p = c.positions[m];
    if (m >= c.macroCount) {
      if (!AllZero(&p, sizeof p)) return Fail(d, PresetError::CtrlPadding);
      continue;
    }
    if (p.macroId != mode.macros.macros[m].id) {
      return Fail(d, PresetError::CtrlPositions, p.macroId);
    }
    if (!CheckFloat(p.position, kF0, kFOne, d, PresetError::CtrlValue, PresetError::CtrlValue,
                    p.macroId)) {
      return false;
    }
  }
  for (uint32_t i = 0; i < kMaxExpressions; ++i) {
    const ExpressionAssignment& e = c.expressions[i];
    if (i >= c.exprCount) {
      if (!AllZero(&e, sizeof e)) return Fail(d, PresetError::CtrlPadding);
      continue;
    }
    if (!Canonical(BitsOf(e.lo)) || !Canonical(BitsOf(e.hi)) ||
        !CheckFloat(e.curve, kFCurveMin, kFCurveMax, d, PresetError::CtrlValue,
                    PresetError::CtrlValue, e.target)) {
      return Fail(d, PresetError::CtrlValue, e.target);
    }
    // A Leaf or Macro row of this build, both ends within its range; a later wave's leaf (a
    // Reserved row) is unsupported rather than wrong.
    const ParamDescriptor* r = Row(e.target);
    if (Reserved(r)) return Fail(d, PresetError::UnsupportedTarget, e.target);
    if (r == nullptr || (r->kind != ParamKind::Leaf && r->kind != ParamKind::Macro)) {
      return Fail(d, PresetError::ExpressionTarget, e.target);
    }
    if (!WithinRow(*r, e.lo) || !WithinRow(*r, e.hi)) {
      return Fail(d, PresetError::ExpressionRange, e.target);
    }
  }
  return true;
}

bool ValidateModeWith(const PresetState& s, uint32_t supported, PresetDiagnostic* d) noexcept {
  if (d != nullptr) *d = PresetDiagnostic{};
  return CheckStat(s, d) && CheckMode(s.mode, supported, d) && CheckControl(s.mode, s.control, d) &&
         CheckSemantics(s, d);
}

bool ValidateStructure(const PresetState& s, uint32_t supported, PresetDiagnostic* d) noexcept {
  if (d != nullptr) *d = PresetDiagnostic{};
  return CheckMode(s.mode, supported, d) && CheckControl(s.mode, s.control, d) &&
         CheckSemantics(s, d);
}

}  // namespace blob

bool ValidateMode(const PresetState& state, PresetDiagnostic* diagnostic) noexcept {
  return blob::ValidateModeWith(state, kSupportedModeFeatures, diagnostic);
}

}  // namespace brainscape
