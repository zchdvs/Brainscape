// Preset-pipeline lens, probe P5: canonical JSON numbers for binary32 values.
// Exhaustive over every finite float32 bit pattern:
//   (a) shortest-roundtrip decimal (std::to_chars float, Ryu-class) parsed back as
//       float (correctly rounded)                                -> must be exact
//   (b) same string parsed as binary64 (what JSON.parse / any double-based JSON
//       library does), then rounded to float (Math.fround)       -> double rounding?
//   (c) "%.9g"-style 9-significant-digit string via the double path
// Reports mismatch counts and string-length statistics.
#include <atomic>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

int main() {
  const unsigned nt = std::max(1u, std::thread::hardware_concurrency());
  std::atomic<uint64_t> finite{0}, badA{0}, badB{0}, badC{0}, lenSum{0}, len9{0};
  std::atomic<uint32_t> firstBadB{0xFFFFFFFFu};
  std::vector<std::thread> th;
  for (unsigned t = 0; t < nt; ++t) {
    th.emplace_back([&, t]() {
      uint64_t f = 0, a = 0, b = 0, c = 0, ls = 0, l9 = 0;
      char buf[64];
      for (uint64_t u = t; u <= 0xFFFFFFFFull; u += nt) {
        const uint32_t bits = static_cast<uint32_t>(u);
        if (((bits >> 23) & 0xFF) == 0xFF) continue;  // inf/nan: not representable in JSON
        float x; std::memcpy(&x, &bits, 4);
        ++f;
        auto r = std::to_chars(buf, buf + sizeof buf, x);  // shortest round-trip
        const size_t n = static_cast<size_t>(r.ptr - buf);
        ls += n;
        float y = 0; std::from_chars(buf, buf + n, y);
        uint32_t yb; std::memcpy(&yb, &y, 4);
        if (yb != bits) ++a;
        double d = 0; std::from_chars(buf, buf + n, d);
        float z = static_cast<float>(d);
        uint32_t zb; std::memcpy(&zb, &z, 4);
        if (zb != bits && !(x == 0 && z == 0 && (zb ^ bits) == 0)) {
          ++b;
          uint32_t cur = firstBadB.load();
          while (bits < cur && !firstBadB.compare_exchange_weak(cur, bits)) {}
        }
        auto r9 = std::to_chars(buf, buf + sizeof buf, x, std::chars_format::general, 9);
        const size_t n9 = static_cast<size_t>(r9.ptr - buf);
        l9 += n9;
        double d9 = 0; std::from_chars(buf, buf + n9, d9);
        float z9 = static_cast<float>(d9);
        uint32_t z9b; std::memcpy(&z9b, &z9, 4);
        if (z9b != bits) ++c;
      }
      finite += f; badA += a; badB += b; badC += c; lenSum += ls; len9 += l9;
    });
  }
  for (auto& x : th) x.join();
  std::printf("threads=%u finite floats=%llu\n", nt, (unsigned long long)finite.load());
  std::printf("(a) shortest -> from_chars<float>:            mismatches=%llu\n", (unsigned long long)badA.load());
  std::printf("(b) shortest -> from_chars<double> -> (float): mismatches=%llu (first bits=0x%08X)\n",
              (unsigned long long)badB.load(), firstBadB.load());
  std::printf("(c) %%.9g    -> from_chars<double> -> (float): mismatches=%llu\n", (unsigned long long)badC.load());
  std::printf("mean length: shortest=%.2f chars, %%.9g=%.2f chars\n", double(lenSum) / finite, double(len9) / finite);
  // A few examples of what typical parameter values look like.
  const float ex[] = {0.55f, 0.1f, 375.0f, 9500.0f, 0.3f * 3.0f, 1.0f / 3.0f, 20000.0f, -24.0f};
  char buf[64];
  for (float v : ex) {
    auto r = std::to_chars(buf, buf + sizeof buf, v); *r.ptr = 0;
    char b64[64]; auto r2 = std::to_chars(b64, b64 + sizeof b64, static_cast<double>(v)); *r2.ptr = 0;
    std::printf("  float %-14s | as-binary64 shortest (JCS/JSON.stringify of the float's exact value): %s\n", buf, b64);
  }
  return 0;
}
