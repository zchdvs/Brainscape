#pragma once
#include <array>
#include <atomic>
#include <bitset>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>

#include "Audition.h"
#include "BrainscapeParam.h"
#include "EventQueue.h"
#include "StateCodec.h"
#include "TestInput.h"
#include "brainscape/Engine.h"
#include "brainscape/HostArenas.h"
#include "brainscape/Preset.h"

namespace brainscape::plugin {

class CurationSession;  // Curation.h

// The structure a preset plays besides its leaves (mode-compiler.md §5.1): the mode, CTRL
// (macro positions and expression assignments) and the stored performance state, as a decoded
// package holds them. Default-constructed it is the default mode, which a leaf-only preset
// plays. Fixed-size, trivially copyable data: state units carry it as words (§9.2).
struct ModeState {
  ModeBlob         mode;
  ControlState     control;
  PerformanceState performance;
  uint32_t         soundRev = 0;  // the package's sound_rev, 0 when not from a package
};

// Where the preset the wrapper plays came from, for the editor's header and the session: the
// factory package it is (FactoryModes.h, by index; -1 for a document file, an unnamed state or
// the default mode), its name ("" for the default mode and unnamed states) and family.
struct PresetSource {
  int          factory = -1;
  juce::String name;
  PresetFamily family  = PresetFamily::None;
};

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
  void setNonRealtime(bool isNonRealtime) noexcept override;
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
  // One parameter per Leaf row (mode-compiler.md §4.1); `id` must be one (Params.h IsLeaf).
  BrainscapeParam& Param(ParamId id) noexcept {
    jassert(IsLeaf(id));
    return *params_[LeafIndex(id)];
  }
  FreezeParam&     Freeze() noexcept { return *freeze_; }
  // The other registered rows (§9.2, host model (b), §3.6): a Macro row (69-76), whose moves
  // are MacroMove events; perf.expression (78), an Expression event; the effect volume (82),
  // a Global row the engine keeps across loads and restarts.
  BrainscapeParam& Macro(ParamId id) noexcept {
    jassert(IsMacroRow(id));
    return *macros_[MacroIndex(id)];
  }
  BrainscapeParam& Expression() noexcept { return *expression_; }
  BrainscapeParam& EffectVolume() noexcept { return *effectVolume_; }
  // Any registered row's parameter but freeze's, or null.
  BrainscapeParam* FindHostParam(ParamId id) noexcept;
  TestInput&       GetTestInput() noexcept { return testInput_; }

  static constexpr bool IsMacroRow(ParamId id) noexcept {
    return static_cast<uint32_t>(id) >= static_cast<uint32_t>(ParamId::MacroActivity) &&
           static_cast<uint32_t>(id) <= static_cast<uint32_t>(ParamId::MacroAux2);
  }
  static constexpr size_t MacroIndex(ParamId id) noexcept {
    return static_cast<uint32_t>(id) - static_cast<uint32_t>(ParamId::MacroActivity);
  }

  // Message thread: plays a decoded preset (a compiled document or package) as a state restore
  // does (§9.2): a Spillover load with Trails at the next block's first frame while audio runs,
  // Exact when nothing has played since the last Init or restart. The leaf mirrors take its
  // leaves (a leaf it lacks, its default), the macro mirrors its CTRL positions, which are
  // pickup references never re-applied (§3.5), and freeze goes off. Refused, with nothing
  // changed, when its mode is invalid; *report says how faithfully it loads. `source` says
  // what it is (CurrentSource): a session saved while a factory package plays restores it.
  bool LoadPresetState(const PresetState& state, LoadReport* report = nullptr,
                       const PresetSource& source = {});
  // What the last load or session restore played, by name (any thread but the audio thread).
  PresetSource CurrentSource() const;
  // What the wrapper plays now, as a preset: the leaf mirrors, the current mode, and CTRL with
  // the macro mirrors as its positions. Not on the audio thread.
  std::unique_ptr<PresetState> CurrentPreset() const;
  ModeState                    CurrentMode() const;
  // Message thread: counts the loads that replaced the preset (LoadPresetState and session
  // restores), so an editor can lock its knobs for pickup after each (mode-compiler.md §3.5).
  uint32_t LoadSerial() const noexcept { return loadSerial_.load(std::memory_order_relaxed); }

