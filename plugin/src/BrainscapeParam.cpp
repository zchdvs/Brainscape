#include "BrainscapeParam.h"

#include <cstdlib>
#include <string>

#include "Number.h"  // brainscape_compiler: the exact binary32 reader (mode-compiler.md §6.5)

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

// Correctly rounded decimal -> binary32 (companion §6.4) by the compiler's exact reader
// (mode-compiler.md §6.5): integers only, no locale and no C library, so typed values and
// compiled ones have the same bits on every host, macOS included, where libc++ gates float
// from_chars. Its lenient entry takes the typed forms looser than JSON (".5", "5.", "05",
// "+25"); a magnitude past FLT_MAX is refused.
bool ParseFloat(const std::string& s, float& out) {
  return bsc::ParseTypedNumber(s.data(), s.size(), &out) == bsc::NumberStatus::Ok;
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
    case DisplayKind::MsOrOff:
      if (unit == "s" || unit == "sec") shift = 3;
      return unit.empty() || unit == "ms" || shift != 0;
    case DisplayKind::Hertz:
    case DisplayKind::FilterCutoff:
      if (unit == "k" || unit == "khz") shift = 3;
      return unit.empty() || unit == "hz" || shift != 0;
    case DisplayKind::Percent:  // the display is in percent, so bare numbers are too
    case DisplayKind::Amount:   // 0-100, so a bare number is a hundredth of the plain range
    case DisplayKind::Signed:   // -100..+100 % of a -1..1 value
      shift = -2;
      return unit.empty() || unit == "%";
    case DisplayKind::Balance:  // not a shift: BalanceDecimal maps it
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
    case DisplayKind::Count:
    case DisplayKind::ReverbMode:
    case DisplayKind::Division:
    case DisplayKind::SubdivPosition:
    case DisplayKind::TimeMode:
    case DisplayKind::TempoRecall:
      return unit.empty();
  }
  return false;
}

// Unsigned decimal digit strings, most significant first.
std::string AddDigits(const std::string& a, const std::string& b) {
  std::string r;
  int         carry = 0;
  for (size_t i = 0; i < a.size() || i < b.size() || carry != 0; ++i) {
    int d = carry;
    if (i < a.size()) d += a[a.size() - 1 - i] - '0';
    if (i < b.size()) d += b[b.size() - 1 - i] - '0';
    r.push_back(static_cast<char>('0' + d % 10));
    carry = d / 10;
  }
  return std::string(r.rbegin(), r.rend());
}

bool DigitsLess(const std::string& a, const std::string& b) {  // no leading zeros
  return a.size() != b.size() ? a.size() < b.size() : a < b;
}

std::string SubDigits(const std::string& a, const std::string& b) {  // a >= b
  std::string r;
  int         borrow = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    int d = a[a.size() - 1 - i] - '0' - borrow;
    if (i < b.size()) d -= b[b.size() - 1 - i] - '0';
    borrow = d < 0 ? 1 : 0;
    r.push_back(static_cast<char>('0' + d + 10 * borrow));
  }
  while (r.size() > 1u && r.back() == '0') r.pop_back();
  return std::string(r.rbegin(), r.rend());
}

std::string MulDigits(const std::string& a, int m) {
  std::string r;
  int         carry = 0;
  for (size_t i = 0; i < a.size() || carry != 0; ++i) {
    int d = carry;
    if (i < a.size()) d += (a[a.size() - 1 - i] - '0') * m;
    r.push_back(static_cast<char>('0' + d % 10));
    carry = d / 10;
  }
  return std::string(r.rbegin(), r.rend());
}

