#include <cstdio>
#include <cmath>
#include <cstring>
#include <xmmintrin.h>
static unsigned Bits(float f){unsigned u; std::memcpy(&u,&f,4); return u;}
// A plausible compare-based canonicalizer (subnormal -> +0, -0 -> +0), as one might write it.
static float CanonCompare(float v) {
  if (v == 0.0f) return 0.0f;                         // folds -0 (and, under DAZ, subnormals)
  if (std::fpclassify(v) == FP_SUBNORMAL) return 0.0f;
  return v;
}
static float CanonCompare2(float v) {                 // signbit-guarded -0 rule
  if (std::signbit(v) && v == 0.0f) return 0.0f;
  if (std::fabs(v) < 1.17549435e-38f && v != 0.0f) return 0.0f;
  return v;
}
int main() {
  volatile float sub = 1e-40f, z = 0.0f;
  const unsigned saved = _mm_getcsr();
  for (int mode = 0; mode < 2; ++mode) {
    _mm_setcsr(mode ? (saved | 0x8040u) : (saved & ~0x8040u));
    const float s = sub;
    const bool eq = (s == z);
    const float c1 = CanonCompare(s), c2 = CanonCompare2(s);
    const int cls = std::fpclassify(s);
    _mm_setcsr(saved);
    std::printf("%s: (1e-40f == 0.0f) -> %d ; fpclassify=%s ; CanonCompare -> 0x%08X ; CanonCompare2 -> 0x%08X\n",
                mode ? "FTZ|DAZ" : "IEEE   ", eq ? 1 : 0,
                cls == FP_SUBNORMAL ? "SUBNORMAL" : (cls == FP_ZERO ? "ZERO" : "other"), Bits(c1), Bits(c2));
  }
  return 0;
}
