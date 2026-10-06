#include "PluginProcessor.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

#include "PluginEditor.h"

namespace brainscape::plugin {

namespace {

static_assert(kNumParams < 32, "touched_ holds one bit per parameter");

// Stand-in for the profile's SanitizeInput (profile §3.7: NaN and ±inf -> +0, every
// finite value unchanged, integer tests only). Replace with dsp/'s exported function
// once InputCondition.h lands.
inline float SanitizeSample(float x) noexcept {
  uint32_t u = 0;
  std::memcpy(&u, &x, sizeof u);
  return (u & 0x7F800000u) == 0x7F800000u ? 0.0f : x;
}

inline float GainFromDb(float db) noexcept {
  return db == 0.f ? 1.f : static_cast<float>(std::pow(10.0, db / 20.0));
}

inline float Peak(const float* x, int n) noexcept {
  float p = 0.f;
  for (int i = 0; i < n; ++i) p = std::max(p, std::fabs(x[i]));
  return p;
}

inline void RaisePeak(std::atomic<float>& meter, float p) noexcept {
  if (p > meter.load(std::memory_order_relaxed)) meter.store(p, std::memory_order_relaxed);
}

}  // namespace

BrainscapeProcessor::BrainscapeProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)) {
  for (size_t i = 0; i < kNumParams; ++i) {
    auto p     = std::make_unique<BrainscapeParam>(kParamTable[i].id, sink_);
    params_[i] = p.get();
    sent_[i]   = p->Plain();
    addParameter(p.release());
  }
  auto f  = std::make_unique<FreezeParam>(sink_);
  freeze_ = f.get();
  addParameter(f.release());
  setLatencySamples(0);  // dry is never delayed (§4.5); the resampled mode will report its own
}

BrainscapeProcessor::~BrainscapeProcessor() = default;

double BrainscapeProcessor::getTailLengthSeconds() const {
  return std::numeric_limits<double>::infinity();
}

bool BrainscapeProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
  const auto in  = layouts.getMainInputChannelSet();
  const auto out = layouts.getMainOutputChannelSet();
  const bool monoOrOff = in == juce::AudioChannelSet::mono() || in.isDisabled();
  if (out == juce::AudioChannelSet::stereo()) return monoOrOff || in == juce::AudioChannelSet::stereo();
  if (out == juce::AudioChannelSet::mono()) return monoOrOff;
  return false;
}

void BrainscapeProcessor::prepareToPlay(double sampleRate, int maximumExpectedSamplesPerBlock) {
  const std::lock_guard<std::mutex> lock(controlMutex_);
  hostRate_.store(sampleRate, std::memory_order_relaxed);
  maxHostBlock_.store(maximumExpectedSamplesPerBlock, std::memory_order_relaxed);
  // A re-prepare at the same rate keeps the ring and the running engine (§4.1).
  if (!engineReady_ || sampleRate != engineRate_) InitEngine(sampleRate);
  testInput_.Prepare(sampleRate);
  setLatencySamples(0);
}

void BrainscapeProcessor::InitEngine(double sampleRate) {
  // TODO(companion §4.2): at other host rates the engine must stay at 48 kHz behind an
  // r8brain-free-src resampler (reported latency). Until then it runs natively at the
  // host rate, and the status says the output is not pedal-exact.
  EngineConfig cfg;
  cfg.sampleRate      = sampleRate;
  cfg.maxBlockSize    = kMaxChunk;
  cfg.historyFrames   = 1u << 22;
  cfg.looperFrames    = 0;
  cfg.stereoInput     = true;
  cfg.ditherRingWrite = true;
  // Off the audio thread: 16.9 MiB, every page touched by Init's ring clear.
  auto arenas  = std::make_unique<host::HeapArenas>(PlanMemory(cfg));
  engineReady_ = arenas->ok() && engine_.Init(cfg, arenas->get());
  arenas_      = std::move(arenas);
  engineRate_  = sampleRate;
  statusEngineRate_.store(engineReady_ ? sampleRate : 0.0, std::memory_order_relaxed);
  statusReady_.store(engineReady_, std::memory_order_relaxed);
  if (engineReady_) PushAllAfterInit();
}

