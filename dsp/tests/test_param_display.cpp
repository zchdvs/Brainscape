#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "FpEnvTestUtil.h"
#include "brainscape/ParamDisplay.h"
#include "catch.hpp"

using namespace brainscape;

namespace {

uint32_t Bits(float v) {
  uint32_t u = 0;
  std::memcpy(&u, &v, sizeof u);
  return u;
}

float FromBits(uint32_t u) {
  float v = 0.f;
  std::memcpy(&v, &u, sizeof v);
  return v;
}

std::string Format(ParamId id, float v) {
  char buf[32];
  FormatPlain(id, v, buf, sizeof buf);
  return buf;
}

}  // namespace

TEST_CASE("every descriptor has display metadata in table order") {
  for (size_t i = 0; i < kNumParams; ++i) {
    const ParamDisplay* m = FindParamDisplay(kParamTable[i].id);
    REQUIRE(m != nullptr);
    CHECK(m->id == kParamTable[i].id);
    CHECK(std::strlen(m->title) > 0);
    CHECK(std::strlen(GroupTitle(m->group)) > 0);
  }
  CHECK(FindParamDisplay(static_cast<ParamId>(0)) == nullptr);
  CHECK(FindParamDisplay(static_cast<ParamId>(kNumParams + 1)) == nullptr);
}

TEST_CASE("taper endpoints are exactly min and max, and the taper is monotonic") {
  for (const ParamDescriptor& d : kParamTable) {
    INFO((d.name != nullptr ? d.name : "(retired)"));
    CHECK(Bits(PlainFromNormalized(d.id, 0.0f)) == Bits(Canonicalize(d.id, d.min)));
    CHECK(Bits(PlainFromNormalized(d.id, 1.0f)) == Bits(d.max));
    float prev = PlainFromNormalized(d.id, 0.0f);
    for (int k = 1; k <= 4096; ++k) {  // a 12-bit pot: code / codeMax, exact in binary32
      const float p = PlainFromNormalized(d.id, static_cast<float>(k) / 4096.0f);
      REQUIRE(std::isfinite(p));
      REQUIRE(p >= prev);
      REQUIRE(p >= d.min);
      REQUIRE(p <= d.max);
      prev = p;
    }
  }
}

TEST_CASE("normalised view inverts the taper closely and keeps defaults in range") {
  for (const ParamDescriptor& d : kParamTable) {
    INFO((d.name != nullptr ? d.name : "(retired)"));
    const ParamDisplay* m = FindParamDisplay(d.id);
    const float nd        = NormalizedFromPlain(d.id, d.def);
    CHECK(nd >= 0.0f);
    CHECK(nd <= 1.0f);
    if (m->steps >= 2) continue;
    const double span = static_cast<double>(d.max) - d.min;
    for (int k = 0; k <= 1000; ++k) {
      const auto  p    = static_cast<float>(d.min + span * k / 1000.0);
      const float back = PlainFromNormalized(d.id, NormalizedFromPlain(d.id, p));
      REQUIRE(std::fabs(static_cast<double>(back) - p) <= 1e-5 * span);
    }
  }
}

TEST_CASE("canonicalization uses bit tests: non-finite to min, subnormal and -0 to +0") {
  const float nan  = std::numeric_limits<float>::quiet_NaN();
  const float inf  = std::numeric_limits<float>::infinity();
  const float tiny = FromBits(0x00000001u);
  CHECK(Canonicalize(ParamId::DelayMs, nan) == 1.0f);
  CHECK(Canonicalize(ParamId::DelayMs, inf) == 1.0f);
  CHECK(Canonicalize(ParamId::DelayMs, -inf) == 1.0f);
  CHECK(Canonicalize(ParamId::DelayMs, 9000.0f) == 5000.0f);
  CHECK(Bits(Canonicalize(ParamId::Mix, -0.0f)) == 0u);
  CHECK(Bits(Canonicalize(ParamId::Mix, tiny)) == 0u);
  CHECK(Bits(Canonicalize(ParamId::TransposeSt, -tiny)) == 0u);
  CHECK(Canonicalize(ParamId::DelayMs, tiny) == 1.0f);  // +0 then clamped to min
  // Authored values pass bit for bit: no grid (companion §5.5).
  CHECK(Bits(Canonicalize(ParamId::TransposeSt, 7.02f)) == Bits(7.02f));
  CHECK(Bits(Canonicalize(ParamId::FilterMorph, 0.4f)) == Bits(0.4f));
  CHECK(PlainFromNormalized(ParamId::Mix, nan) == 0.0f);
  CHECK(PlainFromNormalized(ParamId::Mix, inf) == 1.0f);
  CHECK(PlainFromNormalized(ParamId::Mix, -1.0f) == 0.0f);
}

