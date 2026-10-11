#include "bench/HopMicro.h"

#include <cstring>

#include "bench/HopForms.h"
#include "detail/OnsetDetector.h"

namespace brainscape::detail {

// The hop micro's access to the detector's buffers (OnsetDetector.h names it a friend; the host
// unit test dsp/tests/test_onset.cpp has its own, in another program).
struct OnsetDetectorProbe {
  static void Push(OnsetDetector& d, float x) {
    d.ana_[d.anaPos_] = x;
    d.anaPos_         = (d.anaPos_ + 1u) & (kOnsetFftSize - 1u);
  }
  static void  AnalyzeHop(OnsetDetector& d) { d.AnalyzeHop(); }
  static float Flux(const OnsetDetector& d) { return d.flux_; }
  static fw::bench::HopBuffers Buffers(OnsetDetector& d) {
    fw::bench::HopBuffers b;
    b.ana         = d.ana_;
    b.hann        = d.hann_;
    b.re          = d.re_;
    b.im          = d.im_;
    b.twCos       = d.twCos_;
    b.twSin       = d.twSin_;
    b.prevW       = d.prevW_;
    b.prevAvg     = d.prevAvg_;
    b.peakMem     = d.peakMem_;
    b.anaPos      = d.anaPos_;
    b.whitenDecay = d.whitenDecay_;
    return b;
  }
};

}  // namespace brainscape::detail

namespace brainscape::fw::bench {

using detail::kOnsetBins;
using detail::OnsetDetector;
using detail::OnsetDetectorProbe;

namespace {

constexpr uint32_t kHops  = 64;
constexpr uint32_t kForms = 3;

// What a hop leaves: the flux, the magnitudes of bins 1-256 and the three whitening states.
struct HopState {
  float flux;
  float mags[kOnsetBins];
  float prevW[kOnsetBins], prevAvg[kOnsetBins], peakMem[kOnsetBins];
};
HopState g_before, g_after[kForms];

void Save(const HopBuffers& b, float flux, HopState* s) {
  s->flux = flux;
  std::memcpy(s->mags, b.re, sizeof s->mags);
  std::memcpy(s->prevW, b.prevW, sizeof s->prevW);
  std::memcpy(s->prevAvg, b.prevAvg, sizeof s->prevAvg);
  std::memcpy(s->peakMem, b.peakMem, sizeof s->peakMem);
}
void Restore(const HopBuffers& b, const HopState& s) {
  std::memcpy(b.prevW, s.prevW, sizeof s.prevW);
  std::memcpy(b.prevAvg, s.prevAvg, sizeof s.prevAvg);
  std::memcpy(b.peakMem, s.peakMem, sizeof s.peakMem);
}
bool Same(const HopState& a, const HopState& b) {
  // Bins 1-256 of the magnitudes (bin 0 is not computed by every form) and every state bin.
  return std::memcmp(&a.flux, &b.flux, sizeof a.flux) == 0 &&
         std::memcmp(a.mags + 1, b.mags + 1, sizeof(float) * (kOnsetBins - 1u)) == 0 &&
         std::memcmp(a.prevW, b.prevW, sizeof a.prevW) == 0 && std::memcmp(a.prevAvg, b.prevAvg, sizeof a.prevAvg) == 0 &&
         std::memcmp(a.peakMem, b.peakMem, sizeof a.peakMem) == 0;
}

float Run(OnsetDetector& d, uint32_t form) {
  if (form == 0u) {
    OnsetDetectorProbe::AnalyzeHop(d);
    return OnsetDetectorProbe::Flux(d);
  }
  const HopBuffers b = OnsetDetectorProbe::Buffers(d);
  return form == 1u ? HopTextbook(b) : HopP1v2(b);
}

// The input: plucked noise bursts over a sawtooth, deterministic and well inside the engine's
// ±2^16 clamp (Engine.cpp kDetectorBound).
float Sample(uint32_t n, uint32_t* rng) {
  *rng              = *rng * 1664525u + 1013904223u;
  const float noise = static_cast<float>(static_cast<int32_t>(*rng >> 8) - (1 << 23)) / static_cast<float>(1 << 23);
  const uint32_t t  = n % 4096u;  // a pluck every 4,096 samples, decaying linearly
  const float    env = t < 2048u ? 1.0f - static_cast<float>(t) / 2048.0f : 0.0f;
  const float    saw = static_cast<float>(n % 97u) / 97.0f - 0.5f;
  return 0.6f * noise * env + 0.3f * saw;
}

}  // namespace

bool RunHopMicro(const MicroClock& clock, float* warm, void (*emit)(const MicroLine&)) {
  static_assert(sizeof(float) * kHopWarmFloats >= sizeof(float) * (512u * 4u + 256u * 2u + 257u * 3u),
                "raise kHopWarmFloats");
  OnsetDetector d;
  d.Init(warm, 48000.0);
  static const char* const kNames[kForms] = {"AnalyzeHop as built (P3's rewrite)", "the textbook form (before step 1)",
                                             "P1's v2"};
  MicroLine lines[kForms][2];
  for (uint32_t f = 0; f < kForms; ++f) {
    for (uint32_t c = 0; c < 2; ++c) {
      lines[f][c].group = "hop";
      lines[f][c].name  = std::string(kNames[f]) + (c ? ", caches cleaned" : ", caches warm");
      lines[f][c].units = 1;
    }
  }
  uint32_t rng = 0x5EEDu, n = 0, differ[kForms] = {};
  for (uint32_t i = 0; i < 2048u; ++i) OnsetDetectorProbe::Push(d, Sample(n++, &rng));  // a full window first
  for (uint32_t h = 0; h < kHops; ++h) {
    for (uint32_t i = 0; i < 256u; ++i) OnsetDetectorProbe::Push(d, Sample(n++, &rng));
    const HopBuffers b = OnsetDetectorProbe::Buffers(d);
    Save(b, 0.f, &g_before);
    for (uint32_t f = 0; f < kForms; ++f) {
      for (uint32_t c = 0; c < 2; ++c) {
        Restore(b, g_before);
        (void)Run(d, f);  // the same code and data once, untimed
        Restore(b, g_before);
        clock.begin(c != 0);
        const uint32_t t0   = clock.now();
        const float    flux = Run(d, f);
        const uint32_t t1   = clock.now();
        clock.end();
        MicroLine& l = lines[f][c];
        if (l.samples == 0 || t1 - t0 < l.min) l.min = t1 - t0;
        if (t1 - t0 > l.max) l.max = t1 - t0;
        l.sum += t1 - t0;
        ++l.samples;
        if (c == 0) Save(b, flux, &g_after[f]);
      }
      if (!Same(g_after[f], g_after[0])) ++differ[f];
    }
    Restore(b, g_after[0]);  // the next hop continues from the engine's state
  }
  bool ok = true;
  for (uint32_t f = 0; f < kForms; ++f) {
    for (uint32_t c = 0; c < 2; ++c) {
      lines[f][c].detail = std::string("\"hopsDiffering\":") + (differ[f] == 0 ? "0" : std::to_string(differ[f]));
      emit(lines[f][c]);
    }
    if (differ[f] != 0) ok = false;
  }
  return ok;
}

}  // namespace brainscape::fw::bench
