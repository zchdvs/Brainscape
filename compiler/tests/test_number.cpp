// The exact binary32 number code (docs/design/mode-compiler.md §6.4, §6.5): the JSON reader's
// grammar and rounding, the canonical writer and its two exceptions, and the lenient typed-text
// reader (§10.4's plugin forms). The hashed sets are bsc_number_check's.
#include <cstdlib>
#include <cstring>
#include <string>

#include "Number.h"
#include "catch.hpp"

namespace {

uint32_t Bits(float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  return u;
}

struct Read {
  bsc::NumberStatus status;
  uint32_t          bits;
};

Read Json(const std::string& s) {
  float y = 0.f;
  const auto st = bsc::ParseJsonNumber(s.data(), s.size(), &y);
  return {st, st == bsc::NumberStatus::Ok ? Bits(y) : 0u};
}

Read Typed(const std::string& s) {
  float y = 0.f;
  const auto st = bsc::ParseTypedNumber(s.data(), s.size(), &y);
  return {st, st == bsc::NumberStatus::Ok ? Bits(y) : 0u};
}

std::string Write(uint32_t bits) {
  char         buf[bsc::kMaxNumberText];
  const size_t n = bsc::WriteNumberBits(bits, buf);
  return std::string(buf, n);
}

}  // namespace

TEST_CASE("JSON numbers read correctly rounded, and the canonical text", "[number]") {
  struct Case {
    const char* text;
    uint32_t    bits;
    const char* canonical;
  };
  // From the design probe's edge set (record §2.3), on every toolchain.
  const Case cases[] = {
      {"0", 0x00000000u, "0"},
      {"-0", 0x80000000u, "-0"},
      {"0.0", 0x00000000u, "0"},
      {"-0.0e5", 0x80000000u, "-0"},
      {"1", 0x3F800000u, "1"},
      {"0.55", 0x3F0CCCCDu, "0.55"},
      {"375", 0x43BB8000u, "375"},
      {"9500", 0x46147000u, "9500"},
      {"20000", 0x469C4000u, "20000"},
      {"1e-46", 0x00000000u, "0"},
      {"7e-46", 0x00000000u, "0"},
      {"7.1e-46", 0x00000001u, "1e-45"},
      {"1.401298464324817e-45", 0x00000001u, "1e-45"},
      {"1.1754942e-38", 0x007FFFFFu, "1.1754942e-38"},
      {"1.17549435e-38", 0x00800000u, "1.1754944e-38"},
      {"3.4028235e38", 0x7F7FFFFFu, "3.4028235e+38"},
      {"3.40282356e38", 0x7F7FFFFFu, "3.4028235e+38"},
      {"340282356779733661637539395458142568447", 0x7F7FFFFFu, "3.4028235e+38"},
      {"0e999999999", 0x00000000u, "0"},
      {"1e-999999999", 0x00000000u, "0"},
      {"1.5e+3", 0x44BB8000u, "1500"},
      {"7.038531e-26", 0x15AE43FDu, "7.0385307e-26"},
      {"7.0385307e-26", 0x15AE43FDu, "7.0385307e-26"},
      {"-7.038531e-26", 0x95AE43FDu, "-7.0385307e-26"},
      {"7.0385313e-26", 0x15AE43FEu, "7.0385313e-26"},
      {"123456789012345678901234567890", 0x6FC77488u, "1.2345679e+29"},
      {"0.30000001192092896", 0x3E99999Au, "0.3"},
      {"1e21", 0x6258D727u, "1e+21"},
      {"1e-7", 0x33D6BF95u, "1e-7"},
      {"123e-2", 0x3F9D70A4u, "1.23"},
      {"405", 0x43CA8000u, "405"},
      {"0.45", 0x3EE66666u, "0.45"},
      {"0.12", 0x3DF5C28Fu, "0.12"},
      {"0.000001", 0x358637BDu, "0.000001"},
      {"1E-7", 0x33D6BF95u, "1e-7"},
      {"100000000000000000000", 0x60AD78ECu, "100000000000000000000"},
  };
  for (const Case& c : cases) {
    INFO(c.text);
    const Read r = Json(c.text);
    REQUIRE(r.status == bsc::NumberStatus::Ok);
    REQUIRE(r.bits == c.bits);
    REQUIRE(Write(r.bits) == c.canonical);
    REQUIRE(Json(Write(r.bits)).bits == c.bits);
  }
}

TEST_CASE("JSON numbers: the grammar is RFC 8259's and overflow is an error", "[number]") {
  for (const char* bad : {"", "-", "+1", "01", "00", "1.", ".5", "1e", "1e+", "1.e5", "0x10",
                          "Infinity", "NaN", "--1", "1.2.3", " 1", "1 ", "1e5.0", "e5"}) {
    INFO(bad);
    REQUIRE(Json(bad).status == bsc::NumberStatus::Syntax);
  }
  for (const char* big : {"340282356779733661637539395458142568448", "1e39", "-3.5e38",
                          "3.4028236e38", "1e999999999"}) {
    INFO(big);
    REQUIRE(Json(big).status == bsc::NumberStatus::Range);
  }
  // A prefix read for a tokenizer: the longest number, then whatever follows.
  float  y    = 0.f;
  size_t used = 0;
  REQUIRE(bsc::ParseJsonNumber("12.5e1,", 7, &y, &used) == bsc::NumberStatus::Ok);
  REQUIRE(used == 6u);
  REQUIRE(Bits(y) == Bits(125.0f));
  REQUIRE(bsc::ParseJsonNumber("0123", 4, &y, &used) == bsc::NumberStatus::Ok);
  REQUIRE(used == 1u);  // the tokenizer then refuses the stray digit
  REQUIRE(Bits(y) == 0u);
}