TEST_CASE("discrete parameters snap to their positions") {
  CHECK(PlainFromNormalized(ParamId::PerfFreeze, 0.49f) == 0.0f);
  CHECK(PlainFromNormalized(ParamId::PerfFreeze, 0.5f) == 1.0f);
  CHECK(PlainFromNormalized(ParamId::ReverbMode, 1.0f) == 3.0f);
  CHECK(NormalizedFromPlain(ParamId::PerfFreeze, 0.7f) == 1.0f);
  CHECK(NormalizedFromPlain(ParamId::PerfFreeze, 0.2f) == 0.0f);
  // The retired rows keep their display rows, never registered (§4.1).
  CHECK((FindParamDisplay(ParamId::OnsetTrigger)->flags & kParamAutomatable) == 0u);
  CHECK((FindParamDisplay(ParamId::PositionSource)->flags & kParamAutomatable) == 0u);
}

TEST_CASE("the mode system's rows have display metadata (mode-compiler.md §4.3)") {
  // Integer leaves are discrete and show the integer the engine reads (§3.7).
  CHECK(Format(ParamId::VoiceCount, 64.0f) == "64");
  CHECK(Format(ParamId::Repeat, 2.5f) == "3");  // RoundHalfAwayI32
  CHECK(Format(ParamId::Repeat, 2.49f) == "2");
  CHECK(FindParamDisplay(ParamId::VoiceCount)->steps == 64);
  CHECK(PlainFromNormalized(ParamId::Repeat, 0.5f) == 9.0f);  // 16 positions: 7.5 rounds up
  CHECK(Format(ParamId::DecayMs, 0.0f) == "Off");
  CHECK(Format(ParamId::DecayMs, 1500.0f) == "1.50 s");
  CHECK(Format(ParamId::GlideCurve, -0.5f) == "-50%");
  CHECK(Format(ParamId::TriggerOffset, 0.0f) == "0%");
  CHECK(Format(ParamId::ReverbMode, 2.0f) == "Large hall");
  CHECK(Format(ParamId::DelaySync, 0.0f) == "Off");
  CHECK(Format(ParamId::DelaySync, 3.0f) == "1/16");  // docs/design/clock.md §5.2
  CHECK(Format(ParamId::DelaySync, 2.5f) == "1/16");  // RoundHalfAwayI32
  CHECK(Format(ParamId::DelaySync, 8.0f) == "1/8D");
  CHECK(Format(ParamId::DelaySync, 13.0f) == "1/1T");
  CHECK(Format(ParamId::DelaySync, 16.0f) == "2/1");
  // The tempo core's rows (docs/design/clock.md §5.1, §10.4): Subdiv's knob positions as rates.
  CHECK(Format(ParamId::PerfSubdiv, 0.0f) == "\xC3\x97" "1/4");
  CHECK(Format(ParamId::PerfSubdiv, 2.0f) == "TAP");
  CHECK(Format(ParamId::PerfSubdiv, 5.0f) == "\xC3\x97" "8");
  CHECK(Format(ParamId::PerfTimeMode, 0.0f) == "Free");
  CHECK(Format(ParamId::PerfTimeMode, 2.0f) == "Tempo");
  CHECK(Format(ParamId::TempoRecall, 0.0f) == "Keep");
  CHECK(Format(ParamId::TempoRecall, 1.0f) == "Preset");
  CHECK(Format(ParamId::EffectVolumeDb, -3.0f) == "-3.0 dB");
  CHECK(Format(ParamId::MacroFilter, 0.5f) == "50%");
  CHECK(Format(ParamId::PerfFreeze, 1.0f) == "On");
  CHECK(Format(ParamId::WetTrimDb, -3.0f) == "-3.0 dB");
  // wet_trim_db trims the wet signal only since r2 (§7.1 R3).
  CHECK(std::string(FindParamDisplay(ParamId::WetTrimDb)->title) == "Wet trim");
  CHECK(std::string(GroupTitle(FindParamDisplay(ParamId::L1DelayMs)->group)) == "Layer 2");
  CHECK(std::string(GroupTitle(FindParamDisplay(ParamId::EffectVolumeDb)->group)) == "Device");
  CHECK(std::string(FindParamDisplay(ParamId::L1TransposeSt)->title) == "Layer 2 transpose");
}

