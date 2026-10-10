#include "detail/FpProfilePrivate.h"

#include "brainscape/ParamDisplay.h"

#include <cassert>
#include <cstdint>
#include <cstring>

#include "brainscape/Mode.h"
#include "detail/Canonical.h"
#include "detail/DetMath.h"
#include "detail/FpEnvGuard.h"

// The taper functions feed the engine (the pedal's pot path and every plugin knob), so
// every function here that computes in floating point is an engine entry point: the
// public function owns the complete FP control word (detail/FpEnvGuard.h) and calls a
// BRAINSCAPE_FP_BODY, as Engine.cpp's entry points do (determinism profile §4.1,
// companion §4.6). FindParamDisplay and GroupTitle only read tables.

namespace brainscape {

namespace {

// Host model (b) (ParamDisplay.h, mode-compiler.md §3.6, Q12): only macros, Mix, the effect
// volume and the performance rows are automatable; Reserved rows carry the flags their
// final kind will have. Every other leaf is kLeaf: registered, but not automatable, since lane
// D's curation slice registered the macro parameters that took their automation over (§12.4).
// Retired rows (27 and 28 since sound revision 2) keep a display row but are never registered.
constexpr uint16_t kLeaf     = 0;
constexpr uint16_t kLeafStep = kParamDiscrete;
constexpr uint16_t kAuto     = kParamAutomatable;
constexpr uint16_t kAutoStep = kParamAutomatable | kParamDiscrete;

using G = ParamGroup;
using K = DisplayKind;
using T = Taper;

constexpr ParamDisplay kDisplayTable[] = {
    {ParamId::DelayMs,             G::GrainDelay,  "Delay time",                   "Time",     T::Quartic, K::Milliseconds, 0,  kLeaf},
    {ParamId::Mix,                 G::GrainDelay,  "Mix",                          "Mix",      T::Linear,  K::Percent,      0,  kAuto},
    {ParamId::Feedback,            G::GrainDelay,  "Feedback",                     "Feedback", T::Linear,  K::Percent,      0,  kLeaf},
    {ParamId::WetTrimDb,           G::GrainDelay,  "Wet trim",                     "Trim",     T::Linear,  K::Decibels,     0,  kLeaf},
    {ParamId::GrainSizeMs,         G::Grains,      "Grain size",                   "Size",     T::Quartic, K::Milliseconds, 0,  kLeaf},
    {ParamId::Overlap,             G::Grains,      "Grain overlap",                "Overlap",  T::Linear,  K::Percent,      0,  kLeaf},
    {ParamId::SprayMs,             G::Grains,      "Grain spray",                  "Spray",    T::Quartic, K::Milliseconds, 0,  kLeaf},
    {ParamId::TransposeSt,         G::Pitch,       "Transpose",                    "Pitch",    T::Linear,  K::Semitones,    0,  kLeaf},
    {ParamId::SpreadCents,         G::Pitch,       "Pitch spread",                 "Spread",   T::Linear,  K::Cents,        0,  kLeaf},
    {ParamId::ReverseProb,         G::Pitch,       "Reverse probability",          "Reverse",  T::Linear,  K::Percent,      0,  kLeaf},
    {ParamId::Jitter,              G::Grains,      "Scheduler jitter",             "Jitter",   T::Linear,  K::Percent,      0,  kLeaf},
    {ParamId::WindowSustain,       G::Window,      "Window sustain",               "Sustain",  T::Linear,  K::Percent,      0,  kLeaf},
    {ParamId::WindowSkew,          G::Window,      "Window skew",                  "Skew",     T::Linear,  K::Balance,      0,  kLeaf},
    {ParamId::WindowSmooth,        G::Window,      "Window smoothness",            "Smooth",   T::Linear,  K::Percent,      0,  kLeaf},
    {ParamId::PanSpread,           G::Grains,      "Pan spread",                   "Pan",      T::Linear,  K::Percent,      0,  kLeaf},
    {ParamId::ModRateHz,           G::Mod,         "Mod rate",                     "Rate",     T::Quartic, K::Hertz,        0,  kLeaf},
    {ParamId::ModDepth,            G::Mod,         "Mod depth",                    "Depth",    T::Linear,  K::Percent,      0,  kLeaf},
    {ParamId::DelayTimeMs,         G::PostDelay,   "Post delay time",              "Time",     T::Square,  K::Milliseconds, 0,  kLeaf},
    {ParamId::DelayFb,             G::PostDelay,   "Post delay feedback",          "Repeats",  T::Linear,  K::Percent,      0,  kLeaf},
    {ParamId::DelayMix,            G::PostDelay,   "Post delay mix",               "Mix",      T::Linear,  K::Percent,      0,  kLeaf},
    {ParamId::ReverbTime,          G::Reverb,      "Reverb time",                  "Time",     T::Linear,  K::Amount,       0,  kLeaf},
    {ParamId::ReverbMix,           G::Reverb,      "Reverb mix",                   "Mix",      T::Linear,  K::Percent,      0,  kLeaf},
    {ParamId::FilterCutoffHz,      G::Filter,      "Filter cutoff",                "Cutoff",   T::Quartic, K::FilterCutoff, 0,  kLeaf},
    {ParamId::FilterRes,           G::Filter,      "Filter resonance",             "Reso",     T::Linear,  K::Percent,      0,  kLeaf},
    {ParamId::FilterMorph,         G::Filter,      "Filter morph",                 "Morph",    T::Linear,  K::FilterMorph,  0,  kLeaf},
    {ParamId::TriggerSens,         G::Triggers,    "Trigger sensitivity",          "Sense",    T::Linear,  K::Percent,      0,  kLeaf},
    {ParamId::OnsetTrigger,        G::Triggers,    "Onset trigger (retired)",      "Onset",    T::Linear,  K::OffOn,        2,  kLeafStep},
    {ParamId::PositionSource,      G::Triggers,    "Position source (retired)",    "Position", T::Linear,  K::LiveMark,     2,  kLeafStep},
    {ParamId::Repeat,              G::Grains,      "Repeat passes",                "Repeat",   T::Linear,  K::Count,        16, kLeafStep},
    {ParamId::DecayMs,             G::Grains,      "Grain decay",                  "Decay",    T::Quartic, K::MsOrOff,      0,  kLeaf},
    {ParamId::VoiceCount,          G::Grains,      "Voice count",                  "Voices",   T::Linear,  K::Count,        64, kLeafStep},
    {ParamId::LevelDb,             G::Grains,      "Layer level",                  "Level",    T::Linear,  K::Decibels,     0,  kLeaf},
    {ParamId::GlideCurve,          G::Pitch,       "Glide curve",                  "Glide",    T::Linear,  K::Signed,       0,  kLeaf},
    {ParamId::SvfCutoffHz,         G::Modifiers,   "Grain filter cutoff",          "Cutoff",   T::Quartic, K::Hertz,        0,  kLeaf},
    {ParamId::SvfRes,              G::Modifiers,   "Grain filter resonance",       "Reso",     T::Linear,  K::Percent,      0,  kLeaf},
    {ParamId::CrushBits,           G::Modifiers,   "Crush bits",                   "Bits",     T::Linear,  K::Count,        16, kLeafStep},
    {ParamId::CrushDownsample,     G::Modifiers,   "Crush downsample",             "Down",     T::Linear,  K::Count,        32, kLeafStep},
    {ParamId::L1DelayMs,           G::Layer2,      "Layer 2 delay time",           "Time",     T::Quartic, K::Milliseconds, 0,  kLeaf},
    {ParamId::L1SprayMs,           G::Layer2,      "Layer 2 spray",                "Spray",    T::Quartic, K::Milliseconds, 0,  kLeaf},
    {ParamId::L1Repeat,            G::Layer2,      "Layer 2 repeat passes",        "Repeat",   T::Linear,  K::Count,        16, kLeafStep},
    {ParamId::L1GrainSizeMs,       G::Layer2,      "Layer 2 grain size",           "Size",     T::Quartic, K::Milliseconds, 0,  kLeaf},
    {ParamId::L1DecayMs,           G::Layer2,      "Layer 2 decay",                "Decay",    T::Quartic, K::MsOrOff,      0,  kLeaf},
    {ParamId::L1VoiceCount,        G::Layer2,      "Layer 2 voice count",          "Voices",   T::Linear,  K::Count,        64, kLeafStep},
    {ParamId::L1LevelDb,           G::Layer2,      "Layer 2 level",                "Level",    T::Linear,  K::Decibels,     0,  kLeaf},
    {ParamId::L1PanSpread,         G::Layer2,      "Layer 2 pan spread",           "Pan",      T::Linear,  K::Percent,      0,  kLeaf},
    {ParamId::L1WindowSustain,     G::Layer2,      "Layer 2 window sustain",       "Sustain",  T::Linear,  K::Percent,      0,  kLeaf},
    {ParamId::L1WindowSkew,        G::Layer2,      "Layer 2 window skew",          "Skew",     T::Linear,  K::Balance,      0,  kLeaf},
    {ParamId::L1WindowSmooth,      G::Layer2,      "Layer 2 window smoothness",    "Smooth",   T::Linear,  K::Percent,      0,  kLeaf},
    {ParamId::L1TransposeSt,       G::Layer2,      "Layer 2 transpose",            "Pitch",    T::Linear,  K::Semitones,    0,  kLeaf},
    {ParamId::L1SpreadCents,       G::Layer2,      "Layer 2 pitch spread",         "Spread",   T::Linear,  K::Cents,        0,  kLeaf},
    {ParamId::L1ReverseProb,       G::Layer2,      "Layer 2 reverse probability",  "Reverse",  T::Linear,  K::Percent,      0,  kLeaf},
    {ParamId::L1GlideCurve,        G::Layer2,      "Layer 2 glide curve",          "Glide",    T::Linear,  K::Signed,       0,  kLeaf},
    {ParamId::L1SvfCutoffHz,       G::Layer2,      "Layer 2 filter cutoff",        "Cutoff",   T::Quartic, K::Hertz,        0,  kLeaf},
    {ParamId::L1SvfRes,            G::Layer2,      "Layer 2 filter resonance",     "Reso",     T::Linear,  K::Percent,      0,  kLeaf},
    {ParamId::L1CrushBits,         G::Layer2,      "Layer 2 crush bits",           "Bits",     T::Linear,  K::Count,        16, kLeafStep},
    {ParamId::L1CrushDownsample,   G::Layer2,      "Layer 2 crush downsample",     "Down",     T::Linear,  K::Count,        32, kLeafStep},
    {ParamId::Intermittency,       G::Scheduler,   "Intermittency",                "Skip",     T::Linear,  K::Percent,      0,  kLeaf},
    {ParamId::BurstCount,          G::Scheduler,   "Burst count",                  "Burst",    T::Linear,  K::Count,        16, kLeafStep},
    {ParamId::BurstSpacingMs,      G::Scheduler,   "Burst spacing",                "Spacing",  T::Square,  K::Milliseconds, 0,  kLeaf},
    {ParamId::StepCount,           G::Scheduler,   "Step count",                   "Steps",    T::Linear,  K::Count,        16, kLeafStep},
    {ParamId::LayerMix,            G::Layer2,      "Layer mix",                    "Mix",      T::Linear,  K::Balance,      0,  kLeaf},
    {ParamId::DryDuckDepth,        G::GrainDelay,  "Dry duck depth",               "Duck",     T::Linear,  K::Percent,      0,  kLeaf},
    {ParamId::DelaySync,           G::PostDelay,   "Post delay sync",              "Sync",     T::Linear,  K::Division,     17, kLeafStep},
    {ParamId::ReverbMode,          G::Reverb,      "Reverb mode",                  "Mode",     T::Linear,  K::ReverbMode,   4,  kLeafStep},
    {ParamId::Modulator0RateHz,    G::Modulation,  "Modulator 1 rate",             "Rate",     T::Quartic, K::Hertz,        0,  kLeaf},
    {ParamId::Modulator0Depth,     G::Modulation,  "Modulator 1 depth",            "Depth",    T::Linear,  K::Percent,      0,  kLeaf},
    {ParamId::Modulator1RateHz,    G::Modulation,  "Modulator 2 rate",             "Rate",     T::Quartic, K::Hertz,        0,  kLeaf},
    {ParamId::Modulator1Depth,     G::Modulation,  "Modulator 2 depth",            "Depth",    T::Linear,  K::Percent,      0,  kLeaf},
    {ParamId::MacroActivity,       G::Macros,      "Activity",                     "Activity", T::Linear,  K::Percent,      0,  kAuto},
    {ParamId::MacroRepeats,        G::Macros,      "Repeats",                      "Repeats",  T::Linear,  K::Percent,      0,  kAuto},
    {ParamId::MacroShape,          G::Macros,      "Shape",                        "Shape",    T::Linear,  K::Percent,      0,  kAuto},
    {ParamId::MacroTime,           G::Macros,      "Time",                         "Time",     T::Linear,  K::Percent,      0,  kAuto},
    {ParamId::MacroSpace,          G::Macros,      "Space",                        "Space",    T::Linear,  K::Percent,      0,  kAuto},
    {ParamId::MacroFilter,         G::Macros,      "Filter",                       "Filter",   T::Linear,  K::Percent,      0,  kAuto},
    {ParamId::MacroAux1,           G::Macros,      "Aux 1",                        "Aux 1",    T::Linear,  K::Percent,      0,  kAuto},
    {ParamId::MacroAux2,           G::Macros,      "Aux 2",                        "Aux 2",    T::Linear,  K::Percent,      0,  kAuto},
    {ParamId::PerfFreeze,          G::Performance, "Freeze",                       "Freeze",   T::Linear,  K::OffOn,        2,  kAutoStep},
    {ParamId::PerfExpression,      G::Performance, "Expression",                   "Expr",     T::Linear,  K::Percent,      0,  kAuto},
    {ParamId::PerfReverse,         G::Performance, "Reverse",                      "Reverse",  T::Linear,  K::OffOn,        2,  kAutoStep},
    {ParamId::PerfLoopLevel,       G::Performance, "Loop level",                   "Loop",     T::Linear,  K::Percent,      0,  kAuto},
    {ParamId::TriggerOffset,       G::Device,      "Trigger offset",               "Offset",   T::Linear,  K::Signed,       0,  kLeaf},
    {ParamId::EffectVolumeDb,      G::Device,      "Effect volume",                "Volume",   T::Linear,  K::Decibels,     0,  kAuto},
    {ParamId::PerfSubdiv,          G::Performance, "Subdivision",                  "Subdiv",   T::Linear,  K::SubdivPosition, 6, kAutoStep},
    {ParamId::PerfTimeMode,        G::Performance, "Time mode",                    "Time mode", T::Linear, K::TimeMode,     3,  kAutoStep},
    {ParamId::TempoRecall,         G::Device,      "Tempo recall",                 "Recall",   T::Linear,  K::TempoRecall,  2,  kLeafStep},
    {ParamId::TempoGlide,          G::Device,      "Tempo glide",                  "Glide",    T::Linear,  K::OffOn,        2,  kLeafStep},
};
static_assert(sizeof(kDisplayTable) / sizeof(kDisplayTable[0]) == kNumParams,
              "every descriptor needs a display row");

// The rows a host may automate under model (b): macros, performance controls (perf.reverse
// and perf.loop_level are Reserved until W2 and the looper), Mix and the effect volume.
constexpr bool HostAutomatable(const ParamDescriptor& d) {
  return d.kind == ParamKind::Macro || d.kind == ParamKind::Performance ||
         d.id == ParamId::PerfReverse || d.id == ParamId::PerfLoopLevel || d.id == ParamId::Mix ||
         d.id == ParamId::EffectVolumeDb;
}

constexpr bool DisplayTableMatchesDescriptors() {
  for (size_t i = 0; i < kNumParams; ++i) {
    const ParamDisplay&    m = kDisplayTable[i];
    const ParamDescriptor& d = kParamTable[i];
    if (m.id != d.id) return false;
    // The discrete flag goes with a step count, and an integer row steps through min..max
    // one by one (mode-compiler.md §4.3).
    if (((m.flags & kParamDiscrete) != 0u) != (m.steps >= 2u)) return false;
    const bool integer = m.kind == DisplayKind::Count || m.kind == DisplayKind::Division ||
                         m.kind == DisplayKind::ReverbMode ||
                         m.kind == DisplayKind::SubdivPosition ||
                         m.kind == DisplayKind::TimeMode || m.kind == DisplayKind::TempoRecall;
    if (integer && static_cast<float>(m.steps - 1u) != d.max - d.min) return false;
    if (((m.flags & kParamAutomatable) != 0u) != HostAutomatable(d)) return false;
  }
  return true;
}
static_assert(DisplayTableMatchesDescriptors(),
              "kDisplayTable must follow kParamTable's order, with step counts and host-model "
              "flags that match the rows");

constexpr const char* kGroupTitles[kNumParamGroups] = {
    "Grain delay", "Grains", "Pitch", "Window", "Mod", "Post delay", "Reverb", "Filter", "Triggers",
    "Scheduler", "Layer 2", "Modifiers", "Modulation", "Macros", "Performance", "Device",
};

constexpr bool SameText(const char* a, const char* b) {
  for (; *a != '\0' && *a == *b; ++a, ++b) {
  }
  return *a == *b;
}

constexpr bool TitlesAreUnique() {
  for (size_t i = 0; i < kNumParams; ++i) {
    if (kDisplayTable[i].title[0] == '\0' || kDisplayTable[i].shortTitle[0] == '\0') return false;
    for (size_t j = 0; j < i; ++j) {
      if (SameText(kDisplayTable[i].title, kDisplayTable[j].title)) return false;
    }
  }
  return true;
}
static_assert(TitlesAreUnique(), "host-facing titles must be unique and non-empty");

uint32_t Bits(float v) noexcept {
  uint32_t u = 0;
  std::memcpy(&u, &v, sizeof u);
  return u;
}

// NaN -> 0, then [0, 1]; subnormals -> 0. Bit tests, as in the canonicalization, so a
// DAZ host thread maps the same inputs to the same outputs.
double UnitFromNormalized(float n) noexcept {
  const uint32_t u        = Bits(n);
  const uint32_t exponent = u & 0x7F800000u;
  const bool     negative = (u >> 31) != 0u;
  if (exponent == 0x7F800000u) return ((u & 0x007FFFFFu) != 0u || negative) ? 0.0 : 1.0;
  if (exponent == 0u || negative) return 0.0;
  if (n >= 1.0f) return 1.0;
  return static_cast<double>(n);
}

// The canonical value, or +0 for an unknown id (Canonicalize's rule).
float Canonical(const ParamDescriptor* d, float plain) noexcept {
  return d != nullptr ? detail::CanonicalValue(*d, plain) : 0.0f;
}

// ── Display text, in integers ────────────────────────────────────────────────────
// snprintf would import the C library's formatter into the engine archive (the §6.3
// symbol audit allows memset, memmove and memcpy only) and follow the host's locale and
// rounding mode. Text counts what it would have written, as snprintf does, and keeps
// what fits.
class Text {
 public:
  Text(char* out, size_t size) noexcept : out_(out), size_(size) {}
  void Put(char c) noexcept {
    if (len_ + 1u < size_) out_[len_] = c;
    ++len_;
  }
  void Put(const char* s) noexcept {
    while (*s != '\0') Put(*s++);
  }
  size_t Finish() noexcept {
    const size_t n = len_ < size_ ? len_ : size_ - 1u;
    out_[n]        = '\0';
    return n;
  }

