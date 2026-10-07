#pragma once
// PROBE (scratch, not project code): in-house exact binary32 <-> decimal for canonical
// JSON, integer arithmetic only. No floating-point instruction, no libc beyond memcpy,
// no table that needs generating, no third-party code.
//
//   Write: the shortest decimal that round-trips (Ryu/std::to_chars "shortest" semantics:
//          fewest significant digits, then closest to the value, ties to even digit),
//          laid out ECMAScript-Number-style. Exact interval test on a fixed-size bignum.
//   Parse: RFC 8259 number grammar, correctly rounded (round-half-even) to binary32 for
//          ANY digit count (first 120 significant digits kept + a sticky digit; every
//          binary32 midpoint has <= 113 significant digits). Overflow is an error;
//          underflow rounds (to a subnormal or zero) like IEEE.
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace bsnum {

// ── fixed-size unsigned bignum, 32-bit limbs, little-endian ─────────────────────────
struct Big {
  static constexpr int kLimbs = 20;  // 640 bits; worst case used below is ~450
  uint32_t w[kLimbs];
  int      n = 0;  // used limbs, no leading zero limb; 0 means zero
};

inline void Set(Big& a, uint64_t v) {
  a.n = 0;
  while (v != 0) { a.w[a.n++] = static_cast<uint32_t>(v); v >>= 32; }
}
inline int Bits(const Big& a) {
  if (a.n == 0) return 0;
  uint32_t t = a.w[a.n - 1];
  int      b = 0;
  while (t != 0) { ++b; t >>= 1; }
  return (a.n - 1) * 32 + b;
}
inline bool IsZero(const Big& a) { return a.n == 0; }
inline void MulSmall(Big& a, uint32_t m) {
  uint64_t c = 0;
  for (int i = 0; i < a.n; ++i) {
    const uint64_t t = static_cast<uint64_t>(a.w[i]) * m + c;
    a.w[i] = static_cast<uint32_t>(t);
    c      = t >> 32;
  }
  if (c != 0) a.w[a.n++] = static_cast<uint32_t>(c);
}
inline void AddSmall(Big& a, uint32_t v) {
  uint64_t c = v;
  for (int i = 0; i < a.n && c != 0; ++i) {
    const uint64_t t = static_cast<uint64_t>(a.w[i]) + c;
    a.w[i] = static_cast<uint32_t>(t);
    c      = t >> 32;
  }
  if (c != 0) a.w[a.n++] = static_cast<uint32_t>(c);
}
inline void Shl(Big& a, int s) {
  if (a.n == 0 || s == 0) return;
  const int limbs = s / 32, bits = s % 32;
  if (bits != 0) {
    uint32_t carry = 0;
    for (int i = 0; i < a.n; ++i) {
      const uint32_t v = a.w[i];
      a.w[i]           = (v << bits) | carry;
      carry            = v >> (32 - bits);
    }
    if (carry != 0) a.w[a.n++] = carry;
  }
  if (limbs != 0) {
    for (int i = a.n - 1; i >= 0; --i) a.w[i + limbs] = a.w[i];
    for (int i = 0; i < limbs; ++i) a.w[i] = 0;
    a.n += limbs;
  }
}
inline int Cmp(const Big& a, const Big& b) {
  if (a.n != b.n) return a.n < b.n ? -1 : 1;
  for (int i = a.n - 1; i >= 0; --i) {
    if (a.w[i] != b.w[i]) return a.w[i] < b.w[i] ? -1 : 1;
  }
  return 0;
}
inline void Sub(Big& a, const Big& b) {  // a -= b, requires a >= b
  int64_t borrow = 0;
  for (int i = 0; i < a.n; ++i) {
    int64_t t = static_cast<int64_t>(a.w[i]) - (i < b.n ? b.w[i] : 0u) - borrow;
    borrow    = t < 0 ? 1 : 0;
    if (t < 0) t += (int64_t{1} << 32);
    a.w[i] = static_cast<uint32_t>(t);
  }
  while (a.n > 0 && a.w[a.n - 1] == 0) --a.n;
}
inline void MulPow5(Big& a, int e) {
  for (; e >= 13; e -= 13) MulSmall(a, 1220703125u);  // 5^13
  uint32_t p = 1;
  for (; e > 0; --e) p *= 5u;
  MulSmall(a, p);
}
inline void MulPow10(Big& a, int e) { MulPow5(a, e); Shl(a, e); }
// q = floor(a / b) with q < 2^qbits; a becomes the remainder.
inline uint64_t DivQuot(Big& a, const Big& b, int qbits) {
  uint64_t q = 0;
  Big      t;
  for (int i = qbits - 1; i >= 0; --i) {
    t = b;
    Shl(t, i);
    if (Cmp(a, t) >= 0) { Sub(a, t); q |= uint64_t{1} << i; }
  }
  return q;
}
// The top `keep` bits of a (keep <= 63) and whether any bit below them is set.
inline uint64_t Top(const Big& a, int keep, bool* sticky, int* shift) {
  const int b = Bits(a);
  const int s = b > keep ? b - keep : 0;
  uint64_t  r = 0;
  for (int i = b - 1; i >= s; --i) r = (r << 1) | ((a.w[i / 32] >> (i % 32)) & 1u);
  bool st = false;
  for (int i = 0; i < s && !st; ++i) st = ((a.w[i / 32] >> (i % 32)) & 1u) != 0;
  *sticky = st;
  *shift  = s;
  return r;
}

inline uint32_t FloatBits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
inline float    BitsFloat(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }

// ── parser ──────────────────────────────────────────────────────────────────────────
enum class Status : uint8_t { Ok, Syntax, Range };

namespace detail {
// value = (q + frac) * 2^x, frac in [0,1) nonzero iff sticky; round half even to binary32.
inline Status Round(uint64_t q, bool sticky, int x, bool neg, uint32_t* out) {
  int qb = 0;
  for (uint64_t t = q; t != 0; t >>= 1) ++qb;
  const int lead   = qb - 1 + x;
  const bool normal = lead >= -126;
  int s = normal ? qb - 24 : -149 - x;
  uint64_t mant;
  if (s <= 0) {
    mant = q << (-s);  // exact (only small integers reach here; sticky is false)
  } else {
    mant = s >= 64 ? 0 : q >> s;
    const bool half = s - 1 < 64 && ((q >> (s - 1)) & 1u) != 0;
    const bool rest = sticky || (s - 1 >= 64 ? q != 0 : (q & ((uint64_t{1} << (s - 1)) - 1)) != 0);
    if (half && (rest || (mant & 1u) != 0)) ++mant;
  }
  uint32_t bits;
  if (normal) {
    int e = lead;
    if (mant == (uint64_t{1} << 24)) { mant >>= 1; ++e; }
    if (e > 127) return Status::Range;
    bits = (static_cast<uint32_t>(e + 127) << 23) | (static_cast<uint32_t>(mant) & 0x7FFFFFu);
  } else {
    bits = static_cast<uint32_t>(mant);  // 2^23 is the smallest normal, by construction
  }
  *out = bits | (neg ? 0x80000000u : 0u);
  return Status::Ok;
}
}  // namespace detail

constexpr int kKeepDigits = 120;

// Parses one JSON number at s[0..len). *used receives the characters consumed (the
// number may be followed by other JSON text). Returns Ok, Syntax or Range (|value| would
// round to infinity).
inline Status Parse(const char* s, size_t len, float* out, size_t* used) {
  size_t i   = 0;
  bool   neg = false;
  if (i < len && s[i] == '-') { neg = true; ++i; }
  if (i >= len || s[i] < '0' || s[i] > '9') return Status::Syntax;
  char     dig[kKeepDigits + 1];
  int      nd = 0;          // kept significant digits
  bool     sticky = false;  // a nonzero digit was dropped past kKeepDigits
  int64_t  dexp = 0;        // decimal exponent of the last kept digit, before 'e'
  auto take = [&](char c, bool fraction) {
    if (nd == 0 && c == '0') { if (fraction) --dexp; return; }  // leading zero
    if (nd < kKeepDigits) { dig[nd++] = c; if (fraction) --dexp; }
    else { if (!fraction) ++dexp; if (c != '0') sticky = true; }
  };
  if (s[i] == '0') { ++i; }  // a leading 0 must stand alone
  else { while (i < len && s[i] >= '0' && s[i] <= '9') take(s[i++], false); }
  if (i < len && s[i] == '.') {
    ++i;
    if (i >= len || s[i] < '0' || s[i] > '9') return Status::Syntax;
    while (i < len && s[i] >= '0' && s[i] <= '9') take(s[i++], true);
  }
  if (i < len && (s[i] == 'e' || s[i] == 'E')) {
    ++i;
    bool eneg = false;
    if (i < len && (s[i] == '+' || s[i] == '-')) { eneg = s[i] == '-'; ++i; }
    if (i >= len || s[i] < '0' || s[i] > '9') return Status::Syntax;
    int64_t ev = 0;
    while (i < len && s[i] >= '0' && s[i] <= '9') {
      if (ev < 100000000) ev = ev * 10 + (s[i] - '0');  // saturates far outside any range
      ++i;
    }
    dexp += eneg ? -ev : ev;
  }
  if (used != nullptr) *used = i;
  if (sticky) { dig[nd++] = '1'; --dexp; }
  while (nd > 0 && dig[nd - 1] == '0') { --nd; ++dexp; }
  if (nd == 0) { *out = BitsFloat(neg ? 0x80000000u : 0u); return Status::Ok; }
  if (nd + dexp > 39) return Status::Range;          // >= 10^39 > FLT_MAX
  if (nd + dexp < -45) { *out = BitsFloat(neg ? 0x80000000u : 0u); return Status::Ok; }  // < 1e-46 < 2^-150
  const int e = static_cast<int>(dexp);
  Big d;
  Set(d, 0);
  for (int k = 0; k < nd; ++k) { MulSmall(d, 10); AddSmall(d, static_cast<uint32_t>(dig[k] - '0')); }
  uint32_t bits = 0;
  Status   st;
  if (e >= 0) {  // d * 5^e * 2^e: an integer
    MulPow5(d, e);
    bool sticky2; int shift;
    const uint64_t q = Top(d, 27, &sticky2, &shift);
    st = detail::Round(q, sticky2, e + shift, neg, &bits);
  } else {       // d / 5^-e * 2^e
    Big b;
    Set(b, 1);
    MulPow5(b, -e);
    const int t = 26 + Bits(b) - Bits(d);
    if (t >= 0) Shl(d, t); else Shl(b, -t);
    const uint64_t q = DivQuot(d, b, 28);
    st = detail::Round(q, !IsZero(d), e - t, neg, &bits);
  }
  if (st == Status::Ok) *out = BitsFloat(bits);
  return st;
}

// ── shortest writer ─────────────────────────────────────────────────────────────────
// Shortest round-trip digits of a finite nonzero |x|: digits[0..n), and the decimal
// exponent k10 of the first digit (value = 0.d1d2... * 10^(k10+1)).
struct Digits { char d[10]; int n; int k10; };

namespace detail {
// floor(m*2^e / 10^k) (< 2^31), remainder num - q*den (in r), den, and M = 2^max(e,0)*10^max(-k,0).
inline uint64_t Scaled(uint32_t m, int e, int k, Big* r, Big* den, Big* M) {
  Set(*M, 1);
  if (e > 0) Shl(*M, e);
  if (k < 0) MulPow10(*M, -k);
  *r = *M;
  MulSmall(*r, m);
  Set(*den, 1);
  if (k > 0) MulPow10(*den, k);
  if (e < 0) Shl(*den, -e);
  return DivQuot(*r, *den, 31);
}
}  // namespace detail

inline Digits Shortest(float x) {
  const uint32_t u    = FloatBits(x) & 0x7FFFFFFFu;
  const uint32_t ef   = u >> 23, frac = u & 0x7FFFFFu;
  const uint32_t m    = ef == 0 ? frac : (frac | 0x800000u);
  const int      e    = ef == 0 ? -149 : static_cast<int>(ef) - 150;
  const bool     incl = (m & 1u) == 0;                       // RNE: even keeps both bounds
  const uint32_t dlo  = (frac == 0 && ef > 1) ? 1u : 2u;     // lower gap halves at 2^k
  // k10 = floor(log10(x)): estimate from the binary exponent, then correct exactly.
  int mb = 0;
  for (uint32_t t = m; t != 0; t >>= 1) ++mb;
  const int l2 = e + mb - 1;
  int k10 = (l2 * 78913) >> 18;  // floor(l2*log10(2)) within 1 (arithmetic shift)
  Big r, den, M;
  for (;;) {  // x >= 10^k10 ?
    const uint64_t q = detail::Scaled(m, e, k10, &r, &den, &M);
    if (q == 0) { --k10; continue; }
    if (q >= 10) { ++k10; continue; }
    break;
  }
  Digits out{};
  for (int p = 1; p <= 9; ++p) {
    const int      k  = k10 - (p - 1);
    const uint64_t lo = detail::Scaled(m, e, k, &r, &den, &M);
    Big            four_r = r;
    Shl(four_r, 2);
    Big g = M;
    if (dlo == 2) Shl(g, 1);
    const int  cl   = Cmp(four_r, g);
    const bool loOk = cl < 0 || (cl == 0 && incl);
    bool       hiOk = false;
    int        side = 0;  // which is closer: -1 lo, 1 hi, 0 tie
    if (!IsZero(r)) {
      Big dist = den;
      Sub(dist, r);
      Big four_d = dist;
      Shl(four_d, 2);
      Big g2 = M;
      Shl(g2, 1);
      const int ch = Cmp(four_d, g2);
      hiOk         = ch < 0 || (ch == 0 && incl);
      side         = Cmp(r, dist);  // r < dist: lo is closer
    }
    if (!loOk && !hiOk) continue;
    uint64_t pick;
    if (loOk && hiOk) pick = side < 0 ? lo : side > 0 ? lo + 1 : ((lo & 1u) == 0 ? lo : lo + 1);
    else pick = loOk ? lo : lo + 1;
    int kk = k;
    while (pick % 10 == 0) { pick /= 10; ++kk; }
    char tmp[12];
    int  n = 0;
    while (pick != 0) { tmp[n++] = static_cast<char>('0' + pick % 10); pick /= 10; }
    for (int j = 0; j < n; ++j) out.d[j] = tmp[n - 1 - j];
    out.n   = n;
    out.k10 = kk + n - 1;
    return out;
  }
  out.n = 0;  // unreachable: 9 digits always round-trip a binary32
  return out;
}

// Canonical text, ECMAScript Number::toString layout: plain notation for 1e-7 < |x| < 1e21,
// otherwise d.ddde[+-]NN. -0 prints as "-0" so the numeric layer is exact (canonical
// preset values are never -0). Returns the length; buf needs 24 bytes. x must be finite.
inline int Write(float x, char* buf) {
  const uint32_t u = FloatBits(x);
  int p = 0;
  if ((u & 0x80000000u) != 0) buf[p++] = '-';
  if ((u & 0x7FFFFFFFu) == 0) { buf[p++] = '0'; return p; }
  const Digits g = Shortest(x);
  const int    n = g.k10 + 1;  // ES's n: value = 0.d * 10^n
  const int    k = g.n;
  if (k <= n && n <= 21) {
    for (int j = 0; j < k; ++j) buf[p++] = g.d[j];
    for (int j = 0; j < n - k; ++j) buf[p++] = '0';
  } else if (0 < n && n <= 21) {
    for (int j = 0; j < n; ++j) buf[p++] = g.d[j];
    buf[p++] = '.';
    for (int j = n; j < k; ++j) buf[p++] = g.d[j];
  } else if (-6 < n && n <= 0) {
    buf[p++] = '0'; buf[p++] = '.';
    for (int j = 0; j < -n; ++j) buf[p++] = '0';
    for (int j = 0; j < k; ++j) buf[p++] = g.d[j];
  } else {
    buf[p++] = g.d[0];
    if (k > 1) { buf[p++] = '.'; for (int j = 1; j < k; ++j) buf[p++] = g.d[j]; }
    buf[p++] = 'e';
    int ex = n - 1;
    buf[p++] = ex < 0 ? '-' : '+';
    if (ex < 0) ex = -ex;
    if (ex >= 10) buf[p++] = static_cast<char>('0' + ex / 10);
    buf[p++] = static_cast<char>('0' + ex % 10);
  }
  return p;
}

}  // namespace bsnum
