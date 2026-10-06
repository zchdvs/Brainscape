#pragma once
#include <cstdint>

#include "brainscape/FpProfile.h"

namespace brainscape {

// One stored leaf of a preset: a permanent parameter id and its exact binary32 plain value
// (docs/design/companion-app.md §6.3, the STAT section). The id is raw, so a package naming
// ids this build lacks still decodes; the load then reports it.
struct PresetLeaf {
  uint32_t id    = 0;
  float    value = 0.f;
};

// A decoded preset package (docs/design/determinism-profile.md §5.10, companion-app.md
// §6.1-§6.3). Today it holds the STAT section's leaves, in ascending id order; the MODE and
// CTRL sections join it with the mode system. Plain fixed-size data, so firmware decodes
// into it without allocating; it is never hashed or serialized as a struct.
//
// The stored performance state of §5.10 step 4 (global reverse, tempo, subdivision) has no
// engine counterpart yet. Freeze is performance state that is never stored: every load
// turns it off.
struct PresetState {
  // Today's 28 leaves, with room for the design's full leaf list.
  static constexpr uint32_t kMaxLeaves = 128;

  uint32_t   leafCount = 0;
  PresetLeaf leaves[kMaxLeaves];
};

// Exact: Restart, then the preset, so the engine starts from the exact-restart state; not
// real-time. Spillover: the preset over the running engine, keeping history, grains,
// scheduler phase and smoothers, with the random-number epoch restarted at the load frame
// (determinism profile §5.9, §5.10; companion-app.md §6.7).
enum class LoadMode : uint8_t { Exact = 0, Spillover = 1 };

// How faithfully a preset loads (determinism profile §5.10 step 2). A load that is not exact
// still applies, but what the engine plays is not what the package stores, so the app shows
// no identity label for it.
struct LoadReport {
  bool     applied       = false;  // false when the engine was not initialized
  bool     exact         = false;  // every count below is 0
  uint32_t unknownIds    = 0;      // leaves naming an id this build lacks, or past kMaxLeaves
  uint32_t missingIds    = 0;      // this build's ids without a leaf: they load their default
  uint32_t duplicateIds  = 0;      // repeated ids: the first leaf counts
  uint32_t changedValues = 0;      // values canonicalization changed (NaN, ±inf, -0,
                                   // subnormals, out of range): packages hold canonical values
};

// The checks of a load without loading (the report's `applied` stays false), for a producer
// that stages a Spillover event. Inside the FP environment guard, as canonicalization is.
// Returns report.exact.
bool CheckPreset(const PresetState& preset, LoadReport* report = nullptr) noexcept;

}  // namespace brainscape
