// PROBE: a dsp/ entry point holding a decoded package on its stack: MSVC x64 inserts a
// __chkstk call for frames over 4 KiB, a symbol outside the profile's MSVC allow list.
#include <cstdint>
#include <cstring>
struct BigState { uint32_t words[1200]; };  // 4800 bytes, e.g. a PresetState with a ModeBlob inside
void Fill(BigState*) noexcept;
uint32_t UsesStack() noexcept { BigState s; Fill(&s); return s.words[7]; }
struct SmallState { uint32_t words[900]; };  // 3600 bytes
void Fill2(SmallState*) noexcept;
uint32_t UsesStack2() noexcept { SmallState s; Fill2(&s); return s.words[7]; }