void BrainscapeProcessor::PushAllAfterInit() {
  // Init resets every parameter and clears freeze (Engine.cpp Init), so the wrapper's
  // values go back in, and Reset snaps the smoothers to them: the start state a preset
  // render begins from.
  for (size_t i = 0; i < kNumParams; ++i) {
    sent_[i] = params_[i]->Plain();
    engine_.SetParam(kParamTable[i].id, sent_[i]);
  }
  engine_.SetFreeze(freeze_->get());
  engine_.Reset();
  stateApplied_ = statePosted_.load(std::memory_order_acquire);  // the mirrors hold it
}

void BrainscapeProcessor::reset() {
  if (engineReady_) engine_.Reset();  // keeps the ring (§4.9)
}

void BrainscapeProcessor::TriggerFromUi() noexcept {
  sink_.Post({WrapperEvent::Type::Trigger, WrapperEvent::Source::Ui, 0u, 1.0f});
}

void BrainscapeProcessor::SetSettings(const WrapperSettings& s) noexcept {
  inputMode_.store(static_cast<int>(s.inputMode), std::memory_order_relaxed);
  inputGainDb_.store(CanonicalGainDb(s.inputGainDb), std::memory_order_relaxed);
  outputGainDb_.store(CanonicalGainDb(s.outputGainDb), std::memory_order_relaxed);
}

WrapperSettings BrainscapeProcessor::GetSettings() const noexcept {
  WrapperSettings s;
  s.inputMode    = static_cast<InputMode>(inputMode_.load(std::memory_order_relaxed));
  s.inputGainDb  = inputGainDb_.load(std::memory_order_relaxed);
  s.outputGainDb = outputGainDb_.load(std::memory_order_relaxed);
  return s;
}

BrainscapeProcessor::Status BrainscapeProcessor::GetStatus() const noexcept {
  Status s;
  s.hostRate        = hostRate_.load(std::memory_order_relaxed);
  s.engineRate      = statusEngineRate_.load(std::memory_order_relaxed);
  s.engineReady     = statusReady_.load(std::memory_order_relaxed);
  s.pedalExact      = s.engineReady && s.engineRate == kPedalRate;
  s.lastHostBlock   = lastHostBlock_.load(std::memory_order_relaxed);
  s.maxHostBlock    = maxHostBlock_.load(std::memory_order_relaxed);
  s.droppedEvents   = sink_.Overflows();
  s.lastLoadInexact = lastLoadInexact_.load(std::memory_order_relaxed);
  return s;
}

// ── State ──────────────────────────────────────────────────────────────────────────

void BrainscapeProcessor::getStateInformation(juce::MemoryBlock& destData) {
  WrapperState state{};
  {
    const std::lock_guard<std::mutex> lock(controlMutex_);
    for (size_t i = 0; i < kNumParams; ++i) state.plain[i] = params_[i]->Plain();
    state.settings = GetSettings();
  }
  std::vector<uint8_t> bytes;
  EncodeState(state, bytes);
  destData.replaceAll(bytes.data(), bytes.size());
}

void BrainscapeProcessor::setStateInformation(const void* data, int sizeInBytes) {
  WrapperState state{};
  if (sizeInBytes <= 0 || !DecodeState(data, static_cast<size_t>(sizeInBytes), state)) return;
  {
    const std::lock_guard<std::mutex> lock(controlMutex_);
    PostStateUnit(state.plain);
    for (size_t i = 0; i < kNumParams; ++i) params_[i]->StoreMirror(state.plain[i]);
    SetSettings(state.settings);
    lastLoadInexact_.store(state.unknownIds + state.missingIds > 0u, std::memory_order_relaxed);
  }
  // Outside the lock, since a host may call back in. Each inner setValue sees its own
  // normalised view and returns at once (companion §5.3). Freeze never loads engaged.
  for (BrainscapeParam* p : params_) p->setValueNotifyingHost(p->getValue());
  freeze_->setValueNotifyingHost(0.0f);
}

