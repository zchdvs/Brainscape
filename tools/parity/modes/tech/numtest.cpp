// PROBE: exhaustive and adversarial checks of bsnum.h (in-house binary32 <-> decimal),
// cross-checked against the standard library where it has float <charconv>.
//   numtest info                  toolchain and <charconv> availability
//   numtest roundtrip [step]      every finite binary32 (step 1): Write -> Parse exact;
//                                 digits == std::to_chars shortest; std::from_chars(text)
//                                 exact; decimal->binary64->binary32 double-rounding cases;
//                                 FNV-1a of every text (identical on every toolchain?)
//   numtest halfway [stride]      exact midpoints between adjacent floats, and the decimal
//                                 just above / below them (incl. >120-digit sticky forms)
//                                 parse to the analytically expected bits; std cross-check
//   numtest random [count]        random JSON numbers: Parse vs std::from_chars<float>
//   numtest edge                  hand-written edge strings
//   numtest speed                 throughput
#include "bsnum.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if __has_include(<version>)
#include <version>
#endif
#if !defined(NO_STD_CHARCONV) && __has_include(<charconv>)
#include <charconv>
#if defined(__cpp_lib_to_chars) || defined(_MSC_VER) || (defined(_GLIBCXX_RELEASE) && _GLIBCXX_RELEASE >= 11)
#define HAVE_STD_FLOAT 1
#endif
#endif

using namespace bsnum;

static uint64_t Fnv(uint64_t h, const char* p, size_t n) {
  for (size_t i = 0; i < n; ++i) { h ^= static_cast<unsigned char>(p[i]); h *= 0x100000001b3ull; }
  return h;
}
static uint64_t FnvU32(uint64_t h, uint32_t v) {
  char b[4] = {char(v), char(v >> 8), char(v >> 16), char(v >> 24)};
  return Fnv(h, b, 4);
}
static unsigned Threads() { unsigned t = std::thread::hardware_concurrency(); return t ? t : 1; }

