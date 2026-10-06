// Generates the ONE stored battery input (interleaved stereo float32, 48 kHz, 30 s).
// Run once; every build then reads the same bytes, so libm differences in this
// generator can never leak into the comparison.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>
static const double kPi = 3.14159265358979323846;

int main(int argc, char** argv) {
  const char* out = argc > 1 ? argv[1] : "input.f32";
  const double sr = 48000.0;
  const size_t n  = static_cast<size_t>(30.0 * sr);
  std::vector<float> buf(2 * n);
  uint32_t x = 0x12345678u;
  auto rnd = [&]() {
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return static_cast<double>(x & 0xFFFFFF) / 8388608.0 - 1.0;
  };
  const double notes[8] = {110.0, 146.83, 196.0, 246.94, 164.81, 220.0, 293.66, 329.63};
  double lpL = 0, lpR = 0;
  for (size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / sr;
    double l = 0, r = 0;
    // Section A (0-12 s) and C (17-30 s): plucks every 0.5 s / 0.375 s.
    const bool silent = (t >= 12.0 && t < 13.5);
    if (!silent && (t < 12.0 || t >= 17.0)) {
      const double period = t < 12.0 ? 0.5 : 0.375;
      const double tn     = std::fmod(t, period);
      const int    idx    = static_cast<int>(t / period) % 8;
      const double f0     = notes[idx];
      const double env    = std::exp(-tn * 5.0);
      const double att    = tn < 0.002 ? 1.0 : std::exp(-(tn - 0.002) * 300.0);
      double v = 0;
      for (int h = 1; h <= 6; ++h) v += std::sin(2 * kPi * f0 * h * t + 0.3 * h) / (h * h * 0.6 + 0.4);
      const double nz = rnd();
      l = 0.3 * env * v + 0.35 * att * nz;
      double w = 0;
      for (int h = 1; h <= 6; ++h) w += std::sin(2 * kPi * f0 * 1.003 * h * t + 0.7 * h) / (h * h * 0.6 + 0.4);
      r = 0.28 * env * w + 0.3 * att * nz;
    }
    // Section B (13.5-17 s): sustained distorted chord + slow swell, no transients.
    if (t >= 13.5 && t < 17.0) {
      const double sw = (t - 13.5) / 3.5;
      double v = std::sin(2 * kPi * 82.41 * t) + std::sin(2 * kPi * 123.47 * t) + std::sin(2 * kPi * 164.81 * t);
      v = std::tanh(2.5 * v) * 0.4 * sw;
      l = v;
      r = 0.9 * v + 0.05 * std::sin(2 * kPi * 0.5 * t) * v;
    }
    // A low-passed noise bed throughout (-50 dBFS-ish) except during the silent gap.
    if (!silent) {
      lpL += 0.05 * (rnd() - lpL);
      lpR += 0.05 * (rnd() - lpR);
      l += 0.01 * lpL;
      r += 0.01 * lpR;
    }
    buf[2 * i]     = static_cast<float>(l);
    buf[2 * i + 1] = static_cast<float>(r);
  }
  FILE* f = std::fopen(out, "wb");
  if (!f) return 1;
  std::fwrite(buf.data(), sizeof(float), buf.size(), f);
  std::fclose(f);
  std::printf("wrote %s (%zu frames)\n", out, n);
  return 0;
}
