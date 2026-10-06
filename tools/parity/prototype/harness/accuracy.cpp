// Accuracy + cross-platform probe for DetMath.h over the engine's real argument sets.
//  accuracy <outdir> <tag> [<othertag> ...]
// Every build dumps, per test, the in-tree results and the platform-libm results for
// a deterministic (integer-generated) argument set. A GCC x86-64 build with
// __float128 also computes a quad-precision reference, prints max ulp errors for the
// in-tree functions and for its own libm, and — for every <othertag> dump found
// (e.g. the MSVC/UCRT build) — checks that the other build's in-tree results are
// bit-identical to its own and scores the other libm against the reference.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "brainscape/DetMath.h"

#if defined(__GNUC__) && !defined(__clang__) && defined(__x86_64__)
#include <quadmath.h>
#define HAVE_QUAD 1
typedef __float128 Q;
#endif

using namespace brainscape;

static std::string gDir, gTag;
static std::vector<std::string> gOthers;

static void Dump(const std::string& name, const void* p, size_t bytes) {
  const std::string path = gDir + "/" + gTag + "_" + name + ".bin";
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return;
  std::fwrite(p, 1, bytes, f);
  std::fclose(f);
}
static bool Load(const std::string& tag, const std::string& name, void* p, size_t bytes) {
  const std::string path = gDir + "/" + tag + "_" + name + ".bin";
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  const bool ok = std::fread(p, 1, bytes, f) == bytes;
  std::fclose(f);
  return ok;
}

static int64_t OrdF(float x) { int32_t i; std::memcpy(&i, &x, 4); return i < 0 ? int64_t(INT32_MIN) - i : i; }
static int64_t OrdD(double x) { int64_t i; std::memcpy(&i, &x, 8); return i < 0 ? INT64_MIN - i : i; }
static int64_t UlpF(float a, float b) { const int64_t d = OrdF(a) - OrdF(b); return d < 0 ? -d : d; }

#if HAVE_QUAD
// fractional ulp error of a float vs the exact value q (ulp of the float binade of q)
static double FracUlpF(float v, Q q) {
  if (q == 0) return v == 0 ? 0.0 : 1e9;
  Q aq = q < 0 ? -q : q;
  int e;
  frexpq(aq, &e);                 // aq = m * 2^e, m in [0.5,1)
  int ue = e - 24;                 // float ulp
  if (ue < -149) ue = -149;
  const Q ulp = ldexpq(1.0Q, ue);
  Q d = (Q)v - q;
  if (d < 0) d = -d;
  return (double)(d / ulp);
}
static double FracUlpD(double v, Q q) {
  if (q == 0) return v == 0 ? 0.0 : 1e9;
  Q aq = q < 0 ? -q : q;
  int e;
  frexpq(aq, &e);
  const Q ulp = ldexpq(1.0Q, e - 53);
  Q d = (Q)v - q;
  if (d < 0) d = -d;
  return (double)(d / ulp);
}
#endif

struct FloatTest {
  const char* name;
  std::vector<float> det, lib;
#if HAVE_QUAD
  std::vector<Q> ref;
#endif
};

