// The macro evaluator and the pitch guard (docs/design/mode-compiler.md §3.3, §2.7 L2): the
// guarded floating-point functions the compiler's lint and derive passes call, and that
// sound revision 2's MacroMove events will.
#include <cstring>
#include <memory>

#include "FpEnvTestUtil.h"
#include "brainscape/Mode.h"
#include "brainscape/ModeEval.h"
#include "brainscape/Params.h"
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

// A mode whose activity macro has the one target given.
std::unique_ptr<ModeBlob> OneTarget(ParamId param, float lo, float hi, float curve,
                                    float inLo = 0.0f, float inHi = 1.0f) {
  auto m                = std::make_unique<ModeBlob>();
  m->macros             = MacroTable{};
  m->macros.macroCount  = 1;
  m->macros.targetCount = 1;
  m->macros.macros[0]   = MacroDef{static_cast<uint32_t>(ParamId::MacroActivity), 0, 1, 0};
  m->macros.targets[0]  = MacroTarget{static_cast<uint32_t>(param), lo, hi, inLo, inHi, curve};
  return m;
}

float Eval1(const ModeBlob& m, float position) {
  PresetLeaf out[kMaxMacroTargets];
  REQUIRE(EvalMacro(m, ParamId::MacroActivity, position, out, kMaxMacroTargets) == 1u);
  return out[0].value;
}

}  // namespace

TEST_CASE("EvalMacro: the design's example lands on its stored leaves bit for bit", "[modeeval]") {
  // §2.1 (record §2.6): Time 0.5 on [40, 1500]^2 is 405 ms exactly, Repeats 0.5 on [0, 0.9] the
  // bits of 0.45, Space 0.24 on [0, 0.5] the bits of 0.12.
  REQUIRE(Bits(Eval1(*OneTarget(ParamId::DelayTimeMs, 40.0f, 1500.0f, 2.0f), 0.5f)) ==
          Bits(405.0f));
  REQUIRE(Bits(Eval1(*OneTarget(ParamId::DelayFb, 0.0f, 0.9f, 1.0f), 0.5f)) == 0x3EE66666u);
  REQUIRE(Bits(Eval1(*OneTarget(ParamId::ReverbMix, 0.0f, 0.5f, 1.0f), 0.24f)) == 0x3DF5C28Fu);
  REQUIRE(Bits(0.45f) == 0x3EE66666u);
  REQUIRE(Bits(0.12f) == 0x3DF5C28Fu);
}

TEST_CASE("EvalMacro: the default macros, endpoints and list order", "[modeeval]") {
  const auto mode = std::make_unique<ModeBlob>();  // the schema-default mode (§3.2)
  PresetLeaf out[kMaxMacroTargets];
  REQUIRE(EvalMacro(*mode, ParamId::MacroActivity, 0.0f, out, kMaxMacroTargets) == 2u);
  REQUIRE(out[0].id == static_cast<uint32_t>(ParamId::Overlap));
  REQUIRE(Bits(out[0].value) == Bits(0.25f));
  REQUIRE(out[1].id == static_cast<uint32_t>(ParamId::SprayMs));
  REQUIRE(Bits(out[1].value) == Bits(0.0f));
  REQUIRE(EvalMacro(*mode, ParamId::MacroActivity, 1.0f, out, kMaxMacroTargets) == 2u);
  REQUIRE(Bits(out[0].value) == Bits(0.85f));
  REQUIRE(Bits(out[1].value) == Bits(200.0f));
  // Time at 0.5 on [20, 2000]^2: 20 + 1980 * 0.25.
  REQUIRE(EvalMacro(*mode, ParamId::MacroTime, 0.5f, out, kMaxMacroTargets) == 1u);
  REQUIRE(out[0].id == static_cast<uint32_t>(ParamId::DelayMs));
  REQUIRE(Bits(out[0].value) == Bits(515.0f));
  // Shape is reversed on sustain: 0.9 at 0, 0.1 at 1.
  REQUIRE(EvalMacro(*mode, ParamId::MacroShape, 0.0f, out, kMaxMacroTargets) == 2u);
  REQUIRE(Bits(out[0].value) == Bits(0.9f));
  REQUIRE(EvalMacro(*mode, ParamId::MacroShape, 1.0f, out, kMaxMacroTargets) == 2u);
  REQUIRE(Bits(out[0].value) == Bits(0.1f));
  // Filter: the cutoff's minimum and maximum, the universal endpoints (§3.1).
  REQUIRE(EvalMacro(*mode, ParamId::MacroFilter, 0.0f, out, kMaxMacroTargets) == 1u);
  REQUIRE(Bits(out[0].value) == Bits(40.0f));
  REQUIRE(EvalMacro(*mode, ParamId::MacroFilter, 1.0f, out, kMaxMacroTargets) == 1u);
  REQUIRE(Bits(out[0].value) == Bits(20000.0f));
  // Undefined macros, other rows and a cap.
  REQUIRE(EvalMacro(*mode, ParamId::MacroAux1, 0.5f, out, kMaxMacroTargets) == 0u);
  REQUIRE(EvalMacro(*mode, ParamId::DelayMs, 0.5f, out, kMaxMacroTargets) == 0u);
  REQUIRE(EvalMacro(*mode, static_cast<ParamId>(999), 0.5f, out, kMaxMacroTargets) == 0u);
  REQUIRE(EvalMacro(*mode, ParamId::MacroSpace, 0.5f, out, 1) == 1u);
  REQUIRE(out[0].id == static_cast<uint32_t>(ParamId::DelayMix));
  REQUIRE(EvalMacro(*mode, ParamId::MacroSpace, 0.5f, nullptr, 8) == 0u);
}