// Host model (b), provisionally (mode-compiler.md §3.6, Q12): macros, Mix, the effect volume
// and the performance rows are automatable; the other leaves are registered but not. The plugin
// applies it since lane D's curation slice registered the macro parameters (§12.4).
TEST_CASE("host automation follows the host model") {
  for (const ParamDescriptor& d : kParamTable) {
    INFO((d.name != nullptr ? d.name : "(retired)"));
    const bool automatable = (FindParamDisplay(d.id)->flags & kParamAutomatable) != 0u;
    const auto raw         = static_cast<uint32_t>(d.id);
    const bool want        = d.id == ParamId::Mix || (raw >= 69u && raw <= 80u) ||
                      d.id == ParamId::EffectVolumeDb || raw == 83u || raw == 84u;
    CHECK(automatable == want);
  }
}

TEST_CASE("display text carries units and the named endpoints") {
  CHECK(Format(ParamId::DelayMs, 250.0f) == "250 ms");
  CHECK(Format(ParamId::DelayMs, 1250.0f) == "1.25 s");
  CHECK(Format(ParamId::Mix, 0.5f) == "50%");
  CHECK(Format(ParamId::WetTrimDb, -3.0f) == "-3.0 dB");
  CHECK(Format(ParamId::TransposeSt, 7.0f) == "+7.00 st");
  CHECK(Format(ParamId::TransposeSt, 0.0f) == "0.00 st");
  CHECK(Format(ParamId::FilterCutoffHz, 20000.0f) == "Off");
  CHECK(Format(ParamId::FilterCutoffHz, 2500.0f) == "2.50 kHz");
  CHECK(Format(ParamId::FilterCutoffHz, 40.0f) == "Kill");  // the wet kill (§4.3)
  CHECK(Format(ParamId::FilterCutoffHz, 41.0f) == "41.0 Hz");
  CHECK(Format(ParamId::FilterMorph, 1.0f) == "BP");
  CHECK(Format(ParamId::FilterMorph, 0.4f) == "LP>BP 40%");
  CHECK(Format(ParamId::FilterMorph, 3.0f) == "Notch");
  CHECK(Format(ParamId::PerfFreeze, 1.0f) == "On");
  CHECK(Format(ParamId::WindowSkew, 0.5f) == "0%");  // centred: symmetric window
  CHECK(Format(ParamId::WindowSkew, 0.25f) == "-50%");
  CHECK(Format(ParamId::WindowSkew, 1.0f) == "+100%");
  CHECK(Format(ParamId::WindowSkew, 0.48f) == "-4.0%");
  CHECK(Format(ParamId::ReverbTime, 0.6f) == "60");
  CHECK(Format(ParamId::ReverbTime, 0.05f) == "5.0");
  char small[4];
  CHECK(FormatPlain(ParamId::DelayMs, 250.0f, small, sizeof small) == 3);
  CHECK(std::string(small) == "250");
}

