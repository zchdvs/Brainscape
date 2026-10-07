// The number code's hashed checks (docs/design/mode-compiler.md §6.5, §10.2). Each set is cut
// into fixed index chunks, each chunk hashed on its own (FNV-1a 64) and the chunk hashes
// combined in index order, so a hash never depends on the thread count (record §2.8):
//
//   bsc_number_check --suite pr           every leg, every pull request: the strided round
//                                         trip (step 4099), halfway strings at stride 1009 and
//                                         20 million random strings, against committed hashes
//   bsc_number_check --suite exhaustive   nightly: all 4,278,190,080 finite floats written and
//                                         read back, against the committed hash
//   options: --threads N, --no-std (skip the cross-checks against std::to_chars/from_chars)
//
// The writer's hash is the specified writer's (§6.4: the two eight-digit exceptions); the pure
// shortest writer's hash is printed beside it, which is the design probe's (b9dddab197d0eb00
// exhaustively, 0530292719017104 at step 4099, record §2.3).
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "Number.h"

#if defined(_MSC_VER)
#include <crtdbg.h>
#include <stdlib.h>
#endif

#if __has_include(<version>)
#include <version>
#endif
#if __has_include(<charconv>)
#include <charconv>
#if defined(__cpp_lib_to_chars) || defined(_MSC_VER) || \
    (defined(_GLIBCXX_RELEASE) && _GLIBCXX_RELEASE >= 11)
#define BSC_HAVE_STD_FLOAT 1
#endif
#endif

namespace {

// ── The committed hashes ──────────────────────────────────────────────────────────────────
// The per-pull-request sets measured identical with MSVC 19.40, GCC 11.4 and Clang 14, the
// exhaustive round trip with MSVC 19.40 and GCC 11.4, all on x86-64; the design asks for one
// linux-arm64 and one macOS run before they are relied on (§10.2), which no host here offered.
constexpr uint64_t kRoundTrip4099     = 0x0530292719017104ull;  // the specified writer
constexpr uint64_t kRoundTrip4099Pure = 0x0530292719017104ull;  // the probe's (record §2.3)
constexpr uint64_t kHalfway1009       = 0x364148d9494c086bull;
constexpr uint64_t kRandom20M         = 0x6410cbbde1b9cc99ull;
constexpr uint64_t kExhaustive        = 0x62c26d79be6bab5aull;  // every finite float
constexpr uint64_t kExhaustivePure    = 0xb9dddab197d0eb00ull;  // the probe's (record §2.3)

constexpr uint64_t kFnvBasis = 0xcbf29ce484222325ull;

uint64_t Fnv(uint64_t h, const void* data, size_t n) {
  const auto* p = static_cast<const unsigned char*>(data);
  for (size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 0x100000001b3ull;
  }
  return h;
}
uint64_t FnvU32(uint64_t h, uint32_t v) {
  const unsigned char b[4] = {static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8),
                              static_cast<unsigned char>(v >> 16),
                              static_cast<unsigned char>(v >> 24)};
  return Fnv(h, b, 4);
}
// Chunk hashes combined in order, each as 8 little-endian bytes.
uint64_t Combine(const std::vector<uint64_t>& hashes) {
  uint64_t h = kFnvBasis;
  for (const uint64_t c : hashes) {
    unsigned char b[8];
    for (int i = 0; i < 8; ++i) b[i] = static_cast<unsigned char>(c >> (8 * i));
    h = Fnv(h, b, 8);
  }
  return h;
}

