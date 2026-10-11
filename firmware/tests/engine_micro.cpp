// ctest firmware_engine_micro: the bench image's engine micros (firmware/bench/EngineMicro.h) run
// once on the host with a stand-in clock, to check their setups before a Seed runs them: every
// line is produced, each population reaches its voice count, every birth at 32 and 64 voices is a
// steal, the heap holds 63 voices after a pop, and (Debug) no engine assertion fires.
#include <cstdio>
#include <memory>
#include <new>
#include <vector>

#include "bench/EngineMicro.h"
#include "detail/Granular.h"

using namespace brainscape::fw::bench;

namespace {

uint32_t g_ticks = 0;
uint32_t Now() { return g_ticks += 7u; }
void     Begin(bool) {}
void     End() {}

std::vector<MicroLine> g_lines;
void Emit(const MicroLine& l) { g_lines.push_back(l); }

}  // namespace

int main() {
  MicroMemory m;
  m.ringFrames = 1u << 16;
  std::vector<int16_t> ring(2u * m.ringFrames);
  std::vector<float>   lut(brainscape::detail::kWindowLutSize), wl(kMicroMaxFrames), wr(kMicroMaxFrames);
  void*                core = ::operator new(kMicroCoreBytes, std::align_val_t(16));
  m.core = core;
  m.ring = ring.data();
  m.lut  = lut.data();
  m.wetL = wl.data();
  m.wetR = wr.data();
  MicroClock clock;
  clock.begin = Begin;
  clock.now   = Now;
  clock.end   = End;
  const bool ok = RunEngineMicros(clock, m, Emit);
  ::operator delete(core, std::align_val_t(16));
  uint32_t groups[9] = {};
  for (const MicroLine& l : g_lines) {
    std::printf("%-9s %-75s samples=%u units=%u %s\n", l.group, l.name.c_str(), l.samples, l.units, l.detail.c_str());
    const std::string g = l.group;
    groups[g == "renderrun" ? 0 : g == "call" ? 1 : g == "birth" ? 2 : g == "heap" ? 3 : g == "pass" ? 4
                                 : g == "sweep" ? 5 : g == "detmath" ? 6 : g == "calibration" ? 8 : 7]++;
  }
  // 6 paths x 3 legs x 2 directions + unity's 3 legs, warm and cold, and the fade line.
  const bool counts = groups[0] == (6u * 3u * 2u + 3u) * 2u + 1u && groups[1] == 1u && groups[2] == 3u &&
                      groups[3] == 2u && groups[4] == 1u && groups[5] == 1u && groups[6] == 6u && groups[7] == 0u &&
                      groups[8] == 1u;
  std::printf("%s\n", ok && counts ? "engine micros: ok" : "engine micros: FAILED");
  return ok && counts ? 0 : 1;
}
