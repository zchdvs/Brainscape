// review-numerics probe: DetMath (prototype dsp_det/include/brainscape/DetMath.h)
// outside its documented domain. The profile (§5.11) routes macro fan-out and
// expression curves through PowF, whose base x is a normalized position in [0, 1].
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "brainscape/DetMath.h"

using namespace brainscape::detmath;

static uint32_t Bits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }

int main() {
  const float ys[] = {0.25f, 0.5f, 0.999f, 1.0f, 1.0005f, 1.001f, 1.5f, 2.0f, 3.0f, 4.0f};
  std::printf("PowF(0, y):\n");
  for (float y : ys) {
    const float r = PowF(0.0f, y);
    std::printf("  y=%-8g -> %-14g bits 0x%08X\n", y, r, Bits(r));
  }
  std::printf("PowF(x, 2) for tiny x:\n");
  const float xs[] = {1e-40f, 1.17549435e-38f, 1e-30f, 1e-20f, 5.42e-20f};
  for (float x : xs) {
    const float r = PowF(x, 2.0f);
    std::printf("  x=%-14g -> %-14g bits 0x%08X\n", x, r, Bits(r));
  }
  std::printf("LogD(0) = %.17g, LogD(-1) = %.17g\n", LogD(0.0), LogD(-1.0));
  std::printf("Exp2D(-1100) = %.17g  Exp2D(-1030) = %.17g  Exp2D(1030) = %.17g\n", Exp2D(-1100.0),
              Exp2D(-1030.0), Exp2D(1030.0));
  return 0;
}
