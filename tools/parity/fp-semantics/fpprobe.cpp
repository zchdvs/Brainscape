// fp-isa lens: x86-64 probes for cross-ISA floating-point semantics.
// Build (MSVC):  cl /O2 /fp:precise /arch:AVX2 /EHsc fpprobe.cpp
// Build (GCC):   g++ -O2 -mavx2 -mfma -std=c++17 fpprobe.cpp
// Sections: [1] FTZ tininess boundary (x86 after-rounding vs Arm FZ before-rounding)
//           [2] constant folding vs runtime under FTZ
//           [3] default NaN / NaN propagation bits
//           [4] float->int conversion of NaN / out-of-range (runtime vs folded)
//           [5] uint32->float conversion exhaustive check (vectorizable loop)
//           [6] subnormal cost (dependent chain), FTZ/DAZ off vs on
//           [7] one-pole with zero input: stuck subnormal state in IEEE mode
#include <immintrin.h>

#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

static uint32_t Bits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
static float FromBits(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }

static const unsigned kDefaultCsr = 0x1F80u;  // all exceptions masked, RN, FTZ=0, DAZ=0
static void SetCsr(unsigned v) { _mm_setcsr(v); }

// Opaque multiply so the optimizer cannot fold it (the hardware does it at run time).
#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
static float HwMul(float a, float b) {
  __m128 r = _mm_mul_ss(_mm_set_ss(a), _mm_set_ss(b));
  return _mm_cvtss_f32(r);
}

// Exact product of two binary32 values fits in binary64 (24+24 <= 53 bits).
static double ExactMul(float a, float b) { return static_cast<double>(a) * static_cast<double>(b); }

// Arm FPSCR.FZ / FPCR.FZ (AH=0) model: tiny is decided BEFORE rounding on the exact value.
static float ArmFzMul(float a, float b) {
  if (std::fpclassify(a) == FP_SUBNORMAL) a = std::copysign(0.f, a);  // input flush
  if (std::fpclassify(b) == FP_SUBNORMAL) b = std::copysign(0.f, b);
  const double p = ExactMul(a, b);
  if (p != 0.0 && std::fabs(p) < static_cast<double>(std::numeric_limits<float>::min()))
    return static_cast<float>(std::copysign(0.0, p));
  return static_cast<float>(p);  // round once (default env, FTZ off in this function's caller)
}

// x86 MXCSR.FTZ model: tiny decided AFTER rounding with unbounded exponent (SDM).
// Rounding with unbounded exponent == round the value scaled up by 2^64 (exact scaling).
static float X86FtzMul(float a, float b) {
  if (std::fpclassify(a) == FP_SUBNORMAL) a = std::copysign(0.f, a);  // DAZ
  if (std::fpclassify(b) == FP_SUBNORMAL) b = std::copysign(0.f, b);
  const double p = ExactMul(a, b);
  const double scaled = p * 18446744073709551616.0;  // 2^64, exact
  const float  ru     = static_cast<float>(scaled);   // 24-bit rounding, normal range
  if (ru != 0.f && std::fabs(static_cast<double>(ru)) <
                       static_cast<double>(std::numeric_limits<float>::min()) * 18446744073709551616.0)
    return static_cast<float>(std::copysign(0.0, p));
  return static_cast<float>(p);
}

static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static uint32_t Rand32() {
  g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
  return static_cast<uint32_t>(g_rng >> 16);
}

