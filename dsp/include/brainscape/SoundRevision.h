#pragma once
#include <cstdint>

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

}  // namespace brainscape
