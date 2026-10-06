// For each one-pole/smoother coefficient the engine derives at 48 kHz, enumerate the float
// states v whose decay product c*v (exact) lands in the FTZ disagreement band
// [FLT_MIN - 2^-151, FLT_MIN): there, x86 FTZ returns FLT_MIN and Arm FZ returns 0.
// A ramp/decay to 0 that passes through such a v snaps/stalls one sample apart on the two ISAs.
// Then simulate decays from a fine grid of start values and count trajectories that hit one.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

static uint32_t Bits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
static float FromBits(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }

int main() {
  const double sr = 48000.0;
  struct C { const char* name; float c; } cs[] = {
      {"Smoother SetTau(10ms)", -static_cast<float>(std::expm1(-1.0 / (10.0f * 0.001 * sr)))},
      {"Smoother SetTau(100ms) norm_", -static_cast<float>(std::expm1(-1.0 / (100.0f * 0.001 * sr)))},
      {"LpCoef(8)   tamer dc", -static_cast<float>(std::expm1(-6.283185307179586 * 8.0 / sr))},
      {"LpCoef(20)  pd dc", -static_cast<float>(std::expm1(-6.283185307179586 * 20.0 / sr))},
      {"LpCoef(100) tamer hp", -static_cast<float>(std::expm1(-6.283185307179586 * 100.0 / sr))},
      {"LpCoef(5500) rv damp", -static_cast<float>(std::expm1(-6.283185307179586 * 5500.0 / sr))},
      {"LpCoef(6000) pd lp", -static_cast<float>(std::expm1(-6.283185307179586 * 6000.0 / sr))},
      {"LpCoef(8000) tamer lp fb=0", -static_cast<float>(std::expm1(-6.283185307179586 * 8000.0 / sr))},
      {"LpCoef(11000) rv bw", -static_cast<float>(std::expm1(-6.283185307179586 * 11000.0 / sr))},
  };
  const double fmin = std::numeric_limits<float>::min();
  const double bandLo = fmin - std::ldexp(1.0, -151);
  for (const auto& e : cs) {
    // floats v with c*v in [bandLo, fmin): v in [bandLo/c, fmin/c)
    const float vhi = static_cast<float>(fmin / e.c);
    int hits = 0;
    float hitV = 0.f;
    for (int d = -64; d <= 64; ++d) {
      const float v = FromBits(Bits(vhi) + d);
      const double p = static_cast<double>(e.c) * static_cast<double>(v);  // exact
      if (p >= bandLo && p < fmin) { ++hits; hitV = v; }
    }
    // Trajectory count: decays v <- v + c*(0-v) from start values k/2^16 (IEEE products are
    // identical to both FTZ models until the band; check whether the trajectory hits hitV).
    int trajHits = 0;
    if (hits) {
      for (uint32_t k = 1; k <= 65536; ++k) {
        float v = static_cast<float>(k) / 65536.0f;
        for (int n = 0; n < 400000; ++n) {
          const double p = static_cast<double>(e.c) * static_cast<double>(v);
          if (p < fmin) { if (p >= bandLo) ++trajHits; break; }
          v = v + static_cast<float>(e.c * -v);  // normal-range: identical on all models
        }
      }
    }
    std::printf("%-28s c=%.9g (0x%08X): band states=%d%s%s", e.name, (double)e.c, Bits(e.c), hits,
                hits ? " v=" : "", hits ? "" : "\n");
    if (hits) std::printf("0x%08X; start values k/65536 whose ramp-to-0 hits it: %d of 65536\n", Bits(hitV), trajHits);
  }
  return 0;
}
