#pragma once
#include "brainscape/FpProfile.h"

// Private half of the determinism-profile tripwires (docs/design/determinism-profile.md
// §3.3), included first by every dsp/ source and by every private header with
// floating-point bodies. The pragmas stay out of the public headers, where they would
// change consumers' code generation. GCC ignores the STDC pragma and relies on
// -ffp-contract=off from brainscape::fp_profile. The test-only negative-control build
// (BrainscapeFpProfile.cmake) contracts on purpose, so it keeps the compiler's mode.
#if defined(BRAINSCAPE_FP_NEGATIVE_CONTROL)
#elif defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#elif defined(_MSC_VER)
#pragma fp_contract(off)
#endif