  // The curation slice's document (mode-compiler.md §9.1): message thread only.
  CurationSession& Curation() noexcept { return *curation_; }
  // The editor's view (BrainscapeEditor::View), kept while the editor is closed. Message thread.
  int  EditorView() const noexcept { return editorView_; }
  void SetEditorView(int view) noexcept { editorView_ = view; }

  // A monitoring trim on the output, after the output level: the curation slice's level-matched
  // A/B (§9.1). Never saved, never part of a preset or a render. Any thread.
  void  SetMonitorTrimDb(float db) noexcept;
  float MonitorTrimDb() const noexcept { return monitorTrimDb_.load(std::memory_order_relaxed); }

  // A momentary footswitch-style trigger (companion §5.7), applied at the next block.
  void TriggerFromUi() noexcept;

  // Scripted producers: applies `e` at absolute engine frame `frame`, counted from the
  // last Init or restart (companion §4.10); a stamp made before a restart is void. A
  // parameter value is canonicalized here, as every producer's is, and the mirror follows
  // when the event applies. Any thread.
  void PostAt(uint64_t frame, WrapperEvent e) noexcept;

  void            SetSettings(const WrapperSettings& s) noexcept;
  WrapperSettings GetSettings() const noexcept;

  // What the last transport start did while "Restart on transport start" was on (§4.9).
  enum class TransportStart : uint8_t { None, Restarted, SpareNotReady };

  struct Status {
    double   hostRate      = 0.0;  // 0 until prepareToPlay
    double   engineRate    = 0.0;
    bool     engineReady   = false;
    // Engine at the pedal's 48 kHz. Pedal-exact only for renders from the exact-restart
    // state: an offline audition, or a bounce with the restart option (§4.9).
    bool     pedalRate     = false;
    int      lastHostBlock = 0;
    int      maxHostBlock  = 0;
    uint32_t droppedEvents = 0;  // lost events only (EventSink)
    bool     lastLoadInexact = false;
    bool           restartOnStart = false;
    // A spare engine restarted with the preset the live engine plays: a transport start
    // whose first block moves no parameter now restarts.
    bool           spareReady     = false;
    TransportStart lastStart      = TransportStart::None;
    uint64_t       engineCalls    = 0;  // Process calls since prepareToPlay: one per <= 512 frames
  };
  Status GetStatus() const noexcept;

  // GUI polling: onsets since the last call (companion §4.7) and peak levels since the
  // last call (linear, after the input functions / after the output level).
  uint32_t ConsumeOnsets() noexcept { return onsets_.exchange(0u, std::memory_order_relaxed); }
  float    ConsumeInputPeak() noexcept { return inPeak_.exchange(0.f, std::memory_order_relaxed); }
  float    ConsumeOutputPeak() noexcept { return outPeak_.exchange(0.f, std::memory_order_relaxed); }

  // Message thread: renders the test input (the loaded file in File loop, dsp/'s test signal
  // otherwise) through the current preset into `wav` on a worker thread (§4.9).
  bool                StartAudition(const juce::File& wav, juce::String& error);
  AuditionJob::Result GetAudition() const { return audition_.Get(); }

  // Tests: while the returned lock is held, the spare engine's worker does nothing.
  std::unique_lock<std::mutex> PauseSpareWorker() { return std::unique_lock<std::mutex>(spareMutex_); }

 private:
  // An engine with its arenas, and the preset values its last Exact load applied. Values
  // are every leaf by ordinal (Params.h kLeafParams), as a complete preset holds them.
  // And the effect volume it started with: a Global row, which the engine keeps across loads,
  // so a spare restarted with another one would not play what the live engine plays.
  struct EngineSlot {
    Engine                             engine;
    std::unique_ptr<host::HeapArenas>  arenas;
    std::array<float, kNumLeafParams>  preset{};
    ModeState                          mode;
    float                              effectVolume = 0.f;
  };
  using Values = std::array<float, kNumLeafParams>;
  static constexpr size_t kModeWords = sizeof(ModeState) / sizeof(uint32_t);

