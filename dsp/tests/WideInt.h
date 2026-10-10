#pragma once
#include <cstdint>

// A test-only exact signed 128-bit integer (two's complement over two 64-bit words), the big-integer
// arithmetic the tempo core's reference models use (docs/design/clock.md §8.2). Deliberately a
// different construction from dsp/src/IntMath.cpp's: products from 16-bit digits, division by
// shift and subtract, so a shared mistake is unlikely. Overflow beyond 127 bits is a test bug:
// every operation that could overflow checks and sets `overflow`, which the tests REQUIRE clear.
// Portable C++17: no __int128 (MSVC has none), no intrinsics.
namespace brainscape::testing {

struct I128 {
  uint64_t hi = 0, lo = 0;

  static bool& Overflow() {
    static bool flag = false;
    return flag;
  }

  I128() = default;
  constexpr I128(uint64_t h, uint64_t l) : hi(h), lo(l) {}
  static I128 FromI64(int64_t v) {
    return I128(v < 0 ? ~0ull : 0ull, static_cast<uint64_t>(v));
  }
  static I128 FromU64(uint64_t v) { return I128(0, v); }

  bool Negative() const { return (hi >> 63) != 0; }
  bool IsZero() const { return hi == 0 && lo == 0; }
  bool FitsI64() const {
    return (hi == 0 && (lo >> 63) == 0) || (hi == ~0ull && (lo >> 63) == 1);
  }
  int64_t ToI64() const {
    if (!FitsI64()) Overflow() = true;
    return static_cast<int64_t>(lo);
  }
  bool FitsU64() const { return hi == 0; }
  uint64_t ToU64() const {
    if (!FitsU64()) Overflow() = true;
    return lo;
  }

  friend I128 operator+(I128 a, I128 b) {
    I128 r(a.hi + b.hi, a.lo + b.lo);
    if (r.lo < a.lo) ++r.hi;
    if (a.Negative() == b.Negative() && r.Negative() != a.Negative()) Overflow() = true;
    return r;
  }
  friend I128 operator-(I128 a) {
    I128 r(~a.hi, ~a.lo);
    r.lo += 1;
    if (r.lo == 0) ++r.hi;
    if (a.hi == (1ull << 63) && a.lo == 0) Overflow() = true;
    return r;
  }
  friend I128 operator-(I128 a, I128 b) { return a + (-b); }

  friend bool operator==(I128 a, I128 b) { return a.hi == b.hi && a.lo == b.lo; }
  friend bool operator!=(I128 a, I128 b) { return !(a == b); }
  friend bool operator<(I128 a, I128 b) {
    if (a.Negative() != b.Negative()) return a.Negative();
    return a.hi != b.hi ? a.hi < b.hi : a.lo < b.lo;
  }
  friend bool operator>(I128 a, I128 b) { return b < a; }
  friend bool operator<=(I128 a, I128 b) { return !(b < a); }
  friend bool operator>=(I128 a, I128 b) { return !(a < b); }

  // The magnitude as hi:lo (exact for every value but −2^127, an overflow here).
  void Magnitude(uint64_t* h, uint64_t* l) const {
    const I128 m = Negative() ? -*this : *this;
    *h = m.hi;
    *l = m.lo;
  }

  // Exact product of two unsigned 64-bit values from 16-bit digits.
  static void MulU64(uint64_t a, uint64_t b, uint64_t* h, uint64_t* l) {
    uint32_t r[8] = {};  // 16-bit digits of the product, in 32-bit cells
    for (int i = 0; i < 4; ++i) {
      const uint64_t ai = (a >> (16 * i)) & 0xFFFFu;
      uint64_t carry = 0;
      for (int j = 0; j < 4; ++j) {
        const uint64_t bj = (b >> (16 * j)) & 0xFFFFu;
        const uint64_t t = ai * bj + r[i + j] + carry;
        r[i + j] = static_cast<uint32_t>(t & 0xFFFFu);
        carry = t >> 16;
      }
      for (int k = i + 4; carry != 0 && k < 8; ++k) {
        const uint64_t t = r[k] + carry;
        r[k] = static_cast<uint32_t>(t & 0xFFFFu);
        carry = t >> 16;
      }
    }
    *l = 0;
    *h = 0;
    for (int i = 0; i < 4; ++i) *l |= static_cast<uint64_t>(r[i]) << (16 * i);
    for (int i = 0; i < 4; ++i) *h |= static_cast<uint64_t>(r[i + 4]) << (16 * i);
  }

