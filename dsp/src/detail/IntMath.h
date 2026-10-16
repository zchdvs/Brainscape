#pragma once
#include <cstdint>

// Integer helpers of the tempo core (docs/design/clock.md §2.1): every rounding the clock design
// calls "round" is one of these, never a C++ `/` on a signed value, since `/` truncates toward
// zero and `>>` of a negative value is implementation-defined in C++17. Integer-only, with no
// floating-point arithmetic, so they need no FP environment guard. Their bodies are out of line
// in IntMath.cpp, an object kept out of the pedal's ITCM (§9.6): an inline helper would compile
// into every includer, Engine.cpp among them. They run at control rate only.
//
// The 128-bit intermediate is built from 32-bit limbs, because 32-bit arm-none-eabi GCC has no
// __int128 and MSVC has none at all; one code path on every build keeps nothing
// platform-dependent. On the Cortex-M7 they compile to the 64-bit division helpers the symbol
// audit already allows (tools/ci/audit_symbols.py).
namespace brainscape::intmath {

// round-half-up(a·b / c) for c > 0, through the exact 128-bit product. The caller keeps c > 0
// and the rounded quotient below 2^64. A call outside that is a caller error: asserted in Debug,
// and given a defined result in Release on every build, never a trap or a platform's own answer:
// c == 0 returns 0, a quotient of 2^64 or more returns UINT64_MAX.
uint64_t MulDivRoundU64(uint64_t a, uint64_t b, uint64_t c) noexcept;

// The signed form: the magnitude |a|·|b| / |c| rounded half up, so ties go away from zero, with
// the sign of a·b/c. c != 0 and the result inside int64_t, as above (Release: c == 0 returns 0;
// a result outside int64_t saturates to INT64_MAX or INT64_MIN).
int64_t MulDivRoundI64(int64_t a, int64_t b, int64_t c) noexcept;

// floor(a / b), ceil(a / b) and a − b·floor(a / b) for b != 0 (FloorMod's result has b's sign, so
// it lies in [0, b) for b > 0). b == 0 and INT64_MIN / −1 are caller errors (Release: 0, except
// FloorModI64(a, −1), which is 0 and well defined).
int64_t FloorDivI64(int64_t a, int64_t b) noexcept;
int64_t CeilDivI64(int64_t a, int64_t b) noexcept;
int64_t FloorModI64(int64_t a, int64_t b) noexcept;

}  // namespace brainscape::intmath