 private:
  char*  out_;
  size_t size_;
  size_t len_ = 0;
};

// x with `decimals` (0-2) digits after the point: the exact binary value rounded to the
// nearest, ties to even, which is what printf("%.Nf") prints on a correctly rounding C
// library; the sign comes from the sign bit, so -0.04 prints "-0.0" as there. Callers
// pass canonical values times small constants, |x| < 2^24.
void PutFixed(Text* t, double x, uint32_t decimals, bool plus) noexcept {
  uint64_t u = 0;
  std::memcpy(&u, &x, sizeof u);
  const bool     negative = (u >> 63) != 0u;
  const uint32_t biased   = static_cast<uint32_t>(u >> 52) & 0x7FFu;
  uint64_t       mant     = u & ((uint64_t{1} << 52) - 1u);
  if (biased != 0u) mant |= uint64_t{1} << 52;
  const int32_t  e     = static_cast<int32_t>(biased == 0u ? 1u : biased) - 1075;  // x = mant * 2^e
  const uint64_t scale = decimals == 0u ? 1u : (decimals == 1u ? 10u : 100u);
  const uint64_t num   = mant * scale;  // < 2^53 * 100 < 2^60
  uint64_t       q     = 0;             // |x| * scale, rounded
  if (e >= 0) {
    q = e < 4 ? num << e : uint64_t{1} << 62;  // |x| >= 2^52: never a parameter value
  } else if (e > -64) {
    const auto     k    = static_cast<uint32_t>(-e);
    const uint64_t rem  = num & ((uint64_t{1} << k) - 1u);
    const uint64_t half = uint64_t{1} << (k - 1u);
    q                   = num >> k;
    if (rem > half || (rem == half && (q & 1u) != 0u)) ++q;
  }  // else |x| * scale < 2^-4: rounds to 0
  if (negative) {
    t->Put('-');
  } else if (plus) {
    t->Put('+');
  }
  const uint64_t whole = q / scale;
  char           digits[24];
  uint32_t       n = 0;
  uint64_t       w = whole;
  do {
    digits[n++] = static_cast<char>('0' + w % 10u);
    w /= 10u;
  } while (w != 0u);
  while (n > 0u) t->Put(digits[--n]);
  if (decimals > 0u) {
    t->Put('.');
    const uint64_t frac = q % scale;
    if (decimals == 2u) t->Put(static_cast<char>('0' + frac / 10u));
    t->Put(static_cast<char>('0' + frac % 10u));
  }
}

void PutFixed(Text* t, double x, uint32_t decimals, bool plus, const char* suffix) noexcept {
  PutFixed(t, x, decimals, plus);
  t->Put(suffix);
}

// §5.2's note values by code, as row 63 shows them (clock.md §5.4).
constexpr const char* kDivisionNames[] = {"Off",  "1/32", "1/16T", "1/16", "1/8T", "1/16D",
                                          "1/8",  "1/4T", "1/8D",  "1/4",  "1/2T", "1/4D",
                                          "1/2",  "1/1T", "1/2D",  "1/1",  "2/1"};
static_assert(sizeof kDivisionNames / sizeof kDivisionNames[0] == kMaxSyncDivision + 1u &&
                  kMaxSyncDivision + 1u == tempo::kSyncCodes,
              "a name a code");

// The straight note value of 3 · 2^q ticks, 2^(q - 5) whole notes: "1/32" at q = 0, "1/1" at 5,
// "2/1" at 6, "1/64" at -1.
void PutStraight(Text* t, int32_t q) noexcept {
  if (q < -20) q = -20;  // never: a duration at the folds' and Subdivs' extremes is within ±10
  if (q > 20) q = 20;
  char           digits[12];
  uint32_t       n = 0;
  const uint32_t v = 1u << static_cast<uint32_t>(q <= 5 ? 5 - q : q - 5);
  for (uint32_t w = v; w != 0u; w /= 10u) digits[n++] = static_cast<char>('0' + w % 10u);
  if (q <= 5) t->Put("1/");
  while (n > 0u) t->Put(digits[--n]);
  if (q > 5) t->Put("/1");
}

// A note value of odd · 2^p ticks (odd 1, 3 or 9), named as §5.2 names them: 3 · 2^p straight,
// 2^p a triplet of the straight 3 · 2^(p - 1), 9 · 2^p dotted, of the straight 3 · 2^(p + 1).
void PutNote(Text* t, uint32_t odd, int32_t p) noexcept {
  if (odd == 3u) return PutStraight(t, p);
  PutStraight(t, odd == 1u ? p - 1 : p + 1);
  t->Put(odd == 1u ? 'T' : 'D');
}

void FormatMs(Text* t, float v) noexcept {
  if (v < 10.0f) return PutFixed(t, v, 2, false, " ms");
  if (v < 100.0f) return PutFixed(t, v, 1, false, " ms");
  if (v < 1000.0f) return PutFixed(t, v, 0, false, " ms");
  PutFixed(t, static_cast<double>(v) * 0.001, 2, false, " s");
}

void FormatHz(Text* t, float v) noexcept {
  if (v < 10.0f) return PutFixed(t, v, 2, false, " Hz");
  if (v < 100.0f) return PutFixed(t, v, 1, false, " Hz");
  if (v < 1000.0f) return PutFixed(t, v, 0, false, " Hz");
  if (v < 10000.0f) return PutFixed(t, static_cast<double>(v) * 0.001, 2, false, " kHz");
  PutFixed(t, static_cast<double>(v) * 0.001, 1, false, " kHz");
}

void FormatMorph(Text* t, float v) noexcept {
  static constexpr const char* kNames[] = {"LP", "BP", "HP", "Notch"};
  assert(v >= 0.0f && v <= 3.0f);  // canonical
  const auto   seg = static_cast<int>(v);
  const double pc  = (static_cast<double>(v) - seg) * 100.0;
  if (seg >= 3 || pc < 0.5) return t->Put(kNames[seg < 3 ? seg : 3]);
  if (pc >= 99.5) return t->Put(kNames[seg + 1]);
  t->Put(kNames[seg]);
  t->Put('>');
  t->Put(kNames[seg + 1]);
  t->Put(' ');
  PutFixed(t, pc, 0, false, "%");
}

// ── Bodies of the entry points (detail/FpEnvGuard.h explains the split) ───────────

BRAINSCAPE_FP_BODY size_t FormatSyncedTimeBody(tempo::SyncTarget target, uint8_t code,
                                               uint32_t nsPerQuarter, uint8_t subdiv,
                                               uint8_t timeMode, uint32_t rate, char* out,
                                               size_t outSize) noexcept {
  Text t(out, outSize);
  if (code == 0u) {
    t.Put("Off");
    return t.Finish();
  }
  const tempo::SyncedTime d =
      tempo::SyncedDuration(target, code, nsPerQuarter, subdiv, timeMode, rate);
  if (d.frames == 0u) return t.Finish();  // an input out of range: no text
  // §5.1's rates by code (TAP, ×1/4, ×1/2, ×2, ×4, ×8), the × in UTF-8.
  static constexpr const char* kRates[] = {"TAP", "\xC3\x97" "1/4", "\xC3\x97" "1/2",
                                           "\xC3\x97" "2", "\xC3\x97" "4", "\xC3\x97" "8"};
  t.Put(kDivisionNames[code]);
  if (d.subdiv != tempo::kSubdivTap) {
    t.Put(" \xC2\xB7 Subdiv ");
    t.Put(kRates[d.subdiv]);
  }
  if (d.subdiv != tempo::kSubdivTap || d.octaves != 0) {
    // What plays: the note's ticks as odd · 2^p, times the Subdiv's s / 24 (3 · 2^k ticks, so
    // 2^(k - 3)) and the fold's 2^octaves.
    uint32_t odd = tempo::NoteTicks(code);
    int32_t  p   = d.octaves - 3;
    while ((odd & 1u) == 0u) {
      odd >>= 1u;
      ++p;
    }
    for (uint32_t s = tempo::SubdivTicks(d.subdiv) / 3u; s > 1u; s >>= 1u) ++p;
    t.Put(" \xE2\x86\x92 ");
    PutNote(&t, odd, p);
  }
  t.Put(" \xC2\xB7 ");
  FormatMs(&t, static_cast<float>(static_cast<double>(d.frames) * 1000.0 / rate));
  return t.Finish();
}

BRAINSCAPE_FP_BODY float PlainFromNormalizedBody(ParamId id, float normalized) noexcept {
  const ParamDescriptor* d = FindParam(id);
  const ParamDisplay*    m = FindParamDisplay(id);
  if (d == nullptr || m == nullptr) return 0.0f;
  const double n    = UnitFromNormalized(normalized);
  const double lo   = d->min;
  const double hi   = d->max;
  const double span = hi - lo;
  if (m->steps >= 2) {
    const double last = m->steps - 1u;
    const double pos  = n * last;
    assert(pos >= 0.0 && pos <= last);
    const auto   k    = static_cast<uint32_t>(pos + 0.5);  // pos in [0, last]: in range
    const double frac = k / last;
    const double step = span * frac;
    return Canonical(d, static_cast<float>(lo + step));
  }
  double shaped = n;
  if (m->taper == Taper::Square) {
    shaped = n * n;
  } else if (m->taper == Taper::Quartic) {
    const double sq = n * n;
    shaped          = sq * sq;
  }
  const double offset = span * shaped;
  return Canonical(d, static_cast<float>(lo + offset));
}

BRAINSCAPE_FP_BODY float NormalizedFromPlainBody(ParamId id, float plain) noexcept {
  const ParamDescriptor* d = FindParam(id);
  const ParamDisplay*    m = FindParamDisplay(id);
  if (d == nullptr || m == nullptr) return 0.0f;
  const double lo    = d->min;
  const double hi    = d->max;
  const double span  = hi - lo;
  const double above = static_cast<double>(Canonical(d, plain)) - lo;
  const double x     = above / span;  // in [0, 1]
  if (m->steps >= 2) {
    const double last = m->steps - 1u;
    const double pos  = x * last;
    assert(pos >= 0.0 && pos <= last);  // x in [0, 1]: the value is canonical
    const auto   k    = static_cast<uint32_t>(pos + 0.5);
    return static_cast<float>(k / last);
  }
  if (m->taper == Taper::Square) return static_cast<float>(detmath::SqrtD(x));
  if (m->taper == Taper::Quartic) {
    const double r = detmath::SqrtD(x);
    return static_cast<float>(detmath::SqrtD(r));
  }
  return static_cast<float>(x);
}

BRAINSCAPE_FP_BODY size_t FormatPlainBody(ParamId id, float plain, char* out,
                                          size_t outSize) noexcept {
  out[0]                   = '\0';
  const ParamDescriptor* d = FindParam(id);
  const ParamDisplay*    m = FindParamDisplay(id);
  if (d == nullptr || m == nullptr) return 0;
  const float v = Canonical(d, plain);
  Text        t(out, outSize);
  switch (m->kind) {
    case DisplayKind::Milliseconds:
      FormatMs(&t, v);
      break;
    case DisplayKind::Hertz:
      FormatHz(&t, v);
      break;
    case DisplayKind::FilterCutoff:
      // The engine bypasses the stage at max - 0.5 Hz (Engine.cpp, RebuildPostParams) and kills
      // the wet signal at the minimum (WetGainTarget, mode-compiler.md §4.3).
      if (v >= d->max - 0.5f) {
        t.Put("Off");
      } else if (!(v > d->min)) {
        t.Put("Kill");
      } else {
        FormatHz(&t, v);
      }
      break;
    case DisplayKind::Percent: {
      const double pc = static_cast<double>(v) * 100.0;
      PutFixed(&t, pc, pc < 10.0 ? 1u : 0u, false, "%");
      break;
    }
    case DisplayKind::Balance: {
      const double pc  = (static_cast<double>(v) - 0.5) * 200.0;
      const double mag = pc < 0.0 ? -pc : pc;
      if (mag < 0.05) {
        t.Put("0%");
      } else {
        PutFixed(&t, pc, mag < 10.0 ? 1u : 0u, true, "%");
      }
      break;
    }
    case DisplayKind::Amount: {
      const double a = static_cast<double>(v) * 100.0;
      PutFixed(&t, a, a < 10.0 ? 1u : 0u, false);
      break;
    }
    case DisplayKind::Decibels:
      if (v > -0.05f && v < 0.05f) {
        t.Put("0.0 dB");
      } else {
        PutFixed(&t, v, 1, true, " dB");
      }
      break;
    case DisplayKind::Semitones:
      if (v > -0.005f && v < 0.005f) {
        t.Put("0.00 st");
      } else {
        PutFixed(&t, v, 2, true, " st");
      }
      break;
    case DisplayKind::Cents:
      PutFixed(&t, v, 1, false, " ct");
      break;
    case DisplayKind::FilterMorph:
      FormatMorph(&t, v);
      break;
    case DisplayKind::OffOn:
      t.Put(v >= 0.5f ? "On" : "Off");
      break;
    case DisplayKind::LiveMark:
      t.Put(v >= 0.5f ? "Mark" : "Live");
      break;
    case DisplayKind::Count:  // the integer the engine reads (mode-compiler.md §3.7)
      PutFixed(&t, detmath::RoundHalfAwayI32(v), 0, false);
      break;
    case DisplayKind::MsOrOff:
      if (v == 0.0f) {
        t.Put("Off");
      } else {
        FormatMs(&t, v);
      }
      break;
    case DisplayKind::Signed: {
      const double pc  = static_cast<double>(v) * 100.0;
      const double mag = pc < 0.0 ? -pc : pc;
      if (mag < 0.05) {
        t.Put("0%");
      } else {
        PutFixed(&t, pc, mag < 10.0 ? 1u : 0u, true, "%");
      }
      break;
    }
    case DisplayKind::ReverbMode: {
      static constexpr const char* kModes[] = {"Bright room", "Dark medium", "Large hall", "Ambient"};
      const int32_t n = detmath::RoundHalfAwayI32(v);
      t.Put(kModes[n < 0 ? 0 : (n > 3 ? 3 : n)]);
      break;
    }
    case DisplayKind::Division: {
      // docs/design/clock.md §5.2, §5.4: the note value's name, its triplet or dotted suffix in
      // upper case ("1/8D", "1/16T"); the host's text stays the code's name whatever the tempo.
      const int32_t n = detmath::RoundHalfAwayI32(v);
      t.Put(kDivisionNames[n < 0 ? 0 : (n > 16 ? 16 : n)]);
      break;
    }
    case DisplayKind::SubdivPosition: {
      // §5.1: the knob's positions in the Microcosm's CC#5 order, written as rates (x, U+00D7
      // in UTF-8), never as note values.
      static constexpr const char* kPositions[] = {"\xC3\x97" "1/4", "\xC3\x97" "1/2", "TAP",
                                                   "\xC3\x97" "2",   "\xC3\x97" "4",
                                                   "\xC3\x97" "8"};
      const int32_t n = detmath::RoundHalfAwayI32(v);
      t.Put(kPositions[n < 0 ? 0 : (n > 5 ? 5 : n)]);
      break;
    }
    case DisplayKind::TimeMode: {
      static constexpr const char* kModes[] = {"Free", "Subdiv", "Tempo"};
      const int32_t n = detmath::RoundHalfAwayI32(v);
      t.Put(kModes[n < 0 ? 0 : (n > 2 ? 2 : n)]);
      break;
    }
    case DisplayKind::TempoRecall:
      t.Put(v >= 0.5f ? "Preset" : "Keep");
      break;
  }
  return t.Finish();
}

}  // namespace

const ParamDisplay* FindParamDisplay(ParamId id) noexcept {
  const auto raw = static_cast<uint32_t>(id);
  if (raw < 1 || raw > kNumParams) return nullptr;
  return &kDisplayTable[raw - 1];
}

const char* GroupTitle(ParamGroup group) noexcept {
  const auto i = static_cast<size_t>(group);
  return i < kNumParamGroups ? kGroupTitles[i] : "";
}

float PlainFromNormalized(ParamId id, float normalized) noexcept {
  const detail::FpEnvGuard guard;
  return PlainFromNormalizedBody(id, normalized);
}

float NormalizedFromPlain(ParamId id, float plain) noexcept {
  const detail::FpEnvGuard guard;
  return NormalizedFromPlainBody(id, plain);
}

size_t FormatPlain(ParamId id, float plain, char* out, size_t outSize) noexcept {
  if (out == nullptr || outSize == 0) return 0;
  const detail::FpEnvGuard guard;
  return FormatPlainBody(id, plain, out, outSize);
}

size_t FormatSyncedTime(tempo::SyncTarget target, uint8_t code, uint32_t nsPerQuarter,
                        uint8_t subdiv, uint8_t timeMode, uint32_t rate, char* out,
                        size_t outSize) noexcept {
  if (out == nullptr || outSize == 0) return 0;
  const detail::FpEnvGuard guard;
  return FormatSyncedTimeBody(target, code, nsPerQuarter, subdiv, timeMode, rate, out, outSize);
}

}  // namespace brainscape
