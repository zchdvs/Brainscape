#pragma once
#include <cstdint>

#include "brainscape/FpProfile.h"

namespace brainscape {

// The sound revision (docs/design/determinism-profile.md §5.12): certifies the dsp/
// sources, shared constants, canonical EngineConfig and the determinism profile's
// rules and flags, for every input. Any change that CAN change engine output bumps
// it; golden coverage is not the definition. It keys the golden-hash file (§6.1).
//
// 0 means no revision has been minted: the engine is still changing (profile §8.4
// step 10 mints internal revision 1), so the golden harness only reports and
// refuses to mint.
inline constexpr uint32_t kSoundRevision = 0;

// The toolchain that compiled this dsp/ library (profile §5.12): compiler, version,
// target and the floating-point flags. For triage only, carried in the parity reply and
// in golden reports: identity is the sound revision, and every conforming toolchain
// produces the same output.
struct ToolchainId {
  const char* compiler;     // "msvc", "gcc", "clang" or "appleclang"
  const char* version;      // the compiler's own version string
  const char* target;       // architecture and system, e.g. "x86_64-linux", "armv7e-m-none"
  const char* fpFlags;      // the profile's flags and the FP and target flags of the
                            // build configuration, as CMake passed them
  const char* fpFlagsHash;  // SHA-256 of fpFlags, first 16 hex digits
};
const ToolchainId& BuildToolchain() noexcept;

}  // namespace brainscape
