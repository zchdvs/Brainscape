#pragma once
#include <cstdlib>

#include "brainscape/FpProfile.h"
#include "brainscape/Memory.h"

#if defined(_WIN32)
#include <malloc.h>
#endif

// Host-side convenience for tests, offline rendering, and the plugin wrapper:
// maps every tier to aligned heap allocations. Firmware never includes this —
// it places arenas via linker sections (docs/design/grain-engine.md §7).
//
// Uses _aligned_malloc / posix_memalign rather than C++17 aligned operator new:
// the aligned-new runtime symbols are availability-gated on Apple platforms
// (macOS 10.14+), so a plugin pinning an older deployment target would stop
// compiling (review finding).
namespace brainscape::host {

class HeapArenas {
 public:
  explicit HeapArenas(const MemoryPlan& plan) {
    for (size_t t = 0; t < kNumTiers; ++t) {
      const size_t align = plan.align[t] > sizeof(void*) ? plan.align[t] : sizeof(void*);
      arenas_.bytes[t]   = plan.bytes[t];
      arenas_.base[t]    = nullptr;
      if (plan.bytes[t] != 0) {
#if defined(_WIN32)
        arenas_.base[t] = _aligned_malloc(plan.bytes[t], align);
#else
        void* p = nullptr;
        if (posix_memalign(&p, align, plan.bytes[t]) != 0) p = nullptr;
        arenas_.base[t] = p;
#endif
      }
    }
  }
  ~HeapArenas() {
    for (size_t t = 0; t < kNumTiers; ++t) {
      if (arenas_.base[t] != nullptr) {
#if defined(_WIN32)
        _aligned_free(arenas_.base[t]);
#else
        std::free(arenas_.base[t]);
#endif
      }
    }
  }
  HeapArenas(const HeapArenas&) = delete;
  HeapArenas& operator=(const HeapArenas&) = delete;

  bool ok() const {
    for (size_t t = 0; t < kNumTiers; ++t) {
      if (arenas_.bytes[t] != 0 && arenas_.base[t] == nullptr) return false;
    }
    return true;
  }
  const Arenas& get() const { return arenas_; }

 private:
  Arenas arenas_{};
};

}  // namespace brainscape::host
