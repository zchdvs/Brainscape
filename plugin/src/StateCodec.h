#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "brainscape/Params.h"

namespace brainscape::plugin {

enum class InputMode : uint32_t { Mono = 0, Stereo = 1 };  // Mono: R := L (companion §4.8)

// Global wrapper settings, saved with the session but never part of a preset (§4.8). The
// processor starts the Standalone in Mono (BrainscapeProcessor's constructor).
struct WrapperSettings {
  InputMode inputMode    = InputMode::Stereo;
  float     inputGainDb  = 0.f;
  float     outputGainDb = 0.f;
};
inline constexpr float kWrapperGainRangeDb = 24.f;

// DAW session state (companion §6.9, before .bsp packages exist): every leaf as exact
// binary32 bits plus the wrapper settings. Little-endian fields written one by one, never
// a struct. Layout v1:
//   "BSWS"  u32 version=1  u32 n  n x {u32 ParamId, u32 bits}  u32 m  m x {u32 key, u32 bits}
// Freeze is a performance state and is never stored (companion §6.2).
struct WrapperState {
  float           plain[kNumParams];
  WrapperSettings settings;
  uint32_t        unknownIds = 0;  // ids in the blob this build lacks (ignored)
  uint32_t        missingIds = 0;  // ids this build has that the blob lacks (defaults)
};

inline constexpr uint32_t kStateFormatVersion = 1;

void EncodeState(const WrapperState& state, std::vector<uint8_t>& out);
// Complete-state semantics: ids absent from the blob load their defaults. Values are
// canonicalized, so canonical values round-trip bit for bit. Returns false (and leaves
// `out` untouched) for a malformed or newer blob.
bool DecodeState(const void* data, size_t bytes, WrapperState& out);

float CanonicalGainDb(float db) noexcept;

}  // namespace brainscape::plugin
