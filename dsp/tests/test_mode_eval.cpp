// The macro evaluator and the pitch guard (docs/design/mode-compiler.md §3.3, §2.7 L2): the
// guarded floating-point functions the compiler's lint and derive passes call, and whose bodies
// the engine's MacroMove and Expression events run (sound revision 2); with EvalExpression.
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "FpEnvTestUtil.h"
#include "brainscape/Mode.h"
#include "brainscape/ModeEval.h"
#include "brainscape/Params.h"
#include "brainscape/Preset.h"
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

// Determinism profile §4.1: each exported helper owns the control word, so a hostile caller
// (FTZ|DAZ or FZ|DN with round-toward-zero) gets the clean bits and its own word back; on x64 a
// caller with every exception unmasked also gets them, so FP work outside a guard would trap.
TEST_CASE("EvalMacro, EvalExpression and NearGuardMs ignore the caller's FP environment",
          "[modeeval]") {
  const auto m = OneTarget(ParamId::SprayMs, 0.0f, 2000.0f, 3.0f);
  // The pedal on a leaf at curve 2 and, reversed, on the default Filter macro (curve 4).
  const auto   mode = std::make_unique<ModeBlob>();
  ControlState ctrl;
  ctrl.exprCount      = 2;
  ctrl.expressions[0] = ExpressionAssignment{static_cast<uint32_t>(ParamId::DelayMs), 20.0f, 2000.0f, 2.0f};
  ctrl.expressions[1] = ExpressionAssignment{static_cast<uint32_t>(ParamId::MacroFilter), 1.0f, 0.0f, 1.0f};
  detail::FpWord words[] = {testing::kHostileFpWord,
#if defined(BRAINSCAPE_FPENV_X64)
                            testing::kTrapAllFpWord,
#endif
  };
  for (const detail::FpWord word : words) {
    INFO("caller's word " << word);
    size_t wordsLost = 0;  // calls after which the caller's word was not handed back
    for (uint32_t code = 0; code <= 4095u; code += 7u) {
      const float position = static_cast<float>(code) / 4095.0f;
      const float clean    = Eval1(*m, position);
      PresetLeaf  cleanExpr[2];
      REQUIRE(EvalExpression(*mode, ctrl, position, cleanExpr, 2) == 2u);
      PresetLeaf out[1], expr[2];
      size_t     n = 0, nx = 0;
      {
        const testing::HostileFpScope hostile(word);
        n = EvalMacro(*m, ParamId::MacroActivity, position, out, 1);
        wordsLost += detail::ReadFpControl() != word ? 1u : 0u;
        nx = EvalExpression(*mode, ctrl, position, expr, 2);
        wordsLost += detail::ReadFpControl() != word ? 1u : 0u;
      }
      REQUIRE(n == 1u);
      REQUIRE(Bits(out[0].value) == Bits(clean));
      REQUIRE(nx == 2u);
      for (size_t i = 0; i < nx; ++i) {
        REQUIRE(expr[i].id == cleanExpr[i].id);
        REQUIRE(Bits(expr[i].value) == Bits(cleanExpr[i].value));
      }
    }
    const float guard        = NearGuardMs(123.0f, 7.0f, 0.5f, 33.0f);
    float       hostileGuard = 0.0f;
    {
      const testing::HostileFpScope hostile(word);
      hostileGuard = NearGuardMs(123.0f, 7.0f, 0.5f, 33.0f);
      wordsLost += detail::ReadFpControl() != word ? 1u : 0u;
    }
    REQUIRE(Bits(guard) == Bits(hostileGuard));
    REQUIRE(wordsLost == 0u);
  }
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

TEST_CASE("EvalExpression: CTRL's assignments in order, a leaf or a macro's targets", "[modeeval]") {
  const auto   mode = std::make_unique<ModeBlob>();  // the default macros (§3.2)
  ControlState ctrl;                                 // six positions, no assignments
  PresetLeaf   out[kMaxExpressions * kMaxMacroTargets];
  // Without assignments, or without CTRL, the pedal does nothing (§3.4).
  REQUIRE(EvalExpression(*mode, ctrl, 0.5f, out, 32) == 0u);
  ctrl.exprCount      = 3;
  ctrl.expressions[0] = ExpressionAssignment{static_cast<uint32_t>(ParamId::ReverbMix), 0.0f, 0.8f, 1.0f};
  ctrl.expressions[1] = ExpressionAssignment{static_cast<uint32_t>(ParamId::MacroFilter), 1.0f, 0.0f, 1.0f};
  ctrl.expressions[2] = ExpressionAssignment{static_cast<uint32_t>(ParamId::DelayMs), 20.0f, 2000.0f, 2.0f};
  REQUIRE(EvalExpression(*mode, ctrl, 0.0f, out, 32) == 3u);
  REQUIRE(out[0].id == static_cast<uint32_t>(ParamId::ReverbMix));
  REQUIRE(Bits(out[0].value) == Bits(0.0f));
  // The filter macro at position 1 (the assignment reversed): its target, the cutoff, at max.
  REQUIRE(out[1].id == static_cast<uint32_t>(ParamId::FilterCutoffHz));
  REQUIRE(Bits(out[1].value) == Bits(20000.0f));
  REQUIRE(out[2].id == static_cast<uint32_t>(ParamId::DelayMs));
  REQUIRE(Bits(out[2].value) == Bits(20.0f));
  REQUIRE(EvalExpression(*mode, ctrl, 1.0f, out, 32) == 3u);
  REQUIRE(Bits(out[0].value) == Bits(0.8f));
  REQUIRE(Bits(out[1].value) == Bits(40.0f));  // the filter macro at 0: the kill
  REQUIRE(Bits(out[2].value) == Bits(2000.0f));
  // At 0.5 each maps as a macro target with in_range [0, 1] would: EvalMacro's numbers.
  REQUIRE(EvalExpression(*mode, ctrl, 0.5f, out, 32) == 3u);
  REQUIRE(Bits(out[2].value) == Bits(515.0f));  // 20 + 1980 * 0.25
  PresetLeaf viaMacro[kMaxMacroTargets];
  REQUIRE(EvalMacro(*mode, ParamId::MacroFilter, 0.5f, viaMacro, kMaxMacroTargets) == 1u);
  REQUIRE(Bits(out[1].value) == Bits(viaMacro[0].value));
  // The position is canonicalized as perf.expression's value; the cap holds.
  REQUIRE(EvalExpression(*mode, ctrl, FromBits(0x7FC00000u), out, 32) == 3u);  // NaN: 0
  REQUIRE(Bits(out[0].value) == Bits(0.0f));
  REQUIRE(EvalExpression(*mode, ctrl, 2.0f, out, 32) == 3u);
  REQUIRE(Bits(out[0].value) == Bits(0.8f));
  REQUIRE(EvalExpression(*mode, ctrl, 0.5f, out, 2) == 2u);
  REQUIRE(EvalExpression(*mode, ctrl, 0.5f, nullptr, 32) == 0u);
  // A macro the mode leaves undefined moves nothing.
  ctrl.expressions[1].target = static_cast<uint32_t>(ParamId::MacroAux1);
  REQUIRE(EvalExpression(*mode, ctrl, 0.5f, out, 32) == 2u);
  ctrl.present = 0;
  REQUIRE(EvalExpression(*mode, ctrl, 0.5f, out, 32) == 0u);
}

// ── UsesTempo (docs/design/clock.md §6.6, D21) ───────────────────────────────────────────────

namespace {

void SetSyncLeaf(PresetState* s, float v) {
  s->leaves[0] = PresetLeaf{static_cast<uint32_t>(ParamId::DelaySync), v};
  s->leafCount = 1;
}

}  // namespace

TEST_CASE("UsesTempo: the clock source, a synced base delay, row 63 at any reachable end",
          "[mode-eval][tempo]") {
  auto s = std::make_unique<PresetState>();
  REQUIRE_FALSE(UsesTempo(*s));  // the default mode reads no tempo
  s->mode.schedule.sources = static_cast<uint8_t>(s->mode.schedule.sources | kSourceClock);
  REQUIRE(UsesTempo(*s));
  s->mode.schedule.sources = kDefaultSources;
  s->mode.layers[0].baseSync = 3;  // 1/16
  REQUIRE(UsesTempo(*s));
  s->mode.layers[0].baseSync = 0;
  s->mode.layers[1].baseSync = 9;  // a second layer the mode does not have
  REQUIRE_FALSE(UsesTempo(*s));
  s->mode.schedule.layerCount = 2;
  REQUIRE(UsesTempo(*s));
  s->mode.schedule.layerCount = 1;
  s->mode.layers[1].baseSync  = 0;
  // Row 63's stored value, read as RoundHalfAwayI32 of its canonical value (code 0 below 0.5).
  struct Stored {
    uint32_t bits;
    bool     uses;
  };
  for (const Stored& v : {Stored{Bits(0.0f), false}, Stored{Bits(0.49999997f), false},
                          Stored{Bits(0.5f), true}, Stored{Bits(1.0f), true},
                          Stored{Bits(16.0f), true}, Stored{Bits(40.0f), true},
                          Stored{0x80000000u, false}, Stored{Bits(-3.0f), false},
                          Stored{0x00000001u, false}, Stored{0x7FC00000u, false},
                          Stored{0x7F800000u, false}, Stored{0xFF800000u, false}}) {
    INFO(v.bits);
    SetSyncLeaf(s.get(), FromBits(v.bits));
    REQUIRE(UsesTempo(*s) == v.uses);
  }
  SetSyncLeaf(s.get(), 0.0f);
  // Reached only through a macro: either end of the target's range counts.
  auto withMacro = std::make_unique<PresetState>(*s);
  withMacro->mode = *OneTarget(ParamId::DelaySync, 0.0f, 0.4f, 1.0f);
  REQUIRE_FALSE(UsesTempo(*withMacro));
  withMacro->mode = *OneTarget(ParamId::DelaySync, 0.0f, 4.0f, 1.0f);
  REQUIRE(UsesTempo(*withMacro));
  withMacro->mode = *OneTarget(ParamId::DelaySync, 6.0f, 0.0f, 1.0f);
  REQUIRE(UsesTempo(*withMacro));
  withMacro->mode = *OneTarget(ParamId::DelayTimeMs, 10.0f, 1000.0f, 1.0f);
  REQUIRE_FALSE(UsesTempo(*withMacro));
  // Or through an expression assignment.
  s->control.exprCount      = 1;
  s->control.expressions[0] =
      ExpressionAssignment{static_cast<uint32_t>(ParamId::DelaySync), 0.0f, 0.25f, 1.0f};
  REQUIRE_FALSE(UsesTempo(*s));
  s->control.expressions[0].hi = 12.0f;
  REQUIRE(UsesTempo(*s));
  s->control.exprCount = 0;  // a stale entry past the count is not read
  REQUIRE_FALSE(UsesTempo(*s));
}

TEST_CASE("UsesTempo: no factory package reads tempo (D21)", "[mode-eval][tempo]") {
  const std::string dir = BRAINSCAPE_FACTORY_PACKAGES;
  FILE*             m   = std::fopen((dir + "/MANIFEST").c_str(), "rb");
  REQUIRE(m != nullptr);
  char   line[512];
  size_t packages = 0;
  while (std::fgets(line, sizeof line, m) != nullptr) {
    std::string path(line);
    while (!path.empty() && (path.back() == '\n' || path.back() == '\r')) path.pop_back();
    path = path.substr(path.rfind(' ') + 1);                       // the document's path
    path = dir + "/" + path.substr(0, path.size() - 5) + ".bsp";  // its package
    INFO(path);
    FILE* f = std::fopen(path.c_str(), "rb");
    REQUIRE(f != nullptr);
    std::vector<uint8_t> bytes(kMaxPackageBytes + 1u);
    const size_t         n = std::fread(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    auto state = std::make_unique<PresetState>();
    REQUIRE(DecodePreset(bytes.data(), n, state.get()));
    REQUIRE_FALSE(UsesTempo(*state));
    ++packages;
  }
  std::fclose(m);
  REQUIRE(packages == 18u);
}
