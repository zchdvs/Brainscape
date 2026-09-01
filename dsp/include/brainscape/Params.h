#pragma once
#include <cstddef>
#include <cstdint>

namespace brainscape {

// Permanent parameter identities (docs/design/grain-engine.md §6): IDs and names are
// never reused or reassigned once released — retire and rename instead. Values are
// plain (denormalized) units everywhere; normalizing to [0,1] is the plugin wrapper's
// job (VST3/CLAP convention, docs/research/preset-parameter-and-patch-format.md).
enum class ParamId : uint32_t {
  DelayMs        = 1,   // layer0.position.base_ms — grain position behind the write head
  Mix            = 2,   // global wet/dry, linear crossfade (grain-delay-theory.md §3.11)
  Feedback       = 3,   // feedback.amount (the taming chain lands with the post chain)
  OutTrimDb      = 4,   // out_trim_db
  GrainSizeMs    = 5,   // layer0.size_ms
  Overlap        = 6,   // scheduler.overlap — target voices = kMaxGrains * overlap^3
  SprayMs        = 7,   // layer0.position.spray_ms
  PitchSt        = 8,   // layer0.pitch.st (±24 st = the design's r_max = 4 ratio ceiling)
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
                        // (design §2.6 endpoint semantics: Filter fully CW = bypass)
  FilterRes      = 24,  // post.filter.res
  FilterMorph    = 25,  // post.filter.morph — 0..3 continuous LP -> BP -> HP -> Notch
};

struct ParamDescriptor {
  ParamId     id;
  const char* name;  // stable string name — doubles as the preset schema key
  float       min, max, def;
  const char* unit;
};

// Table order must match contiguous ids starting at 1 (checked by static_assert
// in Engine.cpp) so id -> slot lookup is a subtraction. Retiring a parameter later
// leaves a tombstone row (null name) that keeps its slot and id — it is filtered
// from host-visible listings but never renumbered, per the design's never-reuse rule.
//
// Range notes: Feedback now reaches 1.1 — feedback > 1.0 is the design's
// deliberate self-oscillation feature (§2.3, §11), bounded by the taming chain's
// saturator (the 0.95 cap was documented provisional until the tamer landed).
// "global.mix" is deliberately outside the mode-file leaf namespace (design §6:
// Mix is a global performance control, not a per-mode leaf a macro can target).
// FilterCutoffHz's default sits at its max = exact stage bypass.
inline constexpr ParamDescriptor kParamTable[] = {
    {ParamId::DelayMs,        "layer0.position.base_ms",   1.0f,   5000.0f,  250.0f,   "ms"},
    {ParamId::Mix,            "global.mix",                0.0f,   1.0f,     0.5f,     ""},
    {ParamId::Feedback,       "feedback.amount",           0.0f,   1.1f,     0.0f,     ""},
    {ParamId::OutTrimDb,      "out_trim_db",               -24.0f, 24.0f,    0.0f,     "dB"},
    {ParamId::GrainSizeMs,    "layer0.size_ms",            1.0f,   500.0f,   90.0f,    "ms"},
    {ParamId::Overlap,        "scheduler.overlap",         0.0f,   1.0f,     0.55f,    ""},
    {ParamId::SprayMs,        "layer0.position.spray_ms",  0.0f,   2000.0f,  20.0f,    "ms"},
    {ParamId::PitchSt,        "layer0.pitch.st",           -24.0f, 24.0f,    0.0f,     "st"},
    {ParamId::SpreadCents,    "layer0.pitch.spread_cents", 0.0f,   100.0f,   0.0f,     "c"},
    {ParamId::ReverseProb,    "layer0.pitch.reverse_prob", 0.0f,   1.0f,     0.0f,     ""},
    {ParamId::Jitter,         "scheduler.jitter",          0.0f,   1.0f,     0.2f,     ""},
    {ParamId::WindowSustain,  "layer0.window.sustain",     0.0f,   1.0f,     0.3f,     ""},
    {ParamId::WindowSkew,     "layer0.window.skew",        0.0f,   1.0f,     0.5f,     ""},
    {ParamId::WindowSmooth,   "layer0.window.smoothness",  0.0f,   1.0f,     0.7f,     ""},
    {ParamId::PanSpread,      "layer0.pan_spread",         0.0f,   1.0f,     0.5f,     ""},
    {ParamId::ModRateHz,      "post.mod.rate_hz",          0.01f,  10.0f,    0.4f,     "Hz"},
    {ParamId::ModDepth,       "post.mod.depth",            0.0f,   1.0f,     0.0f,     ""},
    {ParamId::DelayTimeMs,    "post.delay.time_ms",        10.0f,  2000.0f,  350.0f,   "ms"},
    {ParamId::DelayFb,        "post.delay.fb",             0.0f,   0.9f,     0.3f,     ""},
    {ParamId::DelayMix,       "post.delay.mix",            0.0f,   1.0f,     0.0f,     ""},
    {ParamId::ReverbTime,     "post.reverb.time",          0.0f,   1.0f,     0.5f,     ""},
    {ParamId::ReverbMix,      "post.reverb.mix",           0.0f,   1.0f,     0.0f,     ""},
    {ParamId::FilterCutoffHz, "post.filter.cutoff_hz",     40.0f,  20000.0f, 20000.0f, "Hz"},
    {ParamId::FilterRes,      "post.filter.res",           0.0f,   1.0f,     0.1f,     ""},
    {ParamId::FilterMorph,    "post.filter.morph",         0.0f,   3.0f,     0.0f,     ""},
};
inline constexpr size_t kNumParams = sizeof(kParamTable) / sizeof(kParamTable[0]);

const ParamDescriptor* Descriptors(size_t* count) noexcept;
const ParamDescriptor* FindParam(ParamId id) noexcept;

}  // namespace brainscape
