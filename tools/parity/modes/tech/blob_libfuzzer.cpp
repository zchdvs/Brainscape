// PROBE: libFuzzer target for the package decoder/validator (clang -fsanitize=fuzzer,address,undefined).
// Both modes: with hash checks (what the pedal runs) and with the fuzzing hook that skips
// them, so coverage-guided mutation reaches the inner validators.
#include <cstdint>
#include <cstring>
#include <vector>

#include "blob_format.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  std::vector<uint8_t> exact(data, data + size);  // exact-size heap copy for ASan
  bsp::DecodedPackage d;
  bsp::Decode(exact.empty() ? nullptr : exact.data(), exact.size(), &d, false);
  const bsp::Error e = bsp::Decode(exact.empty() ? nullptr : exact.data(), exact.size(), &d, true);
  if (e == bsp::Error::None) {
    // accepted state must satisfy the decoded invariants the engine relies on
    if (d.layerCount < 1 || d.layerCount > bsp::kMaxLayers) __builtin_trap();
    for (uint32_t l = 0; l < d.layerCount; ++l)
      if (uint32_t(d.layers[l].pitchFirst) + d.layers[l].pitchN > d.pitchCount) __builtin_trap();
    for (uint32_t m = 0; m < d.macroCount; ++m)
      if (uint32_t(d.macros[m].first) + d.macros[m].n > d.targetCount) __builtin_trap();
  }
  return 0;
}
