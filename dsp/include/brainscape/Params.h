#pragma once
#include <cstddef>
#include <cstdint>

#include "brainscape/FpProfile.h"

namespace brainscape {

// Permanent parameter identities (docs/design/mode-compiler.md §4, grain-engine.md §6): IDs
// and names are never reused or reassigned once released; retire and rename instead. Values
// are plain (denormalized) units everywhere; normalizing to [0,1] is the plugin wrapper's job
// (VST3/CLAP convention, docs/research/preset-parameter-and-patch-format.md). A leaf's name
// is its path in the preset document: layerN.a.b lives at layers[N].a.b, any other leaf at its
// dotted path (mode-compiler.md §2.2).
//
// IDs 1-28 keep their numbers from sound revision 1. The rest are the design's final rows,
// most of them Reserved until the wave that builds them (mode-compiler.md §4.2): the table is
// complete now so that no later feature renumbers anything. Until the first public sound
// revision (step 6) names, ranges and tapers may still change in place (§1.4 principle 5).
enum class ParamId : uint32_t {
  // ── Sound revision 1's rows ───────────────────────────────────────────────────────
  DelayMs        = 1,   // layer0.position.base_ms — grain position behind the write head
  Mix            = 2,   // global wet/dry: dry at unity to the middle, wet at unity from it
                        // (sound revision 3, mode-compiler.md §7.1 R3b; was a linear crossfade)
  Feedback       = 3,   // feedback.amount (the taming chain lands with the post chain)
  WetTrimDb      = 4,   // wet_trim_db (was out_trim_db): the mode's level match, on the wet
                        // signal only, after the post chain (mode-compiler.md §7.1 R3)
  GrainSizeMs    = 5,   // layer0.size_ms
  Overlap        = 6,   // scheduler.overlap — target voices = kMaxGrains * overlap^3
  SprayMs        = 7,   // layer0.position.spray_ms
  TransposeSt    = 8,   // layer0.pitch.transpose_st (was layer0.pitch.st; ±24 st = the
                        // design's r_max = 4 ratio ceiling): since sound revision 5 an offset
                        // over the pitch set, which with the default set {0} is revision 1's
                        // pitch bit for bit
  SpreadCents    = 9,   // layer0.pitch.spread_cents
  ReverseProb    = 10,  // layer0.pitch.reverse_prob
  Jitter         = 11,  // scheduler.jitter — synchronous <-> asynchronous morph
  WindowSustain  = 12,  // layer0.window.sustain — flat-top fraction (1 = rectangular)
  WindowSkew     = 13,  // layer0.window.skew — attack/decay balance
  WindowSmooth   = 14,  // layer0.window.smoothness — piecewise -> half-cosine morph
  PanSpread      = 15,  // layer0.pan_spread — per-grain equal-power pan width
  ModRateHz      = 16,  // post.mod.rate_hz
  ModDepth       = 17,  // post.mod.depth — 0 is exactly transparent
  DelayTimeMs    = 18,  // post.delay.time_ms (the Space knob's delay half, design §2.6)
  DelayFb        = 19,  // post.delay.fb
  DelayMix       = 20,  // post.delay.mix — 0 is exactly transparent
  ReverbTime     = 21,  // post.reverb.time
  ReverbMix      = 22,  // post.reverb.mix — 0 is exactly transparent
  FilterCutoffHz = 23,  // post.filter.cutoff_hz — at max the stage is EXACTLY bypassed
                        // (design §2.6 endpoint semantics: Filter fully CW = bypass); at
                        // min the wet is killed (mode-compiler.md §7.2)
  FilterRes      = 24,  // post.filter.res
  FilterMorph    = 25,  // post.filter.morph — 0..3 continuous LP -> BP -> HP -> Notch
  TriggerSens    = 26,  // trigger.sensitivity — onset-detector threshold (1 = hair trigger)
  // Retired at sound revision 2 into mode structure (mode-compiler.md §4.2, R2b): the onset
  // trigger is `onset` in scheduler.sources (ModeSchedule::sources), mark positioning a layer's
  // position.source (ModeLayer::source). The ids are never reused; the rows keep only them.
  // A SetParam on either is a no-op and a preset leaf naming either is unknown.
  OnsetTrigger   = 27,  // was scheduler.onset_trigger
  PositionSource = 28,  // was layer0.position.source
  // ── Layer 0 (W1: 29-31; W3: 32-37) ───────────────────────────────────────────────
  Repeat          = 29,  // layer0.position.repeat: passes over one region, integer
  DecayMs         = 30,  // layer0.decay_ms: 60 dB fall as the position ages, 0 = off
  VoiceCount      = 31,  // layer0.voice_count: voices this layer may sound, integer
  LevelDb         = 32,  // layer0.level_db
  GlideCurve      = 33,  // layer0.pitch.glide.curve
  SvfCutoffHz     = 34,  // layer0.svf.cutoff_hz: the per-grain filter modifier
  SvfRes          = 35,  // layer0.svf.res
  CrushBits       = 36,  // layer0.crush.bits: the bit-crush modifier, integer
  CrushDownsample = 37,  // layer0.crush.downsample, integer
  // ── Layer 1 (W3): layer 0's leaves for the second layer ──────────────────────────
  L1DelayMs         = 38,  // layer1.position.base_ms
  L1SprayMs         = 39,  // layer1.position.spray_ms
  L1Repeat          = 40,  // layer1.position.repeat
  L1GrainSizeMs     = 41,  // layer1.size_ms
  L1DecayMs         = 42,  // layer1.decay_ms
  L1VoiceCount      = 43,  // layer1.voice_count
  L1LevelDb         = 44,  // layer1.level_db
  L1PanSpread       = 45,  // layer1.pan_spread
  L1WindowSustain   = 46,  // layer1.window.sustain
  L1WindowSkew      = 47,  // layer1.window.skew
  L1WindowSmooth    = 48,  // layer1.window.smoothness
  L1TransposeSt     = 49,  // layer1.pitch.transpose_st
  L1SpreadCents     = 50,  // layer1.pitch.spread_cents
  L1ReverseProb     = 51,  // layer1.pitch.reverse_prob
  L1GlideCurve      = 52,  // layer1.pitch.glide.curve
  L1SvfCutoffHz     = 53,  // layer1.svf.cutoff_hz
  L1SvfRes          = 54,  // layer1.svf.res
  L1CrushBits       = 55,  // layer1.crush.bits
  L1CrushDownsample = 56,  // layer1.crush.downsample
  // ── Scheduler, layers, dry duck, post (W1: 57-59; W2: 60, 63; W3: 61, 62, 64) ───────
  // Leaves since sound revision 4 (mode-compiler.md §7.5, R9): 57-59.
  Intermittency  = 57,  // scheduler.intermittency: probability a birth or trigger is skipped
  BurstCount     = 58,  // scheduler.burst.count: grains per trigger, integer
  BurstSpacingMs = 59,  // scheduler.burst.spacing_ms: between a burst's grains (0: a frame)
  StepCount      = 60,  // scheduler.steps.count, integer
  LayerMix       = 61,  // layer_mix
  DryDuckDepth   = 62,  // dry_duck.depth
  DelaySync      = 63,  // post.delay.sync: 0 off, then tempo divisions (discrete)
  ReverbMode     = 64,  // post.reverb.mode: bright room, dark medium, large hall, ambient
  // ── Modulators (W3) ──────────────────────────────────────────────────────────────
  Modulator0RateHz = 65,  // modulator0.rate_hz
  Modulator0Depth  = 66,  // modulator0.depth
  Modulator1RateHz = 67,  // modulator1.rate_hz
  Modulator1Depth  = 68,  // modulator1.depth
  // ── Macros (design §3): positions in [0, 1], moved by MacroMove events from r2 ─────
  MacroActivity = 69,  // macro.activity
  MacroRepeats  = 70,  // macro.repeats
  MacroShape    = 71,  // macro.shape
  MacroTime     = 72,  // macro.time
  MacroSpace    = 73,  // macro.space
  MacroFilter   = 74,  // macro.filter
  MacroAux1     = 75,  // macro.aux1 (no knob: expression, MIDI and hosts)
  MacroAux2     = 76,  // macro.aux2
  // ── Performance controls: their own events, never preset leaves ───────────────────
  PerfFreeze     = 77,  // perf.freeze: the Freeze event's host face (plugin/src/BrainscapeParam.cpp)
  PerfExpression = 78,  // perf.expression: the pedal position, an Expression event from r2
  PerfReverse    = 79,  // perf.reverse: the FWD/REV toggle, also stored performance state (W2)
  PerfLoopLevel  = 80,  // perf.loop_level (reserved for the looper, companion Q29)
  // ── Device settings (design §3.8): outside presets, kept by every load ───────────
  TriggerOffset  = 81,  // global.trigger_offset: the calibration gesture's offset (phase D)
  EffectVolumeDb = 82,  // global.effect_volume_db: Shift+Mix, the player's wet level