  void     InitEngine(double sampleRate);
  void     LoadAfterRestart() noexcept;
  void     RestartTimeline() noexcept;
  uint32_t NextGeneration() noexcept;
  void     PostStateUnit(const float* plain, const ModeState& mode) noexcept;
  void     SetMacroMirrors(const ControlState& control) noexcept;
  void     NotifyHostOfMirrors();
  // Applies `e`'s effect on the leaves to `values` (a SetParam on a leaf, or the fan-out of a
  // MacroMove or Expression through the active mode), marking each leaf it writes in *touched.
  // Audio thread.
  void     Track(const WrapperEvent& e, Values& values, std::bitset<kNumLeafParams>* touched) const noexcept;
  void     ApplyStateUnit() noexcept;
  void     CheckTransportStart() noexcept;
  void     RestartAtTransportStart() noexcept;
  void     DrainEvents() noexcept;
  void     InsertPending(const WrapperEvent& e) noexcept;
  void     EmitDue(uint64_t frame, uint64_t blockStart, uint64_t chunkStart, size_t* pending,
                   const juce::MidiBuffer& midi, juce::MidiBufferIterator* midiIt, bool* frameZero) noexcept;
  void     EmitPending(size_t from, size_t to, WrapperEvent::Source rank, uint32_t offset) noexcept;
  bool     Applies(const WrapperEvent& e) const noexcept;
  void     EmitLive(const WrapperEvent* events, size_t count, uint32_t offset) noexcept;
  void     Emit(const WrapperEvent& e, uint32_t offset) noexcept;
  void     Emit(const Engine::BlockEvent& e) noexcept;
  void     WriteBackMirrors() noexcept;
  void     RenderChunk(const float* hostInL, const float* hostInR, float* outL, float* outR,
                       int offset, int numFrames) noexcept;

  // The spare engine of §4.7 and §4.9, prepared by a worker thread.
  void EnsureSpareWorker();
  void SpareWorkerLoop();
  void PrepareSpare();
  void FreeSpare() noexcept;

  EventSink                                        sink_;
  std::array<BrainscapeParam*, kNumLeafParams>     params_{};  // by leaf ordinal; owned by
                                                               // AudioProcessor
  FreezeParam*                                     freeze_ = nullptr;
  std::array<BrainscapeParam*, kMaxMacros>         macros_{};  // by macro id - 69
  BrainscapeParam*                                 expression_   = nullptr;
  BrainscapeParam*                                 effectVolume_ = nullptr;

  // The mode the wrapper's preset plays (message side): written under controlMutex_ and
  // modeMutex_, read under either. The audio thread plays its own copy, activeMode_, which
  // state units update; the spare's worker reads this under modeMutex_ alone, which is never
  // held while another lock is taken.
  ModeState          mode_;
  mutable std::mutex modeMutex_;
  PresetSource       source_;  // what mode_ came from: under controlMutex_

  // Two slots at most: the live engine and, while the restart option is on, the spare. The
  // audio thread swaps the two pointers at a transport start; slots are allocated and
  // freed only off the audio thread, with the spare's state or the audio thread's absence
  // ruling out a concurrent swap.
  std::array<std::unique_ptr<EngineSlot>, 2> slots_;
  EngineSlot*                                live_  = nullptr;
  EngineSlot*                                spare_ = nullptr;
  EngineConfig                               config_{};
  bool                                       engineReady_ = false;
  double                                     engineRate_  = 0.0;

  // Guards prepare/state calls against each other; never taken on the audio thread.
  // Restore generations advance only under it.
  mutable std::mutex controlMutex_;

  // State restores travel as a unit (companion §4.7): a sequence lock over one slot that
  // also holds the restore's generation. The audio thread applies one as a Spillover load
  // at its next block's first frame (restore_), or folds it into an Exact load.
  std::atomic<uint32_t>                       stateSeq_{0};
  std::atomic<uint32_t>                       stateGen_{0};
  std::array<std::atomic<float>, kNumLeafParams> stateSlot_{};
  std::array<std::atomic<uint32_t>, kModeWords>  modeSlot_{};  // the unit's ModeState, as words
  ModeState                                   activeMode_;   // audio thread: what plays
  uint64_t                                    activeModeHash_ = 0;
  ModeState                                   unitMode_;     // audio thread: a unit being read
  uint32_t                                    seqApplied_ = 0;  // audio thread
  std::unique_ptr<PresetState>                restore_;         // audio thread
  bool                                        loadPending_ = false;
  // Audio thread: parameter and freeze events posted before the last applied restore or
  // Init are dropped (the restore or the Init snapshot replaced them).
  uint32_t                                    generationFloor_ = 0;

