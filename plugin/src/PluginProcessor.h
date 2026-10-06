#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

#include <juce_audio_processors/juce_audio_processors.h>

#include "BrainscapeParam.h"
#include "EventQueue.h"
#include "StateCodec.h"
#include "TestInput.h"
#include "brainscape/Engine.h"
#include "brainscape/HostArenas.h"

namespace brainscape::plugin {

// The one AudioProcessor behind every format and the companion app (companion §2.1),
// hosting brainscape::Engine under the wrapper obligations of companion §4.12.
class BrainscapeProcessor final : public juce::AudioProcessor {
 public:
  static constexpr uint32_t kMaxChunk  = 512;      // the engine's maxBlockSize, always (§4.1)
  static constexpr double   kPedalRate = 48000.0;  // the pedal's only rate (§2.3)

  BrainscapeProcessor();
  ~BrainscapeProcessor() override;

  void prepareToPlay(double sampleRate, int maximumExpectedSamplesPerBlock) override;
  void releaseResources() override {}
  void reset() override;
  bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
  void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;
  using AudioProcessor::processBlock;

  juce::AudioProcessorEditor* createEditor() override;
  bool                        hasEditor() const override { return true; }

  const juce::String getName() const override { return "Brainscape"; }
  bool               acceptsMidi() const override { return true; }
  bool               producesMidi() const override { return false; }
  bool               isMidiEffect() const override { return false; }
  // Tails run from 0.27 s to forever (§4.5); VST3 maps infinity to kInfiniteTail.
  double getTailLengthSeconds() const override;

  int                getNumPrograms() override { return 1; }
  int                getCurrentProgram() override { return 0; }
  void               setCurrentProgram(int) override {}
  const juce::String getProgramName(int) override { return {}; }
  void               changeProgramName(int, const juce::String&) override {}

  void getStateInformation(juce::MemoryBlock& destData) override;
  void setStateInformation(const void* data, int sizeInBytes) override;

  // ── Wrapper API for the editor and tests ─────────────────────────────────────────
  BrainscapeParam& Param(ParamId id) noexcept { return *params_[static_cast<size_t>(id) - 1u]; }
  FreezeParam&     Freeze() noexcept { return *freeze_; }
  TestInput&       GetTestInput() noexcept { return testInput_; }

  // A momentary footswitch-style trigger (companion §5.7), applied at the next block.
  void TriggerFromUi() noexcept;

  // Scripted producers: applies `e` at absolute engine frame `frame` (frames since the
  // last Init), splitting the host block there (companion §4.10). A parameter value is
  // canonicalized here, as every producer's is, and the mirror follows when the event
  // applies. Any thread.
  void PostAt(uint64_t frame, WrapperEvent e) noexcept;

  void            SetSettings(const WrapperSettings& s) noexcept;
  WrapperSettings GetSettings() const noexcept;

  struct Status {
    double   hostRate      = 0.0;  // 0 until prepareToPlay
    double   engineRate    = 0.0;
    bool     engineReady   = false;
    // Engine at the pedal's 48 kHz. Not "pedal-exact" yet: the parity contract also needs
    // Restart, exact preset loads and frame-stamped host and editor events (plugin/README.md,
    // "Current limitations").
    bool     pedalRate     = false;
    int      lastHostBlock = 0;
    int      maxHostBlock  = 0;
    uint32_t droppedEvents = 0;  // lost events only (EventSink)
    bool     lastLoadInexact = false;
  };
  Status GetStatus() const noexcept;

  // GUI polling: onsets since the last call (companion §4.7) and peak levels since the
  // last call (linear, after the input functions / after the output level).
  uint32_t ConsumeOnsets() noexcept { return onsets_.exchange(0u, std::memory_order_relaxed); }
  float    ConsumeInputPeak() noexcept { return inPeak_.exchange(0.f, std::memory_order_relaxed); }
  float    ConsumeOutputPeak() noexcept { return outPeak_.exchange(0.f, std::memory_order_relaxed); }

