#pragma once
#include <cstdint>

// Scoped flush-to-zero / denormals-are-zero (docs/design/grain-engine.md §9;
// docs/research/vst-and-shared-dsp.md rec #5). Constructed at the top of every
// top-level Process() call on both firmware and plugin — never assume the host's
// FP state, and always restore it on exit.

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#define BRAINSCAPE_DENORMAL_SSE 1
#include <immintrin.h>
#elif defined(__aarch64__)
#define BRAINSCAPE_DENORMAL_AARCH64 1
#elif defined(__arm__) && defined(__VFP_FP__)
#define BRAINSCAPE_DENORMAL_ARM32 1
#endif

namespace brainscape {

struct ScopedDenormalGuard {
#if defined(BRAINSCAPE_DENORMAL_SSE)
  // MXCSR: FTZ = bit 15 (0x8000), DAZ = bit 6 (0x0040)
  unsigned int saved_;
  ScopedDenormalGuard() noexcept : saved_(_mm_getcsr()) { _mm_setcsr(saved_ | 0x8040u); }
  ~ScopedDenormalGuard() noexcept { _mm_setcsr(saved_); }
#elif defined(BRAINSCAPE_DENORMAL_AARCH64)
  // FPCR: FZ = bit 24
  uint64_t saved_;
  ScopedDenormalGuard() noexcept {
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(saved_));
    const uint64_t v = saved_ | (1ull << 24);
    __asm__ __volatile__("msr fpcr, %0" ::"r"(v));
  }
  ~ScopedDenormalGuard() noexcept { __asm__ __volatile__("msr fpcr, %0" ::"r"(saved_)); }
#elif defined(BRAINSCAPE_DENORMAL_ARM32)
  // FPSCR: FZ = bit 24 (Cortex-M7 VFP)
  uint32_t saved_;
  ScopedDenormalGuard() noexcept {
    __asm__ __volatile__("vmrs %0, fpscr" : "=r"(saved_));
    const uint32_t v = saved_ | (1u << 24);
    __asm__ __volatile__("vmsr fpscr, %0" ::"r"(v));
  }
  ~ScopedDenormalGuard() noexcept { __asm__ __volatile__("vmsr fpscr, %0" ::"r"(saved_)); }
#else
  ScopedDenormalGuard() noexcept {}
  ~ScopedDenormalGuard() noexcept {}
#endif
  ScopedDenormalGuard(const ScopedDenormalGuard&) = delete;
  ScopedDenormalGuard& operator=(const ScopedDenormalGuard&) = delete;
};

}  // namespace brainscape
