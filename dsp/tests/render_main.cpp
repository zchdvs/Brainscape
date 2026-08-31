// Offline render harness: runs the exact engine the pedal will run, on the host,
// and writes a WAV — seconds of iteration instead of a flash cycle
// (docs/research/vst-and-shared-dsp.md rec #7).
#include <cmath>
#include <cstdio>
#include <vector>

#include "brainscape/Engine.h"
#include "brainscape/HostArenas.h"
#include "WavWriter.h"

using namespace brainscape;

int main(int argc, char** argv) {
  const char* path    = argc > 1 ? argv[1] : "brainscape_render.wav";
  const double sr     = 48000.0;
  const size_t frames = static_cast<size_t>(sr * 4.0);  // 4 s

  EngineConfig cfg;
  cfg.sampleRate    = sr;
  cfg.maxBlockSize  = 512;
  cfg.historyFrames = 1u << 21;  // ~43.7 s @ 48 kHz, 8 MiB — plenty for the demo

  host::HeapArenas arenas(PlanMemory(cfg));
  Engine engine;
  if (!engine.Init(cfg, arenas.get())) {
    std::fprintf(stderr, "engine init failed\n");
    return 1;
  }
  engine.SetParam(ParamId::DelayMs, 375.0f);
  engine.SetParam(ParamId::Mix, 0.5f);
  engine.SetParam(ParamId::Feedback, 0.6f);
  engine.SetParam(ParamId::OutTrimDb, 0.0f);
  engine.Reset();

  // Test signal: a short 220 Hz burst, then a few staccato plucks (decaying sines).
  std::vector<float> input(frames, 0.0f);
  for (size_t n = 0; n < static_cast<size_t>(sr * 0.25); ++n) {
    input[n] = 0.5f * std::sin(2.0 * 3.14159265358979 * 220.0 * (double(n) / sr)) *
               (1.0f - float(n) / float(sr * 0.25));
  }
  for (int hit = 0; hit < 5; ++hit) {
    const size_t start = static_cast<size_t>(sr * (1.0 + 0.5 * hit));
    const double freq  = 220.0 * std::pow(2.0, hit / 5.0);
    for (size_t n = 0; n < static_cast<size_t>(sr * 0.08) && start + n < frames; ++n) {
      input[start + n] +=
          0.7f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * freq * (double(n) / sr)) *
                                    std::exp(-double(n) / (sr * 0.02)));
    }
  }

  std::vector<float> outL(frames), outR(frames);
  size_t pos = 0;
  while (pos < frames) {
    const auto n = static_cast<uint32_t>(std::min<size_t>(cfg.maxBlockSize, frames - pos));
    const float* ins[2]  = {input.data() + pos, input.data() + pos};
    float*       outs[2] = {outL.data() + pos, outR.data() + pos};
    Engine::ProcessContext ctx;
    ctx.in        = ins;
    ctx.out       = outs;
    ctx.numFrames = n;
    engine.Process(ctx);
    pos += n;
  }

  if (!tools::WriteWavFloat32Stereo(path, outL.data(), outR.data(), frames,
                                    static_cast<uint32_t>(sr))) {
    std::fprintf(stderr, "failed to write %s\n", path);
    return 1;
  }
  std::printf("wrote %s (%zu frames, delay 375 ms, fb 0.6, mix 0.5)\n", path, frames);
  return 0;
}
