#pragma once
#include <cstddef>

#include "bench/EngineMicro.h"

// D4's timing of the onset hop's analysis (docs/design/cpu-budget.md §4.1, §8.1 image 6): on an
// onset detector of the bench's own, fed a deterministic signal, each of 64 hops is analysed by the
// engine's AnalyzeHop (P3's rewrite, step 1 as built), the textbook form it replaced and P1's v2
// (bench/HopForms.h), each from the same state, with the D-cache warm and cleaned, and each form's
// flux, magnitudes and whitening state compared bit for bit with the engine's. Only the
// brainscape_bench_hop image links it (its forms in ITCM); ctest firmware_hop_forms runs it with a
// stand-in clock.
namespace brainscape::fw::bench {

inline constexpr size_t kHopWarmFloats = 4096;  // at least detail::OnsetDetector::WarmFloats()

// `warm`: kHopWarmFloats floats where the detector's tables and state live (on the Seed the AXI
// SRAM, as the engine's Warm arena). False when a form's bits differ from the engine's.
bool RunHopMicro(const MicroClock& clock, float* warm, void (*emit)(const MicroLine&));

}  // namespace brainscape::fw::bench
