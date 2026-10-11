// ctest firmware_hop_forms: the hop image's micro (firmware/bench/HopMicro.h) with a stand-in
// clock. The textbook form and P1's v2 (firmware/bench/HopForms.h) must leave the same flux,
// magnitudes and whitening state as the engine's AnalyzeHop on every one of its 64 hops, before a
// Seed times them against each other (docs/design/cpu-budget.md §8.1, image 6; D4).
#include <cstdio>
#include <vector>

#include "bench/HopMicro.h"

using namespace brainscape::fw::bench;

namespace {
uint32_t g_ticks = 0;
uint32_t Now() { return g_ticks += 3u; }
void     Begin(bool) {}
void     End() {}
uint32_t g_lines = 0;
void     Emit(const MicroLine& l) {
  ++g_lines;
  std::printf("%-5s %-55s samples=%u %s\n", l.group, l.name.c_str(), l.samples, l.detail.c_str());
}
}  // namespace

int main() {
  std::vector<float> warm(kHopWarmFloats);
  MicroClock         clock;
  clock.begin   = Begin;
  clock.now     = Now;
  clock.end     = End;
  const bool ok = RunHopMicro(clock, warm.data(), Emit);
  const bool good = ok && g_lines == 6u;
  std::printf("%s\n", good ? "hop forms: ok" : "hop forms: FAILED");
  return good ? 0 : 1;
}