static void Report(FloatTest& t) {
  const size_t n = t.det.size();
  Dump(std::string(t.name) + "_det", t.det.data(), n * 4);
  Dump(std::string(t.name) + "_lib", t.lib.data(), n * 4);
  size_t detVsLib = 0;
  int64_t maxDL = 0;
  for (size_t i = 0; i < n; ++i) {
    const int64_t u = UlpF(t.det[i], t.lib[i]);
    if (u) ++detVsLib;
    if (u > maxDL) maxDL = u;
  }
  std::printf("[%s] %-14s n=%zu  in-tree != own-libm: %zu (max %lld ulp)\n", gTag.c_str(), t.name, n,
              detVsLib, (long long)maxDL);
#if HAVE_QUAD
  double maxDet = 0, maxLib = 0;
  size_t detNotCR = 0, libNotCR = 0;
  for (size_t i = 0; i < n; ++i) {
    const float cr = (float)t.ref[i];
    const double ed = FracUlpF(t.det[i], t.ref[i]), el = FracUlpF(t.lib[i], t.ref[i]);
    if (ed > maxDet) maxDet = ed;
    if (el > maxLib) maxLib = el;
    if (std::memcmp(&cr, &t.det[i], 4)) ++detNotCR;
    if (std::memcmp(&cr, &t.lib[i], 4)) ++libNotCR;
  }
  std::printf("[%s] %-14s vs exact: in-tree max %.4f ulp (not-CR %zu) | glibc max %.4f ulp (not-CR %zu)\n",
              gTag.c_str(), t.name, maxDet, detNotCR, maxLib, libNotCR);
  for (const auto& other : gOthers) {
    std::vector<float> od(n), ol(n);
    if (!Load(other, std::string(t.name) + "_det", od.data(), n * 4) ||
        !Load(other, std::string(t.name) + "_lib", ol.data(), n * 4)) {
      std::printf("   (no dump for %s)\n", other.c_str());
      continue;
    }
    size_t detMismatch = 0, olNotCR = 0, olVsGlibc = 0;
    double maxOl = 0;
    for (size_t i = 0; i < n; ++i) {
      if (std::memcmp(&od[i], &t.det[i], 4)) ++detMismatch;
      const float cr = (float)t.ref[i];
      if (std::memcmp(&cr, &ol[i], 4)) ++olNotCR;
      if (std::memcmp(&ol[i], &t.lib[i], 4)) ++olVsGlibc;
      const double e = FracUlpF(ol[i], t.ref[i]);
      if (e > maxOl) maxOl = e;
    }
    std::printf("   vs %-10s in-tree bit-mismatch %zu | %s libm max %.4f ulp (not-CR %zu, != glibc %zu)\n",
                other.c_str(), detMismatch, other.c_str(), maxOl, olNotCR, olVsGlibc);
  }
#endif
}

// Double-kernel test: relative error in double ulps vs quad.
struct DoubleTest {
  const char* name;
  std::vector<double> det, lib;
#if HAVE_QUAD
  std::vector<Q> ref;
#endif
};
static void ReportD(DoubleTest& t) {
  const size_t n = t.det.size();
  Dump(std::string(t.name) + "_det", t.det.data(), n * 8);
  Dump(std::string(t.name) + "_lib", t.lib.data(), n * 8);
  size_t dl = 0;
  for (size_t i = 0; i < n; ++i) if (std::memcmp(&t.det[i], &t.lib[i], 8)) ++dl;
  std::printf("[%s] %-14s n=%zu  in-tree != own-libm (double bits): %zu\n", gTag.c_str(), t.name, n, dl);
#if HAVE_QUAD
  double md = 0, ml = 0;
  for (size_t i = 0; i < n; ++i) {
    const double a = FracUlpD(t.det[i], t.ref[i]), b = FracUlpD(t.lib[i], t.ref[i]);
    if (a > md) md = a;
    if (b > ml) ml = b;
  }
  std::printf("[%s] %-14s vs exact: in-tree max %.3f double-ulp | glibc max %.3f double-ulp\n",
              gTag.c_str(), t.name, md, ml);
  for (const auto& other : gOthers) {
    std::vector<double> od(n), ol(n);
    if (!Load(other, std::string(t.name) + "_det", od.data(), n * 8) ||
        !Load(other, std::string(t.name) + "_lib", ol.data(), n * 8)) continue;
    size_t mm = 0;
    double mo = 0;
    for (size_t i = 0; i < n; ++i) {
      if (std::memcmp(&od[i], &t.det[i], 8)) ++mm;
      const double e = FracUlpD(ol[i], t.ref[i]);
      if (e > mo) mo = e;
    }
    std::printf("   vs %-10s in-tree bit-mismatch %zu | %s libm max %.3f double-ulp\n", other.c_str(), mm,
                other.c_str(), mo);
  }
#endif
}