  friend I128 operator*(I128 a, I128 b) {
    const bool neg = a.Negative() != b.Negative();
    uint64_t ah, al, bh, bl;
    a.Magnitude(&ah, &al);
    b.Magnitude(&bh, &bl);
    if (ah != 0 && bh != 0) Overflow() = true;
    uint64_t h, l, h2, l2, h3, l3;
    MulU64(al, bl, &h, &l);
    MulU64(ah, bl, &h2, &l2);
    MulU64(al, bh, &h3, &l3);
    if (h2 != 0 || h3 != 0) Overflow() = true;
    const uint64_t s1 = h + l2;
    const uint64_t hsum = s1 + l3;
    if (s1 < h || hsum < s1 || (hsum >> 63) != 0) {
      // A magnitude of 2^127 or more (−2^127 itself is never needed here).
      Overflow() = true;
    }
    I128 m(hsum, l);
    if (m.IsZero()) return m;
    return neg ? -m : m;
  }

  // floor(a / b) and the floor remainder (sign of b), by shift and subtract on magnitudes.
  static void DivModFloor(I128 a, I128 b, I128* q, I128* r) {
    if (b.IsZero()) {
      Overflow() = true;
      *q = I128();
      *r = I128();
      return;
    }
    uint64_t nh, nl, dh, dl;
    a.Magnitude(&nh, &nl);
    b.Magnitude(&dh, &dl);
    uint64_t qh = 0, ql = 0, rh = 0, rl = 0;
    for (int i = 127; i >= 0; --i) {
      // r = r·2 + bit i of n
      rh = (rh << 1) | (rl >> 63);
      rl <<= 1;
      const uint64_t bit = i >= 64 ? (nh >> (i - 64)) & 1u : (nl >> i) & 1u;
      rl |= bit;
      if (rh > dh || (rh == dh && rl >= dl)) {
        const uint64_t borrow = rl < dl ? 1u : 0u;
        rl -= dl;
        rh = rh - dh - borrow;
        if (i >= 64) qh |= 1ull << (i - 64);
        else ql |= 1ull << i;
      }
    }
    I128 tq(qh, ql), tr(rh, rl);  // truncated quotient and remainder of the magnitudes
    const bool negQ = a.Negative() != b.Negative();
    if (negQ) tq = -tq;
    if (a.Negative() && !tr.IsZero()) tr = -tr;  // remainder of truncation has a's sign
    // To floor: when the remainder is nonzero and its sign differs from b's.
    if (!tr.IsZero() && (tr.Negative() != b.Negative())) {
      tq = tq - I128::FromI64(1);
      tr = tr + b;
    }
    *q = tq;
    *r = tr;
  }
  static I128 FloorDiv(I128 a, I128 b) {
    I128 q, r;
    DivModFloor(a, b, &q, &r);
    return q;
  }
  static I128 CeilDiv(I128 a, I128 b) { return -FloorDiv(-a, b); }
};

inline I128 W(int64_t v) { return I128::FromI64(v); }
inline I128 WU(uint64_t v) { return I128::FromU64(v); }

// The reference round-half-up(a·b / c) for c > 0: the full 128-bit quotient by shift and subtract
// (a 65-bit running remainder), then the half-up step; false when the result is 2^64 or more.
inline bool RefMulDivRoundU64(uint64_t a, uint64_t b, uint64_t c, uint64_t* q) {
  uint64_t nh, nl;
  I128::MulU64(a, b, &nh, &nl);
  uint64_t qh = 0, ql = 0, r = 0;
  for (int i = 127; i >= 0; --i) {
    const uint64_t top = r >> 63;  // the remainder's 65th bit after the shift
    r = (r << 1) | (i >= 64 ? (nh >> (i - 64)) & 1u : (nl >> i) & 1u);
    if (top != 0 || r >= c) {
      r -= c;  // modulo 2^64: the true difference is below c
      if (i >= 64) qh |= 1ull << (i - 64);
      else ql |= 1ull << i;
    }
  }
  // Half up: 2r ≥ c.
  if (r >= c - r) {
    ++ql;
    if (ql == 0) ++qh;
  }
  if (qh != 0) return false;
  *q = ql;
  return true;
}

// The reference signed form: the magnitude rounded half up (ties away from zero), signed.
inline bool RefMulDivRoundI64(int64_t a, int64_t b, int64_t c, int64_t* q) {
  auto mag = [](int64_t v) {
    return v < 0 ? 0u - static_cast<uint64_t>(v) : static_cast<uint64_t>(v);
  };
  uint64_t m;
  if (!RefMulDivRoundU64(mag(a), mag(b), mag(c), &m)) return false;
  const bool neg = ((a < 0) != (b < 0)) != (c < 0);
  if (m == 0) {
    *q = 0;
    return true;
  }
  if (neg) {
    if (m > (1ull << 63)) return false;
    *q = m == (1ull << 63) ? INT64_MIN : -static_cast<int64_t>(m);
  } else {
    if (m > static_cast<uint64_t>(INT64_MAX)) return false;
    *q = static_cast<int64_t>(m);
  }
  return true;
}

}  // namespace brainscape::testing