// The previous snprintf formatter, kept as the reference: FormatPlain prints the same
// text from integers (no libc import in the engine archive, no locale). Exact decimal
// ties (k + 1/2 at the shown precision, exactly representable) are compared too: both
// round them to even on a correctly rounding C library.
namespace {

std::string RefFixed(const char* fmt, double v) {
  char buf[64];
  std::snprintf(buf, sizeof buf, fmt, v);
  return buf;
}

std::string RefMs(float v) {
  if (v < 10.0f) return RefFixed("%.2f ms", v);
  if (v < 100.0f) return RefFixed("%.1f ms", v);
  if (v < 1000.0f) return RefFixed("%.0f ms", v);
  return RefFixed("%.2f s", static_cast<double>(v) * 0.001);
}

std::string RefHz(float v) {
  if (v < 10.0f) return RefFixed("%.2f Hz", v);
  if (v < 100.0f) return RefFixed("%.1f Hz", v);
  if (v < 1000.0f) return RefFixed("%.0f Hz", v);
  if (v < 10000.0f) return RefFixed("%.2f kHz", static_cast<double>(v) * 0.001);
  return RefFixed("%.1f kHz", static_cast<double>(v) * 0.001);
}

std::string RefFormat(ParamId id, float plain) {
  const ParamDescriptor& d = *FindParam(id);
  const ParamDisplay&    m = *FindParamDisplay(id);
  const float            v = Canonicalize(id, plain);
  switch (m.kind) {
    case DisplayKind::Milliseconds: return RefMs(v);
    case DisplayKind::Hertz: return RefHz(v);
    case DisplayKind::FilterCutoff:
      return v >= d.max - 0.5f ? "Off" : v <= d.min ? "Kill" : RefHz(v);  // the wet kill
    case DisplayKind::Percent: {
      const double pc = static_cast<double>(v) * 100.0;
      return RefFixed(pc < 10.0 ? "%.1f%%" : "%.0f%%", pc);
    }
    case DisplayKind::Balance: {
      const double pc  = (static_cast<double>(v) - 0.5) * 200.0;
      const double mag = std::fabs(pc);
      return mag < 0.05 ? "0%" : RefFixed(mag < 10.0 ? "%+.1f%%" : "%+.0f%%", pc);
    }
    case DisplayKind::Amount: {
      const double a = static_cast<double>(v) * 100.0;
      return RefFixed(a < 10.0 ? "%.1f" : "%.0f", a);
    }
    case DisplayKind::Decibels:
      return (v > -0.05f && v < 0.05f) ? "0.0 dB" : RefFixed("%+.1f dB", v);
    case DisplayKind::Semitones:
      return (v > -0.005f && v < 0.005f) ? "0.00 st" : RefFixed("%+.2f st", v);
    case DisplayKind::Cents: return RefFixed("%.1f ct", v);
    case DisplayKind::FilterMorph: {
      static const char* kNames[] = {"LP", "BP", "HP", "Notch"};
      const auto   seg = static_cast<int>(v);
      const double pc  = (static_cast<double>(v) - seg) * 100.0;
      if (seg >= 3 || pc < 0.5) return kNames[seg < 3 ? seg : 3];
      if (pc >= 99.5) return kNames[seg + 1];
      return std::string(kNames[seg]) + ">" + kNames[seg + 1] + " " + RefFixed("%.0f%%", pc);
    }
    case DisplayKind::OffOn: return v >= 0.5f ? "On" : "Off";
    case DisplayKind::LiveMark: return v >= 0.5f ? "Mark" : "Live";
    case DisplayKind::Count: return std::to_string(std::lround(v));  // half away from zero
    case DisplayKind::MsOrOff: return v == 0.0f ? std::string("Off") : RefMs(v);
    case DisplayKind::Signed: {
      const double pc  = static_cast<double>(v) * 100.0;
      const double mag = std::fabs(pc);
      return mag < 0.05 ? "0%" : RefFixed(mag < 10.0 ? "%+.1f%%" : "%+.0f%%", pc);
    }
    case DisplayKind::ReverbMode: {
      static const char* kModes[] = {"Bright room", "Dark medium", "Large hall", "Ambient"};
      const long n = std::lround(v);
      return kModes[n < 0 ? 0 : (n > 3 ? 3 : n)];
    }
    case DisplayKind::Division: {
      static const char* kNames[] = {"Off", "1/32", "1/16T", "1/16", "1/8T", "1/16D",
                                     "1/8", "1/4T", "1/8D",  "1/4",  "1/2T", "1/4D",
                                     "1/2", "1/1T", "1/2D",  "1/1",  "2/1"};
      const long n = std::lround(v);
      return kNames[n <= 0 ? 0 : (n > 16 ? 16 : n)];
    }
    case DisplayKind::SubdivPosition: {
      static const char* kPositions[] = {"\xC3\x97" "1/4", "\xC3\x97" "1/2", "TAP",
                                         "\xC3\x97" "2",   "\xC3\x97" "4",   "\xC3\x97" "8"};
      const long n = std::lround(v);
      return kPositions[n <= 0 ? 0 : (n > 5 ? 5 : n)];
    }
    case DisplayKind::TimeMode: {
      static const char* kModes[] = {"Free", "Subdiv", "Tempo"};
      const long n = std::lround(v);
      return kModes[n <= 0 ? 0 : (n > 2 ? 2 : n)];
    }
    case DisplayKind::TempoRecall: return v >= 0.5f ? "Preset" : "Keep";
  }
  return "";
}

}  // namespace