int main(int argc, char** argv) {
  if (argc < 3) return 1;
  gDir = argv[1];
  gTag = argv[2];
  for (int i = 3; i < argc; ++i) gOthers.push_back(argv[i]);

  // T1: SemitonesToRatio / OutTrim: exp2f over x = k * 2^-22, |x| <= 2 (st/12), plus
  // the trim mapping dB * 0.16609640474436813f for dB = k/1024 in [-24, 24].
  {
    FloatTest t{"exp2f"};
    auto add = [&](float x) {
      t.det.push_back(detmath::Exp2F(x));
      t.lib.push_back(std::exp2(x));
#if HAVE_QUAD
      t.ref.push_back(exp2q((Q)x));
#endif
    };
    for (int32_t k = -(1 << 23); k <= (1 << 23); ++k) add(static_cast<float>(k) * 0x1p-22f);
    for (int32_t k = -24 * 1024; k <= 24 * 1024; ++k) add((static_cast<float>(k) * 0x1p-10f) * 0.16609640474436813f);
    Report(t);
  }
  // T2: jitter draw logf(1 - u*0.999f), u = k * 2^-24 — EXHAUSTIVE over the RNG's range.
  {
    FloatTest t{"logf_jitter"};
    for (uint32_t k = 0; k < (1u << 24); ++k) {
      const float u = static_cast<float>(k) * (1.0f / 16777216.0f);
      const float m = u * 0.999f;
      const float x = 1.0f - m;
      t.det.push_back(detmath::LogF(x));
      t.lib.push_back(std::log(x));
#if HAVE_QUAD
      t.ref.push_back(logq((Q)x));
#endif
    }
    Report(t);
  }
  // T3/T4: equal-power pan / SVF morph weights: cos/sin(p * 1.5707963267948966f), p = k * 2^-22.
  {
    FloatTest tc{"cosf_pan"}, ts{"sinf_pan"};
    for (uint32_t k = 0; k <= (1u << 22); ++k) {
      const float p = static_cast<float>(k) * 0x1p-22f;
      const float x = p * 1.5707963267948966f;
      double s, c;
      detmath::SinCosD(static_cast<double>(x), &s, &c);
      tc.det.push_back(static_cast<float>(c));
      ts.det.push_back(static_cast<float>(s));
      tc.lib.push_back(std::cos(x));
      ts.lib.push_back(std::sin(x));
#if HAVE_QUAD
      tc.ref.push_back(cosq((Q)x));
      ts.ref.push_back(sinq((Q)x));
#endif
    }
    Report(tc);
    Report(ts);
  }
  // T5: normalization powf(target, -p): target = clamp(64*o^3, 1, 64), p = 1 - 0.5*dec.
  {
    FloatTest t{"powf_norm"};
    for (uint32_t i = 0; i <= 4096; ++i) {
      const float o = static_cast<float>(i) * (1.0f / 4096.0f);
      float tg = 64.0f * o;
      tg = tg * o;
      tg = tg * o;
      if (tg < 1.0f) tg = 1.0f;
      for (uint32_t j = 0; j <= 256; ++j) {
        const float dec = static_cast<float>(j) * (1.0f / 256.0f);
        const float h = 0.5f * dec;
        const float p = 1.0f - h;
        t.det.push_back(detmath::PowF(tg, -p));
        t.lib.push_back(std::pow(tg, -p));
#if HAVE_QUAD
        t.ref.push_back(powq((Q)tg, -(Q)p));
#endif
      }
    }
    Report(t);
  }
  // T6: SVF damping r^0.25 (sqrt(sqrt) in double) vs powf(r, 0.25f), r in [0, 0.995].
  {
    FloatTest t{"pow025_res"};
    for (uint32_t k = 0; k <= (1u << 20); ++k) {
      float r = static_cast<float>(k) * 0x1p-20f;
      if (r > 0.995f) r = 0.995f;
      t.det.push_back(static_cast<float>(detmath::SqrtD(detmath::SqrtD(static_cast<double>(r)))));
      t.lib.push_back(std::pow(r, 0.25f));
#if HAVE_QUAD
      t.ref.push_back(sqrtq(sqrtq((Q)r)));
#endif
    }
    Report(t);
  }
  // T7: one-pole coefficients -expm1(-2*pi*fc/sr) and smoother taus, as floats.
  {
    FloatTest t{"lpcoef_f"};
    DoubleTest d{"expm1_d"};
    for (uint32_t k = 0; k <= (1u << 20); ++k) {
      const double x = -1.5 * static_cast<double>(k) / static_cast<double>(1u << 20);
      const double a = detmath::Expm1D(x), b = std::expm1(x);
      d.det.push_back(a);
      d.lib.push_back(b);
      t.det.push_back(-static_cast<float>(a));
      t.lib.push_back(-static_cast<float>(b));
#if HAVE_QUAD
      d.ref.push_back(expm1q((Q)x));
      t.ref.push_back(-expm1q((Q)x));
#endif
    }
    ReportD(d);
    Report(t);
  }
  // T8: double sin kernel for the SVF frequency: sin(pi * f), f in [0, 0.25] (as written: 3.14159265358979 * f).
  {
    DoubleTest d{"sin_svf_d"};
    for (uint32_t k = 0; k <= (1u << 20); ++k) {
      const double f = 0.25 * static_cast<double>(k) / static_cast<double>(1u << 20);
      const double x = 3.14159265358979 * f;
      d.det.push_back(detmath::SinD(x));
      d.lib.push_back(std::sin(x));
#if HAVE_QUAD
      d.ref.push_back(sinq((Q)x));
#endif
    }
    ReportD(d);
  }
  // T9: tables — window LUT (4096), Hann (512), twiddles (256 cos + 256 sin), as built.
  {
    FloatTest w{"table_window"}, h{"table_hann"}, tw{"table_twiddle"};
    for (uint32_t i = 0; i < 4096; ++i) {
      const double x = static_cast<double>(i) / 4095.0;
      const double c = detmath::CosPi(x);
      const double om = 1.0 - c;
      w.det.push_back(static_cast<float>(0.5 * om));
      w.lib.push_back(static_cast<float>(0.5 * (1.0 - std::cos(3.14159265358979323846 * x))));
#if HAVE_QUAD
      w.ref.push_back(0.5Q * (1.0Q - cosq(M_PIq * (Q)x)));
#endif
    }
    for (uint32_t i = 0; i < 512; ++i) {
      const double c = detmath::CosPi(static_cast<double>(2u * i) / 512);
      const double om = 1.0 - c;
      h.det.push_back(static_cast<float>(0.5 * om));
      h.lib.push_back(static_cast<float>(0.5 * (1.0 - std::cos(2.0 * 3.14159265358979323846 * static_cast<double>(i) / 512))));
#if HAVE_QUAD
      h.ref.push_back(0.5Q * (1.0Q - cosq(2 * M_PIq * (Q)i / 512)));
#endif
    }
    for (uint32_t k = 0; k < 256; ++k) {
      double s, c;
      detmath::SinCosPi(static_cast<double>(2u * k) / 512, &s, &c);
      tw.det.push_back(static_cast<float>(c));
      tw.det.push_back(static_cast<float>(-s));
      tw.lib.push_back(static_cast<float>(std::cos(2.0 * 3.14159265358979323846 * k / 512)));
      tw.lib.push_back(static_cast<float>(-std::sin(2.0 * 3.14159265358979323846 * k / 512)));
#if HAVE_QUAD
      tw.ref.push_back(cosq(2 * M_PIq * (Q)k / 512));
      tw.ref.push_back(-sinq(2 * M_PIq * (Q)k / 512));
#endif
    }
    Report(w);
    Report(h);
    Report(tw);
  }
  // T10: lround/llround replacement equality on halves, near-halves and random values.
  {
    size_t mism = 0, n = 0;
    uint32_t x = 1u;
    for (int32_t k = -200000; k <= 200000; ++k) {
      const double base = static_cast<double>(k) * 0.5;
      const double vals[3] = {base, std::nextafter(base, -1e9), std::nextafter(base, 1e9)};
      for (double v : vals) {
        ++n;
        if (detmath::RoundHalfAwayI32(v) != static_cast<int32_t>(std::lround(v))) ++mism;
        if (detmath::RoundHalfAwayI64(v * 4096.0) != std::llround(v * 4096.0)) ++mism;
      }
      x ^= x << 13; x ^= x >> 17; x ^= x << 5;
      const double r = (static_cast<double>(x) - 2147483648.0) / 1024.0;
      ++n;
      if (detmath::RoundHalfAwayI32(r) != static_cast<int32_t>(std::lround(r))) ++mism;
    }
    std::printf("[%s] lround/llround equality: %zu mismatches over %zu cases\n", gTag.c_str(), mism, n);
  }
  return 0;
}