 private:
  void InitEngine(double sampleRate);
  void PushAllAfterInit();
  uint32_t NextGeneration() noexcept;
  void PostStateUnit(const float* plain) noexcept;
  void ApplyStateUnit() noexcept;
  void DrainEvents(uint64_t blockStart) noexcept;
  void InsertPending(const WrapperEvent& e) noexcept;
  size_t DuePending(uint64_t frame) const noexcept;
  void ApplyPending(size_t due, WrapperEvent::Source rank) noexcept;
  void ErasePending(size_t due) noexcept;
  void ApplyLive(const WrapperEvent& e) noexcept;
  void ApplyEvent(const WrapperEvent& e) noexcept;
  void ApplyMidi(const uint8_t* data, int numBytes) noexcept;
  void WriteBackMirrors() noexcept;
  void RenderChunk(const float* hostInL, const float* hostInR, float* outL, float* outR,
                   int offset, int numFrames) noexcept;

  EventSink                                        sink_;
  std::array<BrainscapeParam*, kNumParams>         params_{};  // owned by AudioProcessor
  FreezeParam*                                     freeze_ = nullptr;

  Engine                            engine_;
  std::unique_ptr<host::HeapArenas> arenas_;
  bool                              engineReady_ = false;
  double                            engineRate_  = 0.0;

  // Guards prepare/state calls against each other; never taken on the audio thread.
  // Restore generations advance only under it.
  std::mutex controlMutex_;

  // State restores travel as a unit (companion §4.7): a sequence lock over one slot that
  // also holds the restore's generation.
  std::atomic<uint32_t>                       stateSeq_{0};
  std::atomic<uint32_t>                       stateGen_{0};
  std::array<std::atomic<float>, kNumParams>  stateSlot_{};
  uint32_t                                    seqApplied_ = 0;  // audio thread
  // Audio thread: parameter and freeze events posted before the last applied restore or
  // Init are dropped (the restore or the Init snapshot replaced them), and so are stamped
  // events posted before the last Init (their frames counted from the replaced engine).
  uint32_t                                    generationFloor_ = 0;
  uint32_t                                    initGeneration_  = 0;

  // Audio thread: drained events, split by same-frame rank, events stamped for later
  // frames (sorted by frame, then arrival), and the values sent.
  std::array<WrapperEvent, WrapperQueue::capacity()> hostEvents_{};
  std::array<WrapperEvent, WrapperQueue::capacity()> uiEvents_{};
  std::array<WrapperEvent, WrapperQueue::capacity()> pending_{};
  size_t                                             hostCount_ = 0, uiCount_ = 0, pendingCount_ = 0;
  uint64_t                                           framePos_ = 0;  // engine frames since Init
  std::array<float, kNumParams>                      sent_{};
  uint32_t                                           touched_ = 0;  // bit per ParamId - 1

  // Audio thread scratch: sanitized engine input and the right output of a mono bus.
  std::array<float, kMaxChunk> inL_{}, inR_{}, outRScratch_{};
  float                        inGain_ = 1.f, outGain_ = 1.f;
  float                        inGainDb_ = 0.f, outGainDb_ = 0.f;

  std::atomic<int>   inputMode_{static_cast<int>(InputMode::Stereo)};
  std::atomic<float> inputGainDb_{0.f}, outputGainDb_{0.f};

  std::atomic<double>   hostRate_{0.0};
  std::atomic<double>   statusEngineRate_{0.0};
  std::atomic<bool>     statusReady_{false};
  std::atomic<int>      lastHostBlock_{0}, maxHostBlock_{0};
  std::atomic<bool>     lastLoadInexact_{false};
  std::atomic<uint32_t> onsets_{0};
  std::atomic<float>    inPeak_{0.f}, outPeak_{0.f};

  TestInput testInput_;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BrainscapeProcessor)
};

}  // namespace brainscape::plugin
