#pragma once
#include <cstdint>

// Floating-point environment guard (docs/design/determinism-profile.md §4.1). Every
// engine entry point that runs floating-point code saves the calling thread's control
// word, writes the COMPLETE word the profile requires, and restores the caller's word
// on exit:
//   x86-64     MXCSR = 0x1F80  round to nearest, FTZ = DAZ = 0, exceptions masked,
//                              flags clear
//   AArch64    FPCR  = 0       round to nearest; FZ, FZ16, DN, AH, FIZ, NEP, AHP clear;
//                              traps off
//   Cortex-M7  FPSCR = 0       round to nearest, FZ = DN = AHP = 0, flags clear
// A whole-word write neutralizes whatever the host left behind: a rounding mode (a host
// thread in round-toward-zero changed every golden hash), flush bits (JUCE's
// ScopedNoDenormals, host FTZ) or FPCR.AH. ORing bits into the word, as the old
// denormal guard did, cannot clear what others set. Subnormals are kept (gradual
// underflow, §4.2); recursive state is flushed in code instead (FlushTiny.h).
//
// Ordering: compilers may move floating-point arithmetic across a control-register
// write, even an asm volatile with a "memory" clobber, because the arithmetic has no
// memory dependence on it. So an entry point constructs the guard and does no
// floating-point arithmetic of its own; all of its work sits in a separate
// BRAINSCAPE_FP_BODY function, which the compiler cannot inline into it.

// Branch order (§4.1): ARM64EC also defines _M_X64, so the Arm branches come first.
#if defined(_M_ARM64EC)
#error "brainscape determinism profile: ARM64EC is out of profile; its emulated MXCSR may not reach FPCR (determinism-profile.md section 3.8)"
#elif defined(_M_ARM64)
#error "brainscape determinism profile: native Windows ARM64 is not a v1 target (determinism-profile.md sections 3.8, 4.1)"
#elif defined(__aarch64__)
#define BRAINSCAPE_FPENV_AARCH64 1
#elif defined(__arm__) && defined(__ARM_FP)
// __ARM_FP (ACLE: hardware FP available), NOT __VFP_FP__ — GCC defines the latter
// even for soft-float builds with no FPU, where vmrs/vmsr fail to assemble.
#define BRAINSCAPE_FPENV_ARM32 1
#elif defined(__x86_64__) || defined(_M_X64)
#define BRAINSCAPE_FPENV_X64 1
#if defined(_MSC_VER) && !defined(__clang__)
#include <atomic>
#include <xmmintrin.h>
#endif
#else
#error "brainscape determinism profile: no floating-point environment guard for this target"
#endif

#if defined(_MSC_VER)
#define BRAINSCAPE_FP_BODY __declspec(noinline)
#elif defined(__has_attribute)
#if __has_attribute(noipa)
// GCC: noinline alone still allows partial inlining and IPA clones.
#define BRAINSCAPE_FP_BODY __attribute__((noipa))
#else
#define BRAINSCAPE_FP_BODY __attribute__((noinline))
#endif
#else
#define BRAINSCAPE_FP_BODY __attribute__((noinline))
#endif

namespace brainscape::detail {

#if defined(BRAINSCAPE_FPENV_X64)
using FpWord = uint32_t;
inline constexpr FpWord kFpProfileWord = 0x1F80u;
inline constexpr FpWord kFpFlushBits   = 0x8040u;  // FTZ | DAZ
inline constexpr FpWord kFpFlagBits    = 0x003Fu;  // IE DE ZE OE UE PE

inline FpWord ReadFpControl() noexcept {
#if defined(_MSC_VER) && !defined(__clang__)
  std::atomic_signal_fence(std::memory_order_seq_cst);
  const FpWord w = _mm_getcsr();
  std::atomic_signal_fence(std::memory_order_seq_cst);
  return w;
#else
  FpWord w;
  __asm__ __volatile__("stmxcsr %0" : "=m"(w) : : "memory");
  return w;
#endif
}
inline void WriteFpControl(FpWord w) noexcept {
#if defined(_MSC_VER) && !defined(__clang__)
  std::atomic_signal_fence(std::memory_order_seq_cst);
  _mm_setcsr(w);
  std::atomic_signal_fence(std::memory_order_seq_cst);
#else
  __asm__ __volatile__("ldmxcsr %0" : : "m"(w) : "memory");
#endif
}
#elif defined(BRAINSCAPE_FPENV_AARCH64)
using FpWord = uint64_t;
inline constexpr FpWord kFpProfileWord = 0;
inline constexpr FpWord kFpFlushBits   = FpWord{1} << 24;  // FZ
inline constexpr FpWord kFpFlagBits    = 0x9Fu;            // FPSR: IOC DZC OFC UFC IXC IDC

inline FpWord ReadFpControl() noexcept {
  FpWord w;
  __asm__ __volatile__("mrs %0, fpcr" : "=r"(w) : : "memory");
  return w;
}
inline void WriteFpControl(FpWord w) noexcept {
  __asm__ __volatile__("msr fpcr, %0" : : "r"(w) : "memory");
}
#elif defined(BRAINSCAPE_FPENV_ARM32)
using FpWord = uint32_t;
inline constexpr FpWord kFpProfileWord = 0;
inline constexpr FpWord kFpFlushBits   = FpWord{1} << 24;  // FZ
inline constexpr FpWord kFpFlagBits    = 0x9Fu;            // IOC DZC OFC UFC IXC IDC

inline FpWord ReadFpControl() noexcept {
  FpWord w;
  __asm__ __volatile__("vmrs %0, fpscr" : "=r"(w) : : "memory");
  return w;
}
inline void WriteFpControl(FpWord w) noexcept {
  __asm__ __volatile__("vmsr fpscr, %0" : : "r"(w) : "memory");
}
#endif

#if defined(BRAINSCAPE_FPENV_TEST_HOOKS)
// Test-only build of the engine (determinism profile §6.4): the guard can force
// flushing on, and it collects the exception flags each guarded call raised. Never
// part of a shipping build, which has no mutable global state.
namespace fpenv_test {
inline bool   forceFlush = false;
inline FpWord flags      = 0;
}  // namespace fpenv_test
#endif

class FpEnvGuard {
 public:
  FpEnvGuard() noexcept : saved_(ReadFpControl()) {
#if defined(BRAINSCAPE_FPENV_TEST_HOOKS)
#if defined(BRAINSCAPE_FPENV_AARCH64)
    __asm__ __volatile__("msr fpsr, xzr" : : : "memory");
#endif
    WriteFpControl(fpenv_test::forceFlush ? (kFpProfileWord | kFpFlushBits) : kFpProfileWord);
#else
    WriteFpControl(kFpProfileWord);
#endif
  }
  ~FpEnvGuard() noexcept {
#if defined(BRAINSCAPE_FPENV_TEST_HOOKS)
#if defined(BRAINSCAPE_FPENV_AARCH64)
    FpWord status;
    __asm__ __volatile__("mrs %0, fpsr" : "=r"(status) : : "memory");
#else
    const FpWord status = ReadFpControl();
#endif
    fpenv_test::flags |= status & kFpFlagBits;
#endif
    WriteFpControl(saved_);
  }
  FpEnvGuard(const FpEnvGuard&)            = delete;
  FpEnvGuard& operator=(const FpEnvGuard&) = delete;

 private:
  FpWord saved_;
};

}  // namespace brainscape::detail
