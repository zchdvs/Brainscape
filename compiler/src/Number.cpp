#include "Number.h"

#include <cstring>

// Exact binary32 <-> decimal in integers (docs/design/mode-compiler.md §6.5). Grown from the
// design's probe (record §2.3), which round-tripped all 4,278,190,080 finite floats, wrote
// std::to_chars's digits for every one and read 1,889,845,423 strings at and beside halfway
// points correctly; this is that algorithm, plus the writer's one specified exception and the
// lenient typed-text grammar.
//
// Reading keeps the first 120 significant digits and a sticky digit for the rest (every binary32
// midpoint has at most 113 significant digits, so any input length rounds correctly), then
// multiplies by 5^e for a positive exponent or divides by 5^-e for a negative one, exactly, and
// rounds half to even. Writing tests, for 1 to 9 digits, the floor and ceiling candidates
// exactly against the rounding interval (half-width below powers of two, endpoints included
// when the significand is even) and keeps the closest valid one, the even digit on a tie.
namespace bsc {

namespace {

// ── A fixed-size unsigned integer: 32-bit limbs, least significant first ─────────────────
struct Big {
  static constexpr int kLimbs = 20;  // 640 bits; the arithmetic below needs about 450
  uint32_t w[kLimbs];
  int      n = 0;  // limbs in use, no leading zero limb; 0 is zero
};

void Set(Big& a, uint64_t v) {
  a.n = 0;
  while (v != 0) {
    a.w[a.n++] = static_cast<uint32_t>(v);
    v >>= 32;
  }
}

int Bits(const Big& a) {
  if (a.n == 0) return 0;
  uint32_t t = a.w[a.n - 1];
  int      b = 0;
  while (t != 0) {
    ++b;
    t >>= 1;
  }
  return (a.n - 1) * 32 + b;
}

bool IsZero(const Big& a) { return a.n == 0; }

void MulSmall(Big& a, uint32_t m) {
  uint64_t c = 0;
  for (int i = 0; i < a.n; ++i) {
    const uint64_t t = static_cast<uint64_t>(a.w[i]) * m + c;
    a.w[i]           = static_cast<uint32_t>(t);
    c                = t >> 32;
  }
  if (c != 0) a.w[a.n++] = static_cast<uint32_t>(c);
}

void AddSmall(Big& a, uint32_t v) {
  uint64_t c = v;
  for (int i = 0; i < a.n && c != 0; ++i) {
    const uint64_t t = static_cast<uint64_t>(a.w[i]) + c;
    a.w[i]           = static_cast<uint32_t>(t);
    c                = t >> 32;
  }
  if (c != 0) a.w[a.n++] = static_cast<uint32_t>(c);
}

void Shl(Big& a, int s) {
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

int Cmp(const Big& a, const Big& b) {
  if (a.n != b.n) return a.n < b.n ? -1 : 1;
  for (int i = a.n - 1; i >= 0; --i) {
    if (a.w[i] != b.w[i]) return a.w[i] < b.w[i] ? -1 : 1;
  }
  return 0;
}

void Sub(Big& a, const Big& b) {  // a -= b, with a >= b
  int64_t borrow = 0;
  for (int i = 0; i < a.n; ++i) {
    int64_t t = static_cast<int64_t>(a.w[i]) - (i < b.n ? b.w[i] : 0u) - borrow;
    borrow    = t < 0 ? 1 : 0;
    if (t < 0) t += int64_t{1} << 32;
    a.w[i] = static_cast<uint32_t>(t);
  }
  while (a.n > 0 && a.w[a.n - 1] == 0) --a.n;
}

void MulPow5(Big& a, int e) {
  for (; e >= 13; e -= 13) MulSmall(a, 1220703125u);  // 5^13
  uint32_t p = 1;
  for (; e > 0; --e) p *= 5u;
  MulSmall(a, p);
}

void MulPow10(Big& a, int e) {
  MulPow5(a, e);
  Shl(a, e);
}

// floor(a / b), which must be below 2^qbits; a becomes the remainder.
uint64_t DivQuot(Big& a, const Big& b, int qbits) {
  uint64_t q = 0;
  Big      t;
  for (int i = qbits - 1; i >= 0; --i) {
    t = b;
    Shl(t, i);
    if (Cmp(a, t) >= 0) {
      Sub(a, t);
      q |= uint64_t{1} << i;
    }
  }
  return q;
}

// The top `keep` (<= 63) bits of a, whether any bit below them is set, and how many are below.
uint64_t Top(const Big& a, int keep, bool* sticky, int* shift) {
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

// ── Reading ───────────────────────────────────────────────────────────────────────────────
constexpr int kKeepDigits = 120;

// A decimal number as read: up to 120 significant digits (and a sticky one), the decimal
// exponent of the last of them, and the sign.
struct Decimal {
  char    digits[kKeepDigits + 1];
  int     count    = 0;
  int64_t exponent = 0;
  bool    sticky   = false;  // a nonzero digit was dropped past kKeepDigits
  bool    negative = false;

  void Take(char c, bool fraction) {
    if (count == 0 && c == '0') {  // a leading zero
      if (fraction) --exponent;
      return;
    }
    if (count < kKeepDigits) {
      digits[count++] = c;
      if (fraction) --exponent;
    } else {
      if (!fraction) ++exponent;
      if (c != '0') sticky = true;
    }
  }
};

bool Digit(char c) { return c >= '0' && c <= '9'; }

// An exponent's digits from s[*i], saturating far outside any range.
void ReadExponent(const char* s, size_t length, size_t* i, bool negative, Decimal* d) {
  int64_t v = 0;
  while (*i < length && Digit(s[*i])) {
    if (v < 100000000) v = v * 10 + (s[*i] - '0');
    ++*i;
  }
  d->exponent += negative ? -v : v;
}

// value = (q + frac) * 2^x, frac in [0, 1) nonzero exactly when `sticky`: binary32, round half
// to even.
NumberStatus Round(uint64_t q, bool sticky, int x, bool negative, uint32_t* out) {
  int qb = 0;
  for (uint64_t t = q; t != 0; t >>= 1) ++qb;
  const int  lead   = qb - 1 + x;
  const bool normal = lead >= -126;
  const int  s      = normal ? qb - 24 : -149 - x;
  uint64_t   mant;
  if (s <= 0) {
    mant = q << (-s);  // exact: only small integers reach here, without a sticky part
  } else {
    mant            = s >= 64 ? 0 : q >> s;
    const bool half = s - 1 < 64 && ((q >> (s - 1)) & 1u) != 0;
    const bool rest =
        sticky || (s - 1 >= 64 ? q != 0 : (q & ((uint64_t{1} << (s - 1)) - 1)) != 0);
    if (half && (rest || (mant & 1u) != 0)) ++mant;
  }
  uint32_t bits;
  if (normal) {
    int e = lead;
    if (mant == (uint64_t{1} << 24)) {
      mant >>= 1;
      ++e;
    }
    if (e > 127) return NumberStatus::Range;
    bits = (static_cast<uint32_t>(e + 127) << 23) | (static_cast<uint32_t>(mant) & 0x7FFFFFu);
  } else {
    bits = static_cast<uint32_t>(mant);  // a carry into 2^23 is the smallest normal, as it must
  }
  *out = bits | (negative ? 0x80000000u : 0u);
  return NumberStatus::Ok;
}

NumberStatus Convert(Decimal& d, float* out) {
  if (d.sticky) {
    d.digits[d.count++] = '1';
    --d.exponent;
  }
  while (d.count > 0 && d.digits[d.count - 1] == '0') {
    --d.count;
    ++d.exponent;
  }
  const uint32_t sign = d.negative ? 0x80000000u : 0u;
  uint32_t       bits = sign;
  if (d.count > 0) {
    if (d.count + d.exponent > 39) return NumberStatus::Range;  // >= 10^39 > FLT_MAX
    if (d.count + d.exponent >= -45) {                         // below that, under 2^-150: 0
      const int e = static_cast<int>(d.exponent);
      Big       m;
      Set(m, 0);
      for (int k = 0; k < d.count; ++k) {
        MulSmall(m, 10);
        AddSmall(m, static_cast<uint32_t>(d.digits[k] - '0'));
      }
      NumberStatus st;
      if (e >= 0) {  // m * 5^e * 2^e, an integer
        MulPow5(m, e);
        bool sticky = false;
        int  shift  = 0;
        const uint64_t q = Top(m, 27, &sticky, &shift);
        st = Round(q, sticky, e + shift, d.negative, &bits);
      } else {  // m / 5^-e * 2^e
        Big b;
        Set(b, 1);
        MulPow5(b, -e);
        const int t = 26 + Bits(b) - Bits(m);
        if (t >= 0) {
          Shl(m, t);
        } else {
          Shl(b, -t);
        }
        const uint64_t q = DivQuot(m, b, 28);
        st               = Round(q, !IsZero(m), e - t, d.negative, &bits);
      }
      if (st != NumberStatus::Ok) return st;
    }
  }
  std::memcpy(out, &bits, sizeof bits);
  return NumberStatus::Ok;
}

// ── Writing ───────────────────────────────────────────────────────────────────────────────
// floor(m * 2^e / 10^k) (below 2^31), the remainder of that division in r, the divisor in den,
// and M = 2^max(e, 0) * 10^max(-k, 0) (the numerator's scale) in M.
uint64_t Scaled(uint32_t m, int e, int k, Big* r, Big* den, Big* M) {
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

}  // namespace

NumberStatus ParseJsonNumber(const char* s, size_t length, float* out, size_t* used) noexcept {
  Decimal d;
  size_t  i = 0;
  if (i < length && s[i] == '-') {
    d.negative = true;
    ++i;
  }
  if (i >= length || !Digit(s[i])) return NumberStatus::Syntax;
  if (s[i] == '0') {
    ++i;  // a leading 0 stands alone
  } else {
    while (i < length && Digit(s[i])) d.Take(s[i++], false);
  }
  if (i < length && s[i] == '.') {
    ++i;
    if (i >= length || !Digit(s[i])) return NumberStatus::Syntax;
    while (i < length && Digit(s[i])) d.Take(s[i++], true);
  }
  if (i < length && (s[i] == 'e' || s[i] == 'E')) {
    ++i;
    bool negative = false;
    if (i < length && (s[i] == '+' || s[i] == '-')) negative = s[i++] == '-';
    if (i >= length || !Digit(s[i])) return NumberStatus::Syntax;
    ReadExponent(s, length, &i, negative, &d);
  }
  if (used != nullptr) {
    *used = i;
  } else if (i != length) {
    return NumberStatus::Syntax;
  }
  return Convert(d, out);
}

NumberStatus ParseTypedNumber(const char* s, size_t length, float* out) noexcept {
  Decimal d;
  size_t  i = 0;
  if (i < length && (s[i] == '+' || s[i] == '-')) d.negative = s[i++] == '-';
  size_t mantissa = 0;
  while (i < length && Digit(s[i])) {
    d.Take(s[i++], false);
    ++mantissa;
  }
  if (i < length && s[i] == '.') {
    ++i;
    while (i < length && Digit(s[i])) {
      d.Take(s[i++], true);
      ++mantissa;
    }
  }
  if (mantissa == 0) return NumberStatus::Syntax;
  if (i < length && (s[i] == 'e' || s[i] == 'E')) {
    ++i;
    bool negative = false;
    if (i < length && (s[i] == '+' || s[i] == '-')) negative = s[i++] == '-';
    if (i >= length || !Digit(s[i])) return NumberStatus::Syntax;
    ReadExponent(s, length, &i, negative, &d);
  }
  if (i != length) return NumberStatus::Syntax;
  return Convert(d, out);
}

ShortestDigits Shortest(uint32_t bits) noexcept {
  const uint32_t u    = bits & 0x7FFFFFFFu;
  const uint32_t ef   = u >> 23, frac = u & 0x7FFFFFu;
  const uint32_t m    = ef == 0 ? frac : (frac | 0x800000u);
  const int      e    = ef == 0 ? -149 : static_cast<int>(ef) - 150;
  const bool     incl = (m & 1u) == 0;                    // even: both bounds round back
  const uint32_t dlo  = (frac == 0 && ef > 1) ? 1u : 2u;  // the lower gap halves at 2^k
  // k10 = floor(log10(x)): estimated from the binary exponent, then corrected exactly.
  int mb = 0;
  for (uint32_t t = m; t != 0; t >>= 1) ++mb;
  const int l2  = e + mb - 1;
  int       k10 = (l2 * 78913) >> 18;  // floor(l2 * log10(2)), within one
  Big       r, den, M;
  for (;;) {
    const uint64_t q = Scaled(m, e, k10, &r, &den, &M);
    if (q == 0) {
      --k10;
      continue;
    }
    if (q >= 10) {
      ++k10;
      continue;
    }
    break;
  }
  ShortestDigits out;
  for (int p = 1; p <= 9; ++p) {
    const int      k  = k10 - (p - 1);
    const uint64_t lo = Scaled(m, e, k, &r, &den, &M);
    Big            fourR = r;
    Shl(fourR, 2);
    Big g = M;
    if (dlo == 2) Shl(g, 1);
    const int  cl   = Cmp(fourR, g);
    const bool loOk = cl < 0 || (cl == 0 && incl);
    bool       hiOk = false;
    int        side = 0;  // the closer candidate: -1 floor, 1 ceiling, 0 a tie
    if (!IsZero(r)) {
      Big dist = den;
      Sub(dist, r);
      Big fourD = dist;
      Shl(fourD, 2);
      Big g2 = M;
      Shl(g2, 1);
      const int ch = Cmp(fourD, g2);
      hiOk         = ch < 0 || (ch == 0 && incl);
      side         = Cmp(r, dist);
    }
    if (!loOk && !hiOk) continue;
    uint64_t pick;
    if (loOk && hiOk) {
      pick = side < 0 ? lo : side > 0 ? lo + 1 : ((lo & 1u) == 0 ? lo : lo + 1);
    } else {
      pick = loOk ? lo : lo + 1;
    }
    int kk = k;
    while (pick % 10 == 0) {
      pick /= 10;
      ++kk;
    }
    char tmp[12];
    int  n = 0;
    while (pick != 0) {
      tmp[n++] = static_cast<char>('0' + pick % 10);
      pick /= 10;
    }
    for (int j = 0; j < n; ++j) out.digits[j] = tmp[n - 1 - j];
    out.count    = n;
    out.exponent = kk + n - 1;
    return out;
  }
  return out;  // unreachable: nine digits always round-trip a binary32
}

size_t LayoutNumber(bool negative, const ShortestDigits& g, char* buf) noexcept {
  size_t p = 0;
  if (negative) buf[p++] = '-';
  const int n = g.exponent + 1;  // ECMAScript's n: value = 0.d1d2... * 10^n
  const int k = g.count;
  if (k <= n && n <= 21) {
    for (int j = 0; j < k; ++j) buf[p++] = g.digits[j];
    for (int j = 0; j < n - k; ++j) buf[p++] = '0';
  } else if (0 < n && n <= 21) {
    for (int j = 0; j < n; ++j) buf[p++] = g.digits[j];
    buf[p++] = '.';
    for (int j = n; j < k; ++j) buf[p++] = g.digits[j];
  } else if (-6 < n && n <= 0) {
    buf[p++] = '0';
    buf[p++] = '.';
    for (int j = 0; j < -n; ++j) buf[p++] = '0';
    for (int j = 0; j < k; ++j) buf[p++] = g.digits[j];
  } else {
    buf[p++] = g.digits[0];
    if (k > 1) {
      buf[p++] = '.';
      for (int j = 1; j < k; ++j) buf[p++] = g.digits[j];
    }
    buf[p++] = 'e';
    int ex   = n - 1;
    buf[p++] = ex < 0 ? '-' : '+';
    if (ex < 0) ex = -ex;
    if (ex >= 10) buf[p++] = static_cast<char>('0' + ex / 10);
    buf[p++] = static_cast<char>('0' + ex % 10);
  }
  return p;
}

size_t WriteNumberBits(uint32_t bits, char* out) noexcept {
  if ((bits & 0x7F800000u) == 0x7F800000u) return 0;  // NaN, infinities
  const bool negative = (bits & 0x80000000u) != 0u;
  if ((bits & 0x7FFFFFFFu) == 0u) {
    size_t p = 0;
    if (negative) out[p++] = '-';
    out[p++] = '0';
    return p;
  }
  // The two floats whose shortest text, read as binary64 and then rounded to binary32, gives
  // the neighbouring float: eight digits instead of seven (§6.4).
  if ((bits & 0x7FFFFFFFu) == 0x15AE43FDu) {
    static const char kText[] = "7.0385307e-26";
    size_t            p       = 0;
    if (negative) out[p++] = '-';
    std::memcpy(out + p, kText, sizeof kText - 1);
    return p + sizeof kText - 1;
  }
  return LayoutNumber(negative, Shortest(bits), out);
}

size_t WriteNumber(float value, char* out) noexcept {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof bits);
  return WriteNumberBits(bits, out);
}

}  // namespace bsc
