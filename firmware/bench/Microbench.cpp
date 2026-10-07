#include "bench/Microbench.h"

#include "detail/FlushTiny.h"  // the engine's own flush (determinism-profile.md §4.3)
#include "platform/Platform.h"
#include "stm32h7xx.h"

namespace brainscape::fw::bench {

namespace {

// Copies of one instruction.
#define BS_X8(i) i i i i i i i i
#define BS_X16(i) BS_X8(i) BS_X8(i)

// One timed loop: s14 = x, s15 = a, s16 = b, PRE before the clock starts; CYCCNT read before
// the loop and after the chain's last result is moved out (so the read waits for it).
#define BS_TIMED_LOOP(PRE, BODY, POST)                                    \
  __asm__ volatile(                                                       \
      "vmov s14, %[x]\n\t"                                                \
      "vmov s15, %[a]\n\t"                                                \
      "vmov s16, %[b]\n\t" PRE                                            \
      "dsb\n\t"                                                           \
      "ldr %[t0], [%[cyc]]\n\t"                                           \
      "1:\n\t" BODY                                                       \
      "subs %[n], %[n], #1\n\t"                                           \
      "bne 1b\n\t" POST                                                   \
      "vmov %[r], s14\n\t"                                                \
      "ldr %[t1], [%[cyc]]\n\t"                                           \
      : [t0] "=&r"(t0), [t1] "=&r"(t1), [n] "+r"(n), [r] "=&r"(result)    \
      : [x] "r"(x), [a] "r"(a), [b] "r"(b), [cyc] "r"(cyc)                \
      : "s14", "s15", "s16", "d4", "d5", "cc", "memory")

uint32_t Chain(FpOp op, uint32_t x, uint32_t a, uint32_t b, uint32_t n) {
  volatile uint32_t* const cyc = &DWT->CYCCNT;
  uint32_t                 t0 = 0, t1 = 0, result = 0;
  switch (op) {
    case FpOp::Mul: BS_TIMED_LOOP("", BS_X16("vmul.f32 s14, s14, s15\n\t"), ""); break;
    case FpOp::Add: BS_TIMED_LOOP("", BS_X16("vadd.f32 s14, s14, s15\n\t"), ""); break;
    case FpOp::Div: BS_TIMED_LOOP("", BS_X16("vdiv.f32 s14, s14, s15\n\t"), ""); break;
    // sqrt of a subnormal is normal, so the operand is the fixed s15, not the chain.
    case FpOp::Sqrt: BS_TIMED_LOOP("", BS_X16("vsqrt.f32 s14, s15\n\t"), ""); break;
    case FpOp::MulPair:
      BS_TIMED_LOOP("", BS_X8("vmul.f32 s14, s14, s15\n\tvmul.f32 s14, s14, s16\n\t"), "");
      break;
    case FpOp::MulF64:
      BS_TIMED_LOOP("vcvt.f64.f32 d4, s14\n\tvcvt.f64.f32 d5, s15\n\t",
                    BS_X16("vmul.f64 d4, d4, d5\n\t"), "vcvt.f32.f64 s14, d4\n\t");
      break;
    case FpOp::AddPair:
      BS_TIMED_LOOP("", BS_X8("vadd.f32 s14, s14, s15\n\tvsub.f32 s14, s14, s15\n\t"), "");
      break;
  }
  (void)result;
  return t1 - t0;
}

#undef BS_TIMED_LOOP
#undef BS_X16
#undef BS_X8

// Eight independent one-poles, s = s * k + c (normal values throughout), with or without a
// flush after each update. noinline and volatile coefficients keep the loop as written.
template <FlushForm kForm>
__attribute__((noinline)) uint32_t FlushLoop(uint32_t n) {
  volatile float vk = 0.999f, vc = 0.001f;
  const float    k = vk, c = vc;
  float          s[8] = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f};
  const uint32_t t0   = DWT->CYCCNT;
  for (uint32_t i = 0; i < n; ++i) {
    for (float& v : s) {
      v = v * k + c;
      if (kForm == FlushForm::BitTest) detail::FlushTiny(v);
      // The two-compare form of the profile's first draft (|x| < 1e-20f).
      if (kForm == FlushForm::Compare && v < 0x1.79ca10p-67f && v > -0x1.79ca10p-67f) v = 0.f;
    }
    __asm__ volatile("" : : : "memory");
  }
  const uint32_t t1   = DWT->CYCCNT;
  volatile float sink = s[0] + s[1] + s[2] + s[3] + s[4] + s[5] + s[6] + s[7];
  (void)sink;
  return t1 - t0;
}

// One one-pole, s = s * k + c, with the flush on the recursion: every step waits for the
// previous step's flushed value (the engine's tamer, SVF and reverb states).
template <FlushForm kForm>
__attribute__((noinline)) uint32_t FlushChain(uint32_t n) {
  volatile float vk = 0.999f, vc = 0.001f, v0 = 0.5f;
  const float    k = vk, c = vc;
  float          v  = v0;
  const uint32_t t0 = DWT->CYCCNT;
  for (uint32_t i = 0; i < n; ++i) {
    v = v * k + c;
    if (kForm == FlushForm::BitTest) detail::FlushTiny(v);
    if (kForm == FlushForm::Compare && v < 0x1.79ca10p-67f && v > -0x1.79ca10p-67f) v = 0.f;
    __asm__ volatile("" : "+t"(v) : : "memory");
  }
  const uint32_t t1   = DWT->CYCCNT;
  volatile float sink = v;
  (void)sink;
  return t1 - t0;
}

}  // namespace

uint32_t RunFpChain(FpOp op, uint32_t xBits, uint32_t aBits, uint32_t bBits, uint32_t fpscr,
                    uint32_t iterations) {
  __disable_irq();
  const uint32_t saved = ReadFpscr();
  WriteFpscr(fpscr);
  const uint32_t cycles = Chain(op, xBits, aBits, bBits, iterations);
  WriteFpscr(saved);
  __enable_irq();
  return cycles;
}

uint32_t RunFlushLoop(FlushForm form, uint32_t iterations, uint32_t chains) {
  __disable_irq();
  uint32_t cycles = 0;
  if (chains == 1u) {
    switch (form) {
      case FlushForm::None: cycles = FlushChain<FlushForm::None>(iterations); break;
      case FlushForm::BitTest: cycles = FlushChain<FlushForm::BitTest>(iterations); break;
      case FlushForm::Compare: cycles = FlushChain<FlushForm::Compare>(iterations); break;
    }
  } else {
    switch (form) {
      case FlushForm::None: cycles = FlushLoop<FlushForm::None>(iterations); break;
      case FlushForm::BitTest: cycles = FlushLoop<FlushForm::BitTest>(iterations); break;
      case FlushForm::Compare: cycles = FlushLoop<FlushForm::Compare>(iterations); break;
    }
  }
  __enable_irq();
  return cycles;
}

}  // namespace brainscape::fw::bench
