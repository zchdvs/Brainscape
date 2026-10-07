#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "Render.h"

// 48 kHz stereo WAV files for listening (docs/design/mode-compiler.md §11.3). The 16-bit form is
// what the audition writes; render identity is the float32 hash (Hash.h), never the WAV's bytes.
namespace bsa {

// One sample as 16-bit PCM: round(x * 32768), half away from zero, saturated to
// [-32768, 32767]; NaN as 0. Exact in binary64 for every float, so every machine writes the
// same bytes for the same render.
int16_t ToPcm16(float x);

// The canonical 44-byte RIFF header and the interleaved samples.
std::vector<uint8_t> WavPcm16(const Stereo& s);
std::vector<uint8_t> WavFloat32(const Stereo& s);
bool                 WriteFile(const std::string& path, const std::vector<uint8_t>& bytes);
bool                 WriteText(const std::string& path, const std::string& text);

// Reads a 16-bit PCM or float32 stereo WAV this module wrote (tests and tools): false otherwise.
bool ReadWav(const std::vector<uint8_t>& bytes, Stereo* out, uint32_t* rate, uint16_t* bits);

}  // namespace bsa
