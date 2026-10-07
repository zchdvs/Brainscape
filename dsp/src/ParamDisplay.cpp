#include "detail/FpProfilePrivate.h"

#include "brainscape/ParamDisplay.h"

#include <cassert>
#include <cstdint>
#include <cstring>

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

constexpr uint16_t kAuto = kParamAutomatable;
constexpr uint16_t kStep = kParamAutomatable | kParamDiscrete;

using G = ParamGroup;
using K = DisplayKind;
using T = Taper;

constexpr ParamDisplay kDisplayTable[] = {
    {ParamId::DelayMs,        G::GrainDelay, "Delay time",          "Time",     T::Quartic, K::Milliseconds, 0, kAuto},
    {ParamId::Mix,            G::GrainDelay, "Mix",                 "Mix",      T::Linear,  K::Percent,      0, kAuto},
    {ParamId::Feedback,       G::GrainDelay, "Feedback",            "Feedback", T::Linear,  K::Percent,      0, kAuto},
    {ParamId::WetTrimDb,      G::GrainDelay, "Wet trim",            "Trim",     T::Linear,  K::Decibels,     0, kAuto},
    {ParamId::GrainSizeMs,    G::Grains,     "Grain size",          "Size",     T::Quartic, K::Milliseconds, 0, kAuto},
    {ParamId::Overlap,        G::Grains,     "Grain overlap",       "Overlap",  T::Linear,  K::Percent,      0, kAuto},
    {ParamId::SprayMs,        G::Grains,     "Grain spray",         "Spray",    T::Quartic, K::Milliseconds, 0, kAuto},
    {ParamId::TransposeSt,    G::Pitch,      "Transpose",           "Pitch",    T::Linear,  K::Semitones,    0, kAuto},
    {ParamId::SpreadCents,    G::Pitch,      "Pitch spread",        "Spread",   T::Linear,  K::Cents,        0, kAuto},
    {ParamId::ReverseProb,    G::Pitch,      "Reverse probability", "Reverse",  T::Linear,  K::Percent,      0, kAuto},
    {ParamId::Jitter,         G::Grains,     "Scheduler jitter",    "Jitter",   T::Linear,  K::Percent,      0, kAuto},
    {ParamId::WindowSustain,  G::Window,     "Window sustain",      "Sustain",  T::Linear,  K::Percent,      0, kAuto},
    {ParamId::WindowSkew,     G::Window,     "Window skew",         "Skew",     T::Linear,  K::Balance,      0, kAuto},
    {ParamId::WindowSmooth,   G::Window,     "Window smoothness",   "Smooth",   T::Linear,  K::Percent,      0, kAuto},
    {ParamId::PanSpread,      G::Grains,     "Pan spread",          "Pan",      T::Linear,  K::Percent,      0, kAuto},
    {ParamId::ModRateHz,      G::Mod,        "Mod rate",            "Rate",     T::Quartic, K::Hertz,        0, kAuto},
    {ParamId::ModDepth,       G::Mod,        "Mod depth",           "Depth",    T::Linear,  K::Percent,      0, kAuto},
    {ParamId::DelayTimeMs,    G::PostDelay,  "Post delay time",     "Time",     T::Square,  K::Milliseconds, 0, kAuto},
    {ParamId::DelayFb,        G::PostDelay,  "Post delay feedback", "Repeats",  T::Linear,  K::Percent,      0, kAuto},
    {ParamId::DelayMix,       G::PostDelay,  "Post delay mix",      "Mix",      T::Linear,  K::Percent,      0, kAuto},
    {ParamId::ReverbTime,     G::Reverb,     "Reverb time",         "Time",     T::Linear,  K::Amount,       0, kAuto},
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
    "Grain delay", "Grains", "Pitch", "Window", "Mod", "Post delay", "Reverb", "Filter", "Triggers",
};

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
      // The engine bypasses the stage at max - 0.5 Hz (Engine.cpp, RebuildPostParams).
      if (v >= d->max - 0.5f) {
        t.Put("Off");
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

}  // namespace brainscape
