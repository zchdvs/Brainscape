#pragma once
#include <cstdint>

#include "brainscape/FpProfile.h"

namespace brainscape {

// The sound revision (docs/design/determinism-profile.md §5.12): certifies the dsp/
// sources, shared constants, canonical EngineConfig and the determinism profile's
// rules and flags, for every input. Any change that CAN change engine output bumps
// it; golden coverage is not the definition. It keys the golden-hash file (§6.1).
//
// A bump is exactly one and regenerates dsp/tests/golden/golden.json with the harness's
// --mode mint in the same pull request; sound-rev.yml fails a sound-relevant change
// without one (tools/ci/sound_rev_gate.py). Revisions before the first published one
// are internal (§1.5). 1: the first minted revision (profile §8.4 step 10). 2: the mode
// runtime (docs/design/mode-compiler.md §7.6 item 4): domain routing, rows 27 and 28 retired
// into mode structure, the wet-only trim, effect volume and cutoff kill, LoadPreset with the
// mode and CTRL, macro and expression events, Trails and FastCut mode switches.
inline constexpr uint32_t kSoundRevision = 2;

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