TEST_CASE("EvalMacro: positions are canonicalized; in_range clamps", "[modeeval]") {
  const auto m = OneTarget(ParamId::DelayMs, 100.0f, 900.0f, 1.0f, 0.25f, 0.75f);
  REQUIRE(Eval1(*m, FromBits(0x7FC00000u)) == 100.0f);  // NaN: the minimum, 0
  REQUIRE(Eval1(*m, -1.0f) == 100.0f);
  REQUIRE(Eval1(*m, FromBits(0x80000000u)) == 100.0f);  // -0
  REQUIRE(Eval1(*m, 0.25f) == 100.0f);                  // at in_lo: u = 0
  REQUIRE(Eval1(*m, 0.5f) == 500.0f);
  REQUIRE(Eval1(*m, 0.75f) == 900.0f);  // at in_hi: u = 1
  REQUIRE(Eval1(*m, 2.0f) == 900.0f);
  REQUIRE(Eval1(*m, FromBits(0x7F800000u)) == 100.0f);  // +inf: the minimum, as SetParam
  // Values are canonicalized for the target's row: a range end past the row is clamped.
  const auto wide = OneTarget(ParamId::Mix, 0.0f, 1.0f, 1.0f);
  REQUIRE(Eval1(*wide, 1.0f) == 1.0f);
}

TEST_CASE("EvalMacro: monotonic with exact endpoints over every 12-bit pot code", "[modeeval]") {
  // Record §2.6's measurement, as a test: nine exponents from 1/16 to 16, both directions.
  const float curves[] = {0.0625f, 0.125f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f};
  for (const float curve : curves) {
    for (const bool reversed : {false, true}) {
      const float lo = reversed ? 2000.0f : 20.0f, hi = reversed ? 20.0f : 2000.0f;
      const auto  m        = OneTarget(ParamId::DelayMs, lo, hi, curve);
      float       previous = Eval1(*m, 0.0f);
      REQUIRE(previous == lo);
      uint32_t steps = 0;
      for (uint32_t code = 1; code <= 4095u; ++code) {
        const float v = Eval1(*m, static_cast<float>(code) / 4095.0f);
        if (reversed) {
          REQUIRE(v <= previous);
        } else {
          REQUIRE(v >= previous);
        }
        steps += v != previous ? 1u : 0u;
        previous = v;
      }
      REQUIRE(previous == hi);
      REQUIRE(steps > 100u);
    }
  }
}

TEST_CASE("EvalMacro and NearGuardMs ignore the caller's FP environment", "[modeeval]") {
  const auto m = OneTarget(ParamId::SprayMs, 0.0f, 2000.0f, 3.0f);
  for (uint32_t code = 0; code <= 4095u; code += 7u) {
    const float position = static_cast<float>(code) / 4095.0f;
    const float clean    = Eval1(*m, position);
    PresetLeaf  out[1];
    size_t      n = 0;
    {
      const testing::HostileFpScope hostile;
      n = EvalMacro(*m, ParamId::MacroActivity, position, out, 1);
    }
    REQUIRE(n == 1u);
    REQUIRE(Bits(out[0].value) == Bits(clean));
  }
  const float guard        = NearGuardMs(123.0f, 7.0f, 0.5f, 33.0f);
  float       hostileGuard = 0.0f;
  {
    const testing::HostileFpScope hostile;
    hostileGuard = NearGuardMs(123.0f, 7.0f, 0.5f, 33.0f);
  }
  REQUIRE(Bits(guard) == Bits(hostileGuard));
}

TEST_CASE("NearGuardMs: size * (r - 1) for the highest pitch, 0 at or below unison", "[modeeval]") {
  REQUIRE(NearGuardMs(100.0f, 12.0f, 0.0f, 0.0f) == 100.0f);     // an octave: r = 2
  REQUIRE(NearGuardMs(100.0f, 0.0f, 12.0f, 0.0f) == 100.0f);     // transpose composes
  REQUIRE(NearGuardMs(100.0f, 24.0f, 24.0f, 100.0f) == 300.0f);  // clamped to +24: r = 4
  REQUIRE(NearGuardMs(100.0f, 0.0f, 0.0f, 0.0f) == 0.0f);
  REQUIRE(NearGuardMs(100.0f, -12.0f, 0.0f, 50.0f) == 0.0f);
  const float fifth = NearGuardMs(90.0f, 7.0f, 0.0f, 0.0f);  // 90 * (2^(7/12) - 1) = 44.83
  REQUIRE(fifth > 44.8f);
  REQUIRE(fifth < 44.9f);
  REQUIRE(NearGuardMs(90.0f, 7.0f, 0.0f, 10.0f) > fifth);  // detune reaches higher
}