void BrainscapeProcessor::PostStateUnit(const float* plain) noexcept {
  const uint32_t seq = stateSeq_.load(std::memory_order_relaxed);
  stateSeq_.store(seq + 1u, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  for (size_t i = 0; i < kNumParams; ++i) stateSlot_[i].store(plain[i], std::memory_order_relaxed);
  stateSeq_.store(seq + 2u, std::memory_order_release);
  statePosted_.fetch_add(1u, std::memory_order_release);
}

void BrainscapeProcessor::ApplyStateUnit() noexcept {
  const uint32_t posted = statePosted_.load(std::memory_order_acquire);
  if (posted == stateApplied_) return;
  const uint32_t seq = stateSeq_.load(std::memory_order_acquire);
  if ((seq & 1u) != 0u) return;  // being written: the next block takes it
  float plain[kNumParams];
  for (size_t i = 0; i < kNumParams; ++i) plain[i] = stateSlot_[i].load(std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_acquire);
  if (stateSeq_.load(std::memory_order_relaxed) != seq) return;
  for (size_t i = 0; i < kNumParams; ++i) {
    sent_[i] = plain[i];
    engine_.SetParam(kParamTable[i].id, plain[i]);
  }
  touched_ = (1u << kNumParams) - 1u;
  engine_.SetFreeze(false);
  stateApplied_ = posted;
}

// ── Events ─────────────────────────────────────────────────────────────────────────

void BrainscapeProcessor::DrainEvents() noexcept {
  hostCount_ = uiCount_ = 0;
  WrapperEvent e;
  for (size_t k = 0; k < WrapperQueue::capacity() && sink_.Queue().Pop(e); ++k) {
    if (e.source == WrapperEvent::Source::Host) {
      hostEvents_[hostCount_++] = e;
    } else {
      uiEvents_[uiCount_++] = e;
    }
  }
}

void BrainscapeProcessor::ApplyEvent(const WrapperEvent& e) noexcept {
  switch (e.type) {
    case WrapperEvent::Type::Param:
      if (e.id >= 1u && e.id <= kNumParams) {
        sent_[e.id - 1u] = e.value;
        touched_ |= 1u << (e.id - 1u);
        engine_.SetParam(static_cast<ParamId>(e.id), e.value);
      }
      break;
    case WrapperEvent::Type::Freeze:
      engine_.SetFreeze(e.value >= 0.5f);
      break;
    case WrapperEvent::Type::Trigger:
      engine_.Trigger(Engine::TriggerSource::Footswitch);
      break;
  }
}

void BrainscapeProcessor::ApplyMidi(const uint8_t* data, int numBytes) noexcept {
  // Note-on is a trigger event (companion §5.7). Raw bytes: building a MidiMessage can
  // allocate for long SysEx.
  if (numBytes >= 3 && (data[0] & 0xF0u) == 0x90u && data[2] > 0u) {
    engine_.Trigger(Engine::TriggerSource::MidiNote, static_cast<float>(data[2]) / 127.0f);
  }
}

void BrainscapeProcessor::WriteBackMirrors() noexcept {
  // The mirrors follow what the engine was sent, so two producers racing on one
  // parameter settle on the engine's value within a block.
  for (size_t i = 0; touched_ != 0u && i < kNumParams; ++i) {
    if ((touched_ & (1u << i)) != 0u) params_[i]->StoreMirror(sent_[i]);
  }
  touched_ = 0u;
}

// ── Audio ──────────────────────────────────────────────────────────────────────────

void BrainscapeProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) {
  // No juce::ScopedNoDenormals: the engine's own guard owns the FP control word around
  // every entry point (companion §4.6, profile §4.1).
  const int numSamples = buffer.getNumSamples();
  if (numSamples > 0) lastHostBlock_.store(numSamples, std::memory_order_relaxed);

  // Frame 0 of the block, in the fixed same-frame order (§4.7): state load, host
  // automation, MIDI, UI.
  if (engineReady_) {
    ApplyStateUnit();
    DrainEvents();
    for (size_t k = 0; k < hostCount_; ++k) ApplyEvent(hostEvents_[k]);
  }
  auto       midiIt  = midi.cbegin();
  const auto midiEnd = midi.cend();
  for (; midiIt != midiEnd && (*midiIt).samplePosition <= 0; ++midiIt) {
    if (engineReady_) ApplyMidi((*midiIt).data, (*midiIt).numBytes);
  }
  if (engineReady_) {
    for (size_t k = 0; k < uiCount_; ++k) ApplyEvent(uiEvents_[k]);
    if (sink_.TakeResync()) {  // the queue overflowed: re-send every mirror
      for (size_t i = 0; i < kNumParams; ++i) {
        ApplyEvent({WrapperEvent::Type::Param, WrapperEvent::Source::Ui,
                    static_cast<uint32_t>(kParamTable[i].id), params_[i]->Plain()});
      }
      engine_.SetFreeze(freeze_->get());
    }
    WriteBackMirrors();
  }

  const int numOut = std::min(getTotalNumOutputChannels(), buffer.getNumChannels());
  const int numIn  = std::min(getTotalNumInputChannels(), buffer.getNumChannels());
  // Zero-frame calls (VST3 sends them whenever buses exist): parameters are applied
  // above; Process is skipped (§4.3).
  if (numSamples <= 0) return;
  if (!engineReady_ || numOut == 0) {
    buffer.clear();
    return;
  }

  // The host buffer is in place (input channel c is output channel c). The input is
  // copied to scratch before Process writes, so no layout can alias (§4.4). A missing or
  // disabled input bus feeds zeros, never a null pointer (§4.3).
  const float* hostInL = numIn > 0 ? buffer.getReadPointer(0) : nullptr;
  const float* hostInR = numIn > 1 ? buffer.getReadPointer(1) : hostInL;
  float*       outL    = buffer.getWritePointer(0);
  float*       outR    = numOut > 1 ? buffer.getWritePointer(1) : nullptr;

  const float inDb = inputGainDb_.load(std::memory_order_relaxed);
  if (inDb != inGainDb_) {
    inGainDb_ = inDb;
    inGain_   = GainFromDb(inDb);
  }
  const float outDb = outputGainDb_.load(std::memory_order_relaxed);
  if (outDb != outGainDb_) {
    outGainDb_ = outDb;
    outGain_   = GainFromDb(outDb);
  }

  // Sub-blocks of at most 512 frames, split at every MIDI event's frame (§4.3, §4.10).
  int frame = 0;
  while (frame < numSamples) {
    for (; midiIt != midiEnd && (*midiIt).samplePosition <= frame; ++midiIt) {
      ApplyMidi((*midiIt).data, (*midiIt).numBytes);
    }
    int end = std::min(numSamples, frame + static_cast<int>(kMaxChunk));
    if (midiIt != midiEnd) end = std::min(end, (*midiIt).samplePosition);
    RenderChunk(hostInL, hostInR, outL, outR, frame, end - frame);
    frame = end;
  }
  for (; midiIt != midiEnd; ++midiIt) ApplyMidi((*midiIt).data, (*midiIt).numBytes);

  for (int c = 2; c < numOut; ++c) buffer.clear(c, 0, numSamples);
  onsets_.fetch_add(engine_.ConsumeOnsetCount(), std::memory_order_relaxed);
}

