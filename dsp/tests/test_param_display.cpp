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
    INFO(d.name);
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
    INFO(d.name);
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
  CHECK(Bits(Canonicalize(ParamId::PitchSt, -tiny)) == 0u);
  CHECK(Canonicalize(ParamId::DelayMs, tiny) == 1.0f);  // +0 then clamped to min
  // Authored values pass bit for bit: no grid (companion §5.5).
  CHECK(Bits(Canonicalize(ParamId::PitchSt, 7.02f)) == Bits(7.02f));
  CHECK(Bits(Canonicalize(ParamId::FilterMorph, 0.4f)) == Bits(0.4f));
  CHECK(PlainFromNormalized(ParamId::Mix, nan) == 0.0f);
  CHECK(PlainFromNormalized(ParamId::Mix, inf) == 1.0f);
  CHECK(PlainFromNormalized(ParamId::Mix, -1.0f) == 0.0f);
}

TEST_CASE("discrete parameters snap to their positions") {
  CHECK(PlainFromNormalized(ParamId::OnsetTrigger, 0.49f) == 0.0f);
  CHECK(PlainFromNormalized(ParamId::OnsetTrigger, 0.5f) == 1.0f);
  CHECK(PlainFromNormalized(ParamId::PositionSource, 1.0f) == 1.0f);
  CHECK(NormalizedFromPlain(ParamId::PositionSource, 0.7f) == 1.0f);
  CHECK(NormalizedFromPlain(ParamId::PositionSource, 0.2f) == 0.0f);
}

TEST_CASE("display text carries units and the named endpoints") {
  CHECK(Format(ParamId::DelayMs, 250.0f) == "250 ms");
  CHECK(Format(ParamId::DelayMs, 1250.0f) == "1.25 s");
  CHECK(Format(ParamId::Mix, 0.5f) == "50%");
  CHECK(Format(ParamId::OutTrimDb, -3.0f) == "-3.0 dB");
  CHECK(Format(ParamId::PitchSt, 7.0f) == "+7.00 st");
  CHECK(Format(ParamId::PitchSt, 0.0f) == "0.00 st");
  CHECK(Format(ParamId::FilterCutoffHz, 20000.0f) == "Off");
  CHECK(Format(ParamId::FilterCutoffHz, 2500.0f) == "2.50 kHz");
  CHECK(Format(ParamId::FilterMorph, 1.0f) == "BP");
  CHECK(Format(ParamId::FilterMorph, 0.4f) == "LP>BP 40%");
  CHECK(Format(ParamId::FilterMorph, 3.0f) == "Notch");
  CHECK(Format(ParamId::OnsetTrigger, 1.0f) == "On");
  CHECK(Format(ParamId::PositionSource, 0.0f) == "Live");
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
    case DisplayKind::FilterCutoff: return v >= d.max - 0.5f ? "Off" : RefHz(v);
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
          if (differ <= 10) UNSCOPED_INFO(d.name << " " << v << ": '" << buf << "' vs printf '" << want << "'");
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
    INFO(d.name);
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