// A Balance display shows plain p as b = (p - 0.5) * 200 %, so typed b is plain
// (100 + b) / 200 = (100 + b) * 5e-3. Computed here exactly in decimal, so the parser's
// single rounding still lands on the float nearest the typed value (companion §6.4).
std::string BalanceDecimal(const std::string& mantissa, long exponent) {
  const bool  negative = !mantissa.empty() && mantissa[0] == '-';
  std::string digits;
  long        fraction = 0;
  bool        point    = false;
  for (size_t i = negative ? 1u : 0u; i < mantissa.size(); ++i) {
    if (mantissa[i] == '.') {
      point = true;
    } else {
      digits.push_back(mantissa[i]);
      fraction += point ? 1 : 0;
    }
  }
  const size_t lead = digits.find_first_not_of('0');
  if (lead == std::string::npos) return "0.5";
  digits.erase(0, lead);
  const long pointExp  = exponent - fraction;  // |b| = digits * 10^pointExp
  const long magnitude = static_cast<long>(digits.size()) + pointExp;
  if (magnitude > 3) return negative ? "0" : "1";  // |b| >= 1000 %: clamped either way
  // |b| < 1e-12 %, so |plain - 0.5| < 5e-15, far inside half the float spacing at 0.5.
  if (magnitude < -12) return "0.5";
  const long        scale = pointExp < 0 ? -pointExp : 0;  // integers in units of 10^-scale
  const std::string b     = digits + std::string(static_cast<size_t>(pointExp > 0 ? pointExp : 0), '0');
  const std::string h     = "100" + std::string(static_cast<size_t>(scale), '0');
  if (negative && DigitsLess(h, b)) return "0";  // below -100 %: clamped to the minimum
  const std::string sum = negative ? SubDigits(h, b) : AddDigits(h, b);
  return MulDigits(sum, 5) + "e-" + std::to_string(scale + 3);
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
      {DisplayKind::MsOrOff, "off", 0.0f},    {DisplayKind::Division, "off", 0.0f},
  };
  // The cutoff's two named ends, as FormatPlain shows them: Off (the bypass, its maximum) and
  // Kill (the wet kill, its minimum; mode-compiler.md §4.3).
  if (m.kind == DisplayKind::FilterCutoff && (t == "off" || t == "kill")) {
    out = t == "off" ? d.max : d.min;
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
    plainOut = Canonicalize(id, named);
    return true;
  }
  std::string mantissa, suffix;
  long        exponent = 0, shift = 0;
  if (!SplitNumber(t, mantissa, exponent, suffix) || !UnitShift(m->kind, suffix, shift)) {
    return false;
  }
  const std::string decimal = m->kind == DisplayKind::Balance
                                  ? BalanceDecimal(mantissa, exponent)
                                  : mantissa + "e" + std::to_string(exponent + shift);
  float v = 0.f;
  if (!ParseFloat(decimal, v)) return false;
  plainOut = Canonicalize(id, v);
  return true;
}

juce::String FormatPlainText(ParamId id, float plain) {
  char buf[48];
  FormatPlain(id, plain, buf, sizeof buf);
  return juce::String(buf);
}

WrapperEvent::Type BrainscapeParam::EventTypeFor(ParamId id) noexcept {
  if (FindParam(id) != nullptr && FindParam(id)->kind == ParamKind::Macro) return WrapperEvent::Type::Macro;
  if (id == ParamId::PerfExpression) return WrapperEvent::Type::Expression;
  return WrapperEvent::Type::Param;
}

BrainscapeParam::BrainscapeParam(ParamId id, EventSink& sink)
    : juce::RangedAudioParameter(juce::ParameterID{Descriptor(id).name, 1},
                                 FindParamDisplay(id)->title, Attributes(*FindParamDisplay(id))),
      id_(id),
      eventType_(EventTypeFor(id)),
      display_(*FindParamDisplay(id)),
      defaultPlain_(Canonicalize(id, Descriptor(id).def)),
      sink_(sink),
      plain_(defaultPlain_),
      range_(Descriptor(id).min, Descriptor(id).max,
             [id](float, float, float n) { return PlainFromNormalized(id, n); },
             [id](float, float, float v) { return NormalizedFromPlain(id, v); },
             [id](float, float, float v) { return Canonicalize(id, v); }) {}

void BrainscapeParam::SetPlainNotifyingHost(float plain) {
  beginChangeGesture();
  SetPlainInGesture(plain);
  endChangeGesture();
}

void BrainscapeParam::SetPlainInGesture(float plain) {
  const float canonical = Canonicalize(id_, plain);
  if (canonical == Plain()) return;
  plain_.store(canonical, std::memory_order_relaxed);
  sink_.Post({eventType_, WrapperEvent::Source::Ui, EventId(), canonical});
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
  sink_.Post({eventType_, WrapperEvent::Source::Host, EventId(), plain});
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
    : juce::AudioParameterBool(juce::ParameterID{FindParam(ParamId::PerfFreeze)->name, 1},
                               FindParamDisplay(ParamId::PerfFreeze)->title, false),
      sink_(sink) {}

void FreezeParam::valueChanged(bool on) {
  sink_.Post({WrapperEvent::Type::Freeze, WrapperEvent::Source::Host, 0u, on ? 1.0f : 0.0f});
}

}  // namespace brainscape::plugin
