#include "brainscape/ParamDisplay.h"

#include <cmath>
#include <cstdio>
#include <cstring>

// The taper functions feed the engine (the pedal's pot path and every plugin knob), so
// they belong inside the full control-word guard once profile §5.3 lands; until then
// they run in the caller's environment.

namespace brainscape {

namespace {

constexpr uint16_t kAuto = kParamAutomatable;
constexpr uint16_t kStep = kParamAutomatable | kParamDiscrete;

using G = ParamGroup;
using K = DisplayKind;
using T = Taper;

constexpr ParamDisplay kDisplayTable[] = {
    {ParamId::DelayMs,        G::GrainDelay, "Delay time",          "Time",     T::Quartic, K::Milliseconds, 0, kAuto},
    {ParamId::Mix,            G::GrainDelay, "Mix",                 "Mix",      T::Linear,  K::Percent,      0, kAuto},
    {ParamId::Feedback,       G::GrainDelay, "Feedback",            "Feedback", T::Linear,  K::Percent,      0, kAuto},
    {ParamId::OutTrimDb,      G::GrainDelay, "Output trim",         "Trim",     T::Linear,  K::Decibels,     0, kAuto},
    {ParamId::GrainSizeMs,    G::Grains,     "Grain size",          "Size",     T::Quartic, K::Milliseconds, 0, kAuto},
    {ParamId::Overlap,        G::Grains,     "Grain overlap",       "Overlap",  T::Linear,  K::Percent,      0, kAuto},
    {ParamId::SprayMs,        G::Grains,     "Grain spray",         "Spray",    T::Quartic, K::Milliseconds, 0, kAuto},
    {ParamId::PitchSt,        G::Pitch,      "Pitch",               "Pitch",    T::Linear,  K::Semitones,    0, kAuto},
    {ParamId::SpreadCents,    G::Pitch,      "Pitch spread",        "Spread",   T::Linear,  K::Cents,        0, kAuto},
    {ParamId::ReverseProb,    G::Pitch,      "Reverse probability", "Reverse",  T::Linear,  K::Percent,      0, kAuto},
    {ParamId::Jitter,         G::Grains,     "Scheduler jitter",    "Jitter",   T::Linear,  K::Percent,      0, kAuto},
    {ParamId::WindowSustain,  G::Window,     "Window sustain",      "Sustain",  T::Linear,  K::Percent,      0, kAuto},
    {ParamId::WindowSkew,     G::Window,     "Window skew",         "Skew",     T::Linear,  K::Percent,      0, kAuto},
    {ParamId::WindowSmooth,   G::Window,     "Window smoothness",   "Smooth",   T::Linear,  K::Percent,      0, kAuto},
    {ParamId::PanSpread,      G::PanMod,     "Pan spread",          "Pan",      T::Linear,  K::Percent,      0, kAuto},
    {ParamId::ModRateHz,      G::PanMod,     "Mod rate",            "Rate",     T::Quartic, K::Hertz,        0, kAuto},
    {ParamId::ModDepth,       G::PanMod,     "Mod depth",           "Depth",    T::Linear,  K::Percent,      0, kAuto},
    {ParamId::DelayTimeMs,    G::PostDelay,  "Post delay time",     "Time",     T::Square,  K::Milliseconds, 0, kAuto},
    {ParamId::DelayFb,        G::PostDelay,  "Post delay feedback", "Repeats",  T::Linear,  K::Percent,      0, kAuto},
    {ParamId::DelayMix,       G::PostDelay,  "Post delay mix",      "Mix",      T::Linear,  K::Percent,      0, kAuto},
    {ParamId::ReverbTime,     G::Reverb,     "Reverb time",         "Time",     T::Linear,  K::Percent,      0, kAuto},
    {ParamId::ReverbMix,      G::Reverb,     "Reverb mix",          "Mix",      T::Linear,  K::Percent,      0, kAuto},
    {ParamId::FilterCutoffHz, G::Filter,     "Filter cutoff",       "Cutoff",   T::Quartic, K::FilterCutoff, 0, kAuto},
    {ParamId::FilterRes,      G::Filter,     "Filter resonance",    "Reso",     T::Linear,  K::Percent,      0, kAuto},
    {ParamId::FilterMorph,    G::Filter,     "Filter morph",        "Morph",    T::Linear,  K::FilterMorph,  0, kAuto},
    {ParamId::TriggerSens,    G::Triggers,   "Trigger sensitivity", "Sense",    T::Linear,  K::Percent,      0, kAuto},
    {ParamId::OnsetTrigger,   G::Triggers,   "Onset trigger",       "Onset",    T::Linear,  K::OffOn,        2, kStep},
    {ParamId::PositionSource, G::Triggers,   "Position source",     "Position", T::Linear,  K::LiveMark,     2, kStep},
};
static_assert(sizeof(kDisplayTable) / sizeof(kDisplayTable[0]) == kNumParams,
              "every descriptor needs a display row");

constexpr bool DisplayTableMatchesDescriptors() {
  for (size_t i = 0; i < kNumParams; ++i) {
    if (kDisplayTable[i].id != kParamTable[i].id) return false;
  }
  return true;
}
static_assert(DisplayTableMatchesDescriptors(), "kDisplayTable must follow kParamTable's order");

constexpr const char* kGroupTitles[kNumParamGroups] = {
    "Grain delay", "Grains", "Pitch", "Window", "Pan / Mod", "Post delay", "Reverb", "Filter",
    "Triggers",
};

uint32_t Bits(float v) noexcept {
  uint32_t u = 0;
  std::memcpy(&u, &v, sizeof u);
  return u;
}

// NaN -> 0, then [0, 1]; subnormals -> 0. Bit tests, as in CanonicalizePlain, so a DAZ
// host thread maps the same inputs to the same outputs.
double UnitFromNormalized(float n) noexcept {
  const uint32_t u        = Bits(n);
  const uint32_t exponent = u & 0x7F800000u;
  const bool     negative = (u >> 31) != 0u;
  if (exponent == 0x7F800000u) return ((u & 0x007FFFFFu) != 0u || negative) ? 0.0 : 1.0;
  if (exponent == 0u || negative) return 0.0;
  if (n >= 1.0f) return 1.0;
  return static_cast<double>(n);
}

int Print(char* out, size_t outSize, const char* fmt, double v) noexcept {
  return std::snprintf(out, outSize, fmt, v);
}

int FormatMs(char* out, size_t outSize, float v) noexcept {
  if (v < 10.0f) return Print(out, outSize, "%.2f ms", v);
  if (v < 100.0f) return Print(out, outSize, "%.1f ms", v);
  if (v < 1000.0f) return Print(out, outSize, "%.0f ms", v);
  return Print(out, outSize, "%.2f s", static_cast<double>(v) * 0.001);
}

int FormatHz(char* out, size_t outSize, float v) noexcept {
  if (v < 10.0f) return Print(out, outSize, "%.2f Hz", v);
  if (v < 100.0f) return Print(out, outSize, "%.1f Hz", v);
  if (v < 1000.0f) return Print(out, outSize, "%.0f Hz", v);
  if (v < 10000.0f) return Print(out, outSize, "%.2f kHz", static_cast<double>(v) * 0.001);
  return Print(out, outSize, "%.1f kHz", static_cast<double>(v) * 0.001);
}

int FormatMorph(char* out, size_t outSize, float v) noexcept {
  static constexpr const char* kNames[] = {"LP", "BP", "HP", "Notch"};
  const auto seg  = static_cast<int>(v);  // canonical: v in [0, 3]
  const double pc = (static_cast<double>(v) - seg) * 100.0;
  if (seg >= 3 || pc < 0.5) return std::snprintf(out, outSize, "%s", kNames[seg < 3 ? seg : 3]);
  if (pc >= 99.5) return std::snprintf(out, outSize, "%s", kNames[seg + 1]);
  return std::snprintf(out, outSize, "%s>%s %.0f%%", kNames[seg], kNames[seg + 1], pc);
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

float CanonicalizePlain(ParamId id, float plain) noexcept {
  const ParamDescriptor* d = FindParam(id);
  if (d == nullptr) return 0.0f;
  const uint32_t exponent = Bits(plain) & 0x7F800000u;
  if (exponent == 0x7F800000u) return d->min;
  if (exponent == 0u) plain = 0.0f;
  if (plain < d->min) plain = d->min;
  if (plain > d->max) plain = d->max;
  return plain;
}

float PlainFromNormalized(ParamId id, float normalized) noexcept {
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
    const auto   k    = static_cast<uint32_t>(pos + 0.5);  // pos in [0, last]: in range
    const double frac = k / last;
    const double step = span * frac;
    return CanonicalizePlain(id, static_cast<float>(lo + step));
  }
  double shaped = n;
  if (m->taper == Taper::Square) {
    shaped = n * n;
  } else if (m->taper == Taper::Quartic) {
    const double sq = n * n;
    shaped          = sq * sq;
  }
  const double offset = span * shaped;
  return CanonicalizePlain(id, static_cast<float>(lo + offset));
}

float NormalizedFromPlain(ParamId id, float plain) noexcept {
  const ParamDescriptor* d = FindParam(id);
  const ParamDisplay*    m = FindParamDisplay(id);
  if (d == nullptr || m == nullptr) return 0.0f;
  const double lo    = d->min;
  const double hi    = d->max;
  const double span  = hi - lo;
  const double above = static_cast<double>(CanonicalizePlain(id, plain)) - lo;
  const double x     = above / span;  // in [0, 1]
  if (m->steps >= 2) {
    const double last = m->steps - 1u;
    const double pos  = x * last;
    const auto   k    = static_cast<uint32_t>(pos + 0.5);
    return static_cast<float>(k / last);
  }
  if (m->taper == Taper::Square) return static_cast<float>(std::sqrt(x));
  if (m->taper == Taper::Quartic) {
    const double r = std::sqrt(x);
    return static_cast<float>(std::sqrt(r));
  }
  return static_cast<float>(x);
}

size_t FormatPlain(ParamId id, float plain, char* out, size_t outSize) noexcept {
  if (out == nullptr || outSize == 0) return 0;
  out[0]                   = '\0';
  const ParamDescriptor* d = FindParam(id);
  const ParamDisplay*    m = FindParamDisplay(id);
  if (d == nullptr || m == nullptr) return 0;
  const float v = CanonicalizePlain(id, plain);
  int written   = 0;
  switch (m->kind) {
    case DisplayKind::Milliseconds:
      written = FormatMs(out, outSize, v);
      break;
    case DisplayKind::Hertz:
      written = FormatHz(out, outSize, v);
      break;
    case DisplayKind::FilterCutoff:
      // The engine bypasses the stage at max - 0.5 Hz (Engine.cpp, RebuildPostParams).
      written = v >= d->max - 0.5f ? std::snprintf(out, outSize, "Off") : FormatHz(out, outSize, v);
      break;
    case DisplayKind::Percent: {
      const double pc = static_cast<double>(v) * 100.0;
      written         = Print(out, outSize, pc < 10.0 ? "%.1f%%" : "%.0f%%", pc);
      break;
    }
    case DisplayKind::Decibels:
      written = (v > -0.05f && v < 0.05f) ? std::snprintf(out, outSize, "0.0 dB")
                                          : Print(out, outSize, "%+.1f dB", v);
      break;
    case DisplayKind::Semitones:
      written = (v > -0.005f && v < 0.005f) ? std::snprintf(out, outSize, "0.00 st")
                                            : Print(out, outSize, "%+.2f st", v);
      break;
    case DisplayKind::Cents:
      written = Print(out, outSize, "%.1f ct", v);
      break;
    case DisplayKind::FilterMorph:
      written = FormatMorph(out, outSize, v);
      break;
    case DisplayKind::OffOn:
      written = std::snprintf(out, outSize, "%s", v >= 0.5f ? "On" : "Off");
      break;
    case DisplayKind::LiveMark:
      written = std::snprintf(out, outSize, "%s", v >= 0.5f ? "Mark" : "Live");
      break;
  }
  if (written < 0) {
    out[0] = '\0';
    return 0;
  }
  const auto len = static_cast<size_t>(written);
  return len < outSize ? len : outSize - 1;
}

}  // namespace brainscape