void BrainscapeProcessor::RenderChunk(const float* hostInL, const float* hostInR, float* outL,
                                      float* outR, int offset, int numFrames) noexcept {
  float* l = inL_.data();
  float* r = inR_.data();
  if (!testInput_.Render(l, r, numFrames)) {
    if (hostInL != nullptr) {
      std::copy(hostInL + offset, hostInL + offset + numFrames, l);
      std::copy(hostInR + offset, hostInR + offset + numFrames, r);
    } else {
      std::fill(l, l + numFrames, 0.f);
      std::fill(r, r + numFrames, 0.f);
    }
  }
  // Input level, then the live input function (§4.8). At 0 dB the input passes bit-exact.
  for (int i = 0; i < numFrames; ++i) {
    l[i] = SanitizeSample(inGain_ == 1.f ? l[i] : l[i] * inGain_);
    r[i] = SanitizeSample(inGain_ == 1.f ? r[i] : r[i] * inGain_);
  }
  if (static_cast<InputMode>(inputMode_.load(std::memory_order_relaxed)) == InputMode::Mono) {
    std::copy(l, l + numFrames, r);  // a guitar on input 1 must not feed R = 0
  }
  RaisePeak(inPeak_, std::max(Peak(l, numFrames), Peak(r, numFrames)));

  const float*           ins[2]  = {l, r};
  float*                 outs[2] = {outL + offset, outR != nullptr ? outR + offset : outRScratch_.data()};
  Engine::ProcessContext ctx;
  ctx.in        = ins;
  ctx.out       = outs;
  ctx.numFrames = static_cast<uint32_t>(numFrames);
  engine_.Process(ctx);

  if (outGain_ != 1.f) {
    for (int i = 0; i < numFrames; ++i) {
      outs[0][i] *= outGain_;
      outs[1][i] *= outGain_;
    }
  }
  RaisePeak(outPeak_, std::max(Peak(outs[0], numFrames), Peak(outs[1], numFrames)));
}

juce::AudioProcessorEditor* BrainscapeProcessor::createEditor() { return new BrainscapeEditor(*this); }

}  // namespace brainscape::plugin

// The factory every JUCE format wrapper calls.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
  return new brainscape::plugin::BrainscapeProcessor();
}
