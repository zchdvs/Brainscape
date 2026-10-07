#pragma once
#include <cstddef>
#include <cstdint>

#include "brainscape/FpProfile.h"

namespace brainscape {

// The compiled mode: a preset's structure, decoded from the package's MODE section
// (docs/design/mode-compiler.md §5.1, §6.2). Fixed-capacity data with fixed-width members and
// `enum class : uint8_t` only (no pointer, bool, size_t, long or plain enum, whose sizes differ
// between x64 and the Cortex-M7), explicit zero padding and no implicit padding, so its layout
// is the same on every target (static_asserts below) and two equal modes are equal word for
// word. It is never copied to bytes or hashed as a struct: the encoder writes it field by
// field (dsp/src/blob/Encode.cpp) and modeHash is the SHA-256 of those bytes.
//
// The structs mirror MODE's chunks at their caps. Entries past every count are zero, as is
// every pad field and every field of an absent layer, and the member initializers are the
// schema-default mode (§5.1), which plays as sound revision 1 does: sources {periodic,
// footswitch, midi_note}, one live layer, the pitch set {0: 1} and the default macros of §3.2.
// A leaf-only producer (the plugin, the audition render, the golden harness) therefore plays
// what it played before modes existed.
//
// Fields marked with a wave (W1-W3, §1.2) are vocabulary this build cannot play yet: content
// that uses them requires a feature bit (kModeFeature*) that DecodePreset and ValidateMode
// reject as UnsupportedFeature until the pull request that builds it, which may still change
// their exact vocabulary (§1.4 principle 5).

// ── Caps (E7) ─────────────────────────────────────────────────────────────────────────────
inline constexpr uint32_t kMaxModeLayers   = 2;
inline constexpr uint32_t kMaxPitchEntries = 8;
inline constexpr uint32_t kMaxSteps        = 16;
inline constexpr uint32_t kMaxModulators   = 2;
inline constexpr uint32_t kMaxRoutes       = 8;
inline constexpr uint32_t kMaxLinks        = 4;
inline constexpr uint32_t kMaxMacros       = 8;
inline constexpr uint32_t kMaxMacroTargets = 8;   // per macro
inline constexpr uint32_t kMaxTargets      = 32;  // over every macro
inline constexpr uint32_t kMaxMarkIndex    = 15;  // the onset mark ring holds 16 (Granular.h)

// ── Feature bits (MODE's `features`) ──────────────────────────────────────────────────────
// What a mode's content requires of the engine beyond the default structure. A package
// declares exactly what its content requires (RequiredModeFeatures); a build plays the bits of
// kSupportedModeFeatures. Macro definitions are not a feature: they are 3a's pipeline, which
// the engine reads only through MacroMove events, so any valid set of them is accepted.
inline constexpr uint32_t kModeFeatureOnset        = 1u << 0;   // 3a (r2): `onset` in the sources
inline constexpr uint32_t kModeFeatureMarkPosition = 1u << 1;   // 3a (r2): a layer on marks
inline constexpr uint32_t kModeFeatureSources      = 1u << 2;   // W1: a default source left out
inline constexpr uint32_t kModeFeaturePitchSet     = 1u << 3;   // W1: a set other than {0: 1}
inline constexpr uint32_t kModeFeatureClock        = 1u << 4;   // W2: `clock`, a subdivision
inline constexpr uint32_t kModeFeatureSteps        = 1u << 5;   // W2: a step table or order
inline constexpr uint32_t kModeFeatureMarkWalk     = 1u << 6;   // W2: mark index, walk, jitter
inline constexpr uint32_t kModeFeatureTempoSync    = 1u << 7;   // W2: a synced base delay
inline constexpr uint32_t kModeFeatureTwoLayers    = 1u << 8;   // W3: a second layer, slot share
inline constexpr uint32_t kModeFeaturePinPosition  = 1u << 9;   // W3: `pin` and its re-arm
inline constexpr uint32_t kModeFeatureGridPosition = 1u << 10;  // W3: `grid` positioning
inline constexpr uint32_t kModeFeatureExpSpray     = 1u << 11;  // W3: the `exp` spray law
inline constexpr uint32_t kModeFeatureQuantize     = 1u << 12;  // W3: scale quantization
inline constexpr uint32_t kModeFeatureGlide        = 1u << 13;  // W3: glide endpoints
inline constexpr uint32_t kModeFeatureSvf          = 1u << 14;  // W3: the per-voice SVF
inline constexpr uint32_t kModeFeatureCrush        = 1u << 15;  // W3: the per-voice bit-crush
inline constexpr uint32_t kModeFeatureModulators   = 1u << 16;  // W3: modulators
inline constexpr uint32_t kModeFeatureRoutes       = 1u << 17;  // W3: modulation routes
inline constexpr uint32_t kModeFeatureLinks        = 1u << 18;  // W3: links between draws
inline constexpr uint32_t kModeFeatureDryDuck      = 1u << 19;  // W3: dry-duck envelope times
inline constexpr uint32_t kModeFeatureAll          = (1u << 20) - 1u;  // every defined bit

// The features this build plays: sound revision 2's onset source and mark positioning (§7.6
// item 4, the structure rows 27 and 28 retired into), and wave 1's source selection (sound
// revision 4, §7.5 R9). Each wave-1 feature widens it in its own pull request.
inline constexpr uint32_t kSupportedModeFeatures =
    kModeFeatureOnset | kModeFeatureMarkPosition | kModeFeatureSources;

// ── SCHD: the scheduler ───────────────────────────────────────────────────────────────────
// The trigger sources, a set of bits.
inline constexpr uint8_t kSourcePeriodic   = 1u << 0;  // the free-running scheduler (left out: r4)
inline constexpr uint8_t kSourceClock      = 1u << 1;  // W2
inline constexpr uint8_t kSourceOnset      = 1u << 2;  // 3a (r2): a burst per detected onset (r4)
inline constexpr uint8_t kSourceFootswitch = 1u << 3;  // footswitch triggers fire (left out: r4)
inline constexpr uint8_t kSourceMidiNote   = 1u << 4;  // MIDI-note triggers fire (left out: r4)
inline constexpr uint8_t kSourceAll        = 0x1Fu;
// Sound revision 1's sources: the scheduler runs, and footswitch and MIDI notes trigger.
inline constexpr uint8_t kDefaultSources = kSourcePeriodic | kSourceFootswitch | kSourceMidiNote;

enum class Subdivision : uint8_t { Quarter, Half, Tap, Double, Quadruple, Octuple };  // 1/4 .. 8x
inline constexpr uint8_t kSubdivisionCount = 6;
enum class StepOrder : uint8_t { Fixed, Shuffle, Random };
inline constexpr uint8_t kStepOrderCount = 3;

struct ModeSchedule {  // SCHD, 8 bytes
  uint8_t     sources    = kDefaultSources;
  uint8_t     layerCount = 1;                     // 1, or 2 (W3)
  Subdivision subdiv     = Subdivision::Quarter;  // W2
  StepOrder   stepOrder  = StepOrder::Fixed;      // W2
  uint8_t     pad[4]     = {};
};

// ── LAYR: one layer's structure ───────────────────────────────────────────────────────────
enum class PositionSource : uint8_t { Live, Mark, Pin, Grid };
inline constexpr uint8_t kPositionSourceCount = 4;
enum class SprayLaw : uint8_t { Uniform, Exp };
inline constexpr uint8_t kSprayLawCount = 2;
enum class MarkWalk : uint8_t { None, Cascade, Random };
inline constexpr uint8_t kMarkWalkCount = 3;
enum class PinRearm : uint8_t { Off, Time, Onset, Manual };
inline constexpr uint8_t kPinRearmCount = 4;
enum class PitchSelect : uint8_t { Cycle, Random };
inline constexpr uint8_t kPitchSelectCount = 2;
enum class QuantizeMode : uint8_t { Off, Scale };
inline constexpr uint8_t kQuantizeModeCount = 2;
enum class ModifierOp : uint8_t { None, Svf, Crush };
inline constexpr uint8_t kModifierOpCount = 3;
enum class SvfBand : uint8_t { Lowpass, Bandpass, Highpass, Notch };
inline constexpr uint8_t kSvfBandCount = 4;
enum class CutoffSource : uint8_t { Fixed, Random, Lfo, Envelope };
inline constexpr uint8_t kCutoffSourceCount = 4;
inline constexpr uint8_t kMaxSyncDivision   = 16;  // base_sync: 0 off, then divisions (W2)
inline constexpr uint16_t kScaleMaskAll     = 0x0FFFu;

struct ModeLayer {  // LAYR entry, 36 bytes: 13 enumerations and a pad, a mask, five floats
  PositionSource source          = PositionSource::Live;
  uint8_t        baseSync        = 0;  // W2: 0 off, 1-16 a tempo division
  SprayLaw       sprayLaw        = SprayLaw::Uniform;
  uint8_t        markIndex       = 0;  // W2: 0-15
  MarkWalk       markWalk        = MarkWalk::None;
  PinRearm       pinRearm        = PinRearm::Off;
  PitchSelect    pitchSelect     = PitchSelect::Cycle;  // W1
  QuantizeMode   quantize        = QuantizeMode::Off;   // W3
  uint8_t        quantizeRoot    = 0;                   // W3: 0-11
  ModifierOp     modifier[2]     = {ModifierOp::None, ModifierOp::None};  // W3, distinct ops
  SvfBand        svfBand         = SvfBand::Lowpass;     // W3, with an SVF
  CutoffSource   svfCutoffSource = CutoffSource::Fixed;  // W3, with an SVF
  uint8_t        pad             = 0;
  uint16_t       scaleMask       = 0;        // W3: one bit per pitch class, with quantization
  float          slotShare       = 1.0f;     // W3: (0, 1]; two layers sum to at most 1
  float          markJitter      = 0.0f;     // W2: 0-1
  float          pinRearmMs      = 1000.0f;  // W3: 10-20000 ms
  float          glideStStart    = 0.0f;     // W3: -24..24 st
  float          glideStEnd      = 0.0f;     // W3: -24..24 st
};

// An absent layer: every field zero, as every entry past a count.
inline constexpr ModeLayer kAbsentModeLayer = {PositionSource::Live, 0, SprayLaw::Uniform, 0,
                                               MarkWalk::None, PinRearm::Off, PitchSelect::Cycle,
                                               QuantizeMode::Off, 0,
                                               {ModifierOp::None, ModifierOp::None},
                                               SvfBand::Lowpass, CutoffSource::Fixed, 0, 0,
                                               0.0f, 0.0f, 0.0f, 0.0f, 0.0f};

// ── PSET: pitch sets (W1) ─────────────────────────────────────────────────────────────────
struct PitchEntry {  // 8 bytes
  float    st     = 0.0f;  // -24..24 st, added to the transpose leaf (§7.5)
  uint16_t weight = 0;     // 1-16
  uint16_t pad    = 0;
};
struct PitchSet {  // PSET entry, 68 bytes
  uint8_t    count  = 0;  // 1-8 for a present layer
  uint8_t    pad[3] = {};
  PitchEntry entries[kMaxPitchEntries] = {};
};
// Every present layer's set without a PSET chunk: one entry, 0 st, weight 1.
inline constexpr PitchSet kDefaultPitchSet = {1, {0, 0, 0}, {PitchEntry{0.0f, 1, 0}}};

// ── STEP: the step table (W2) ─────────────────────────────────────────────────────────────
inline constexpr uint8_t kStepFlagReverse = 1u << 0;  // the only flag; other bits are 0
struct StepEntry {  // 16 bytes
  uint8_t slot     = 0;  // 0-15
  uint8_t ratioIdx = 0;  // 0-7, below the pitch set's count
  uint8_t flags    = 0;
  uint8_t pad      = 0;
  float   posSel   = 0.0f;  // per position source: ms (live), a mark index (mark), a slice
  float   gain     = 0.0f;  // 0-1
  float   prob     = 0.0f;  // 0-1
};
struct StepTable {  // STEP, 260 bytes; present when countMax > 0
  uint8_t   countMax = 0;  // entries, 1-16 (the leaf scheduler.steps.count plays a prefix)
  uint8_t   pad[3]   = {};
  StepEntry entries[kMaxSteps] = {};
};

// ── MODS, ROUT, LINK, DUCK (W3) ───────────────────────────────────────────────────────────
enum class ModulatorType : uint8_t { None, Lfo, Envelope };
inline constexpr uint8_t kModulatorTypeCount = 3;
inline constexpr uint8_t kModulatorShapeCount = 5;  // provisional: sine, triangle, saw,
                                                    // square, random
struct Modulator {  // 12 bytes; MODS holds both, present when the first exists
  ModulatorType type = ModulatorType::None;  // modulators are packed: the second needs the first
  uint8_t       shape = 0;
  uint8_t       sync  = 0;  // 0 free, 1-16 a division
  uint8_t       pad   = 0;
  float         attackMs  = 0.0f;  // provisional ranges: 0-5000 ms
  float         releaseMs = 0.0f;  // 0-20000 ms
};

inline constexpr uint8_t kRouteSourceCount      = 2;  // provisional: modulator 0 or 1
inline constexpr uint8_t kRouteDestinationCount = 6;  // size, cutoff, ratio, position,
                                                      // layer_mix, pan
inline constexpr uint8_t kLinkDrawCount = 6;  // provisional: pitch, pan, position, size,
                                              // reverse, gain
struct Route {  // 8 bytes, a ROUT or LINK entry
  uint8_t from   = 0;
  uint8_t to     = 0;
  uint8_t layer  = 0;  // the layer it acts on (0 where it acts on both)
  uint8_t pad    = 0;
  float   amount = 0.0f;  // -1..1
};
struct RouteTable {  // ROUT, 4 + 8n bytes; present when count > 0
  uint8_t count  = 0;
  uint8_t pad[3] = {};
  Route   entries[kMaxRoutes] = {};
};
struct LinkTable {  // LINK, 4 + 8n bytes; present when count > 0
  uint8_t count  = 0;
  uint8_t pad[3] = {};
  Route   entries[kMaxLinks] = {};
};
struct DryDuck {  // DUCK, 8 bytes; present when not the defaults
  float attackMs  = 5.0f;   // 0.1-500 ms
  float releaseMs = 80.0f;  // 1-5000 ms
};

// ── MACR: macro definitions (3a) ──────────────────────────────────────────────────────────
struct MacroDef {  // 8 bytes
  uint32_t id    = 0;  // a Macro row, 69-76, ascending
  uint8_t  first = 0;  // its first target: the sum of the counts before it
  uint8_t  count = 0;  // 0-8 targets
  uint16_t pad   = 0;
};
struct MacroTarget {  // 24 bytes
  uint32_t param = 0;     // a Leaf row of a present element, not global.mix (E8)
  float    lo    = 0.0f;  // within the target's range; lo > hi reverses the macro (E9)
  float    hi    = 0.0f;
  float    inLo  = 0.0f;  // in_range: 0 <= inLo < inHi <= 1 (E9)
  float    inHi  = 0.0f;
  float    curve = 0.0f;  // the power exponent, 1/16-16 (E9)
};
struct MacroTable {  // MACR, 4 + 8 macros + 24 targets bytes
  uint8_t     macroCount  = 0;
  uint8_t     targetCount = 0;
  uint16_t    pad         = 0;
  MacroDef    macros[kMaxMacros]   = {};
  MacroTarget targets[kMaxTargets] = {};
};
// §3.2's defaults: activity, repeats, shape, time, space and filter; aux1 and aux2 undefined.
inline constexpr MacroTable kDefaultMacroTable = {
    6,
    9,
    0,
    {MacroDef{69, 0, 2, 0}, MacroDef{70, 2, 1, 0}, MacroDef{71, 3, 2, 0}, MacroDef{72, 5, 1, 0},
     MacroDef{73, 6, 2, 0}, MacroDef{74, 8, 1, 0}},
    {MacroTarget{6, 0.25f, 0.85f, 0.0f, 1.0f, 1.0f},      // activity: scheduler.overlap
     MacroTarget{7, 0.0f, 200.0f, 0.0f, 1.0f, 2.0f},      //   layer0.position.spray_ms
     MacroTarget{3, 0.0f, 0.9f, 0.0f, 1.0f, 1.0f},        // repeats: feedback.amount
     MacroTarget{12, 0.9f, 0.1f, 0.0f, 1.0f, 1.0f},       // shape: layer0.window.sustain
     MacroTarget{13, 0.3f, 0.7f, 0.0f, 1.0f, 1.0f},       //   layer0.window.skew
     MacroTarget{1, 20.0f, 2000.0f, 0.0f, 1.0f, 2.0f},    // time: layer0.position.base_ms
     MacroTarget{20, 0.0f, 0.5f, 0.0f, 1.0f, 1.0f},       // space: post.delay.mix
     MacroTarget{22, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f},       //   post.reverb.mix
     MacroTarget{23, 40.0f, 20000.0f, 0.0f, 1.0f, 4.0f}}};  // filter: post.filter.cutoff_hz

// A SHA-256 digest, as data.
struct Digest32 {
  uint8_t bytes[32] = {};
};

// SHA-256 of the default mode's MODE payload (the member initializers below, encoded):
// checked by test_blob.cpp, and by the compiler's encoding of an empty document (lane A).
inline constexpr Digest32 kDefaultModeHash = {
    {0x02, 0x30, 0xc1, 0xaf, 0xf5, 0xc4, 0xaf, 0x6d, 0xe5, 0xa8, 0x17, 0x0f,
     0xd9, 0x58, 0xba, 0xad, 0xf6, 0x6b, 0x0b, 0xc2, 0x75, 0x74, 0x94, 0x0d,
     0x41, 0x0d, 0x41, 0x90, 0xaa, 0x1a, 0x22, 0xda}};

struct ModeBlob {
  uint32_t     features = 0;  // kModeFeature* bits: exactly what the content requires
  ModeSchedule schedule;
  ModeLayer    layers[kMaxModeLayers] = {ModeLayer{}, kAbsentModeLayer};
  PitchSet     pitch[kMaxModeLayers]  = {kDefaultPitchSet, PitchSet{}};
  StepTable    steps;
  Modulator    modulators[kMaxModulators];
  RouteTable   routes;
  LinkTable    links;
  DryDuck      dryDuck;
  MacroTable   macros = kDefaultMacroTable;
  // SHA-256 of the MODE payload, set by DecodePreset (§6.3): the library groups presets by it.
  // Never trusted by the engine, which compares modes by content (§7.3).
  Digest32     modeHash = kDefaultModeHash;
};

// The layout, the same on x64, arm64 and the M7 (§5.1).
static_assert(sizeof(ModeSchedule) == 8, "ModeBlob layout");
static_assert(sizeof(ModeLayer) == 36 && offsetof(ModeLayer, scaleMask) == 14 &&
                  offsetof(ModeLayer, slotShare) == 16 && offsetof(ModeLayer, glideStEnd) == 32,
              "ModeBlob layout");
static_assert(sizeof(PitchEntry) == 8 && sizeof(PitchSet) == 68, "ModeBlob layout");
static_assert(sizeof(StepEntry) == 16 && sizeof(StepTable) == 260, "ModeBlob layout");
static_assert(sizeof(Modulator) == 12 && sizeof(Route) == 8, "ModeBlob layout");
static_assert(sizeof(RouteTable) == 68 && sizeof(LinkTable) == 36, "ModeBlob layout");
static_assert(sizeof(DryDuck) == 8 && sizeof(MacroDef) == 8 && sizeof(MacroTarget) == 24 &&
                  sizeof(Digest32) == 32,
              "ModeBlob layout");
static_assert(sizeof(MacroTable) == 836, "ModeBlob layout");
static_assert(offsetof(ModeBlob, schedule) == 4 && offsetof(ModeBlob, layers) == 12 &&
                  offsetof(ModeBlob, pitch) == 84 && offsetof(ModeBlob, steps) == 220 &&
                  offsetof(ModeBlob, modulators) == 480 && offsetof(ModeBlob, routes) == 504 &&
                  offsetof(ModeBlob, links) == 572 && offsetof(ModeBlob, dryDuck) == 608 &&
                  offsetof(ModeBlob, macros) == 616 && offsetof(ModeBlob, modeHash) == 1452,
              "ModeBlob layout");
static_assert(sizeof(ModeBlob) == 1484 && alignof(ModeBlob) == 4, "ModeBlob layout");

// The features `mode`'s content requires: the `features` a valid MODE declares. Integer-only.
uint32_t RequiredModeFeatures(const ModeBlob& mode) noexcept;

}  // namespace brainscape
