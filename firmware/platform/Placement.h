#pragma once
#include <cstddef>
#include <string>

#include "brainscape/Engine.h"

// The engine's memory as the pedal places it (docs/design/grain-engine.md §7): the Hot
// arena and the Engine object in DTCM (zero-wait, uncached), the Warm arena in AXI SRAM
// (D1, cached), the Bulk arena in SDRAM (cached write-back by libDaisy's MPU setup). Each
// lives in its own linker section (firmware/linker/seed_h750.ld.in), which the linker checks
// against its region.
namespace brainscape::fw {

// Sized for the canonical EngineConfig (48 kHz, a 2^22 ring, stereo, no looper) at
// maxBlockSize 512, the golden harness's, which covers the pedal's 48. PlanMemory gave, with
// the pinned arm-none-eabi 10.3 at kSoundRevision 2 (the Warm tier holds the active mode,
// 1,616 B more than revision 1's 129,680; sizeof(Engine) is kEngineImplBytes, 7,168 B on the
// M7 and 7,424 B on x86-64 then, 8,192 B and 8,448 B since sound revision 6, where
// brainscape_parity_stream --placement uses the same slot):
//   maxBlockSize  48: Hot 16,768 B   Warm 131,296 B   Bulk 17,545,216 B   Engine 7,168 B (8,192 B at r6)
//   maxBlockSize 512: Hot 20,480 B   Warm 131,296 B   Bulk 17,545,216 B
// Since sound revision 9 the post delay's line holds 4 s, 2^-7 of it and two frames
// (docs/design/clock.md §5.3, D23, §11.16): Bulk 18,325,232 B (the 2^22 ring's 16,777,216 and the
// line's 1,548,016), so the Bulk arena grows from 17 to 18 MiB, 549,136 B spare.
// Every image checks PlanMemory against these at boot (CheckPlacement) and refuses to run
// on a shortfall; the host test firmware_arena_plan does the same on every ctest run.
inline constexpr size_t kHotArenaBytes    = 24u * 1024u;           // DTCM
inline constexpr size_t kWarmArenaBytes   = 136u * 1024u;          // AXI SRAM
inline constexpr size_t kBulkArenaBytes   = 18u * 1024u * 1024u;   // SDRAM
inline constexpr size_t kEngineSlotBytes  = 9u * 1024u;            // DTCM, one Engine (and the
                                                                    // x86-64 one, 8,448 B)
inline constexpr size_t kEngineSlotAlign  = 16u;

// DTCM is 128 KiB and holds the main stack at its top (32 KiB reserved, linker-checked).
static_assert(kHotArenaBytes + kEngineSlotBytes <= 128u * 1024u - 32u * 1024u - 16u * 1024u,
              "the DTCM arena and engine must leave room for the stack and libDaisy's DTCM data");
// AXI SRAM is 512 KiB: the Warm arena, the USB rings (33 KiB), .data and .bss (libDaisy's
// peripheral handles and USB buffers, about 25 KiB). The heap is in SDRAM.
static_assert(kWarmArenaBytes <= 512u * 1024u - 33u * 1024u - 64u * 1024u,
              "the Warm arena must leave AXI SRAM room for the USB rings, .data and .bss");
// SDRAM is 64 MiB: the Bulk arena, the bench's per-block results, and the heap.
static_assert(kBulkArenaBytes <= 32u * 1024u * 1024u, "the Bulk arena must leave SDRAM for the heap");

struct EnginePlacement {
  Arenas arenas{};            // [Hot] DTCM, [Warm] AXI SRAM, [Bulk] SDRAM
  void*  engine = nullptr;    // kEngineSlotBytes in DTCM
};

// The image's one engine placement (its storage is static).
EnginePlacement Placement();

// Whether PlanMemory(cfg) and sizeof(Engine) fit the placement; `why` says what does not.
// Inline, so it compiles into the image, which links the engine archive.
inline bool CheckPlacement(const EngineConfig& cfg, const EnginePlacement& p, const char** why) {
  static_assert(sizeof(Engine) <= kEngineSlotBytes, "raise kEngineSlotBytes");
  static_assert(alignof(Engine) <= kEngineSlotAlign, "raise kEngineSlotAlign");
  const MemoryPlan plan = PlanMemory(cfg);
  static const char* const kTier[kNumTiers] = {
      "the Hot arena (DTCM) is smaller than PlanMemory's", "the Warm arena (AXI SRAM) is smaller than PlanMemory's",
      "the Bulk arena (SDRAM) is smaller than PlanMemory's"};
  for (size_t t = 0; t < kNumTiers; ++t) {
    if (plan.bytes[t] == 0) {
      *why = "PlanMemory refused the configuration";
      return false;
    }
    if (p.arenas.bytes[t] < plan.bytes[t] ||
        reinterpret_cast<uintptr_t>(p.arenas.base[t]) % plan.align[t] != 0) {
      *why = kTier[t];
      return false;
    }
  }
  return true;
}

// Where the linker put everything: per region, the image's static use and the arenas, as
// raw JSON (the hello line's "memory").
std::string MemoryMapJson();

}  // namespace brainscape::fw
