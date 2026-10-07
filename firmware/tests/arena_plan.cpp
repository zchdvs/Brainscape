// ctest firmware_arena_plan: the firmware's engine arenas (firmware/platform/Placement.h) hold
// what PlanMemory asks for in the configurations the images run (maxBlockSize 48, the live
// engine and the bench; 512, the parity image's harness configuration). The images check
// the same at boot and refuse to run on a shortfall; this catches it on every host build,
// before anything is flashed.
#include <cstdint>
#include <cstdio>
#include <initializer_list>

#include "platform/Placement.h"

int main() {
  using namespace brainscape;
  int failures = 0;
  for (const uint32_t maxBlock : {48u, 512u}) {
    EngineConfig ec;
    ec.maxBlockSize = maxBlock;
    fw::EnginePlacement p;  // the firmware's sizes at aligned stand-in addresses
    p.arenas.base[0]  = reinterpret_cast<void*>(uintptr_t{0x20000000u});
    p.arenas.base[1]  = reinterpret_cast<void*>(uintptr_t{0x24000000u});
    p.arenas.base[2]  = reinterpret_cast<void*>(uintptr_t{0xC0000000u});
    p.arenas.bytes[0] = fw::kHotArenaBytes;
    p.arenas.bytes[1] = fw::kWarmArenaBytes;
    p.arenas.bytes[2] = fw::kBulkArenaBytes;
    const MemoryPlan plan = PlanMemory(ec);
    const char*      why  = "";
    const bool       ok   = fw::CheckPlacement(ec, p, &why);
    std::printf("maxBlockSize %3u: Hot %zu / %zu  Warm %zu / %zu  Bulk %zu / %zu  %s%s\n", maxBlock,
                plan.bytes[0], fw::kHotArenaBytes, plan.bytes[1], fw::kWarmArenaBytes, plan.bytes[2],
                fw::kBulkArenaBytes, ok ? "ok" : "FAIL: ", ok ? "" : why);
    if (!ok) ++failures;
  }
  std::printf("sizeof(Engine) %zu / %zu\n", sizeof(Engine), fw::kEngineSlotBytes);
  return failures == 0 ? 0 : 1;
}