TEST_CASE("JSON numbers: long and halfway inputs round correctly", "[number]") {
  // 1 + 2^-24 is the midpoint of 1 and its successor: ties to even (1); a digit past 120 of
  // them above the midpoint rounds up.
  const std::string mid = "1.000000059604644775390625";
  REQUIRE(Json(mid).bits == 0x3F800000u);
  REQUIRE(Json(mid + std::string(200, '0') + "1").bits == 0x3F800001u);
  REQUIRE(Json("1.0000000596046447753906249" + std::string(200, '9')).bits == 0x3F800000u);
  // 1 + 3 * 2^-24: the midpoint of two odd neighbours rounds to the even one above.
  REQUIRE(Json("1.000000178813934326171875").bits == 0x3F800002u);
  // Many leading zeros, then the smallest subnormal.
  REQUIRE(Json("0." + std::string(44, '0') + "1401298464324817").bits == 0x00000001u);
  // Exponents that saturate but cancel.
  REQUIRE(Json("1" + std::string(300, '0') + "e-300").bits == 0x3F800000u);
}

TEST_CASE("The writer: shortest, closest, even on a tie, laid out as ECMAScript does",
          "[number]") {
  REQUIRE(Write(Bits(0.1f)) == "0.1");
  REQUIRE(Write(Bits(1e-6f)) == "0.000001");
  REQUIRE(Write(Bits(1.5e-7f)) == "1.5e-7");
  REQUIRE(Write(Bits(123456.0f)) == "123456");
  REQUIRE(Write(Bits(1.0e20f)) == "100000000000000000000");
  REQUIRE(Write(Bits(1.0e22f)) == "1e+22");
  REQUIRE(Write(Bits(-2.5f)) == "-2.5");
  REQUIRE(Write(Bits(16777216.0f)) == "16777216");
  REQUIRE(Write(Bits(16777218.0f)) == "16777218");
  REQUIRE(Write(0x7F7FFFFFu) == "3.4028235e+38");
  REQUIRE(Write(0x00000001u) == "1e-45");
  REQUIRE(Write(0x80000001u) == "-1e-45");
  REQUIRE(Write(0x7F800000u).empty());  // no text for infinity or NaN
  REQUIRE(Write(0x7FC00000u).empty());
  // The exceptions: the pure shortest text is seven digits; through binary64 it would read as
  // the neighbour, so eight are written.
  const bsc::ShortestDigits g = bsc::Shortest(0x15AE43FDu);
  REQUIRE(std::string(g.digits, static_cast<size_t>(g.count)) == "7038531");
  char         pure[bsc::kMaxNumberText];
  const size_t n = bsc::LayoutNumber(false, g, pure);
  REQUIRE(std::string(pure, n) == "7.038531e-26");
  REQUIRE(Bits(static_cast<float>(std::strtod("7.038531e-26", nullptr))) == 0x15AE43FEu);
  REQUIRE(Bits(static_cast<float>(std::strtod("7.0385307e-26", nullptr))) == 0x15AE43FDu);
  REQUIRE(Bits(static_cast<float>(std::strtod("-7.0385307e-26", nullptr))) == 0x95AE43FDu);
  REQUIRE(Write(0x15AE43FDu).size() <= bsc::kMaxNumberText);
  REQUIRE(Write(0x95AE43FDu) == "-7.0385307e-26");
}

TEST_CASE("Typed text: the plugin's looser forms, rounded the same way", "[number]") {
  struct Case {
    const char* text;
    float       value;
  };
  const Case cases[] = {{".5", 0.5f},  {"5.", 5.0f},    {"05", 5.0f},       {"+25", 25.0f},
                        {"-.5", -0.5f}, {"5.e3", 5000.0f}, {".5e1", 5.0f},  {"0.55", 0.55f},
                        {"007.250", 7.25f}, {"1E2", 100.0f}, {"+0", 0.0f},  {"-0.", -0.0f}};
  for (const Case& c : cases) {
    INFO(c.text);
    const Read r = Typed(c.text);
    REQUIRE(r.status == bsc::NumberStatus::Ok);
    REQUIRE(r.bits == Bits(c.value));
  }
  for (const char* bad : {"", ".", "+", "-", "e5", "1e", "1e+", "--1", "1.2.3", " 1", "1 ",
                          "5 ms", "1,5", "+.e1", "0x1"}) {
    INFO(bad);
    REQUIRE(Typed(bad).status == bsc::NumberStatus::Syntax);
  }
  REQUIRE(Typed("1e39").status == bsc::NumberStatus::Range);
  // The same digits read the same bits as JSON does.
  for (const char* both : {"0.55", "375", "7.038531e-26", "1e-46", "3.40282356e38", "123e-2"}) {
    INFO(both);
    REQUIRE(Typed(both).bits == Json(both).bits);
  }
}

TEST_CASE("Every float of a coarse stride round-trips through the canonical text", "[number]") {
  uint64_t checked = 0;
  for (uint64_t u = 0; u < 0x100000000ull; u += 65537) {
    const auto bits = static_cast<uint32_t>(u);
    if ((bits & 0x7F800000u) == 0x7F800000u) continue;
    const std::string text = Write(bits);
    REQUIRE(Json(text).bits == bits);
    ++checked;
  }
  REQUIRE(checked > 60000u);
}
