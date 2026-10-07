// ctest firmware_live_presets: what the live image builds at boot (firmware/live/LivePresets.h),
// built on the host from the same embedded packages (EmbeddedPackages.h). Every slot must load
// exactly; every structure must hold the onset and mark structure its row names and otherwise
// the default mode and CTRL; and every slot with every structure (what `onset` and `marks`
// stage) must load exactly as a Spillover load, playing that structure. The image checks the
// first two at boot and refuses to run on a failure; this catches it before anything is flashed.
#include <cstdio>
#include <memory>

#include "brainscape/Engine.h"
#include "brainscape/HostArenas.h"
#include "live/LivePresets.h"

int main() {
  using namespace brainscape;
  namespace live = brainscape::fw::live;
  int         failures = 0;
  const char* why      = "";
  const auto  slots    = std::make_unique<PresetState[]>(live::kNumSlots);
  const auto  shapes   = std::make_unique<PresetState[]>(live::kNumStructures);
  if (!live::BuildSlots(slots.get(), &why)) {
    std::printf("BuildSlots: FAIL: %s\n", why);
    return 1;
  }
  if (!live::BuildStructures(shapes.get(), &why)) {
    std::printf("BuildStructures: FAIL: %s\n", why);
    return 1;
  }

  EngineConfig ec;
  ec.maxBlockSize = 48;  // the live image's engine
  host::HeapArenas arenas(PlanMemory(ec));
  auto             engine = std::make_unique<Engine>();
  if (!arenas.ok() || !engine->Init(ec, arenas.get())) {
    std::printf("Engine::Init failed\n");
    return 1;
  }
  for (uint32_t s = 0; s < live::kNumSlots; ++s) {
    LoadReport exact;
    const bool ok = engine->LoadPreset(slots[s], LoadMode::Exact, &exact);
    std::printf("slot %u %-48s onset %d marks %d: Exact %s\n", s, live::kSlots[s].label,
                live::PlaysOnset(slots[s].mode), live::PlaysMarks(slots[s].mode), ok ? "exact" : "NOT EXACT");
    failures += ok ? 0 : 1;
    for (uint32_t i = 0; i < live::kNumStructures; ++i) {
      const auto next = std::make_unique<PresetState>(slots[s]);
      live::WithStructure(next.get(), shapes[i]);
      LoadReport r;
      const bool spill = engine->LoadPreset(*next, LoadMode::Spillover, &r);
      const bool shape = live::PlaysOnset(next->mode) == live::kStructures[i].onset &&
                         live::PlaysMarks(next->mode) == live::kStructures[i].marks;
      if (!spill || !shape || !CheckPreset(*next)) {
        std::printf("  structure %u (%s): FAIL (exact %d, structure %d)\n", i,
                    live::kStructures[i].package ? live::kStructures[i].package : "default", spill, shape);
        ++failures;
      }
    }
  }
  std::printf("%u slots x %u structures: %s\n", live::kNumSlots, live::kNumStructures,
              failures == 0 ? "ok" : "FAIL");
  return failures == 0 ? 0 : 1;
}