  // The C++ spellings of IDs 4 and 8 before their renames, kept so code written against them
  // (the firmware bring-up branch) still builds; new code uses WetTrimDb and TransposeSt.
  OutTrimDb = WetTrimDb,
  PitchSt   = TransposeSt,
};

// What a row is (design §4.1). The kind decides whether presets store the row, what
// SetParam and LoadPreset do with it and whether hosts see it:
//
//   kind         STAT (presets)   SetParam              LoadPreset              host
//   Leaf         yes              stores                default, then stored    registered
//   Macro        never (CTRL)     no-op (MacroMove)     nothing                 automatable
//   Performance  never            no-op (own events)    freeze off              automatable
//   Global       never            stores                kept, across Restart    per row
//   Reserved     never            no-op                 nothing                 not registered
//   Retired      never            no-op                 nothing                 not registered
//
// A Reserved row is an unbuilt feature's ID under its final name; it takes its kind in the
// pull request that builds the feature. A Retired row keeps its ID with a null name and is
// never reused. The engine stores Leaf and Global rows only.
enum class ParamKind : uint8_t { Leaf, Macro, Performance, Global, Reserved, Retired };

// What a change to a row rebuilds (design §4.1, §7.2): a bitmask, since one value can feed
// more than one rebuild (the filter cutoff is Post | Wet). A Reserved row carries the domain
// it will have. Macro and Performance rows act through events and have none. Since sound
// revision 2 the engine dispatches every change on it (R1, §7.1): a change marks each domain
// of its row, and each marked domain rebuilds once before the next frame renders.
enum ParamDomain : uint8_t {
  kDomainNone     = 0,
  kDomainGranular = 1u << 0,  // the scheduler and voice parameters
  kDomainPost     = 1u << 1,  // the post chain
  kDomainMix      = 1u << 2,  // the wet/dry mix
  kDomainFeedback = 1u << 3,  // the feedback gain and its taming chain
  kDomainWet      = 1u << 4,  // the wet gain: trim, effect volume and the cutoff kill
  kDomainDetector = 1u << 5,  // the onset detector's threshold
};
inline constexpr uint8_t kAllParamDomains = 0x3Fu;

struct ParamDescriptor {
  ParamId     id;
  const char* name;  // stable string name, the preset schema key; null for a Retired row
  float       min, max, def;
  const char* unit;
  ParamKind   kind;
  uint8_t     domain;    // ParamDomain bits
  uint16_t    sinceRev;  // the sound revision that made the row a Leaf; 0 for other kinds
                         // (LoadPreset's missing-leaf rule, design §7.3)
};

// Table order must match contiguous ids starting at 1 (checked by static_assert in
// Engine.cpp) so id -> row lookup is a subtraction; Engine.cpp also checks each kind's
// invariants. Retiring a parameter later leaves a tombstone row (null name) that keeps its
// row and id: it is filtered from host-visible listings but never renumbered.
//
// Range notes: Feedback reaches 1.1, the design's bounded self-oscillation (§2.3, §11).
// "global.mix" is outside the macro namespace: no macro may target it (design §3.1).
// FilterCutoffHz's default sits at its max = exact stage bypass. Rows 63 and 65-68 have
// provisional ranges (the division list and the modulators are designed with W2 and W3).
inline constexpr ParamDescriptor kParamTable[] = {
    // id                         name                            min      max       def       unit  kind                    domain                         since
    {ParamId::DelayMs,            "layer0.position.base_ms",      1.0f,    5000.0f,  250.0f,   "ms", ParamKind::Leaf,        kDomainGranular,               1},
    {ParamId::Mix,                "global.mix",                   0.0f,    1.0f,     0.5f,     "",   ParamKind::Leaf,        kDomainMix,                    1},
    {ParamId::Feedback,           "feedback.amount",              0.0f,    1.1f,     0.0f,     "",   ParamKind::Leaf,        kDomainFeedback,               1},
    {ParamId::WetTrimDb,          "wet_trim_db",                  -24.0f,  24.0f,    0.0f,     "dB", ParamKind::Leaf,        kDomainWet,                    1},
    {ParamId::GrainSizeMs,        "layer0.size_ms",               1.0f,    500.0f,   90.0f,    "ms", ParamKind::Leaf,        kDomainGranular,               1},
    {ParamId::Overlap,            "scheduler.overlap",            0.0f,    1.0f,     0.55f,    "",   ParamKind::Leaf,        kDomainGranular,               1},
    {ParamId::SprayMs,            "layer0.position.spray_ms",     0.0f,    2000.0f,  20.0f,    "ms", ParamKind::Leaf,        kDomainGranular,               1},
    {ParamId::TransposeSt,        "layer0.pitch.transpose_st",    -24.0f,  24.0f,    0.0f,     "st", ParamKind::Leaf,        kDomainGranular,               1},
    {ParamId::SpreadCents,        "layer0.pitch.spread_cents",    0.0f,    100.0f,   0.0f,     "c",  ParamKind::Leaf,        kDomainGranular,               1},
    {ParamId::ReverseProb,        "layer0.pitch.reverse_prob",    0.0f,    1.0f,     0.0f,     "",   ParamKind::Leaf,        kDomainGranular,               1},
    {ParamId::Jitter,             "scheduler.jitter",             0.0f,    1.0f,     0.2f,     "",   ParamKind::Leaf,        kDomainGranular,               1},
    {ParamId::WindowSustain,      "layer0.window.sustain",        0.0f,    1.0f,     0.3f,     "",   ParamKind::Leaf,        kDomainGranular,               1},
    {ParamId::WindowSkew,         "layer0.window.skew",           0.0f,    1.0f,     0.5f,     "",   ParamKind::Leaf,        kDomainGranular,               1},
    {ParamId::WindowSmooth,       "layer0.window.smoothness",     0.0f,    1.0f,     0.7f,     "",   ParamKind::Leaf,        kDomainGranular,               1},
    {ParamId::PanSpread,          "layer0.pan_spread",            0.0f,    1.0f,     0.5f,     "",   ParamKind::Leaf,        kDomainGranular,               1},
    {ParamId::ModRateHz,          "post.mod.rate_hz",             0.01f,   10.0f,    0.4f,     "Hz", ParamKind::Leaf,        kDomainPost,                   1},
    {ParamId::ModDepth,           "post.mod.depth",               0.0f,    1.0f,     0.0f,     "",   ParamKind::Leaf,        kDomainPost,                   1},
    {ParamId::DelayTimeMs,        "post.delay.time_ms",           10.0f,   2000.0f,  350.0f,   "ms", ParamKind::Leaf,        kDomainPost,                   1},
    {ParamId::DelayFb,            "post.delay.fb",                0.0f,    0.9f,     0.3f,     "",   ParamKind::Leaf,        kDomainPost,                   1},
    {ParamId::DelayMix,           "post.delay.mix",               0.0f,    1.0f,     0.0f,     "",   ParamKind::Leaf,        kDomainPost,                   1},
    {ParamId::ReverbTime,         "post.reverb.time",             0.0f,    1.0f,     0.5f,     "",   ParamKind::Leaf,        kDomainPost,                   1},
    {ParamId::ReverbMix,          "post.reverb.mix",              0.0f,    1.0f,     0.0f,     "",   ParamKind::Leaf,        kDomainPost,                   1},
    {ParamId::FilterCutoffHz,     "post.filter.cutoff_hz",        40.0f,   20000.0f, 20000.0f, "Hz", ParamKind::Leaf,        kDomainPost | kDomainWet,      1},
    {ParamId::FilterRes,          "post.filter.res",              0.0f,    1.0f,     0.1f,     "",   ParamKind::Leaf,        kDomainPost,                   1},
    {ParamId::FilterMorph,        "post.filter.morph",            0.0f,    3.0f,     0.0f,     "",   ParamKind::Leaf,        kDomainPost,                   1},
    {ParamId::TriggerSens,        "trigger.sensitivity",          0.0f,    1.0f,     0.5f,     "",   ParamKind::Leaf,        kDomainDetector,               1},
    {ParamId::OnsetTrigger,       nullptr,                        0.0f,    1.0f,     0.0f,     "",   ParamKind::Retired,     kDomainNone,                   0},
    {ParamId::PositionSource,     nullptr,                        0.0f,    1.0f,     0.0f,     "",   ParamKind::Retired,     kDomainNone,                   0},
    {ParamId::Repeat,             "layer0.position.repeat",       1.0f,    16.0f,    1.0f,     "",   ParamKind::Reserved,    kDomainGranular,               0},
    {ParamId::DecayMs,            "layer0.decay_ms",              0.0f,    20000.0f, 0.0f,     "ms", ParamKind::Reserved,    kDomainGranular,               0},
    {ParamId::VoiceCount,         "layer0.voice_count",           1.0f,    64.0f,    64.0f,    "",   ParamKind::Reserved,    kDomainGranular,               0},
    {ParamId::LevelDb,            "layer0.level_db",              -24.0f,  6.0f,     0.0f,     "dB", ParamKind::Reserved,    kDomainGranular,               0},
    {ParamId::GlideCurve,         "layer0.pitch.glide.curve",     -1.0f,   1.0f,     0.0f,     "",   ParamKind::Reserved,    kDomainGranular,               0},
    {ParamId::SvfCutoffHz,        "layer0.svf.cutoff_hz",         20.0f,   20000.0f, 20000.0f, "Hz", ParamKind::Reserved,    kDomainGranular,               0},
    {ParamId::SvfRes,             "layer0.svf.res",               0.0f,    1.0f,     0.1f,     "",   ParamKind::Reserved,    kDomainGranular,               0},
    {ParamId::CrushBits,          "layer0.crush.bits",            1.0f,    16.0f,    16.0f,    "",   ParamKind::Reserved,    kDomainGranular,               0},
    {ParamId::CrushDownsample,    "layer0.crush.downsample",      1.0f,    32.0f,    1.0f,     "",   ParamKind::Reserved,    kDomainGranular,               0},
    {ParamId::L1DelayMs,          "layer1.position.base_ms",      1.0f,    5000.0f,  250.0f,   "ms", ParamKind::Reserved,    kDomainGranular,               0},
    {ParamId::L1SprayMs,          "layer1.position.spray_ms",     0.0f,    2000.0f,  20.0f,    "ms", ParamKind::Reserved,    kDomainGranular,               0},
    {ParamId::L1Repeat,           "layer1.position.repeat",       1.0f,    16.0f,    1.0f,     "",   ParamKind::Reserved,    kDomainGranular,               0},
    {ParamId::L1GrainSizeMs,      "layer1.size_ms",               1.0f,    500.0f,   90.0f,    "ms", ParamKind::Reserved,    kDomainGranular,               0},
    {ParamId::L1DecayMs,          "layer1.decay_ms",              0.0f,    20000.0f, 0.0f,     "ms", ParamKind::Reserved,    kDomainGranular,               0},
    {ParamId::L1VoiceCount,       "layer1.voice_count",           1.0f,    64.0f,    64.0f,    "",   ParamKind::Reserved,    kDomainGranular,               0},
    {ParamId::L1LevelDb,          "layer1.level_db",              -24.0f,  6.0f,     0.0f,     "dB", ParamKind::Reserved,    kDomainGranular,               0},
    {ParamId::L1PanSpread,        "layer1.pan_spread",            0.0f,    1.0f,     0.5f,     "",   ParamKind::Reserved,    kDomainGranular,               0},
    {ParamId::L1WindowSustain,    "layer1.window.sustain",        0.0f,    1.0f,     0.3f,     "",   ParamKind::Reserved,    kDomainGranular,               0},
    {ParamId::L1WindowSkew,       "layer1.window.skew",           0.0f,    1.0f,     0.5f,     "",   ParamKind::Reserved,    kDomainGranular,               0},
    {ParamId::L1WindowSmooth,     "layer1.window.smoothness",     0.0f,    1.0f,     0.7f,     "",   ParamKind::Reserved,    kDomainGranular,               0},
    {ParamId::L1TransposeSt,      "layer1.pitch.transpose_st",    -24.0f,  24.0f,    0.0f,     "st", ParamKind::Reserved,    kDomainGranular,               0},
    {ParamId::L1SpreadCents,      "layer1.pitch.spread_cents",    0.0f,    100.0f,   0.0f,     "c",  ParamKind::Reserved,    kDomainGranular,               0},
    {ParamId::L1ReverseProb,      "layer1.pitch.reverse_prob",    0.0f,    1.0f,     0.0f,     "",   ParamKind::Reserved,    kDomainGranular,               0},
    {ParamId::L1GlideCurve,       "layer1.pitch.glide.curve",     -1.0f,   1.0f,     0.0f,     "",   ParamKind::Reserved,    kDomainGranular,               0},
    {ParamId::L1SvfCutoffHz,      "layer1.svf.cutoff_hz",         20.0f,   20000.0f, 20000.0f, "Hz", ParamKind::Reserved,    kDomainGranular,               0},
    {ParamId::L1SvfRes,           "layer1.svf.res",               0.0f,    1.0f,     0.1f,     "",   ParamKind::Reserved,    kDomainGranular,               0},
    {ParamId::L1CrushBits,        "layer1.crush.bits",            1.0f,    16.0f,    16.0f,    "",   ParamKind::Reserved,    kDomainGranular,               0},
    {ParamId::L1CrushDownsample,  "layer1.crush.downsample",      1.0f,    32.0f,    1.0f,     "",   ParamKind::Reserved,    kDomainGranular,               0},
    {ParamId::Intermittency,      "scheduler.intermittency",      0.0f,    1.0f,     0.0f,     "",   ParamKind::Leaf,        kDomainGranular,               4},
    {ParamId::BurstCount,         "scheduler.burst.count",        1.0f,    16.0f,    1.0f,     "",   ParamKind::Leaf,        kDomainGranular,               4},
    {ParamId::BurstSpacingMs,     "scheduler.burst.spacing_ms",   0.0f,    500.0f,   0.0f,     "ms", ParamKind::Leaf,        kDomainGranular,               4},
    {ParamId::StepCount,          "scheduler.steps.count",        1.0f,    16.0f,    16.0f,    "",   ParamKind::Reserved,    kDomainGranular,               0},
    {ParamId::LayerMix,           "layer_mix",                    0.0f,    1.0f,     0.5f,     "",   ParamKind::Reserved,    kDomainGranular,               0},
    {ParamId::DryDuckDepth,       "dry_duck.depth",               0.0f,    1.0f,     0.0f,     "",   ParamKind::Reserved,    kDomainMix,                    0},
    {ParamId::DelaySync,          "post.delay.sync",              0.0f,    16.0f,    0.0f,     "",   ParamKind::Reserved,    kDomainPost,                   0},
    {ParamId::ReverbMode,         "post.reverb.mode",             0.0f,    3.0f,     0.0f,     "",   ParamKind::Reserved,    kDomainPost,                   0},
    {ParamId::Modulator0RateHz,   "modulator0.rate_hz",           0.01f,   10.0f,    0.4f,     "Hz", ParamKind::Reserved,    kDomainGranular,               0},
    {ParamId::Modulator0Depth,    "modulator0.depth",             0.0f,    1.0f,     0.0f,     "",   ParamKind::Reserved,    kDomainGranular,               0},
    {ParamId::Modulator1RateHz,   "modulator1.rate_hz",           0.01f,   10.0f,    0.4f,     "Hz", ParamKind::Reserved,    kDomainGranular,               0},
    {ParamId::Modulator1Depth,    "modulator1.depth",             0.0f,    1.0f,     0.0f,     "",   ParamKind::Reserved,    kDomainGranular,               0},
    {ParamId::MacroActivity,      "macro.activity",               0.0f,    1.0f,     0.5f,     "",   ParamKind::Macro,       kDomainNone,                   0},
    {ParamId::MacroRepeats,       "macro.repeats",                0.0f,    1.0f,     0.5f,     "",   ParamKind::Macro,       kDomainNone,                   0},
    {ParamId::MacroShape,         "macro.shape",                  0.0f,    1.0f,     0.5f,     "",   ParamKind::Macro,       kDomainNone,                   0},
    {ParamId::MacroTime,          "macro.time",                   0.0f,    1.0f,     0.5f,     "",   ParamKind::Macro,       kDomainNone,                   0},
    {ParamId::MacroSpace,         "macro.space",                  0.0f,    1.0f,     0.5f,     "",   ParamKind::Macro,       kDomainNone,                   0},
    {ParamId::MacroFilter,        "macro.filter",                 0.0f,    1.0f,     0.5f,     "",   ParamKind::Macro,       kDomainNone,                   0},
    {ParamId::MacroAux1,          "macro.aux1",                   0.0f,    1.0f,     0.5f,     "",   ParamKind::Macro,       kDomainNone,                   0},
    {ParamId::MacroAux2,          "macro.aux2",                   0.0f,    1.0f,     0.5f,     "",   ParamKind::Macro,       kDomainNone,                   0},
    {ParamId::PerfFreeze,         "perf.freeze",                  0.0f,    1.0f,     0.0f,     "",   ParamKind::Performance, kDomainNone,                   0},
    {ParamId::PerfExpression,     "perf.expression",              0.0f,    1.0f,     0.0f,     "",   ParamKind::Performance, kDomainNone,                   0},
    {ParamId::PerfReverse,        "perf.reverse",                 0.0f,    1.0f,     0.0f,     "",   ParamKind::Reserved,    kDomainNone,                   0},
    {ParamId::PerfLoopLevel,      "perf.loop_level",              0.0f,    1.0f,     1.0f,     "",   ParamKind::Reserved,    kDomainNone,                   0},
    {ParamId::TriggerOffset,      "global.trigger_offset",        -1.0f,   1.0f,     0.0f,     "",   ParamKind::Reserved,    kDomainDetector,               0},
    {ParamId::EffectVolumeDb,     "global.effect_volume_db",      -24.0f,  12.0f,    0.0f,     "dB", ParamKind::Global,      kDomainWet,                    0},
};
inline constexpr size_t kNumParams = sizeof(kParamTable) / sizeof(kParamTable[0]);  // every row

const ParamDescriptor* Descriptors(size_t* count) noexcept;  // every row, kNumParams of them
const ParamDescriptor* FindParam(ParamId id) noexcept;       // any row; nullptr if unknown

// ── The Leaf rows ─────────────────────────────────────────────────────────────────────
// What a preset stores and a host registers as a plain-value parameter. Consumers iterate
// these, never the whole table: a leaf's ordinal (its index in kLeafParams) is the index of
// its value in a complete preset, ascending by ID as presets order their leaves.
namespace params_detail {

template <size_t N>
struct IdList {
  ParamId ids[N];
};
template <size_t N>
struct IndexList {
  uint16_t index[N];
};

constexpr size_t CountKind(ParamKind kind) noexcept {
  size_t n = 0;
  for (const ParamDescriptor& d : kParamTable) n += d.kind == kind ? 1u : 0u;
  return n;
}

template <size_t N>
constexpr IdList<N> ListKind(ParamKind kind) noexcept {
  IdList<N> out{};
  size_t    k = 0;
  for (const ParamDescriptor& d : kParamTable) {
    if (d.kind == kind) out.ids[k++] = d.id;
  }
  return out;
}

// Row index -> ordinal among the rows of `kind`, or `none` for rows of other kinds.
constexpr IndexList<kNumParams> OrdinalsOfKind(ParamKind kind, uint16_t none) noexcept {
  IndexList<kNumParams> out{};
  uint16_t              k = 0;
  for (size_t i = 0; i < kNumParams; ++i) out.index[i] = kParamTable[i].kind == kind ? k++ : none;
  return out;
}

}  // namespace params_detail

inline constexpr size_t kNumLeafParams = params_detail::CountKind(ParamKind::Leaf);
inline constexpr params_detail::IdList<kNumLeafParams> kLeafParams =
    params_detail::ListKind<kNumLeafParams>(ParamKind::Leaf);
static_assert(kNumParams < 0xFFFFu, "leaf ordinals are 16-bit");

namespace params_detail {
inline constexpr IndexList<kNumParams> kLeafOrdinal =
    OrdinalsOfKind(ParamKind::Leaf, static_cast<uint16_t>(kNumLeafParams));
}  // namespace params_detail

// The ordinal of a Leaf row (its index in kLeafParams), or kNumLeafParams for any other id:
// unknown, Macro, Performance, Global, Reserved or Retired.
constexpr size_t LeafIndex(ParamId id) noexcept {
  const auto raw = static_cast<uint32_t>(id);
  return raw >= 1u && raw <= kNumParams ? params_detail::kLeafOrdinal.index[raw - 1u]
                                        : kNumLeafParams;
}
constexpr size_t LeafIndex(uint32_t rawId) noexcept { return LeafIndex(static_cast<ParamId>(rawId)); }
constexpr bool   IsLeaf(ParamId id) noexcept { return LeafIndex(id) < kNumLeafParams; }
constexpr bool   IsLeaf(uint32_t rawId) noexcept { return LeafIndex(rawId) < kNumLeafParams; }
// The id of leaf `ordinal` (< kNumLeafParams).
constexpr ParamId LeafId(size_t ordinal) noexcept { return kLeafParams.ids[ordinal]; }

// The canonical form of a plain value, exactly as Engine::SetParam stores it
// (docs/design/determinism-profile.md §3.7): NaN and ±inf become the descriptor
// minimum, ±0 and subnormals become +0, then the value is clamped to [min, max]. The
// tests are on the bit pattern, so the result is the same under any host FP
// environment. Presets and events carry only canonical values. Defined for every row,
// whatever its kind; unknown ids give +0.
float Canonicalize(ParamId id, float plainValue) noexcept;

}  // namespace brainscape