static void Info() {
#if defined(_MSC_VER) && !defined(__clang__)
  std::printf("compiler: MSVC %d (_MSC_FULL_VER %d)\n", _MSC_VER, _MSC_FULL_VER);
#elif defined(__clang__)
  std::printf("compiler: clang %s\n", __clang_version__);
#elif defined(__GNUC__)
  std::printf("compiler: gcc %d.%d.%d\n", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#endif
#if defined(_GLIBCXX_RELEASE)
  std::printf("library: libstdc++ _GLIBCXX_RELEASE=%d\n", _GLIBCXX_RELEASE);
#endif
#if defined(_LIBCPP_VERSION)
  std::printf("library: libc++ %d\n", _LIBCPP_VERSION);
#endif
#if defined(_MSVC_STL_VERSION)
  std::printf("library: MSVC STL %d (_MSVC_STL_UPDATE %ld)\n", _MSVC_STL_VERSION, (long)_MSVC_STL_UPDATE);
#endif
#if defined(__cpp_lib_to_chars)
  std::printf("__cpp_lib_to_chars = %ld\n", (long)__cpp_lib_to_chars);
#else
  std::printf("__cpp_lib_to_chars undefined\n");
#endif
#if defined(HAVE_STD_FLOAT)
  std::printf("std float to_chars/from_chars: used for cross-checks\n");
#else
  std::printf("std float to_chars/from_chars: NOT used\n");
#endif
}

static void Roundtrip(uint32_t step) {
  const unsigned nt = Threads();
  constexpr int kChunks = 256;
  std::vector<uint64_t> chunkHash(kChunks, 0);
  std::atomic<int> next{0};
  std::atomic<uint64_t> finite{0}, badRt{0}, badDigits{0}, badStdParse{0}, dblRound{0}, lenSum{0};
  std::atomic<uint32_t> firstBad{0xFFFFFFFFu};
  std::vector<uint32_t> dblList;
  std::atomic_flag lock = ATOMIC_FLAG_INIT;
  auto t0 = std::chrono::steady_clock::now();
  std::vector<std::thread> th;
  for (unsigned t = 0; t < nt; ++t) {
    th.emplace_back([&]() {
      for (;;) {
        const int c = next.fetch_add(1);
        if (c >= kChunks) break;
        uint64_t h = 0xcbf29ce484222325ull, f = 0, br = 0, bd = 0, bs = 0, dr = 0, ls = 0;
        const uint64_t lo = uint64_t(c) << 24, hi = lo + (uint64_t{1} << 24);
        for (uint64_t u = lo; u < hi; u += step) {
          const uint32_t bits = uint32_t(u);
          if ((bits & 0x7F800000u) == 0x7F800000u) continue;
          ++f;
          char buf[32];
          const int n = Write(BitsFloat(bits), buf);
          ls += n;
          h = Fnv(h, buf, size_t(n));
          h = Fnv(h, "\n", 1);
          float y = 0;
          size_t used = 0;
          if (Parse(buf, size_t(n), &y, &used) != Status::Ok || used != size_t(n) || FloatBits(y) != bits) {
            ++br;
            uint32_t cur = firstBad.load();
            while (bits < cur && !firstBad.compare_exchange_weak(cur, bits)) {}
          }
#if defined(HAVE_STD_FLOAT)
          if ((bits & 0x7FFFFFFFu) != 0) {
            // digits and exponent against std::to_chars shortest scientific
            char sb[48];
            auto r = std::to_chars(sb, sb + sizeof sb, BitsFloat(bits & 0x7FFFFFFFu), std::chars_format::scientific);
            std::string sd;
            int sexp = 0;
            const char* p = sb;
            for (; p < r.ptr && *p != 'e'; ++p) if (*p != '.') sd.push_back(*p);
            if (p < r.ptr) sexp = std::atoi(std::string(p + 1, static_cast<const char*>(r.ptr)).c_str());
            const Digits g = Shortest(BitsFloat(bits));
            if (sd != std::string(g.d, size_t(g.n)) || sexp != g.k10) ++bd;
          }
          float z = 0;
          auto rr = std::from_chars(buf, buf + n, z);
          if (rr.ec != std::errc() || rr.ptr != buf + n || FloatBits(z) != bits) ++bs;
          double d = 0;
          std::from_chars(buf, buf + n, d);
          const float zd = static_cast<float>(d);
          if (FloatBits(zd) != bits) {
            ++dr;
            while (lock.test_and_set()) {}
            dblList.push_back(bits);
            lock.clear();
          }
#endif
        }
        chunkHash[size_t(c)] = h;
        finite += f; badRt += br; badDigits += bd; badStdParse += bs; dblRound += dr; lenSum += ls;
      }
    });
  }
  for (auto& x : th) x.join();
  uint64_t total = 0xcbf29ce484222325ull;
  for (uint64_t ch : chunkHash) total = Fnv(total, reinterpret_cast<const char*>(&ch), 8);
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("roundtrip step=%u threads=%u: finite=%llu  %.1f s\n", step, nt, (unsigned long long)finite.load(), secs);
  std::printf("  in-house Write->Parse mismatches        : %llu (first 0x%08X)\n", (unsigned long long)badRt.load(), firstBad.load());
#if defined(HAVE_STD_FLOAT)
  std::printf("  digits/exponent != std::to_chars shortest: %llu\n", (unsigned long long)badDigits.load());
  std::printf("  std::from_chars<float>(text) mismatches : %llu\n", (unsigned long long)badStdParse.load());
  std::printf("  text->binary64->binary32 double rounding: %llu", (unsigned long long)dblRound.load());
  for (uint32_t b : dblList) { char buf[32]; int n = Write(BitsFloat(b), buf); buf[n] = 0; std::printf("  [0x%08X %s]", b, buf); }
  std::printf("\n");
#endif
  std::printf("  mean text length %.3f chars\n", double(lenSum.load()) / double(finite.load()));
  std::printf("  FNV-1a64 of all texts (chunked): %016llx\n", (unsigned long long)total);
}

// Decimal digits of a Big (destroys it).
static std::string BigDec(Big a) {
  if (IsZero(a)) return "0";
  std::vector<uint32_t> parts;
  while (!IsZero(a)) {
    uint64_t rem = 0;
    for (int i = a.n - 1; i >= 0; --i) {
      const uint64_t cur = (rem << 32) | a.w[i];
      a.w[i] = uint32_t(cur / 1000000000u);
      rem    = cur % 1000000000u;
    }
    while (a.n > 0 && a.w[a.n - 1] == 0) --a.n;
    parts.push_back(uint32_t(rem));
  }
  std::string s = std::to_string(parts.back());
  for (size_t i = parts.size() - 1; i-- > 0;) {
    char b[16];
    std::snprintf(b, sizeof b, "%09u", parts[i]);
    s += b;
  }
  return s;
}
static std::string DecMinusOne(std::string s) {  // s >= 1
  int i = int(s.size()) - 1;
  while (s[size_t(i)] == '0') { s[size_t(i)] = '9'; --i; }
  s[size_t(i)]--;
  size_t z = 0;
  while (z + 1 < s.size() && s[z] == '0') ++z;
  return s.substr(z);
}

struct Case { std::string text; uint32_t expect; };

static uint64_t g_stdChecked = 0, g_stdBad = 0;
static bool Check(const std::string& t, uint32_t expect, uint64_t* hash, bool* stdOkOut) {
  float y = 0;
  size_t used = 0;
  const Status st = Parse(t.data(), t.size(), &y, &used);
  bool ok = st == Status::Ok && used == t.size() && FloatBits(y) == expect;
  *hash = FnvU32(*hash, st == Status::Ok ? FloatBits(y) : 0xFFFFFFFFu);
  *stdOkOut = true;
#if defined(HAVE_STD_FLOAT)
  float z = 0;
  auto r = std::from_chars(t.data(), t.data() + t.size(), z);
  if (r.ec == std::errc()) { *stdOkOut = FloatBits(z) == expect; }
#endif
  return ok;
}

static void Halfway(uint32_t stride) {
  const unsigned nt = Threads();
  std::atomic<uint64_t> cases{0}, bad{0}, stdBad{0}, maxLen{0};
  std::atomic<uint32_t> firstBad{0xFFFFFFFFu};
  std::vector<uint64_t> hashes(nt, 0xcbf29ce484222325ull);
  auto t0 = std::chrono::steady_clock::now();
  std::vector<std::thread> th;
  for (unsigned t = 0; t < nt; ++t) {
    th.emplace_back([&, t]() {
      uint64_t h = 0xcbf29ce484222325ull, nc = 0, nb = 0, ns = 0, ml = 0;
      // Every float below FLT_MAX with the stride, plus all of [0x00000000, 0x00000400),
      // every power of two and the binade edges.
      auto one = [&](uint32_t bits) {
        const uint32_t ef = bits >> 23, frac = bits & 0x7FFFFFu;
        const uint32_t m = ef == 0 ? frac : (frac | 0x800000u);
        const int e = ef == 0 ? -149 : int(ef) - 150;
        const uint32_t up = bits + 1;  // next float (bits < FLT_MAX)
        Big H; Set(H, 2ull * m + 1);
        int dexp;
        if (e - 1 >= 0) { Shl(H, e - 1); dexp = 0; } else { MulPow5(H, 1 - e); dexp = -(1 - e); }
        const std::string hd = BigDec(H);
        const uint32_t tie = (m & 1u) == 0 ? bits : up;
        std::vector<Case> cs;
        cs.push_back({hd + "e" + std::to_string(dexp), tie});
        cs.push_back({hd + "1e" + std::to_string(dexp - 1), up});
        cs.push_back({hd + std::string(150, '0') + "1e" + std::to_string(dexp - 151), up});
        cs.push_back({DecMinusOne(hd + "0") + "e" + std::to_string(dexp - 1), bits});
        cs.push_back({DecMinusOne(hd) + std::string(151, '9') + "e" + std::to_string(dexp - 151), bits});
        if (dexp < 0) {  // the exact midpoint as 0.000ddd or ddd.ddd (fraction-digit path)
          const int pointPos = int(hd.size()) + dexp;  // digits before the point
          std::string s = pointPos > 0 ? hd.substr(0, size_t(pointPos)) + "." + hd.substr(size_t(pointPos))
                                       : "0." + std::string(size_t(-pointPos), '0') + hd;
          cs.push_back({s, tie});
          cs.push_back({"-" + s, tie | 0x80000000u});
        }
        for (const Case& c : cs) {
          ++nc;
          bool stdOk = true;
          if (!Check(c.text, c.expect, &h, &stdOk)) {
            ++nb;
            uint32_t cur = firstBad.load();
            while (bits < cur && !firstBad.compare_exchange_weak(cur, bits)) {}
          }
          if (!stdOk) ++ns;
          if (c.text.size() > ml) ml = c.text.size();
        }
      };
      for (uint64_t u = uint64_t(t) * stride; u < 0x7F7FFFFFull; u += uint64_t(stride) * nt) one(uint32_t(u));
      if (t == 0) {
        for (uint32_t b = 0; b < 0x400; ++b) one(b);
        for (uint32_t ef = 1; ef < 255; ++ef) { one(ef << 23); one((ef << 23) - 1); if (ef < 254) one((ef << 23) | 0x7FFFFFu); }
      }
      hashes[t] = h;
      cases += nc; bad += nb; stdBad += ns;
      uint64_t cur = maxLen.load();
      while (ml > cur && !maxLen.compare_exchange_weak(cur, ml)) {}
    });
  }
  for (auto& x : th) x.join();
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("halfway stride=%u: %llu cases (longest %llu chars) %.1f s\n", stride,
              (unsigned long long)cases.load(), (unsigned long long)maxLen.load(), secs);
  std::printf("  in-house Parse != expected bits: %llu (first float 0x%08X)\n", (unsigned long long)bad.load(), firstBad.load());
#if defined(HAVE_STD_FLOAT)
  std::printf("  std::from_chars<float> != expected (when it returned ok): %llu\n", (unsigned long long)stdBad.load());
#endif
}

static uint64_t Mix(uint64_t& s) {  // SplitMix64
  uint64_t z = (s += 0x9e3779b97f4a7c15ull);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
  return z ^ (z >> 31);
}

static void Random(uint64_t count) {
  const unsigned nt = Threads();
  std::atomic<uint64_t> bad{0}, stdRange{0}, cmp{0}, mineRange{0};
  std::vector<uint64_t> hashes(nt);
  auto t0 = std::chrono::steady_clock::now();
  std::vector<std::thread> th;
  for (unsigned t = 0; t < nt; ++t) {
    th.emplace_back([&, t]() {
      uint64_t s = 0x5eed0000ull + t, h = 0xcbf29ce484222325ull, b = 0, sr = 0, c = 0, mr = 0;
      for (uint64_t i = t; i < count; i += nt) {
        s = 0x5eed000000000000ull ^ i;  // keyed on the index: identical set on every toolchain
        std::string str;
        if (Mix(s) & 1) str.push_back('-');
        const int nd = 1 + int(Mix(s) % 40);      // significant-looking digits
        const int ip = int(Mix(s) % uint64_t(nd + 1));  // how many go before the point
        if (ip == 0) {
          str += "0.";
          const int lz = int(Mix(s) % 8);
          str.append(size_t(lz), '0');
          for (int k = 0; k < nd; ++k) str.push_back(char('0' + Mix(s) % 10));
        } else {
          str.push_back(char('1' + Mix(s) % 9));
          for (int k = 1; k < ip; ++k) str.push_back(char('0' + Mix(s) % 10));
          if (ip < nd) {
            str.push_back('.');
            for (int k = ip; k < nd; ++k) str.push_back(char('0' + Mix(s) % 10));
          }
        }
        const int ex = int(Mix(s) % 100) - 60;
        if (Mix(s) % 4 != 0) str += "e" + std::to_string(ex);
        float y = 0;
        size_t used = 0;
        const Status st = Parse(str.data(), str.size(), &y, &used);
        h = FnvU32(h, st == Status::Ok ? FloatBits(y) : 0xFFFFFFFFu);
        if (st == Status::Range) ++mr;
        if (st != Status::Ok || used != str.size()) { if (st != Status::Range) ++b; }
#if defined(HAVE_STD_FLOAT)
        float z = 0;
        auto r = std::from_chars(str.data(), str.data() + str.size(), z);
        if (r.ec == std::errc()) {
          ++c;
          if (st != Status::Ok || FloatBits(z) != FloatBits(y)) ++b;
        } else {
          ++sr;  // out of range for std (overflow, or underflow on some libraries)
          if (st == Status::Ok && (FloatBits(y) & 0x7FFFFFFFu) > 0x00800000u) ++b;
        }
#endif
      }
      hashes[t] = h;
      bad += b; stdRange += sr; cmp += c; mineRange += mr;
    });
  }
  for (auto& x : th) x.join();
  uint64_t total = 0xcbf29ce484222325ull;
  for (uint64_t hh : hashes) total = Fnv(total, reinterpret_cast<const char*>(&hh), 8);
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("random %llu strings (1-40 digits, e-60..e39) %.1f s: disagreements %llu, compared with std %llu, "
              "std out-of-range %llu, in-house Range %llu, hash %016llx\n",
              (unsigned long long)count, secs, (unsigned long long)bad.load(), (unsigned long long)cmp.load(),
              (unsigned long long)stdRange.load(), (unsigned long long)mineRange.load(), (unsigned long long)total);
}

static void Edge() {
  const char* cases[] = {
      "0", "-0", "0.0", "-0.0e5", "1", "0.55", "375", "9500", "20000", "1e-46", "7e-46", "7.1e-46",
      "1.401298464324817e-45", "1.1754942e-38", "1.17549435e-38", "3.4028235e38", "3.40282356e38",
      "340282356779733661637539395458142568447", "340282356779733661637539395458142568448",
      "1e39", "0e999999999", "1e-999999999", "00", "01", "1.", ".5", "+1", "1e", "1e+", "-", "1.5e+3",
      "7.038531e-26", "7.0385313e-26", "123456789012345678901234567890",
      "0.30000001192092896", "1e21", "1e-7", "123e-2",
  };
  for (const char* c : cases) {
    float y = 0;
    size_t used = 0;
    const Status st = Parse(c, std::strlen(c), &y, &used);
    char out[32] = "-";
    if (st == Status::Ok) { int n = Write(y, out); out[n] = 0; }
    std::printf("  %-42s -> %s used=%zu bits=0x%08X text=%s\n", c,
                st == Status::Ok ? "Ok    " : st == Status::Range ? "Range " : "Syntax", used,
                st == Status::Ok ? FloatBits(y) : 0u, out);
  }
}

static void Speed() {
  auto t0 = std::chrono::steady_clock::now();
  uint64_t acc = 0, n = 0;
  char buf[32];
  for (uint32_t b = 0x3C000000u; b < 0x46000000u; b += 997) {  // ~1/128 .. 8192, typical parameter magnitudes
    acc += uint64_t(Write(BitsFloat(b), buf)) + uint8_t(buf[0]);
    ++n;
  }
  const double w = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  t0 = std::chrono::steady_clock::now();
  uint64_t m = 0;
  for (uint32_t b = 0x3C000000u; b < 0x46000000u; b += 997) {
    const int k = Write(BitsFloat(b), buf);
    float y; size_t used;
    Parse(buf, size_t(k), &y, &used);
    acc += FloatBits(y);
    ++m;
  }
  const double p = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() - w;
  std::printf("speed (1 thread): Write %.0f ns/value, Parse %.0f ns/value (acc %llu)\n", w / double(n) * 1e9,
              p / double(m) * 1e9, (unsigned long long)acc);
}

int main(int argc, char** argv) {
  const std::string mode = argc > 1 ? argv[1] : "info";
  if (mode == "info") Info();
  else if (mode == "roundtrip") Roundtrip(argc > 2 ? uint32_t(std::strtoul(argv[2], nullptr, 10)) : 1u);
  else if (mode == "halfway") Halfway(argc > 2 ? uint32_t(std::strtoul(argv[2], nullptr, 10)) : 1009u);
  else if (mode == "random") Random(argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 10000000ull);
  else if (mode == "edge") Edge();
  else if (mode == "speed") Speed();
  return 0;
}
