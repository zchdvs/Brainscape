#pragma once
#include <new>

#include "brainscape/Memory.h"

// Host-side convenience for tests, offline rendering, and the plugin wrapper:
// maps every tier to aligned heap allocations. Firmware never includes this —
// it places arenas via linker sections (docs/design/grain-engine.md §7).
namespace brainscape::host {

class HeapArenas {
 public:
  explicit HeapArenas(const MemoryPlan& plan) {
    for (size_t t = 0; t < kNumTiers; ++t) {
      align_[t]        = plan.align[t] != 0 ? plan.align[t] : 16;
      arenas_.bytes[t] = plan.bytes[t];
      arenas_.base[t]  = plan.bytes[t] != 0
                             ? ::operator new(plan.bytes[t], std::align_val_t(align_[t]))
                             : nullptr;
    }
  }
  ~HeapArenas() {
    for (size_t t = 0; t < kNumTiers; ++t) {
      if (arenas_.base[t] != nullptr) {
        ::operator delete(arenas_.base[t], std::align_val_t(align_[t]));
      }
    }
  }
  HeapArenas(const HeapArenas&) = delete;
  HeapArenas& operator=(const HeapArenas&) = delete;

  const Arenas& get() const { return arenas_; }

 private:
  Arenas arenas_{};
  size_t align_[kNumTiers]{};
};

}  // namespace brainscape::host
