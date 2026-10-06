#pragma once
#include <cstddef>

#include "brainscape/FpProfile.h"

// Engine input conditioning (docs/design/determinism-profile.md §3.7; referenced by
// docs/design/companion-app.md §4.8). Every desktop input path applies exactly one of
// these to every input sample before Engine::Process; the firmware applies neither,
// because libDaisy already delivers exactly i * 2^-23. Both use only integer and exact
// operations, so no compiler flag or host floating-point environment changes their
// results, and they run without the engine's guard. The buffer forms allow in == out.
namespace brainscape {

// Live input (every plugin format, the Standalone's live monitoring, the oracle
// harness): NaN and ±inf become +0; every finite value passes unchanged, subnormals
// included, so the dry path stays bit-exact at Mix 0 and unclipped above 0 dBFS.
float SanitizeInput(float x) noexcept;
void  SanitizeInput(const float* in, float* out, size_t numSamples) noexcept;

// Pedal-faithful input (offline DI renders, golden vectors from recordings, the preset
// tool's render command, the app's optional pedal-faithful monitoring; never the
// default live path): the codec's 24-bit grid. Returns i * 2^-23 with
// i = round-half-away-from-zero(x * 2^23) clamped to [-2^23, 2^23 - 1]; NaN becomes 0
// and ±inf saturate. Values already on the grid, such as the test-signal generator's,
// pass unchanged.
float ConditionInput24(float x) noexcept;
void  ConditionInput24(const float* in, float* out, size_t numSamples) noexcept;

}  // namespace brainscape
