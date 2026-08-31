#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

// Minimal stereo float32 WAV writer for the offline render tool. Test/tool code
// only — never linked into firmware or the realtime path.
namespace brainscape::tools {

inline bool WriteWavFloat32Stereo(const char* path, const float* l, const float* r,
                                  size_t frames, uint32_t sampleRate) {
  std::FILE* f = std::fopen(path, "wb");
  if (f == nullptr) return false;

  const uint32_t dataBytes  = static_cast<uint32_t>(frames * 2 * sizeof(float));
  const uint16_t channels   = 2;
  const uint16_t bits       = 32;
  const uint32_t byteRate   = sampleRate * channels * (bits / 8);
  const uint16_t blockAlign = channels * (bits / 8);
  // fmt (18 bytes, WAVE_FORMAT_IEEE_FLOAT) + fact + data
  const uint32_t riffBytes = 4 + (8 + 18) + (8 + 4) + (8 + dataBytes);

  auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
  auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };

  std::fwrite("RIFF", 1, 4, f);
  u32(riffBytes);
  std::fwrite("WAVE", 1, 4, f);
  std::fwrite("fmt ", 1, 4, f);
  u32(18);
  u16(3);  // IEEE float
  u16(channels);
  u32(sampleRate);
  u32(byteRate);
  u16(blockAlign);
  u16(bits);
  u16(0);  // cbSize
  std::fwrite("fact", 1, 4, f);
  u32(4);
  u32(static_cast<uint32_t>(frames));
  std::fwrite("data", 1, 4, f);
  u32(dataBytes);

  std::vector<float> interleaved(frames * 2);
  for (size_t i = 0; i < frames; ++i) {
    interleaved[2 * i]     = l[i];
    interleaved[2 * i + 1] = r[i];
  }
  const bool ok = std::fwrite(interleaved.data(), sizeof(float), interleaved.size(), f) ==
                  interleaved.size();
  std::fclose(f);
  return ok;
}

}  // namespace brainscape::tools
