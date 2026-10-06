#pragma once
#include <mutex>
#include <thread>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include "StateCodec.h"
#include "brainscape/Params.h"

namespace brainscape::plugin {

// The app's offline audition render (companion §4.9): an input rendered through a preset on
// an engine of its own, from the exact-restart state (the canonical configuration, Init,
// then LoadPreset(Exact)), at 48 kHz in the pedal's 48-frame blocks, with no events. The
// input passes through ConditionInput24 and the input mode first, as the pedal's codec and
// input jacks hand it over. The output is the engine's float32, written to a WAV beside a
// recipe that records what produced it.
struct AuditionInput {
  std::vector<float> l, r;          // at 48 kHz, the silent tail included
  juce::String       description;   // what it is, for the recipe
  double             sourceRate = 48000.0;
  // Converted from another rate by platform code, so identical only on this machine.
  bool               converted = false;
};

inline constexpr int    kAuditionBlock        = 48;   // the pedal's block
inline constexpr double kAuditionTailSeconds  = 4.0;  // silence after the input, for the trails
inline constexpr double kAuditionSignalSeconds = 10.0;

// dsp/'s test signal (determinism profile §5.13): its plucks vector, integer-generated, so
// the input bits are the same on every machine.
AuditionInput TestSignalInput();
// A decoded file (one channel or two) at its own rate, converted to 48 kHz by linear
// interpolation when it is not already there.
AuditionInput FileInput(const juce::AudioBuffer<float>& audio, double sampleRate,
                        const juce::String& name);

struct AuditionOutput {
  std::vector<float> l, r;
};

// Conditions `input` in place and renders it. False when the engine cannot be set up.
bool RenderAudition(const float* preset, InputMode mode, AuditionInput& input,
                    AuditionOutput& out);

// SHA-256 of the interleaved little-endian float32 frames, the golden harness's byte stream.
juce::String InterleavedSha256(const std::vector<float>& l, const std::vector<float>& r);

// Runs one render at a time on a worker thread: renders, writes the 32-bit float WAV and
// `<name>.recipe.json` beside it.
class AuditionJob {
 public:
  enum class State { Idle, Running, Done, Failed };
  struct Result {
    State        state = State::Idle;
    juce::String message;      // what the panel shows
    juce::File   wav;
    juce::String outputSha256;
  };

  ~AuditionJob();

  // Message thread. False while a render runs.
  bool   Start(const juce::File& wav, const float* preset, InputMode mode, AuditionInput input);
  Result Get() const;

 private:
  void Run(juce::File wav, std::vector<float> preset, InputMode mode, AuditionInput input);
  void Finish(const Result& r);

  std::thread        thread_;
  mutable std::mutex mutex_;
  Result             result_;
};

}  // namespace brainscape::plugin
