#pragma once
// Host floating-point environments for the guard tests (determinism profile §4.1,
// §6.4). Tests do no FP arithmetic while a hostile word is installed: they wrap
// single engine calls in HostileFpScope.
#include "detail/FpEnvGuard.h"

namespace brainscape::testing {

#if defined(BRAINSCAPE_FPENV_X64)
// FTZ | DAZ | round toward zero, exceptions masked: what a host with
// ScopedNoDenormals and a stray fesetround(FE_TOWARDZERO) leaves behind.
inline constexpr detail::FpWord kHostileFpWord     = 0xFFC0u;
inline constexpr detail::FpWord kFtzDazFpWord      = 0x9FC0u;  // JUCE's ScopedNoDenormals
inline constexpr detail::FpWord kSubnormalFlagBits = 0x12u;    // DE | UE
// Every exception unmasked: any FP operation outside the guard that is inexact, or
// touches a subnormal, traps.
inline constexpr detail::FpWord kTrapAllFpWord = 0x0000u;
#else
// FZ | DN | round toward zero (FPCR on AArch64, FPSCR on the M7).
inline constexpr detail::FpWord kHostileFpWord     = 0x03C00000u;
inline constexpr detail::FpWord kFtzDazFpWord      = 0x01000000u;  // FZ
inline constexpr detail::FpWord kSubnormalFlagBits = 0x88u;        // IDC | UFC
#endif

class HostileFpScope {
 public:
  explicit HostileFpScope(detail::FpWord word = kHostileFpWord) noexcept
      : saved_(detail::ReadFpControl()) {
    detail::WriteFpControl(word);
  }
  ~HostileFpScope() noexcept { detail::WriteFpControl(saved_); }
  HostileFpScope(const HostileFpScope&)            = delete;
  HostileFpScope& operator=(const HostileFpScope&) = delete;

 private:
  detail::FpWord saved_;
};

}  // namespace brainscape::testing