TEST_CASE("display text matches a correctly rounding printf, ties included") {
  std::vector<float> values;
  for (int k = 0; k <= 4096; ++k) values.push_back(static_cast<float>(k) / 4096.0f);
  for (int k = -2000; k <= 2000; ++k) values.push_back(static_cast<float>(k) * 0.125f);  // ties
  for (int k = 0; k <= 20000; k += 7) values.push_back(static_cast<float>(k) + 0.5f);
  for (const float v : {0.005f, 0.015f, 0.125f, 0.375f, 9.995f, 99.95f, 999.5f, 1250.0f,
                        9999.5f, -0.04f, -0.0f, 0.04999f, -23.95f, 7.02f, 0.4f, 2.5f}) {
    values.push_back(v);
  }
  size_t compared = 0, differ = 0;
  for (const ParamDescriptor& d : kParamTable) {
    for (const float raw : values) {
      for (const float v : {raw, d.min + raw * (d.max - d.min), PlainFromNormalized(d.id, raw)}) {
        char buf[32];
        FormatPlain(d.id, v, buf, sizeof buf);
        const std::string want = RefFormat(d.id, v);
        ++compared;
        if (want != buf) {
          ++differ;
          if (differ <= 10) UNSCOPED_INFO((d.name != nullptr ? d.name : "(retired)") << " " << v << ": '" << buf << "' vs printf '" << want << "'");
        }
      }
    }
  }
  CHECK(compared > 900000u);
  CHECK(differ == 0u);
}

// The taper functions are entry points (companion §4.6): a host thread in FTZ|DAZ and
// round-toward-zero must get the same plain bits, the same normalised view and the same
// text as a clean one.
TEST_CASE("taper and display functions own the FP environment") {
  std::vector<float> in;
  for (int k = 0; k <= 4096; ++k) in.push_back(static_cast<float>(k) / 4096.0f);
  for (const uint32_t u : {0x00000001u, 0x007FFFFFu, 0x00800000u, 0x3F7FFFFFu, 0x7FC00000u}) {
    in.push_back(FromBits(u));
  }
  for (const ParamDescriptor& d : kParamTable) {
    INFO((d.name != nullptr ? d.name : "(retired)"));
    std::vector<float> plainClean(in.size()), plainHostile(in.size());
    std::vector<float> normClean(in.size()), normHostile(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
      plainClean[i] = PlainFromNormalized(d.id, in[i]);
      normClean[i]  = NormalizedFromPlain(d.id, plainClean[i]);
      {
        const testing::HostileFpScope scope;
        plainHostile[i] = PlainFromNormalized(d.id, in[i]);
      }
      {
        const testing::HostileFpScope scope;
        normHostile[i] = NormalizedFromPlain(d.id, plainClean[i]);
      }
      char clean[32], hostile[32];
      FormatPlain(d.id, plainClean[i], clean, sizeof clean);
      {
        const testing::HostileFpScope scope;
        FormatPlain(d.id, plainClean[i], hostile, sizeof hostile);
      }
      REQUIRE(std::string(clean) == hostile);
    }
    REQUIRE(std::memcmp(plainClean.data(), plainHostile.data(), in.size() * sizeof(float)) == 0);
    REQUIRE(std::memcmp(normClean.data(), normHostile.data(), in.size() * sizeof(float)) == 0);
  }
}