uint32_t BitsOf(float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  return u;
}
float FloatOf(uint32_t u) {
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

unsigned g_threads = 0;
bool     g_std     = true;

// Runs fn(chunk) for every chunk on the worker threads.
void Parallel(size_t chunks, const std::function<void(size_t)>& fn) {
  std::atomic<size_t>      next{0};
  std::vector<std::thread> pool;
  for (unsigned t = 0; t < g_threads; ++t) {
    pool.emplace_back([&] {
      for (size_t c; (c = next.fetch_add(1)) < chunks;) fn(c);
    });
  }
  for (auto& t : pool) t.join();
}

struct Counts {
  std::atomic<uint64_t> cases{0}, bad{0}, stdBad{0}, stdCompared{0}, doubleRounding{0};
};

// ── The strided (or exhaustive) round trip ────────────────────────────────────────────────
bool RoundTrip(uint32_t step, uint64_t expect, uint64_t expectPure) {
  constexpr size_t      kChunks = 256;  // 2^24 bit patterns each
  std::vector<uint64_t> hashes(kChunks), pure(kChunks);
  Counts                n;
  std::vector<uint32_t> doubled;
  std::atomic_flag      lock = ATOMIC_FLAG_INIT;
  Parallel(kChunks, [&](size_t c) {
    uint64_t h = kFnvBasis, hp = kFnvBasis, cases = 0, bad = 0, stdBad = 0, dbl = 0, cmp = 0;
    const uint64_t lo = uint64_t{c} << 24, hi = lo + (uint64_t{1} << 24);
    for (uint64_t u = lo; u < hi; u += step) {
      const auto bits = static_cast<uint32_t>(u);
      if ((bits & 0x7F800000u) == 0x7F800000u) continue;
      ++cases;
      char         text[bsc::kMaxNumberText + 1];
      const size_t len = bsc::WriteNumberBits(bits, text);
      h                = Fnv(h, text, len);
      h                = Fnv(h, "\n", 1);
      // The pure shortest text, which differs only for the two exceptions.
      char   ptext[bsc::kMaxNumberText + 1];
      size_t plen = len;
      if ((bits & 0x7FFFFFFFu) == 0x15AE43FDu) {
        plen = bsc::LayoutNumber((bits >> 31) != 0u, bsc::Shortest(bits), ptext);
        hp   = Fnv(hp, ptext, plen);
      } else {
        hp = Fnv(hp, text, len);
      }
      hp = Fnv(hp, "\n", 1);
      float back = 0.f;
      if (bsc::ParseJsonNumber(text, len, &back) != bsc::NumberStatus::Ok || BitsOf(back) != bits) {
        ++bad;
      }
#if defined(BSC_HAVE_STD_FLOAT)
      if (g_std) {
        ++cmp;
        // std::from_chars reads the text exactly, and binary64-then-binary32 (JavaScript's
        // JSON.parse and Math.fround) does too: the exceptions exist for that.
        float z = 0.f;
        auto  r = std::from_chars(text, text + len, z);
        if (r.ec != std::errc() || r.ptr != text + len || BitsOf(z) != bits) ++stdBad;
        double d = 0.0;
        std::from_chars(text, text + len, d);
        if (BitsOf(static_cast<float>(d)) != bits) {
          ++dbl;
          while (lock.test_and_set()) {
          }
          doubled.push_back(bits);
          lock.clear();
        }
        // The digits are std::to_chars's shortest, except the two exceptions' extra digit.
        if ((bits & 0x7FFFFFFFu) != 0u && (bits & 0x7FFFFFFFu) != 0x15AE43FDu) {
          char sb[48];
          auto s = std::to_chars(sb, sb + sizeof sb, FloatOf(bits & 0x7FFFFFFFu),
                                 std::chars_format::scientific);
          std::string sd;
          const char* p = sb;
          for (; p < s.ptr && *p != 'e'; ++p) {
            if (*p != '.') sd.push_back(*p);
          }
          const std::string exponent(p < s.ptr ? p + 1 : p, static_cast<const char*>(s.ptr));
          const int         se = exponent.empty() ? 0 : std::atoi(exponent.c_str());
          const bsc::ShortestDigits g  = bsc::Shortest(bits);
          if (sd != std::string(g.digits, static_cast<size_t>(g.count)) || se != g.exponent) {
            ++stdBad;
          }
        }
      }
#endif
    }
    hashes[c] = h;
    pure[c]   = hp;
    n.cases += cases;
    n.bad += bad;
    n.stdBad += stdBad;
    n.stdCompared += cmp;
    n.doubleRounding += dbl;
  });
  const uint64_t total = Combine(hashes), totalPure = Combine(pure);
  std::printf("round trip, step %u: %llu finite floats, %llu misread\n", step,
              static_cast<unsigned long long>(n.cases.load()),
              static_cast<unsigned long long>(n.bad.load()));
  if (n.stdCompared > 0) {
    std::printf("  std::to_chars digits / std::from_chars disagreements: %llu of %llu; "
                "binary64-then-binary32 misreads: %llu\n",
                static_cast<unsigned long long>(n.stdBad.load()),
                static_cast<unsigned long long>(n.stdCompared.load()),
                static_cast<unsigned long long>(n.doubleRounding.load()));
    for (const uint32_t b : doubled) std::printf("    0x%08X\n", static_cast<unsigned>(b));
  }
  std::printf("  text hash %016llx (committed %016llx); pure shortest %016llx "
              "(committed %016llx)\n",
              static_cast<unsigned long long>(total), static_cast<unsigned long long>(expect),
              static_cast<unsigned long long>(totalPure),
              static_cast<unsigned long long>(expectPure));
  return n.bad == 0 && n.stdBad == 0 && n.doubleRounding == 0 && total == expect &&
         totalPure == expectPure;
}

// ── Halfway strings ───────────────────────────────────────────────────────────────────────
// Built in fixed buffers, not std::string: the set is 13 million strings, and Debug builds
// with checked iterators would spend minutes in the allocator.
struct Text {
  char   s[400];
  size_t n = 0;
  void   Add(char c) { s[n++] = c; }
  void   Add(const char* t, size_t len) {
    std::memcpy(s + n, t, len);
    n += len;
  }
  void Add(const Text& t) { Add(t.s, t.n); }
  void Repeat(char c, size_t count) {
    std::memset(s + n, c, count);
    n += count;
  }
  void Int(int v) {
    char      buf[16];
    const int k = std::snprintf(buf, sizeof buf, "%d", v);
    Add(buf, static_cast<size_t>(k));
  }
};

// A little decimal arithmetic for building them.
struct Dec {
  uint32_t w[24];  // base 2^32, least significant first; (2^25 + 1) * 5^150 fits in 400 bits
  int      n = 0;
  void     Mul(uint32_t m) {
    uint64_t c = 0;
    for (int i = 0; i < n; ++i) {
      const uint64_t t = uint64_t{w[i]} * m + c;
      w[i]             = static_cast<uint32_t>(t);
      c                = t >> 32;
    }
    if (c != 0) w[n++] = static_cast<uint32_t>(c);
  }
  void Decimal(Text* out) const {
    uint32_t a[24];
    int      k = n;
    std::memcpy(a, w, sizeof(uint32_t) * static_cast<size_t>(n));
    uint32_t parts[48];  // base 10^9, least significant first
    int      np = 0;
    while (k > 0) {
      uint64_t rem = 0;
      for (int i = k; i-- > 0;) {
        const uint64_t cur = (rem << 32) | a[i];
        a[i]               = static_cast<uint32_t>(cur / 1000000000u);
        rem                = cur % 1000000000u;
      }
      while (k > 0 && a[k - 1] == 0) --k;
      parts[np++] = static_cast<uint32_t>(rem);
    }
    out->n = 0;
    if (np == 0) {
      out->Add('0');
      return;
    }
    char buf[16];
    int  len = std::snprintf(buf, sizeof buf, "%u", static_cast<unsigned>(parts[np - 1]));
    out->Add(buf, static_cast<size_t>(len));
    for (int i = np - 1; i-- > 0;) {
      len = std::snprintf(buf, sizeof buf, "%09u", static_cast<unsigned>(parts[i]));
      out->Add(buf, static_cast<size_t>(len));
    }
  }
};

// s - 1 for a decimal s >= 1, without leading zeros.
void MinusOne(Text* s) {
  size_t i = s->n - 1;
  while (s->s[i] == '0') s->s[i--] = '9';
  s->s[i]--;
  size_t z = 0;
  while (z + 1 < s->n && s->s[z] == '0') ++z;
  if (z > 0) {
    std::memmove(s->s, s->s + z, s->n - z);
    s->n -= z;
  }
}

struct Case {
  Text     text;
  uint32_t expect = 0;
};

// The exact midpoint between `bits` and the next float, in several spellings, and the decimals
// just above and below it (some past 120 significant digits, so the sticky digit decides).
// Returns the number of cases written (5 or 7).
int Halfway(uint32_t bits, Case* cases) {
  const uint32_t ef = bits >> 23, frac = bits & 0x7FFFFFu;
  const uint32_t m  = ef == 0 ? frac : (frac | 0x800000u);
  const int      e  = ef == 0 ? -149 : static_cast<int>(ef) - 150;
  const uint32_t up = bits + 1;
  Dec            h;
  h.w[0] = 2 * m + 1;
  h.n    = 1;
  int dexp;
  if (e - 1 >= 0) {
    for (int i = 0; i < e - 1; ++i) h.Mul(2);
    dexp = 0;
  } else {
    for (int i = 0; i < 1 - e; ++i) h.Mul(5);
    dexp = -(1 - e);
  }
  Text hd;
  h.Decimal(&hd);
  const uint32_t tie  = (m & 1u) == 0 ? bits : up;
  int            c    = 0;
  auto           next = [&](uint32_t expect) -> Text& {
    cases[c].expect = expect;
    cases[c].text.n = 0;
    return cases[c++].text;
  };
  {
    Text& t = next(tie);  // the midpoint
    t.Add(hd);
    t.Add('e');
    t.Int(dexp);
  }
  {
    Text& t = next(up);  // a digit above it
    t.Add(hd);
    t.Add("1e", 2);
    t.Int(dexp - 1);
  }
  {
    Text& t = next(up);  // far past 120 digits above it
    t.Add(hd);
    t.Repeat('0', 150);
    t.Add("1e", 2);
    t.Int(dexp - 151);
  }
  {
    Text& t = next(bits);  // a digit below it
    t.Add(hd);
    t.Add('0');
    MinusOne(&t);
    t.Add('e');
    t.Int(dexp - 1);
  }
  {
    Text& t = next(bits);  // far past 120 digits below it
    t.Add(hd);
    MinusOne(&t);
    t.Repeat('9', 151);
    t.Add('e');
    t.Int(dexp - 151);
  }
  if (dexp < 0) {  // as 0.000ddd or ddd.ddd, the fraction-digit path
    const int point = static_cast<int>(hd.n) + dexp;
    Text      f;
    if (point > 0) {
      f.Add(hd.s, static_cast<size_t>(point));
      f.Add('.');
      f.Add(hd.s + point, hd.n - static_cast<size_t>(point));
    } else {
      f.Add("0.", 2);
      f.Repeat('0', static_cast<size_t>(-point));
      f.Add(hd);
    }
    next(tie).Add(f);
    Text& neg = next(tie | 0x80000000u);
    neg.Add('-');
    neg.Add(f);
  }
  return c;
}

bool HalfwaySet(uint32_t stride, uint64_t expect) {
  constexpr uint32_t kTop    = 0x7F7FFFFFu;  // below FLT_MAX: the next float exists
  constexpr size_t   kPer    = 4096;         // floats per chunk
  const uint64_t     floats  = (uint64_t{kTop} + stride - 1) / stride;
  const size_t       strided = static_cast<size_t>((floats + kPer - 1) / kPer);
  std::vector<uint64_t> hashes(strided + 1);
  Counts                n;
  std::atomic<size_t>   longest{0};
  auto one = [&](uint32_t bits, uint64_t& h, uint64_t& cases, uint64_t& bad, uint64_t& stdBad,
                 size_t& lng, Case* cs) {
    const int count = Halfway(bits, cs);
    for (int k = 0; k < count; ++k) {
      const Case& c = cs[k];
      ++cases;
      float                   y  = 0.f;
      const bsc::NumberStatus st = bsc::ParseJsonNumber(c.text.s, c.text.n, &y);
      if (st != bsc::NumberStatus::Ok || BitsOf(y) != c.expect) ++bad;
      h = FnvU32(h, st == bsc::NumberStatus::Ok ? BitsOf(y) : 0xFFFFFFFFu);
      if (c.text.n > lng) lng = c.text.n;
#if defined(BSC_HAVE_STD_FLOAT)
      if (g_std) {
        float z = 0.f;
        auto  r = std::from_chars(c.text.s, c.text.s + c.text.n, z);
        if (r.ec == std::errc() && BitsOf(z) != c.expect) ++stdBad;
      }
#endif
    }
  };
  Parallel(strided + 1, [&](size_t chunk) {
    uint64_t                h = kFnvBasis, cases = 0, bad = 0, stdBad = 0;
    size_t                  lng = 0;
    std::unique_ptr<Case[]> cs(new Case[7]);
    if (chunk < strided) {
      const uint64_t first = uint64_t{chunk} * kPer;
      for (uint64_t k = first; k < first + kPer && k < floats; ++k) {
        one(static_cast<uint32_t>(k * stride), h, cases, bad, stdBad, lng, cs.get());
      }
    } else {  // the last chunk: the smallest subnormals and every binade's edges
      for (uint32_t b = 0; b < 0x400; ++b) one(b, h, cases, bad, stdBad, lng, cs.get());
      for (uint32_t ef = 1; ef < 255; ++ef) {
        one(ef << 23, h, cases, bad, stdBad, lng, cs.get());
        one((ef << 23) - 1, h, cases, bad, stdBad, lng, cs.get());
        if (ef < 254) one((ef << 23) | 0x7FFFFFu, h, cases, bad, stdBad, lng, cs.get());
      }
    }
    hashes[chunk] = h;
    n.cases += cases;
    n.bad += bad;
    n.stdBad += stdBad;
    size_t cur = longest.load();
    while (lng > cur && !longest.compare_exchange_weak(cur, lng)) {
    }
  });
  const uint64_t total = Combine(hashes);
  std::printf("halfway, stride %u: %llu strings (longest %zu characters), %llu misread",
              stride, static_cast<unsigned long long>(n.cases.load()), longest.load(),
              static_cast<unsigned long long>(n.bad.load()));
  if (g_std) {
    std::printf(" (std::from_chars: %llu)", static_cast<unsigned long long>(n.stdBad.load()));
  }
  std::printf("\n  hash %016llx (committed %016llx)\n", static_cast<unsigned long long>(total),
              static_cast<unsigned long long>(expect));
  return n.bad == 0 && total == expect;
}

// ── Random strings ────────────────────────────────────────────────────────────────────────
uint64_t Mix(uint64_t& s) {  // SplitMix64
  uint64_t z = (s += 0x9e3779b97f4a7c15ull);
  z          = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
  z          = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
  return z ^ (z >> 31);
}

// The probe's generator, keyed on the index (record §2.3): 1-40 digits, e-60..e39, written
// into a fixed buffer (at most 54 characters). Returns the length.
size_t RandomNumber(uint64_t index, char* out) {
  uint64_t s = 0x5eed000000000000ull ^ index;
  size_t   n = 0;
  if (Mix(s) & 1) out[n++] = '-';
  const int nd = 1 + static_cast<int>(Mix(s) % 40);
  const int ip = static_cast<int>(Mix(s) % static_cast<uint64_t>(nd + 1));
  if (ip == 0) {
    out[n++]          = '0';
    out[n++]          = '.';
    const uint64_t lz = Mix(s) % 8;
    for (uint64_t k = 0; k < lz; ++k) out[n++] = '0';
    for (int k = 0; k < nd; ++k) out[n++] = static_cast<char>('0' + Mix(s) % 10);
  } else {
    out[n++] = static_cast<char>('1' + Mix(s) % 9);
    for (int k = 1; k < ip; ++k) out[n++] = static_cast<char>('0' + Mix(s) % 10);
    if (ip < nd) {
      out[n++] = '.';
      for (int k = ip; k < nd; ++k) out[n++] = static_cast<char>('0' + Mix(s) % 10);
    }
  }
  const int ex = static_cast<int>(Mix(s) % 100) - 60;
  if (Mix(s) % 4 != 0) n += static_cast<size_t>(std::snprintf(out + n, 16, "e%d", ex));
  return n;
}

bool RandomSet(uint64_t count, uint64_t expect) {
  constexpr uint64_t    kPer   = uint64_t{1} << 20;
  const size_t          chunks = static_cast<size_t>((count + kPer - 1) / kPer);
  std::vector<uint64_t> hashes(chunks);
  Counts                n;
  std::atomic<uint64_t> range{0};
  Parallel(chunks, [&](size_t chunk) {
    uint64_t h = kFnvBasis, bad = 0, stdBad = 0, cmp = 0, rng = 0;
    for (uint64_t i = uint64_t{chunk} * kPer; i < (uint64_t{chunk} + 1) * kPer && i < count; ++i) {
      char                    str[64];
      const size_t            len = RandomNumber(i, str);
      float                   y   = 0.f;
      const bsc::NumberStatus st  = bsc::ParseJsonNumber(str, len, &y);
      h = FnvU32(h, st == bsc::NumberStatus::Ok ? BitsOf(y) : 0xFFFFFFFFu);
      if (st == bsc::NumberStatus::Range) ++rng;
      if (st == bsc::NumberStatus::Syntax) ++bad;
#if defined(BSC_HAVE_STD_FLOAT)
      if (g_std) {
        float z = 0.f;
        auto  r = std::from_chars(str, str + len, z);
        if (r.ec == std::errc()) {
          ++cmp;
          if (st != bsc::NumberStatus::Ok || BitsOf(z) != BitsOf(y)) ++stdBad;
        } else if (st == bsc::NumberStatus::Ok && (BitsOf(y) & 0x7FFFFFFFu) > 0x00800000u) {
          ++stdBad;  // std refused (overflow, or underflow on some libraries) a normal number
        }
      }
#endif
    }
    hashes[chunk] = h;
    n.bad += bad;
    n.stdBad += stdBad;
    n.stdCompared += cmp;
    range += rng;
  });
  const uint64_t total = Combine(hashes);
  std::printf("random: %llu strings, %llu refused as syntax, %llu out of range",
              static_cast<unsigned long long>(count), static_cast<unsigned long long>(n.bad.load()),
              static_cast<unsigned long long>(range.load()));
  if (n.stdCompared > 0) {
    std::printf(", std::from_chars disagreements %llu of %llu",
                static_cast<unsigned long long>(n.stdBad.load()),
                static_cast<unsigned long long>(n.stdCompared.load()));
  }
  std::printf("\n  hash %016llx (committed %016llx)\n", static_cast<unsigned long long>(total),
              static_cast<unsigned long long>(expect));
  return n.bad == 0 && n.stdBad == 0 && total == expect;
}

}  // namespace

