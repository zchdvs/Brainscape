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
#elif defined(__arm__) && defined(__ARM_FP)
// __ARM_FP (ACLE: hardware FP available), NOT __VFP_FP__ — GCC defines the latter
// even for soft-float builds with no FPU, where the vmrs/vmsr below fail to
// assemble (review finding, verified with arm-none-eabi-gcc -mfloat-abi=soft).
#define BRAINSCAPE_DENORMAL_ARM32 1
#elif !defined(BRAINSCAPE_ALLOW_NO_DENORMAL_GUARD)
// A silent no-op guard would leave the audio path exposed to denormal stalls on
// exactly the target nobody tested. Opt in explicitly if that is genuinely fine
// (e.g. a soft-float utility build with no audio path).
#error "No denormal-guard implementation for this target; define BRAINSCAPE_ALLOW_NO_DENORMAL_GUARD to accept a no-op."
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
