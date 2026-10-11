#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// The bench's per-function micro of the engine's internals (docs/design/cpu-budget.md §8.2,
// SuiteMicro): RenderRun per voice-frame by path, rate class, envelope leg and direction, with
// the ring's lines warm and forced cold, and with a FastCut fade; one RenderRun call; a trigger
// birth on the worst path (mark positioning at r > 1, detune, reverse, spray, pan) at 0, 32 and
// 64 voices, a steal at 32 and 64; the voice heap's push and pop; a repeat pass boundary; the
// 64-slot sweep the heap replaced; and the DetMath calls a birth makes. Each runs on a grain core
// of its own (never the engine's), through a friend of detail::GranularCore, so it changes no
// engine state. The timing is the caller's: DWT with interrupts off on the Seed, a stand-in on
// the host, where ctest firmware_engine_micro runs every micro once to check its setup.
namespace brainscape::fw::bench {

struct MicroClock {
  void (*begin)(bool cold) = nullptr;  // before the first timestamp (cold: caches cleaned and invalidated)
  uint32_t (*now)()        = nullptr;  // a timestamp
  void (*end)()            = nullptr;  // after the second
};

struct MicroLine {
  const char* group = "";  // renderrun, call, birth, heap, pass, sweep, detmath
  std::string name;
  uint32_t    samples = 0;  // measurements
  uint32_t    units   = 0;  // what one measurement covers: frames or calls
  uint32_t    min = 0, max = 0;
  uint64_t    sum = 0;
  std::string detail;  // more JSON members ("key":value,...), or empty
};

// The micros' own memory: on the Seed the core and its window table in DTCM beside the engine's,
// the ring in SDRAM, as the engine's are placed.
inline constexpr uint32_t kMicroMaxFrames = 512;
inline constexpr size_t   kMicroCoreBytes = 16384;  // at least MicroCoreBytes()
struct MicroMemory {
  void*    core       = nullptr;  // kMicroCoreBytes, 16-byte aligned
  int16_t* ring       = nullptr;  // interleaved stereo, ringFrames frames
  uint32_t ringFrames = 0;        // a power of two, at least 2^16
  float*   lut        = nullptr;  // detail::kWindowLutSize floats
  float*   wetL       = nullptr;  // kMicroMaxFrames each
  float*   wetR       = nullptr;
};
size_t MicroCoreBytes();

// Runs every micro, handing each result to `emit`. False when a micro's setup failed (a population
// that never reached its voice count): the line says which.
bool RunEngineMicros(const MicroClock& clock, const MicroMemory& mem, void (*emit)(const MicroLine&));

}  // namespace brainscape::fw::bench
