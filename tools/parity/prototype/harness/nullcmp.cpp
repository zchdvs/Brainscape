// nullcmp a.f32 b.f32 : sample-level comparison of two interleaved-stereo float32 dumps.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

static std::vector<float> Load(const char* p) {
  std::vector<float> v;
  FILE* f = std::fopen(p, "rb");
  if (!f) return v;
  std::fseek(f, 0, SEEK_END);
  long b = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  v.resize(static_cast<size_t>(b) / 4);
  if (std::fread(v.data(), 4, v.size(), f) != v.size()) v.clear();
  std::fclose(f);
  return v;
}

int main(int argc, char** argv) {
  if (argc < 3) return 1;
  auto a = Load(argv[1]), b = Load(argv[2]);
  if (a.empty() || a.size() != b.size()) { std::printf("size mismatch\n"); return 2; }
  const size_t n = a.size();
  size_t first = SIZE_MAX, ndiff = 0;
  double sa = 0, sb = 0, sab = 0, sd = 0, maxd = 0;
  for (size_t i = 0; i < n; ++i) {
    if (std::memcmp(&a[i], &b[i], 4) != 0) { if (first == SIZE_MAX) first = i; ++ndiff; }
    const double x = a[i], y = b[i], d = x - y;
    sa += x * x; sb += y * y; sab += x * y; sd += d * d;
    if (std::fabs(d) > maxd) maxd = std::fabs(d);
  }
  const double rmsA = std::sqrt(sa / n), rmsD = std::sqrt(sd / n);
  const double corr = sab / (std::sqrt(sa * sb) + 1e-300);
  // Time at which 100 ms windows first fall below 0.5 correlation (decorrelation time).
  const size_t win = 2 * 4800;
  double decor = -1;
  for (size_t s = 0; s + win <= n; s += win) {
    double wa = 0, wb = 0, wab = 0;
    for (size_t i = s; i < s + win; ++i) { wa += double(a[i]) * a[i]; wb += double(b[i]) * b[i]; wab += double(a[i]) * b[i]; }
    if (wa > 1e-12 && wab / std::sqrt(wa * wb) < 0.5) { decor = (s / 2) / 48000.0; break; }
  }
  if (ndiff == 0) {
    std::printf("IDENTICAL (%zu samples)\n", n);
    return 0;
  }
  std::printf("differ: %zu/%zu samples, first at frame %zu (%.4f s), max|d|=%.3g, residual %.1f dBFS, "
              "signal %.1f dBFS, null depth %.1f dB, corr %.6f, decorrelated(<0.5) at %s%.2f s\n",
              ndiff, n, first / 2, (first / 2) / 48000.0, maxd, 20 * std::log10(rmsD + 1e-300),
              20 * std::log10(rmsA + 1e-300), 20 * std::log10((rmsD + 1e-300) / (rmsA + 1e-300)), corr,
              decor < 0 ? "never " : "", decor < 0 ? 0.0 : decor);
  return 0;
}
