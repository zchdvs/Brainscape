#include "BrainscapeParam.h"

#include <cstdlib>
#include <string>

#if __has_include(<version>)
#include <version>
#endif
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
#include <charconv>
#define BRAINSCAPE_HAVE_FLOAT_FROM_CHARS 1
#endif

namespace brainscape::plugin {

namespace {

// Set while this thread is inside its own setValueNotifyingHost, so the nested setValue
// does not re-map the plain value it just stored (another thread's sets still apply).
thread_local bool tlsOwnNotify = false;

const ParamDescriptor& Descriptor(ParamId id) { return *FindParam(id); }

juce::AudioProcessorParameterWithIDAttributes Attributes(const ParamDisplay& m) {
  return juce::AudioProcessorParameterWithIDAttributes().withAutomatable(
      (m.flags & kParamAutomatable) != 0u);
}

// Correctly rounded decimal -> binary32 (companion §6.4). from_chars is locale-free;
// the strtof fallback (older libc++) follows LC_NUMERIC, which JUCE leaves at "C".
// TODO(companion §6.4): vendored fast_float once it is pinned.
bool ParseFloat(const std::string& s, float& out) {
#if defined(BRAINSCAPE_HAVE_FLOAT_FROM_CHARS)
  const char* end = s.data() + s.size();
  const auto  res = std::from_chars(s.data(), end, out);
  return res.ec == std::errc() && res.ptr == end;
#else
  char*       end = nullptr;
  const float v   = std::strtof(s.c_str(), &end);
  if (end == s.c_str() || *end != '\0') return false;
  out = v;
  return true;
#endif
}

// Splits "12.5e-1 ms" into the decimal mantissa "12.5", its exponent -1 and the suffix
// "ms". Returns false if no digits lead the text.
bool SplitNumber(const std::string& t, std::string& mantissa, long& exponent,
                 std::string& suffix) {
  const auto isDigit = [&](size_t k) { return k < t.size() && t[k] >= '0' && t[k] <= '9'; };
  const size_t start = (!t.empty() && t[0] == '+') ? 1u : 0u;
  size_t       i     = (!t.empty() && (t[0] == '+' || t[0] == '-')) ? 1u : 0u;
  bool         digits = false;
  for (; isDigit(i); ++i) digits = true;
  if (i < t.size() && t[i] == '.') {
    for (++i; isDigit(i); ++i) digits = true;
  }
  if (!digits) return false;
  mantissa = t.substr(start, i - start);
  exponent = 0;
  if (i < t.size() && t[i] == 'e') {
    size_t j = i + 1;
    if (j < t.size() && (t[j] == '+' || t[j] == '-')) ++j;
    const size_t expDigits = j;
    while (j < t.size() && t[j] >= '0' && t[j] <= '9') ++j;
    if (j > expDigits && j - expDigits < 6) {
      exponent = std::strtol(t.substr(i + 1, j - i - 1).c_str(), nullptr, 10);
      i        = j;
    }
  }
  suffix = juce::String(t.substr(i)).trim().toStdString();
  return true;
}

// Decimal-exponent shift that converts the typed unit into the plain unit, or false if
// the suffix is not a unit of this parameter.
bool UnitShift(DisplayKind kind, const std::string& unit, long& shift) {
  shift = 0;
  switch (kind) {
    case DisplayKind::Milliseconds:
      if (unit == "s" || unit == "sec") shift = 3;
      return unit.empty() || unit == "ms" || shift != 0;
    case DisplayKind::Hertz:
    case DisplayKind::FilterCutoff:
      if (unit == "k" || unit == "khz") shift = 3;
      return unit.empty() || unit == "hz" || shift != 0;
    case DisplayKind::Percent:  // the display is in percent, so bare numbers are too
      shift = -2;
      return unit.empty() || unit == "%";
    case DisplayKind::Decibels:
      return unit.empty() || unit == "db";
    case DisplayKind::Semitones:
      return unit.empty() || unit == "st";
    case DisplayKind::Cents:
      return unit.empty() || unit == "c" || unit == "ct" || unit == "cents";
    case DisplayKind::FilterMorph:
    case DisplayKind::OffOn:
    case DisplayKind::LiveMark:
      return unit.empty();
  }
  return false;
}

bool NamedValue(const ParamDisplay& m, const ParamDescriptor& d, const std::string& t,
                float& out) {
  struct Name {
    DisplayKind kind;
    const char* text;
    float       value;
  };
  static const Name kNames[] = {
      {DisplayKind::OffOn, "on", 1.0f},     {DisplayKind::OffOn, "yes", 1.0f},
      {DisplayKind::OffOn, "true", 1.0f},   {DisplayKind::OffOn, "off", 0.0f},
      {DisplayKind::OffOn, "no", 0.0f},     {DisplayKind::OffOn, "false", 0.0f},
      {DisplayKind::LiveMark, "live", 0.0f}, {DisplayKind::LiveMark, "mark", 1.0f},
      {DisplayKind::FilterMorph, "lp", 0.0f}, {DisplayKind::FilterMorph, "bp", 1.0f},
      {DisplayKind::FilterMorph, "hp", 2.0f}, {DisplayKind::FilterMorph, "notch", 3.0f},
  };
  if (m.kind == DisplayKind::FilterCutoff && t == "off") {
    out = d.max;
    return true;
  }
  for (const Name& n : kNames) {
    if (n.kind == m.kind && t == n.text) {
      out = n.value;
      return true;
    }
  }
  return false;
}

}  // namespace

bool ParsePlainText(ParamId id, const juce::String& text, float& plainOut) {
  const ParamDescriptor* d = FindParam(id);
  const ParamDisplay*    m = FindParamDisplay(id);
  if (d == nullptr || m == nullptr) return false;
  const std::string t = text.trim().toLowerCase().toStdString();
  if (t.empty()) return false;

  float named = 0.f;
  if (NamedValue(*m, *d, t, named)) {
    plainOut = CanonicalizePlain(id, named);
    return true;
  }
  std::string mantissa, suffix;
  long        exponent = 0, shift = 0;
  if (!SplitNumber(t, mantissa, exponent, suffix) || !UnitShift(m->kind, suffix, shift)) {
    return false;
  }
  float v = 0.f;
  if (!ParseFloat(mantissa + "e" + std::to_string(exponent + shift), v)) return false;
  plainOut = CanonicalizePlain(id, v);
  return true;
}

juce::String FormatPlainText(ParamId id, float plain) {
  char buf[48];
  FormatPlain(id, plain, buf, sizeof buf);
  return juce::String(buf);
}

BrainscapeParam::BrainscapeParam(ParamId id, EventSink& sink)
    : juce::RangedAudioParameter(juce::ParameterID{Descriptor(id).name, 1},
                                 FindParamDisplay(id)->title, Attributes(*FindParamDisplay(id))),
      id_(id),
      display_(*FindParamDisplay(id)),
      defaultPlain_(CanonicalizePlain(id, Descriptor(id).def)),
      sink_(sink),
      plain_(defaultPlain_),
      range_(Descriptor(id).min, Descriptor(id).max,
             [id](float, float, float n) { return PlainFromNormalized(id, n); },
             [id](float, float, float v) { return NormalizedFromPlain(id, v); },
             [id](float, float, float v) { return CanonicalizePlain(id, v); }) {}

void BrainscapeParam::SetPlainNotifyingHost(float plain) {
  beginChangeGesture();
  SetPlainInGesture(plain);
  endChangeGesture();
}

void BrainscapeParam::SetPlainInGesture(float plain) {
  const float canonical = CanonicalizePlain(id_, plain);
  if (canonical == Plain()) return;
  plain_.store(canonical, std::memory_order_relaxed);
  sink_.Post({WrapperEvent::Type::Param, WrapperEvent::Source::Ui, static_cast<uint32_t>(id_),
              canonical});
  tlsOwnNotify = true;
  setValueNotifyingHost(getValue());
  tlsOwnNotify = false;
}

float BrainscapeParam::getValue() const { return NormalizedFromPlain(id_, Plain()); }

void BrainscapeParam::setValue(float newValue) {
  // An exact echo of the stored normalised view (VST3 setComponentState, a host replaying
  // what it read) keeps the exact plain bits; anything else is a lossy host set.
  if (tlsOwnNotify || newValue == getValue()) return;
  const float plain = PlainFromNormalized(id_, newValue);
  plain_.store(plain, std::memory_order_relaxed);
  sink_.Post({WrapperEvent::Type::Param, WrapperEvent::Source::Host, static_cast<uint32_t>(id_),
              plain});
}

float BrainscapeParam::getDefaultValue() const { return NormalizedFromPlain(id_, defaultPlain_); }

juce::String BrainscapeParam::getText(float normalisedValue, int maximumStringLength) const {
  const juce::String s = FormatPlainText(id_, PlainFromNormalized(id_, normalisedValue));
  return maximumStringLength > 0 ? s.substring(0, maximumStringLength) : s;
}

float BrainscapeParam::getValueForText(const juce::String& text) const {
  float plain = 0.f;
  if (!ParsePlainText(id_, text, plain)) return getValue();
  return NormalizedFromPlain(id_, plain);
}

juce::String BrainscapeParam::getCurrentValueAsText() const { return FormatPlainText(id_, Plain()); }

int BrainscapeParam::getNumSteps() const {
  return display_.steps >= 2 ? static_cast<int>(display_.steps)
                             : AudioProcessorParameter::getDefaultNumParameterSteps();
}

bool BrainscapeParam::isDiscrete() const { return (display_.flags & kParamDiscrete) != 0u; }

bool BrainscapeParam::isBoolean() const { return display_.kind == DisplayKind::OffOn; }

FreezeParam::FreezeParam(EventSink& sink)
    : juce::AudioParameterBool(juce::ParameterID{"perf.freeze", 1}, "Freeze", false),
      sink_(sink) {}

void FreezeParam::valueChanged(bool on) {
  sink_.Post({WrapperEvent::Type::Freeze, WrapperEvent::Source::Host, 0u, on ? 1.0f : 0.0f});
}

}  // namespace brainscape::plugin
