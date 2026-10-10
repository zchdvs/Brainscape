// The tempo core's integer helpers (docs/design/clock.md §2.1, §8.2): MulDivRoundU64 and the
// signed form against a 128-bit reference on edge operands (0, 1, 2^64 − 1, products straddling
// 2^64 and approaching 2^128) and 10^7 random ones, ties included; FloorDiv, CeilDiv and FloorMod
// on every sign combination. Each check has a perturbed control proving it can fail.
#include <cstdint>
#include <vector>

#include "WideInt.h"
#include "catch.hpp"
#include "detail/IntMath.h"

#if defined(_MSC_VER) && defined(_M_X64) && !defined(__clang__)
#include <intrin.h>
#endif

using namespace brainscape;
using brainscape::testing::I128;
using brainscape::testing::RefMulDivRoundI64;
using brainscape::testing::RefMulDivRoundU64;
using brainscape::testing::W;

namespace {

struct SplitMix {
  uint64_t s;
  explicit SplitMix(uint64_t seed) : s(seed) {}
  uint64_t Next() {
    uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  // A value of a random bit length 0-64, so small, medium and huge operands all occur.
  uint64_t Wide() {
    const unsigned bits = static_cast<unsigned>(Next() % 65);
    if (bits == 0) return 0;
    const uint64_t v = Next();
    return bits == 64 ? v : v & ((1ull << bits) - 1u);
  }
};

// The native 128-bit check where the compiler has one: an independent third opinion.
bool NativeMulDivRoundU64(uint64_t a, uint64_t b, uint64_t c, uint64_t* q, bool* available) {
#if defined(__SIZEOF_INT128__)
  *available = true;
  const unsigned __int128 n = static_cast<unsigned __int128>(a) * b;
  unsigned __int128 f = n / c;
  const unsigned __int128 r = n % c;
  if (r >= c - r) ++f;
  if ((f >> 64) != 0) return false;
  *q = static_cast<uint64_t>(f);
  return true;
#elif defined(_MSC_VER) && defined(_M_X64) && !defined(__clang__)
  *available = true;
  uint64_t hi;
  const uint64_t lo = _umul128(a, b, &hi);
  if (hi >= c) return false;
  uint64_t r;
  uint64_t f = _udiv128(hi, lo, c, &r);
  if (r >= c - r) {
    if (f == UINT64_MAX) return false;
    ++f;
  }
  *q = f;
  return true;
#else
  (void)a; (void)b; (void)c; (void)q;
  *available = false;
  return false;
#endif
}

// round-half-up holds for q: with r = a·b − q·c (exact, modulo 2^128 and then signed, which is
// exact because |r| ≤ c), −c ≤ 2r < c. Multiplications only, no division: independent of both
// dividers.
bool HalfUpHolds(uint64_t a, uint64_t b, uint64_t c, uint64_t q) {
  uint64_t ph, pl, qh, ql;
  I128::MulU64(a, b, &ph, &pl);
  I128::MulU64(q, c, &qh, &ql);
  const uint64_t borrow = pl < ql ? 1u : 0u;
  const I128 r(ph - qh - borrow, pl - ql);
  // |r| ≤ c < 2^64 means r's high word is 0 or all ones.
  if (!(r.hi == 0 || r.hi == ~0ull)) return false;
  const I128 two_r = r + r;
  const I128 cw(0, c);
  return two_r >= -cw && two_r < cw;
}

std::vector<uint64_t> EdgeOperands() {
  std::vector<uint64_t> v = {0,
                             1,
                             2,
                             3,
                             5,
                             7,
                             1000000000ull,
                             (1ull << 31) - 1,
                             1ull << 31,
                             (1ull << 32) - 1,
                             1ull << 32,
                             (1ull << 32) + 1,
                             24ull << 32,           // K
                             576ull << 32,          // the duration divisor
                             48000ull << 32,        // R·2^32
                             1ull << 53,
                             (1ull << 63) - 1,
                             1ull << 63,
                             (1ull << 63) + 1,
                             0xFFFFFFFFFFFFFFFEull,
                             0xFFFFFFFFFFFFFFFFull,
                             0x8000000080000000ull,
                             0xFFFFFFFF00000000ull,
                             0x00000000FFFFFFFFull * 3,
                             10000000000000000019ull};
  return v;
}

}  // namespace

TEST_CASE("WideInt: the reference's 128-bit arithmetic", "[intmath][reference]") {
  I128::Overflow() = false;
  // Products and floor division against values worked by hand.
  REQUIRE(W(-7) * W(3) == W(-21));
  REQUIRE(I128::FloorDiv(W(-7), W(2)) == W(-4));
  REQUIRE(I128::FloorDiv(W(7), W(-2)) == W(-4));
  REQUIRE(I128::FloorDiv(W(-7), W(-2)) == W(3));
  REQUIRE(I128::CeilDiv(W(-7), W(2)) == W(-3));
  REQUIRE(I128::CeilDiv(W(7), W(2)) == W(4));
  const I128 big = W(INT64_MAX) * W(INT64_MAX);  // 2^126 − 2^64 + 1
  REQUIRE(big.hi == 0x3FFFFFFFFFFFFFFFull);
  REQUIRE(big.lo == 1);
  REQUIRE(I128::FloorDiv(big, W(INT64_MAX)) == W(INT64_MAX));
  REQUIRE_FALSE(I128::Overflow());
  // The overflow flag itself works.
  (void)(big * W(4));
  REQUIRE(I128::Overflow());
  I128::Overflow() = false;
  // The reference divider on known quotients.
  uint64_t q = 0;
  REQUIRE(RefMulDivRoundU64(3, 1, 2, &q));
  REQUIRE(q == 2);  // 1.5 → 2
  REQUIRE(RefMulDivRoundU64(5, 1, 4, &q));
  REQUIRE(q == 1);  // 1.25 → 1
  REQUIRE(RefMulDivRoundU64(UINT64_MAX, UINT64_MAX, UINT64_MAX, &q));
  REQUIRE(q == UINT64_MAX);
  REQUIRE_FALSE(RefMulDivRoundU64(UINT64_MAX, 2, 1, &q));
}

TEST_CASE("MulDivRoundU64: edge operands against the 128-bit reference", "[intmath]") {
  const std::vector<uint64_t> e = EdgeOperands();
  uint64_t checked = 0, native = 0;
  for (uint64_t a : e) {
    for (uint64_t b : e) {
      for (uint64_t c : e) {
        if (c == 0) continue;
        uint64_t ref;
        if (!RefMulDivRoundU64(a, b, c, &ref)) continue;  // outside the contract
        const uint64_t got = intmath::MulDivRoundU64(a, b, c);
        INFO("a=" << a << " b=" << b << " c=" << c);
        REQUIRE(got == ref);
        REQUIRE(HalfUpHolds(a, b, c, got));
        bool available = false;
        uint64_t nat = 0;
        const bool fits = NativeMulDivRoundU64(a, b, c, &nat, &available);
        if (available) {
          REQUIRE(fits);
          REQUIRE(nat == ref);
          ++native;
        }
        ++checked;
      }
    }
  }
  REQUIRE(checked > 5000);
  INFO("native cross-checks: " << native);
  SUCCEED();
}

TEST_CASE("MulDivRoundU64: random operands and exact ties", "[intmath]") {
#if defined(NDEBUG)
  constexpr uint64_t kRandom = 10000000;  // §8.2's 10^7
#else
  constexpr uint64_t kRandom = 1000000;  // Debug: a tenth, the same generator
#endif
  SplitMix rng(0x7E3D0C1Bu);
  uint64_t checked = 0, ties = 0;
  for (uint64_t i = 0; i < kRandom; ++i) {
    uint64_t a, b, c;
    switch (i % 4) {
      case 0:  // ties: a = m, b odd, c = 2m: a·b / c = b / 2, exactly half way
        a = (rng.Wide() >> 1) | 1u;
        b = (rng.Wide() | 1u) >> (rng.Next() % 33);
        b |= 1u;
        c = a << 1;
        break;
      case 1:  // near ties, c = 2m ± 1
        a = (rng.Wide() >> 1) | 1u;
        b = rng.Wide() | 1u;
        c = (a << 1) + ((rng.Next() & 1u) ? 1u : static_cast<uint64_t>(-1));
        if (c == 0) c = 1;
        break;
      case 2:  // products straddling 2^64
        a = rng.Next() | (1ull << 32);
        b = (rng.Next() >> 31) | 1u;
        c = rng.Wide() | 1u;
        break;
      default:
        a = rng.Wide();
        b = rng.Wide();
        c = rng.Wide();
        if (c == 0) c = 1;
        break;
    }
    uint64_t ref;
    if (!RefMulDivRoundU64(a, b, c, &ref)) continue;
    const uint64_t got = intmath::MulDivRoundU64(a, b, c);
    if (got != ref || !HalfUpHolds(a, b, c, got)) {
      INFO("a=" << a << " b=" << b << " c=" << c << " got=" << got << " ref=" << ref);
      REQUIRE(got == ref);
      REQUIRE(HalfUpHolds(a, b, c, got));
    }
    if ((i % 4) == 0) ++ties;
    ++checked;
  }
  REQUIRE(checked > kRandom / 2);
  REQUIRE(ties > kRandom / 8);
}

TEST_CASE("MulDivRoundU64: perturbed controls fail the same checks", "[intmath][control]") {
  // Truncation, half-down and half-even must each be caught on the tie set: the checks above can
  // fail.
  SplitMix rng(0x51u);
  int truncCaught = 0, downCaught = 0, evenCaught = 0, refCaught = 0;
  for (int i = 0; i < 1000; ++i) {
    const uint64_t a = (rng.Wide() >> 1) | 1u;
    const uint64_t b = ((rng.Next() >> 40) << 1) | 1u;  // odd
    const uint64_t c = a << 1;
    uint64_t ref;
    if (!RefMulDivRoundU64(a, b, c, &ref)) continue;
    const uint64_t floor = b / 2;           // a·b/c = b/2 exactly half way: floor is (b−1)/2
    const uint64_t halfDown = floor;        // ties down
    const uint64_t halfEven = (floor & 1u) ? floor + 1 : floor;
    if (!HalfUpHolds(a, b, c, floor)) ++truncCaught;
    if (!HalfUpHolds(a, b, c, halfDown)) ++downCaught;
    if (!HalfUpHolds(a, b, c, halfEven) && halfEven != ref) ++evenCaught;
    // And the reference itself disagrees with the perturbed answers.
    if (floor != ref) ++refCaught;
  }
  REQUIRE(truncCaught > 900);
  REQUIRE(downCaught > 900);
  REQUIRE(evenCaught > 300);
  REQUIRE(refCaught > 900);
  // An off-by-one result is caught away from ties too.
  REQUIRE_FALSE(HalfUpHolds(1000, 1000, 7, intmath::MulDivRoundU64(1000, 1000, 7) + 1));
  REQUIRE_FALSE(HalfUpHolds(1000, 1000, 7, intmath::MulDivRoundU64(1000, 1000, 7) - 1));
}

TEST_CASE("MulDivRoundI64: ties away from zero in every sign combination", "[intmath]") {
  struct Case {
    int64_t a, b, c, q;
  };
  const Case cases[] = {
      {3, 1, 2, 2},    {-3, 1, 2, -2},   {3, -1, 2, -2},  {3, 1, -2, -2},  {-3, -1, 2, 2},
      {-3, 1, -2, 2},  {3, -1, -2, 2},   {-3, -1, -2, -2}, {1, 1, 2, 1},   {-1, 1, 2, -1},
      {5, 1, 4, 1},    {-5, 1, 4, -1},   {7, 1, 4, 2},     {-7, 1, 4, -2}, {0, -5, 3, 0},
      {INT64_MIN, 1, 1, INT64_MIN},      {INT64_MIN, 1, -2, int64_t{1} << 62},
      {INT64_MAX, INT64_MAX, INT64_MAX, INT64_MAX},       {INT64_MIN, INT64_MAX, INT64_MAX, INT64_MIN},
      {INT64_MIN, -1, 2, int64_t{1} << 62},              {-1, INT64_MAX, 2, -(int64_t{1} << 62)},
  };
  for (const Case& t : cases) {
    INFO("a=" << t.a << " b=" << t.b << " c=" << t.c);
    REQUIRE(intmath::MulDivRoundI64(t.a, t.b, t.c) == t.q);
    int64_t ref;
    REQUIRE(RefMulDivRoundI64(t.a, t.b, t.c, &ref));
    REQUIRE(ref == t.q);
  }
}

TEST_CASE("MulDivRoundI64: random operands against the reference", "[intmath]") {
#if defined(NDEBUG)
  constexpr int kRandom = 4000000;
#else
  constexpr int kRandom = 400000;
#endif
  SplitMix rng(0xC0FFEEu);
  int checked = 0;
  for (int i = 0; i < kRandom; ++i) {
    auto sign = [&rng]() { return (rng.Next() & 1u) ? int64_t{1} : int64_t{-1}; };
    auto signedWide = [&rng, &sign]() {
      return static_cast<int64_t>(rng.Wide() >> 1) * sign();  // |v| < 2^63
    };
    int64_t a = signedWide(), b = signedWide(), c = signedWide();
    if ((i & 3) == 0) {  // a tie: |a·b / c| = odd / 2
      const int64_t m = static_cast<int64_t>((rng.Wide() >> 3) | 1u);
      a = m * sign();
      b = static_cast<int64_t>(((rng.Next() >> 44) << 1) | 1u) * sign();
      c = m * 2 * sign();
    }
    if (c == 0) continue;
    int64_t ref;
    if (!RefMulDivRoundI64(a, b, c, &ref)) continue;
    const int64_t got = intmath::MulDivRoundI64(a, b, c);
    if (got != ref) {
      INFO("a=" << a << " b=" << b << " c=" << c << " got=" << got << " ref=" << ref);
      REQUIRE(got == ref);
    }
    ++checked;
  }
  REQUIRE(checked > kRandom / 2);
}

TEST_CASE("MulDivRoundI64: a perturbed control (ties toward +inf) is caught", "[intmath][control]") {
  // round-half-up on the signed value (−1.5 → −1) instead of on the magnitude must disagree.
  int caught = 0;
  for (int64_t n = -99; n <= 99; n += 2) {  // n odd: n / 2 is a tie
    const int64_t perturbed = I128::FloorDiv(W(n + 1), W(2)).ToI64();  // floor(n/2 + 1/2)
    int64_t ref;
    REQUIRE(RefMulDivRoundI64(n, 1, 2, &ref));
    REQUIRE(intmath::MulDivRoundI64(n, 1, 2) == ref);
    if (perturbed != ref) ++caught;
  }
  REQUIRE(caught == 50);  // every negative tie
}

TEST_CASE("FloorDiv, CeilDiv, FloorMod: every sign combination", "[intmath]") {
  int truncationDiffers = 0;
  for (int64_t a = -300; a <= 300; ++a) {
    for (int64_t b = -17; b <= 17; ++b) {
      if (b == 0) continue;
      I128 q, r;
      I128::DivModFloor(W(a), W(b), &q, &r);
      const int64_t f = intmath::FloorDivI64(a, b);
      const int64_t m = intmath::FloorModI64(a, b);
      const int64_t c = intmath::CeilDivI64(a, b);
      INFO("a=" << a << " b=" << b);
      REQUIRE(f == q.ToI64());
      REQUIRE(m == r.ToI64());
      REQUIRE(c == I128::CeilDiv(W(a), W(b)).ToI64());
      // The definitions themselves: a = b·f + m with m in b's half-open range; ceil the least
      // integer at or above a / b.
      REQUIRE(a == b * f + m);
      if (b > 0) REQUIRE((m >= 0 && m < b));
      else REQUIRE((m <= 0 && m > b));
      REQUIRE(c == (m == 0 ? f : f + 1));
      if (a / b != f) ++truncationDiffers;  // the perturbed control: C++'s truncating `/`
    }
  }
  REQUIRE(truncationDiffers > 1000);
  // Extremes.
  const int64_t ex[] = {INT64_MIN, INT64_MIN + 1, -3, -2, -1, 0, 1, 2, 3, INT64_MAX - 1, INT64_MAX};
  for (int64_t a : ex) {
    for (int64_t b : ex) {
      if (b == 0 || (a == INT64_MIN && b == -1)) continue;
      I128 q, r;
      I128::DivModFloor(W(a), W(b), &q, &r);
      INFO("a=" << a << " b=" << b);
      REQUIRE(intmath::FloorDivI64(a, b) == q.ToI64());
      REQUIRE(intmath::FloorModI64(a, b) == r.ToI64());
      REQUIRE(intmath::CeilDivI64(a, b) == I128::CeilDiv(W(a), W(b)).ToI64());
    }
  }
  REQUIRE(intmath::FloorModI64(INT64_MIN, -1) == 0);
  REQUIRE_FALSE(I128::Overflow());
}
