#pragma once
#include <cfloat>

// Determinism-profile tripwires (docs/design/determinism-profile.md §3.3), included
// by every public dsp/ header so a consumer compiled with identity-breaking flags
// fails to build. The flags themselves come from brainscape::fp_profile; these only
// catch what the preprocessor can see (GCC's contraction mode, LTO and libm calls
// are left to the CI audits).
#if defined(__FAST_MATH__)  // GCC/Clang -ffast-math, -Ofast
#error "brainscape determinism profile: fast-math is forbidden"
#endif
#if defined(__FINITE_MATH_ONLY__) && __FINITE_MATH_ONLY__  // -ffinite-math-only
#error "brainscape determinism profile: finite-math-only is forbidden"
#endif
#if defined(_MSC_VER) && !defined(__clang__) && (defined(_M_FP_FAST) || defined(_M_FP_CONTRACT))
#error "brainscape determinism profile: /fp:fast and /fp:contract are forbidden"
#endif
#if !defined(FLT_EVAL_METHOD) || FLT_EVAL_METHOD != 0
#error "brainscape determinism profile: FLT_EVAL_METHOD must be 0 (no x87)"
#endif
