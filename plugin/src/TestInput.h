#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include <juce_audio_formats/juce_audio_formats.h>

namespace brainscape::plugin {

// Developer test sources that stand in for the live input, so the engine can be played
// without a guitar: a looped audio file or a plucked-string generator. Platform decoders
// and the playback interpolation are outside the determinism profile (companion §4.9):
// this is for listening, not for parity renders.
class TestInput {
 public:
  enum class Source : int { Live = 0, FileLoop = 1, Pluck = 2 };

  TestInput();
  ~TestInput();
  TestInput(const TestInput&) = delete;
  TestInput& operator=(const TestInput&) = delete;

  void   SetSource(Source s) noexcept { source_.store(static_cast<int>(s), std::memory_order_relaxed); }
  Source GetSource() const noexcept { return static_cast<Source>(source_.load(std::memory_order_relaxed)); }

  // Message thread. Decodes the whole file (WAV, AIFF, FLAC, Ogg; at most 10 minutes)
  // off the audio thread and publishes it lock-free; playback never waits.
  bool         LoadFile(const juce::File& file, juce::String& error);
  juce::String LoadedName() const;  // message thread
  // Message thread: frees loops the audio thread can no longer be reading.
  void CollectGarbage();

  // Non-RT, while the audio thread is stopped (prepareToPlay).
  void Prepare(double sampleRate) noexcept;
  // Audio thread. Overwrites l and r with n frames unless the source is Live; returns
  // whether it did. No allocation, no locks.
  bool Render(float* l, float* r, int n) noexcept;

 private:
  struct Loop {
    juce::AudioBuffer<float> audio;  // stereo
    double                   sampleRate = 48000.0;
    juce::String             name;
  };

  void RenderLoop(const Loop& loop, float* l, float* r, int n) noexcept;
  void RenderPluck(float* l, float* r, int n) noexcept;
  void StartPluck() noexcept;
  float NextNoise() noexcept;

  std::atomic<int> source_{static_cast<int>(Source::Live)};

  // Hazard-pointer handoff: the audio thread announces the loop it reads in inUse_ and
  // re-checks current_, so the message thread never frees a loop being played.
  std::atomic<Loop*>                 current_{nullptr};
  std::atomic<Loop*>                 inUse_{nullptr};
  std::unique_ptr<Loop>              owned_;    // message thread: the published loop
  std::vector<std::unique_ptr<Loop>> retired_;  // message thread: replaced loops
  const Loop*                        lastLoop_  = nullptr;  // audio thread: compared only
  double                             loopPhase_ = 0.0;

  double             sampleRate_ = 48000.0;
  std::vector<float> line_;  // Karplus-Strong delay line, sized once in the constructor
  int                period_       = 1;
  int                lineIndex_    = 0;
  int                untilNext_    = 0;
  uint32_t           patternIndex_ = 0;
  uint32_t           rng_          = 0x9E3779B9u;
};

}  // namespace brainscape::plugin
