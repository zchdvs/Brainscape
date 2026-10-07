// Revision probe: the design's EvalMacro (curve-1 fast path) on the Engram example.
#include "detail/DetMath.h"
#include "detail/FpEnvGuard.h"
#include <cstdio>
#include <cstring>
using namespace brainscape;
static float Ev(float m, float lo, float hi, float curve) {
  float u = m <= 0.f ? 0.f : (m >= 1.f ? 1.f : m);
  float c = (u == 0.f) ? 0.f : (u == 1.f) ? 1.f : (curve == 1.f ? u : detmath::PowF(u, curve));
  if (c == 0.f) return lo; if (c == 1.f) return hi;
  double span = (double)hi - (double)lo, step = span * (double)c;
  return (float)((double)lo + step);
}
static void P(const char* what, float v) { unsigned b; std::memcpy(&b, &v, 4); std::printf("%-40s %.9g  0x%08X\n", what, v, b); }
int main() {
  const detail::FpEnvGuard g;
  P("PowF(0.5,2)", detmath::PowF(0.5f, 2.f));
  P("PowF(0.5,1)", detmath::PowF(0.5f, 1.f));
  P("time [40,1500]^2 @0.5", Ev(0.5f, 40.f, 1500.f, 2.f));
  P("time [40,1500]^2 @0.479", Ev(0.479f, 40.f, 1500.f, 2.f));
  P("repeats [0,0.9]^1 @0.5", Ev(0.5f, 0.f, 0.9f, 1.f));
  P("0.45f", 0.45f);
  P("space reverb [0,0.5]^1 @0.24", Ev(0.24f, 0.f, 0.5f, 1.f));
  P("0.12f", 0.12f);
  P("repeats [0,0.92]^1.3 @0.577", Ev(0.577f, 0.f, 0.92f, 1.3f));
  return 0;
}
