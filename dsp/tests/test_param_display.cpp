#include <cmath>
#include <cstring>
#include <limits>
#include <string>

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
    CHECK(Bits(PlainFromNormalized(d.id, 0.0f)) == Bits(CanonicalizePlain(d.id, d.min)));
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
  CHECK(CanonicalizePlain(ParamId::DelayMs, nan) == 1.0f);
  CHECK(CanonicalizePlain(ParamId::DelayMs, inf) == 1.0f);
  CHECK(CanonicalizePlain(ParamId::DelayMs, -inf) == 1.0f);
  CHECK(CanonicalizePlain(ParamId::DelayMs, 9000.0f) == 5000.0f);
  CHECK(Bits(CanonicalizePlain(ParamId::Mix, -0.0f)) == 0u);
  CHECK(Bits(CanonicalizePlain(ParamId::Mix, tiny)) == 0u);
  CHECK(Bits(CanonicalizePlain(ParamId::PitchSt, -tiny)) == 0u);
  CHECK(CanonicalizePlain(ParamId::DelayMs, tiny) == 1.0f);  // +0 then clamped to min
  // Authored values pass bit for bit: no grid (companion §5.5).
  CHECK(Bits(CanonicalizePlain(ParamId::PitchSt, 7.02f)) == Bits(7.02f));
  CHECK(Bits(CanonicalizePlain(ParamId::FilterMorph, 0.4f)) == Bits(0.4f));
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