int main() {
  std::printf("== fp-isa x86 probe (%s)\n",
#if defined(_MSC_VER) && !defined(__clang__)
              "MSVC"
#elif defined(__clang__)
              "Clang"
#else
              "GCC"
#endif
  );
  std::printf("FLT_EVAL_METHOD=%d sizeof(long)=%zu sizeof(size_t)=%zu char_signed=%d\n",
              (int)FLT_EVAL_METHOD, sizeof(long), sizeof(size_t), (int)std::numeric_limits<char>::is_signed);

  // ---------------------------------------------------------------- [1]
  std::printf("\n[1] FTZ tininess boundary: a=0x3F7FFFFE (1-2^-23), b=0x00800001 (FLT_MIN*(1+2^-23))\n");
  const float a = FromBits(0x3F7FFFFEu), b = FromBits(0x00800001u);
  std::printf("    exact product = FLT_MIN*(1-2^-46) = %.17g (FLT_MIN=%.17g)\n", ExactMul(a, b),
              (double)std::numeric_limits<float>::min());
  const struct { const char* name; unsigned csr; } modes[] = {
      {"IEEE (FTZ=0,DAZ=0)", kDefaultCsr},
      {"FTZ=1,DAZ=0      ", kDefaultCsr | 0x8000u},
      {"FTZ=0,DAZ=1      ", kDefaultCsr | 0x0040u},
      {"FTZ=1,DAZ=1 (repo guard)", kDefaultCsr | 0x8040u},
  };
  for (auto& m : modes) {
    SetCsr(m.csr);
    const float r = HwMul(a, b);
    const unsigned flags = _mm_getcsr() & 0x3Fu;
    SetCsr(kDefaultCsr);
    std::printf("    x86 hw %-26s -> 0x%08X  (MXCSR flags 0x%02X)\n", m.name, Bits(r), flags);
  }
  std::printf("    model Arm FZ=1 (before rounding)   -> 0x%08X\n", Bits(ArmFzMul(a, b)));
  std::printf("    model x86 FTZ (after rounding)     -> 0x%08X\n", Bits(X86FtzMul(a, b)));

  // Sweep: random operands whose exact product lands within a few ULP of FLT_MIN.
  {
    uint64_t n = 0, hwVsX86Model = 0, armVsX86 = 0, armVsIeee = 0, x86FtzVsIeee = 0, inBand = 0;
    uint64_t bandAndDiffer = 0;
    const uint64_t kIters = 20000000;
    for (uint64_t i = 0; i < kIters; ++i) {
      // a in [0.5,1): random mantissa; b normal, chosen so a*b ~ FLT_MIN * (1 +- small)
      const float fa = FromBits(0x3F000000u | (Rand32() & 0x7FFFFFu));
      float target   = std::numeric_limits<float>::min() / fa;  // ~(2^-126, 2^-125]
      uint32_t tb    = Bits(target);
      tb += static_cast<uint32_t>(static_cast<int32_t>(Rand32() % 9) - 4);  // +-4 ulp
      const float fb = FromBits(tb);
      if (std::fpclassify(fb) == FP_SUBNORMAL) continue;  // keep inputs normal here
      ++n;
      SetCsr(kDefaultCsr | 0x8040u);
      const float hwFtz = HwMul(fa, fb);
      SetCsr(kDefaultCsr);
      const float hwIeee = HwMul(fa, fb);
      const float armFz  = ArmFzMul(fa, fb);
      const float x86m   = X86FtzMul(fa, fb);
      if (Bits(hwFtz) != Bits(x86m)) ++hwVsX86Model;
      if (Bits(armFz) != Bits(hwFtz)) ++armVsX86;
      if (Bits(armFz) != Bits(hwIeee)) ++armVsIeee;
      if (Bits(hwFtz) != Bits(hwIeee)) ++x86FtzVsIeee;
      const double p = std::fabs(ExactMul(fa, fb));
      const double fm = std::numeric_limits<float>::min();
      const bool band = p < fm && p >= fm - std::ldexp(1.0, -151);  // [FLT_MIN - 2^-151, FLT_MIN)
      if (band) ++inBand;
      if (band && Bits(armFz) != Bits(hwFtz)) ++bandAndDiffer;
    }
    std::printf("    sweep n=%" PRIu64 ": hw-FTZ vs after-rounding model mismatches=%" PRIu64
                "; ArmFZ-model vs x86-FTZ differ=%" PRIu64 " (all in band: %" PRIu64 " of %" PRIu64
                " band hits); ArmFZ vs IEEE differ=%" PRIu64 "; x86FTZ vs IEEE differ=%" PRIu64 "\n",
                n, hwVsX86Model, armVsX86, bandAndDiffer, inBand, armVsIeee, x86FtzVsIeee);
  }

  // ---------------------------------------------------------------- [2]
  std::printf("\n[2] constant folding vs run time under FTZ|DAZ\n");
  {
    volatile float vk = 1.0e-38f;  // normal (FLT_MIN = 1.1755e-38)
    SetCsr(kDefaultCsr | 0x8040u);
    const float runtime = vk * 0.5f;          // executed with FTZ -> 0
    const float folded  = 1.0e-38f * 0.5f;    // folded by the compiler in IEEE semantics
    volatile float sink = folded;
    const float folded_used = sink * 1.0f;    // DAZ: subnormal input flushed when USED at run time
    SetCsr(kDefaultCsr);
    std::printf("    runtime 1e-38f*0.5f under FTZ = 0x%08X; folded constant = 0x%08X; folded value "
                "after one runtime *1.0f under DAZ = 0x%08X\n",
                Bits(runtime), Bits(folded), Bits(folded_used));
  }

  // ---------------------------------------------------------------- [3]
  std::printf("\n[3] NaN generation / propagation (x86 SSE)\n");
  {
    volatile float zero = 0.f, inf = std::numeric_limits<float>::infinity(), neg1 = -1.f;
    volatile float qn1 = FromBits(0x7FC12345u), qn2 = FromBits(0xFFC54321u);
    std::printf("    0*inf=0x%08X  inf-inf=0x%08X  sqrt(-1)=0x%08X  (x86 'QNaN indefinite' expected 0xFFC00000;"
                " Arm default NaN is 0x7FC00000)\n",
                Bits(zero * inf), Bits(inf - inf), Bits(std::sqrt((float)neg1)));
    std::printf("    qNaN(0x7FC12345)*1=0x%08X  1*qNaN=0x%08X  qn1+qn2=0x%08X  qn2+qn1=0x%08X\n",
                Bits(qn1 * 1.f), Bits(1.f * qn1), Bits(qn1 + qn2), Bits(qn2 + qn1));
  }

  // ---------------------------------------------------------------- [4]
  std::printf("\n[4] float->int conversions (run time; out-of-range is UB in C++)\n");
  {
    volatile float vals[] = {std::numeric_limits<float>::quiet_NaN(), 3.0e9f, -3.0e9f, 5.0e9f, -1.0f,
                             std::numeric_limits<float>::infinity(), 32767.5f, -32767.5f};
    for (float v : vals) {
      const float x = v;
      const int32_t  i32 = static_cast<int32_t>(x);
      const uint32_t u32 = static_cast<uint32_t>(x);
      const int16_t  i16 = static_cast<int16_t>(x);
      std::printf("    %-12g -> int32 0x%08X  uint32 0x%08X  int16 0x%04X\n", (double)x, (uint32_t)i32, u32,
                  (uint16_t)i16);
    }
    // Arm VCVT (FPToFixed) saturates: NaN->0, +big->INT_MAX/UINT_MAX, -big->INT_MIN/0.
    // Compile-time fold of the same UB expression:
    const int32_t foldedBig = static_cast<int32_t>(3.0e9f);
    std::printf("    folded static_cast<int32_t>(3.0e9f) = 0x%08X (run time above)\n", (uint32_t)foldedBig);
  }

  // ---------------------------------------------------------------- [5]
  std::printf("\n[5] uint32->float: vectorizable loop vs correctly-rounded reference, all 2^32 inputs\n");
  {
    const uint32_t kChunk = 1u << 20;
    std::vector<uint32_t> src(kChunk);
    std::vector<float> dst(kChunk);
    uint64_t bad = 0;
    for (uint64_t base = 0; base < (1ull << 32); base += kChunk) {
      for (uint32_t i = 0; i < kChunk; ++i) src[i] = static_cast<uint32_t>(base + i);
      for (uint32_t i = 0; i < kChunk; ++i) dst[i] = static_cast<float>(src[i]);  // vectorized
      for (uint32_t i = 0; i < kChunk; ++i) {
        const float ref = static_cast<float>(static_cast<double>(src[i]));  // exact -> one rounding
        if (Bits(ref) != Bits(dst[i])) ++bad;
      }
    }
    std::printf("    mismatches = %" PRIu64 "\n", bad);
  }

  // ---------------------------------------------------------------- [6]
  std::printf("\n[6] subnormal cost on this CPU (dependent chain x = x*c + d)\n");
  {
    auto run = [](float x0, float c, float d, unsigned csr) {
      SetCsr(csr);
      volatile float vx0 = x0, vc = c, vd = d;
      float x = vx0, cc = vc, dd = vd;
      const int N = 20000000;
      auto t0 = std::chrono::high_resolution_clock::now();
      for (int i = 0; i < N; ++i) x = x * cc + dd;  // no contraction under default flags
      auto t1 = std::chrono::high_resolution_clock::now();
      SetCsr(kDefaultCsr);
      volatile float sink = x; (void)sink;
      return std::chrono::duration<double, std::nano>(t1 - t0).count() / N;
    };
    const float sub = FromBits(0x00012345u);
    std::printf("    normal operands, IEEE          : %6.2f ns/iter\n", run(1.0e-3f, 1.0f, 0.0f, kDefaultCsr));
    std::printf("    subnormal x (x*1+0), IEEE      : %6.2f ns/iter\n", run(sub, 1.0f, 0.0f, kDefaultCsr));
    std::printf("    subnormal x, FTZ|DAZ           : %6.2f ns/iter\n", run(sub, 1.0f, 0.0f, kDefaultCsr | 0x8040u));
    std::printf("    normal->subnormal results (x*0.5 from 1e-37, stays>0 then 0), IEEE: %6.2f ns/iter\n",
                run(1.0e-37f, 0.999999f, 0.0f, kDefaultCsr));
  }

  // ---------------------------------------------------------------- [7]
  std::printf("\n[7] one-pole lp += c*(0 - lp) from 1.0, c = expm1-based 8 Hz @48k (FeedbackTamer dc)\n");
  {
    const float c = -static_cast<float>(std::expm1(-6.283185307179586 * 8.0 / 48000.0));
    for (unsigned csr : {kDefaultCsr, kDefaultCsr | 0x8040u}) {
      SetCsr(csr);
      volatile float vc = c;
      float lp = 1.0f, cc = vc;
      long long firstSub = -1, firstZero = -1, stuckAt = -1;
      float prev = lp;
      for (long long n = 0; n < 2000000; ++n) {
        lp += cc * (0.0f - lp);
        if (firstSub < 0 && lp != 0.f && std::fabs(lp) < std::numeric_limits<float>::min()) firstSub = n;
        if (firstZero < 0 && lp == 0.f) firstZero = n;
        if (stuckAt < 0 && lp == prev && lp != 0.f) stuckAt = n;
        prev = lp;
      }
      SetCsr(kDefaultCsr);
      std::printf("    %s: c=%.9g first subnormal at n=%lld, first zero at n=%lld, stuck (nonzero fixed point) "
                  "at n=%lld, final lp=0x%08X\n",
                  csr == kDefaultCsr ? "IEEE    " : "FTZ|DAZ ", (double)c, firstSub, firstZero, stuckAt,
                  Bits(lp));
    }
  }
  // ---------------------------------------------------------------- [8]
  // Smoother::Next (Smoother.h:18-22) ramping to target 0 under FTZ: on which sample does the
  // stall-snap fire? x86 = real hardware with FTZ|DAZ; Arm = before-rounding model.
  std::printf("\n[8] Smoother ramp-to-0 snap sample under FTZ: x86 hw vs Arm FZ model\n");
  {
    const float coef = -static_cast<float>(std::expm1(-1.0 / (10.0f * 0.001 * 48000.0)));  // SetTau(10, 48k)
    int diverged = 0, tested = 0;
    for (uint32_t k = 1; k <= 16384; ++k) {
      const float start = static_cast<float>(k) / 16384.0f;  // a 14-bit knob grid in (0,1]
      // x86 hardware run
      SetCsr(kDefaultCsr | 0x8040u);
      float v = start; long long snapX = -1;
      volatile float vcoef = coef; const float c = vcoef;
      for (long long n = 0; n < 200000; ++n) {
        const float next = v + HwMul(c, 0.0f - v);
        v = (next == v) ? 0.0f : next;
        if (v == 0.0f) { snapX = n; break; }
      }
      SetCsr(kDefaultCsr);
      // Arm model run (additions of normals cannot land in the band, so only the product is modelled)
      float w = start; long long snapA = -1;
      for (long long n = 0; n < 200000; ++n) {
        float prod = ArmFzMul(c, 0.0f - w);
        float next = w + prod;  // default env: exact-or-normal sums; flush subnormal sums like FZ
        if (next != 0.f && std::fabs(next) < std::numeric_limits<float>::min()) next = std::copysign(0.f, next);
        w = (next == w) ? 0.0f : next;
        if (w == 0.0f) { snapA = n; break; }
      }
      ++tested;
      if (snapX != snapA) {
        if (diverged < 5)
          std::printf("    start=%.9g (k=%u): x86 snaps at n=%lld, Arm-model snaps at n=%lld\n", (double)start, k,
                      snapX, snapA);
        ++diverged;
      }
    }
    std::printf("    %d of %d knob start values snap on a different sample\n", diverged, tested);
  }
  return 0;
}