int main(int argc, char** argv) {
#if defined(_MSC_VER)
  // No debug-CRT dialog under ctest (as dsp/tests/test_main.cpp).
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
  for (int type : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
    _CrtSetReportMode(type, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(type, _CRTDBG_FILE_STDERR);
  }
#endif
  std::string suite = "pr";
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--suite" && i + 1 < argc) suite = argv[++i];
    else if (a == "--threads" && i + 1 < argc) {
      g_threads = static_cast<unsigned>(std::atoi(argv[++i]));
    }
    else if (a == "--no-std") g_std = false;
  }
  if (g_threads == 0) g_threads = std::thread::hardware_concurrency();
  if (g_threads == 0) g_threads = 1;
#if !defined(BSC_HAVE_STD_FLOAT)
  g_std = false;
#endif
  const auto t0 = std::chrono::steady_clock::now();
  bool       ok = true;
  if (suite == "pr") {
    ok = RoundTrip(4099, kRoundTrip4099, kRoundTrip4099Pure) && ok;
    ok = HalfwaySet(1009, kHalfway1009) && ok;
    ok = RandomSet(20000000, kRandom20M) && ok;
  } else if (suite == "exhaustive") {
    ok = RoundTrip(1, kExhaustive, kExhaustivePure);
  } else {
    std::printf("usage: bsc_number_check [--suite pr|exhaustive] [--threads N] [--no-std]\n");
    return 2;
  }
  const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("number check (%s, %u threads, std cross-checks %s): %s in %.1f s\n", suite.c_str(),
              g_threads, g_std ? "on" : "off", ok ? "pass" : "FAIL", s);
  return ok ? 0 : 1;
}
