// fp-isa lens: which compilers contract a*b+c into FMA by default, and which switches stop it.
// Compile to assembly and count fused instructions per function.
#if defined(_MSC_VER)
#include <math.h>
#define FMAF fmaf
#else
#define FMAF __builtin_fmaf
#endif

#if defined(PRAGMA_STDC)
#pragma STDC FP_CONTRACT OFF
#elif defined(PRAGMA_CLANG)
#pragma clang fp contract(off)
#elif defined(PRAGMA_MSVC)
#pragma fp_contract(off)
#elif defined(PRAGMA_GCC_OPT)
#pragma GCC optimize("fp-contract=off")
#endif

extern "C" {
// One expression: ISO C/C++ permits contraction here (FP_CONTRACT ON semantics).
float k_one_expr(float a, float b, float c) { return a * b + c; }

// Two statements: contraction across statements is NOT allowed by the standard ("fast" only).
float k_two_stmt(float a, float b, float c) {
  const float t = a * b;
  return t + c;
}

// Smoother::Next shape (Smoother.h:19).
float k_smoother(float value, float coef, float target) { return value + coef * (target - value); }

// Explicit, portable fused op: std::fma is correctly rounded everywhere (IEEE fusedMultiplyAdd).
float k_stdfma(float a, float b, float c) { return FMAF(a, b, c); }

// Reduction: vectorizing needs reassociation; must stay scalar-ordered without fast-math.
float k_reduce(const float* x, int n) {
  float s = 0.f;
  for (int i = 0; i < n; ++i) s += x[i];
  return s;
}

// Element-wise loop: vectorizable without reassociation (bit-identical lanes).
void k_scale(float* y, const float* x, float g, int n) {
  for (int i = 0; i < n; ++i) y[i] = x[i] * g + 0.5f;
}
}
