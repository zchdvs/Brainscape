#pragma once
#include <cstdint>

// The onset hop's analysis in the forms bench session 2 times against the engine's
// (docs/design/cpu-budget.md §4.1, §8.1 image 6, D4): the textbook form step 1 replaced, and P1's
// v2, the larger rewrite that was not built. Each runs on the detector's own buffers and state, as
// OnsetDetector::AnalyzeHop does, and returns the flux AnalyzeHop leaves in flux_: the window and
// FFT into re and im, the magnitudes of bins 1-256 into re[1..256], the whitening state updated in
// place. The three give the same bits (dsp/tests/test_onset.cpp holds the textbook form against
// the engine's; ctest firmware_hop_forms holds all three). The brainscape_bench_hop image places
// this object's code and tables in ITCM, as the engine's are, so the forms are timed alike.
namespace brainscape::fw::bench {

struct HopBuffers {
  const float* ana;      // [512] the circular input window
  const float* hann;     // [512]
  float*       re;       // [512]
  float*       im;       // [512]
  const float* twCos;    // [256]
  const float* twSin;    // [256]
  float*       prevW;    // [257]
  float*       prevAvg;  // [257]
  float*       peakMem;  // [257]
  uint32_t     anaPos;   // one past the newest sample: the oldest
  float        whitenDecay;
};

float HopTextbook(const HopBuffers& b) noexcept;
float HopP1v2(const HopBuffers& b) noexcept;

}  // namespace brainscape::fw::bench
