#pragma once
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include "Inputs.h"  // tools/audition: S0's inputs
#include "Render.h"  // tools/audition: the shared offline render
#include "StateCodec.h"
#include "brainscape/Params.h"
#include "brainscape/PresetState.h"

namespace brainscape::plugin {

// The app's offline audition render (companion §4.9): an input rendered through a preset on
// an engine of its own, from the exact-restart state (the canonical configuration, Init,
// then LoadPreset(Exact)), at 48 kHz in the pedal's 48-frame blocks, with no events. The
// input passes through ConditionInput24 and the input mode first, as the pedal's codec and
// input jacks hand it over. The render, its hashes and its recipe are tools/audition's
// (mode-compiler.md §8.1, §9.1), shared with `bspc render`; this layer adds what needs JUCE:
// decoding a file, the WAV writer and the worker thread. The output is the engine's float32,
// written to a WAV beside a recipe that records what produced it.
struct AuditionInput {
  std::vector<float> l, r;          // at 48 kHz, the silent tail included
  juce::String       description;   // what it is, for the recipe
  double             sourceRate = 48000.0;
  // Converted from another rate by platform code, so identical only on this machine.
  bool               converted = false;
  juce::String       vector;        // the test-signal vector, or empty for a file
  uint32_t           signalFrames = 0;  // frames the input sounds in, before the tail
};

inline constexpr int    kAuditionBlock         = static_cast<int>(bsa::kPedalBlock);
// The silence after the input, for the trails, and the test signal's length: `bspc render`'s
// (tools/audition, Inputs.h), so the app's test-signal audition in stereo is S0's
// `S0.engaged.plucks`, one hash on the command line and in the app.
inline constexpr double kAuditionTailSeconds   = 10.0;
inline constexpr double kAuditionSignalSeconds = 10.0;
static_assert(kAuditionTailSeconds * bsa::kRate == bsa::kTailFrames, "the app's tail is S0's");
static_assert(kAuditionSignalSeconds * bsa::kRate == bsa::kSignalFrames, "the app's test signal is S0's");

// dsp/'s test signal (determinism profile §5.13): its plucks vector, integer-generated, so
// the input bits are the same on every machine; S0's input (bsa::VectorInput(Plucks)).
AuditionInput TestSignalInput();
// A decoded file (one channel or two) at its own rate, converted to 48 kHz by linear
// interpolation when it is not already there.
AuditionInput FileInput(const juce::AudioBuffer<float>& audio, double sampleRate,
                        const juce::String& name);

struct AuditionOutput {
  std::vector<float> l, r;
};

// Conditions `input` in place and renders it. False when the engine cannot be set up or the
// preset does not load.
bool RenderAudition(const PresetState& preset, InputMode mode, AuditionInput& input,
                    AuditionOutput& out);
// The same for a leaf-only preset (the default mode): every leaf by ordinal, kNumLeafParams
// values (Params.h kLeafParams), as the processor's parameters hold them today.
bool RenderAudition(const float* preset, InputMode mode, AuditionInput& input,
                    AuditionOutput& out);

// SHA-256 of the interleaved little-endian float32 frames, the golden harness's byte stream.
juce::String InterleavedSha256(const std::vector<float>& l, const std::vector<float>& r);
// The same per 1 s segment, the last one short (PARITY's segmented hash, companion §7.4; the
// golden harness's per-second hashes): the first differing second of two renders.
juce::StringArray SegmentSha256(const std::vector<float>& l, const std::vector<float>& r);

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
  bool   Start(const juce::File& wav, const PresetState& preset, InputMode mode, AuditionInput input);
  bool   Start(const juce::File& wav, const float* preset, InputMode mode, AuditionInput input);
  Result Get() const;

 private:
  void Run(juce::File wav, std::shared_ptr<const PresetState> preset, InputMode mode,
           AuditionInput input);
  void Finish(const Result& r);

  std::thread        thread_;
  mutable std::mutex mutex_;
  Result             result_;
};

}  // namespace brainscape::plugin