  // Stamps count on one engine timeline, from frame 0 at the last Init or restart.
  std::atomic<uint32_t> timeline_{0};
  uint64_t              framePos_ = 0;  // audio thread: the timeline's next frame

  // Audio thread: live events waiting for the next block's first frame (they collect over
  // zero-frame calls), events stamped for later frames (sorted by frame, then arrival),
  // the block events of one Process call, and the values sent.
  std::array<WrapperEvent, WrapperQueue::capacity()> hostEvents_{};
  std::array<WrapperEvent, WrapperQueue::capacity()> uiEvents_{};
  std::array<WrapperEvent, WrapperQueue::capacity()> pending_{};
  size_t                                             hostCount_ = 0, uiCount_ = 0, pendingCount_ = 0;
  std::vector<Engine::BlockEvent>                    blockEvents_;
  size_t                                             numBlockEvents_ = 0;
  uint32_t                                           seq_            = 0;
  Values                                             sent_{};
  std::bitset<kNumLeafParams>                        touched_;  // by leaf ordinal
  float                                              sentEffectVolume_ = 0.f;
  bool                                               effectTouched_    = false;
  // A restore applied this block: the effect volume's mirror goes out after it, since an edit
  // posted before the restore lost to it, and no preset holds a device setting.
  bool                                               resendVolume_     = false;
  bool                                               resync_  = false;  // re-send the mirrors

  // Transport starts (§4.9 c): armed by a non-playing block, prepareToPlay or a switch
  // to offline.
  std::atomic<bool>           restartOnStart_{false};
  std::atomic<bool>           armRequest_{true};
  std::atomic<bool>           offline_{false};  // what setNonRealtime last said
  bool                        startArmed_ = true;  // audio thread
  std::atomic<TransportStart> lastStart_{TransportStart::None};

  // The spare's worker. spareState_ says who may touch spare_: the worker while Empty,
  // Preparing or Retired, the audio thread while Swapping; Ready hands it to whichever
  // claims it first.
  std::atomic<int>        spareState_{0};
  Values                  spareSnapshot_{};  // worker: what the Ready spare was loaded with
  Values                  lastSnapshot_{};   // worker: the preset at its previous pass
  ModeState               spareMode_, lastMode_;      // worker: the same for the mode
  float                   spareVolume_ = 0.f, lastVolume_ = 0.f;  // and the effect volume
  // For the status: hashes of the Ready spare's preset and of the values sent to the live
  // engine.
  std::atomic<uint64_t>   spareHash_{0}, sentHash_{0};
  std::mutex              spareMutex_;       // held through each worker pass and InitEngine
  std::condition_variable spareWake_;
  bool                    spareQuit_ = false;  // under spareMutex_
  std::once_flag          spareOnce_;
  std::thread             spareThread_;

  // Audio thread scratch: sanitized engine input and the right output of a mono bus.
  std::array<float, kMaxChunk> inL_{}, inR_{}, outRScratch_{};
  float                        inGain_ = 1.f, outGain_ = 1.f;
  float                        inGainDb_ = 0.f, outGainDb_ = 0.f;

  std::atomic<int>   inputMode_{static_cast<int>(InputMode::Stereo)};
  std::atomic<float> inputGainDb_{0.f}, outputGainDb_{0.f};
  std::atomic<float> monitorTrimDb_{0.f};
  std::atomic<uint32_t> loadSerial_{0};
  int                   editorView_ = 0;

  std::atomic<double>   hostRate_{0.0};
  std::atomic<double>   statusEngineRate_{0.0};
  std::atomic<bool>     statusReady_{false};
  std::atomic<int>      lastHostBlock_{0}, maxHostBlock_{0};
  std::atomic<bool>     lastLoadInexact_{false};
  std::atomic<uint32_t> onsets_{0};
  std::atomic<uint64_t> engineCalls_{0};
  std::atomic<float>    inPeak_{0.f}, outPeak_{0.f};

  TestInput   testInput_;
  AuditionJob audition_;
  // Last, so it is destroyed first: it refers to the processor.
  std::unique_ptr<CurationSession> curation_;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BrainscapeProcessor)
};

}  // namespace brainscape::plugin
