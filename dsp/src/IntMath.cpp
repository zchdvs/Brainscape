#include "detail/IntMath.h"

#include <cassert>
#include <cstdint>

// Integer-only (docs/design/clock.md §2.1): no floating-point arithmetic, so no FP guard. Kept out
// of the pedal's ITCM (firmware/CMakeLists.txt, _bs_not_itcm_members).
namespace brainscape::intmath {
namespace {

constexpr uint64_t kLow32 = 0xFFFFFFFFull;

// The exact 128-bit product a·b as hi:lo, from 32-bit limbs: each partial product is below
// 2^64, and the middle sum of three terms below 2^32 each loses no carry.
void Mul64x64(uint64_t a, uint64_t b, uint64_t* hi, uint64_t* lo) noexcept {
  const uint64_t a0 = a & kLow32, a1 = a >> 32;
  const uint64_t b0 = b & kLow32, b1 = b >> 32;
  const uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
  const uint64_t mid = (p00 >> 32) + (p01 & kLow32) + (p10 & kLow32);
  *lo = (mid << 32) | (p00 & kLow32);
  *hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
}

// Leading zero bits of v != 0, portably (no compiler builtin).
int Nlz64(uint64_t v) noexcept {
  int n = 0;
  if ((v >> 32) == 0) { n += 32; v <<= 32; }
  if ((v >> 48) == 0) { n += 16; v <<= 16; }
  if ((v >> 56) == 0) { n += 8; v <<= 8; }
  if ((v >> 60) == 0) { n += 4; v <<= 4; }
  if ((v >> 62) == 0) { n += 2; v <<= 2; }
  if ((v >> 63) == 0) { n += 1; }
  return n;
}

// (u1:u0) / v for u1 < v, so the quotient fits 64 bits: Knuth's algorithm D on 32-bit digits
// (Hacker's Delight's divlu), with 64-bit operations only. The remainder goes to *r.
uint64_t Div128By64(uint64_t u1, uint64_t u0, uint64_t v, uint64_t* r) noexcept {
  constexpr uint64_t b = 1ull << 32;
  const int s = Nlz64(v);  // normalise so that v's top bit is set
  v <<= s;
  const uint64_t vn1 = v >> 32, vn0 = v & kLow32;
  const uint64_t un32 = s == 0 ? u1 : (u1 << s) | (u0 >> (64 - s));
  const uint64_t un10 = u0 << s;
  const uint64_t un1 = un10 >> 32, un0 = un10 & kLow32;

  uint64_t q1 = un32 / vn1, rhat = un32 - q1 * vn1;
  while (q1 >= b || q1 * vn0 > b * rhat + un1) {
    --q1;
    rhat += vn1;
    if (rhat >= b) break;
  }
  const uint64_t un21 = un32 * b + un1 - q1 * v;  // modulo 2^64; the true value is below v
  uint64_t q0 = un21 / vn1;
  rhat = un21 - q0 * vn1;
  while (q0 >= b || q0 * vn0 > b * rhat + un0) {
    --q0;
    rhat += vn1;
    if (rhat >= b) break;
  }
  *r = (un21 * b + un0 - q0 * v) >> s;
  return q1 * b + q0;
}

// round-half-up(a·b / c) for c > 0; false when it is 2^64 or more.
bool MulDivRound(uint64_t a, uint64_t b, uint64_t c, uint64_t* q) noexcept {
  uint64_t hi, lo;
  Mul64x64(a, b, &hi, &lo);
  if (hi >= c) return false;  // a·b ≥ c·2^64
  uint64_t r;
  const uint64_t f = Div128By64(hi, lo, c, &r);
  // Half up: the fraction r/c is at least 1/2 when r ≥ c − r (r < c, so c − r does not wrap).
  if (r >= c - r) {
    if (f == UINT64_MAX) return false;
    *q = f + 1;
  } else {
    *q = f;
  }
  return true;
}

uint64_t Magnitude(int64_t v) noexcept {
  // 0 − (uint64_t)INT64_MIN is 2^63, exact: no signed overflow.
  return v < 0 ? 0u - static_cast<uint64_t>(v) : static_cast<uint64_t>(v);
}

}  // namespace

uint64_t MulDivRoundU64(uint64_t a, uint64_t b, uint64_t c) noexcept {
  assert(c != 0 && "MulDivRoundU64: division by zero");
  if (c == 0) return 0;
  uint64_t q;
  const bool fits = MulDivRound(a, b, c, &q);
  assert(fits && "MulDivRoundU64: the quotient overflows 64 bits");
  return fits ? q : UINT64_MAX;
}

int64_t MulDivRoundI64(int64_t a, int64_t b, int64_t c) noexcept {
  assert(c != 0 && "MulDivRoundI64: division by zero");
  if (c == 0) return 0;
  const bool negative = ((a < 0) != (b < 0)) != (c < 0);
  uint64_t m;
  if (!MulDivRound(Magnitude(a), Magnitude(b), Magnitude(c), &m)) m = UINT64_MAX;
  if (m == 0) return 0;  // a zero product has no sign
  const uint64_t limit = negative ? (1ull << 63) : (1ull << 63) - 1u;
  assert(m <= limit && "MulDivRoundI64: the result overflows int64_t");
  if (m > limit) return negative ? INT64_MIN : INT64_MAX;
  // −2^63 converts through uint64_t: 0 − m modulo 2^64, then to int64_t, which C++17 defines as
  // implementation-defined for values above INT64_MAX; every supported compiler is two's
  // complement, and the one value that needs it (2^63) is spelled out.
  if (negative) return m == (1ull << 63) ? INT64_MIN : -static_cast<int64_t>(m);
  return static_cast<int64_t>(m);
}

int64_t FloorDivI64(int64_t a, int64_t b) noexcept {
  assert(b != 0 && !(a == INT64_MIN && b == -1) && "FloorDivI64: invalid operands");
  if (b == 0 || (a == INT64_MIN && b == -1)) return 0;
  int64_t q = a / b;
  if (a % b != 0 && ((a < 0) != (b < 0))) --q;
  return q;
}

int64_t CeilDivI64(int64_t a, int64_t b) noexcept {
  assert(b != 0 && !(a == INT64_MIN && b == -1) && "CeilDivI64: invalid operands");
  if (b == 0 || (a == INT64_MIN && b == -1)) return 0;
  int64_t q = a / b;
  if (a % b != 0 && ((a < 0) == (b < 0))) ++q;
  return q;
}

int64_t FloorModI64(int64_t a, int64_t b) noexcept {
  assert(b != 0 && "FloorModI64: division by zero");
  if (b == 0 || b == -1) return 0;  // b == −1 also avoids INT64_MIN % −1, which C++ leaves undefined
  int64_t r = a % b;
  if (r != 0 && ((r < 0) != (b < 0))) r += b;
  return r;
}

}  // namespace brainscape::intmath
