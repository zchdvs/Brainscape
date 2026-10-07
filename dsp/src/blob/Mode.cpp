#include "detail/FpProfilePrivate.h"

#include "brainscape/Mode.h"

#include "Blob.h"

// What a mode's content requires (docs/design/mode-compiler.md §5.3: `features` equal to what
// the content requires), and which elements exist. Integer-only: floats are compared as bits.
namespace brainscape {

namespace blob {

bool PitchSetsDefault(const ModeBlob& mode) noexcept {
  const uint32_t layers = mode.schedule.layerCount <= kMaxModeLayers ? mode.schedule.layerCount
                                                                     : kMaxModeLayers;
  for (uint32_t l = 0; l < layers; ++l) {
    if (!SameBytes(&mode.pitch[l], &kDefaultPitchSet, sizeof(PitchSet))) return false;
  }
  return true;
}

bool DryDuckDefault(const DryDuck& duck) noexcept {
  return BitsOf(duck.attackMs) == kFDuckAtkDef && BitsOf(duck.releaseMs) == kFDuckRelDef;
}

namespace {

bool HasOp(const ModeLayer& layer, ModifierOp op) noexcept {
  return layer.modifier[0] == op || layer.modifier[1] == op;
}

uint32_t LayerFeatures(const ModeLayer& layer) noexcept {
  uint32_t f = 0;
  switch (layer.source) {
    case PositionSource::Live: break;
    case PositionSource::Mark: f |= kModeFeatureMarkPosition; break;
    case PositionSource::Pin: f |= kModeFeaturePinPosition; break;
    case PositionSource::Grid: f |= kModeFeatureGridPosition; break;
  }
  if (layer.baseSync != 0u) f |= kModeFeatureTempoSync;
  if (layer.sprayLaw != SprayLaw::Uniform) f |= kModeFeatureExpSpray;
  if (layer.markIndex != 0u || layer.markWalk != MarkWalk::None ||
      BitsOf(layer.markJitter) != kF0) {
    f |= kModeFeatureMarkWalk;
  }
  if (layer.pinRearm != PinRearm::Off || BitsOf(layer.pinRearmMs) != kFRearmDef) {
    f |= kModeFeaturePinPosition;
  }
  if (layer.pitchSelect != PitchSelect::Cycle) f |= kModeFeaturePitchSet;
  if (layer.quantize != QuantizeMode::Off || layer.quantizeRoot != 0u || layer.scaleMask != 0u) {
    f |= kModeFeatureQuantize;
  }
  if (HasOp(layer, ModifierOp::Svf) || layer.svfBand != SvfBand::Lowpass ||
      layer.svfCutoffSource != CutoffSource::Fixed) {
    f |= kModeFeatureSvf;
  }
  if (HasOp(layer, ModifierOp::Crush)) f |= kModeFeatureCrush;
  if (BitsOf(layer.slotShare) != kFOne) f |= kModeFeatureTwoLayers;
  if (BitsOf(layer.glideStStart) != kF0 || BitsOf(layer.glideStEnd) != kF0) {
    f |= kModeFeatureGlide;
  }
  return f;
}

}  // namespace

bool ElementPresent(const ModeBlob& mode, uint32_t id) noexcept {
  const bool twoLayers = mode.schedule.layerCount >= 2u;
  switch (static_cast<ParamId>(id)) {
    case ParamId::SvfCutoffHz:
    case ParamId::SvfRes: return HasOp(mode.layers[0], ModifierOp::Svf);
    case ParamId::CrushBits:
    case ParamId::CrushDownsample: return HasOp(mode.layers[0], ModifierOp::Crush);
    case ParamId::L1SvfCutoffHz:
    case ParamId::L1SvfRes: return twoLayers && HasOp(mode.layers[1], ModifierOp::Svf);
    case ParamId::L1CrushBits:
    case ParamId::L1CrushDownsample: return twoLayers && HasOp(mode.layers[1], ModifierOp::Crush);
    case ParamId::LayerMix: return twoLayers;  // the balance of two layers
    case ParamId::StepCount: return mode.steps.countMax > 0u;
    case ParamId::Modulator0RateHz:
    case ParamId::Modulator0Depth: return mode.modulators[0].type != ModulatorType::None;
    case ParamId::Modulator1RateHz:
    case ParamId::Modulator1Depth: return mode.modulators[1].type != ModulatorType::None;
    default: break;
  }
  if (id >= static_cast<uint32_t>(ParamId::L1DelayMs) &&
      id <= static_cast<uint32_t>(ParamId::L1CrushDownsample)) {
    return twoLayers;
  }
  return true;
}

}  // namespace blob

uint32_t RequiredModeFeatures(const ModeBlob& mode) noexcept {
  using namespace blob;
  uint32_t       f       = 0;
  const uint8_t  sources = mode.schedule.sources;
  if ((sources & kSourceOnset) != 0u) f |= kModeFeatureOnset;
  if ((sources & kSourceClock) != 0u) f |= kModeFeatureClock;
  if ((sources & kDefaultSources) != kDefaultSources) f |= kModeFeatureSources;
  if (mode.schedule.subdiv != Subdivision::Quarter) f |= kModeFeatureClock;
  if (mode.schedule.stepOrder != StepOrder::Fixed || mode.steps.countMax != 0u) {
    f |= kModeFeatureSteps;
  }
  if (mode.schedule.layerCount >= 2u) f |= kModeFeatureTwoLayers;
  const uint32_t layers = mode.schedule.layerCount <= kMaxModeLayers ? mode.schedule.layerCount
                                                                     : kMaxModeLayers;
  for (uint32_t l = 0; l < layers; ++l) f |= LayerFeatures(mode.layers[l]);
  if (!PitchSetsDefault(mode)) f |= kModeFeaturePitchSet;
  if (mode.modulators[0].type != ModulatorType::None ||
      mode.modulators[1].type != ModulatorType::None) {
    f |= kModeFeatureModulators;
  }
  if (mode.routes.count != 0u) f |= kModeFeatureRoutes;
  if (mode.links.count != 0u) f |= kModeFeatureLinks;
  if (!DryDuckDefault(mode.dryDuck)) f |= kModeFeatureDryDuck;
  return f;
}

}  // namespace brainscape
