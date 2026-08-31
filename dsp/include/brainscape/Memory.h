#pragma once
#include <cstddef>
#include <cstdint>

namespace brainscape {

// Memory seam (docs/design/grain-engine.md §9): dsp/ never allocates and never names
// a platform memory region. The caller sizes arenas from PlanMemory() and hands them
// to Engine::Init(). Firmware maps tiers to linker sections (DTCM / AXI SRAM / SDRAM);
// the desktop host maps all three to heap.
enum class Tier : uint8_t { Hot = 0, Warm = 1, Bulk = 2 };
inline constexpr size_t kNumTiers = 3;

struct MemoryPlan {
  size_t bytes[kNumTiers];
  size_t align[kNumTiers];
};

struct Arenas {
  void*  base[kNumTiers];
  size_t bytes[kNumTiers];
};

}  // namespace brainscape
