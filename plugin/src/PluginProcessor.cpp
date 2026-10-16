#include "PluginProcessor.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <type_traits>
#include <vector>

#include "Curation.h"
#include "FactoryModes.h"
#include "PluginEditor.h"
#include "brainscape/InputCondition.h"
#include "brainscape/ModeEval.h"

// The parity negative control builds the engine with floating-point contraction on
// (cmake/BrainscapeFpProfile.cmake). Configure refuses it in plugin builds; this also
// stops a hand-defined macro, which would switch off the /fp:contract tripwire.
#if defined(BRAINSCAPE_FP_NEGATIVE_CONTROL)
#error "BRAINSCAPE_FP_NEGATIVE_CONTROL is test-only: the plugin and app cannot be built with it"
#endif

namespace brainscape::plugin {

namespace {

enum SpareState : int { kSpareEmpty, kSparePreparing, kSpareReady, kSpareSwapping, kSpareRetired };

// While the restart option is on, the worker re-checks the spare this often: after a
// transport start used it, and whenever the preset moved away from it. It restarts the
// spare only with a preset that held still for one interval, not at every step of a knob
// drag (each restart clears 17 MiB).
constexpr auto kSparePoll = std::chrono::milliseconds(10);

// Every Process call's events fit, whatever the producers queued (three queues' worth,
// a resync and a load); MIDI beyond that is counted lost.
constexpr size_t kMaxBlockEvents = 4 * WrapperQueue::capacity();

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

// No padding anywhere (Mode.h and PresetState.h lay their structs out with explicit pads), so
// two copies of one mode compare equal byte for byte.
static_assert(std::is_trivially_copyable<ModeState>::value &&
                  sizeof(ModeState) == sizeof(ModeBlob) + sizeof(ControlState) + sizeof(PerformanceState) +
                                           sizeof(uint32_t) &&
                  sizeof(ModeState) % sizeof(uint32_t) == 0,
              "state units carry the mode as words");

// A complete preset (companion §6.1): every leaf, as the wrapper holds it by ordinal, and the
// mode it plays (mode-compiler.md §9.2).
void ToPreset(const float* values, const ModeState& mode, PresetState& out) noexcept {
  for (size_t i = 0; i < kNumLeafParams; ++i) {
    out.leaves[i] = {static_cast<uint32_t>(LeafId(i)), values[i]};
  }
  out.leafCount   = static_cast<uint32_t>(kNumLeafParams);
  out.soundRev    = mode.soundRev;
  out.mode        = mode.mode;
  out.control     = mode.control;
  out.performance = mode.performance;
}

bool SameBits(const std::array<float, kNumLeafParams>& a,
              const std::array<float, kNumLeafParams>& b) noexcept {
  return std::memcmp(a.data(), b.data(), sizeof(float) * kNumLeafParams) == 0;
}

bool SameMode(const ModeState& a, const ModeState& b) noexcept {
  return std::memcmp(&a, &b, sizeof(ModeState)) == 0;
}

bool SameFloat(float a, float b) noexcept { return std::memcmp(&a, &b, sizeof a) == 0; }

uint64_t Fnv(uint64_t h, const void* bytes, size_t n) noexcept {  // FNV-1a over the bytes
  const auto* p = static_cast<const unsigned char*>(bytes);
  for (size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 0x100000001b3ull;
  }
  return h;
}

uint64_t ModeHash(const ModeState& m) noexcept { return Fnv(0xcbf29ce484222325ull, &m, sizeof m); }

// What a restart starts from: the leaves' bits, the mode and the effect volume.
uint64_t ValuesHash(const std::array<float, kNumLeafParams>& v, uint64_t modeHash, float volume) noexcept {
  uint64_t h = Fnv(0xcbf29ce484222325ull, v.data(), sizeof(float) * kNumLeafParams);
  h          = Fnv(h, &modeHash, sizeof modeHash);
  return Fnv(h, &volume, sizeof volume);
}

constexpr auto kEffectVolume = static_cast<uint32_t>(ParamId::EffectVolumeDb);
constexpr auto kPerfSubdiv   = static_cast<uint32_t>(ParamId::PerfSubdiv);    // row 83
constexpr auto kPerfTimeMode = static_cast<uint32_t>(ParamId::PerfTimeMode);  // row 84
constexpr auto kTempoRecall  = static_cast<uint32_t>(ParamId::TempoRecall);   // row 85
constexpr auto kTempoGlide   = static_cast<uint32_t>(ParamId::TempoGlide);    // row 86

float FloatOf(uint32_t bits) noexcept {
  float v = 0.f;
  std::memcpy(&v, &bits, sizeof v);
  return v;
}

// The committed tempo in whole µs per quarter, as STAT stores it (clock.md §10.3): rounded half
// up, inside the tempo range.
uint32_t UsFromNs(uint32_t ns) noexcept {
  const uint32_t us = static_cast<uint32_t>((static_cast<uint64_t>(ns) + 500u) / 1000u);
  return us < kMinUsPerQuarter ? kMinUsPerQuarter : (us > kMaxUsPerQuarter ? kMaxUsPerQuarter : us);
}

constexpr size_t kPackageHashAt = 96;  // the package header's package_hash (Preset.h)

// The mode a session's FMOD block restores (StateCodec.h): this build's package of that id,
// with the session's macro mirrors as its CTRL positions, over the session's leaves. False when
// this build has no such package or the whole does not validate (ValidateMode's rules, the
// leaves of absent elements included); *exact cleared when this build's package is not the one
// the session played.
bool RestoreFactoryMode(const WrapperState& state, const float* plain, ModeState* mode, PresetSource* source,
                        bool* exact) {
  const int index = FindFactory(state.factory.id);
  if (index < 0) return false;
  PackageInfo                        info;
  const std::unique_ptr<PresetState> package = DecodeFactory(static_cast<size_t>(index), nullptr, &info);
  if (package == nullptr) return false;
  ModeState m;
  m.mode        = package->mode;
  m.control     = package->control;
  m.performance = package->performance;
  m.soundRev    = package->soundRev;
  for (uint32_t k = 0; k < state.factory.macroCount && k < kMaxMacros; ++k) {
    for (uint32_t j = 0; j < m.control.macroCount && j < kMaxMacros; ++j) {
      if (m.control.positions[j].macroId == state.factory.macroIds[k]) {
        m.control.positions[j].position = state.factory.positions[k];
      }
    }
  }
  auto check = std::make_unique<PresetState>();
  ToPreset(plain, m, *check);
  LoadReport report;
  CheckPreset(*check, &report);
  if (report.invalidMode) return false;
  const FactoryPackage& f = Factory(static_cast<size_t>(index));
  *mode                   = m;
  *source                 = {index, juce::String::fromUTF8(f.name), f.family};
  if (std::memcmp(info.packageHash.bytes, state.factory.packageHash, sizeof info.packageHash.bytes) != 0) {
    *exact = false;  // this build's version of the mode, not the one the session played
  }
  return true;
}

}  // namespace

BrainscapeProcessor::BrainscapeProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      restore_(std::make_unique<PresetState>()),
      blockEvents_(kMaxBlockEvents) {
  // One host parameter per Leaf row, then freeze, the macros, perf.expression and the effect
  // volume (mode-compiler.md §9.2), automatable as host model (b) says (§3.6, Q12); Reserved
  // and Retired rows are not registered. New rows go at the end, so a host's indices hold.
  for (size_t i = 0; i < kNumLeafParams; ++i) {
    auto p     = std::make_unique<BrainscapeParam>(LeafId(i), sink_);
    params_[i] = p.get();
    sent_[i]   = p->Plain();
    addParameter(p.release());
  }
  auto f  = std::make_unique<FreezeParam>(sink_);
  freeze_ = f.get();
  addParameter(f.release());
  for (size_t k = 0; k < kMaxMacros; ++k) {
    auto p     = std::make_unique<BrainscapeParam>(static_cast<ParamId>(static_cast<uint32_t>(ParamId::MacroActivity) + k), sink_);
    macros_[k] = p.get();
    addParameter(p.release());
  }
  SetMacroMirrors(mode_.control);
  auto e      = std::make_unique<BrainscapeParam>(ParamId::PerfExpression, sink_);
  expression_ = e.get();
  addParameter(e.release());
  auto v        = std::make_unique<BrainscapeParam>(ParamId::EffectVolumeDb, sink_);
  effectVolume_ = v.get();
  sentEffectVolume_ = v->Plain();
  addParameter(v.release());
  // The tempo core's Performance rows (clock.md §10.4), appended after 82. Row 85, a device
  // setting, is not registered: it is the Tempo panel's, in the session's settings.
  auto sd     = std::make_unique<BrainscapeParam>(ParamId::PerfSubdiv, sink_);
  subdiv_     = sd.get();
  sentSubdiv_ = sd->Plain();
  addParameter(sd.release());
  auto tm       = std::make_unique<BrainscapeParam>(ParamId::PerfTimeMode, sink_);
  timeMode_     = tm.get();
  sentTimeMode_ = tm->Plain();
  addParameter(tm.release());
  activeModeHash_ = ModeHash(activeMode_);
  curation_       = std::make_unique<CurationSession>(*this);
  setLatencySamples(0);  // dry is never delayed (§4.5); the resampled mode will report its own
  // The Standalone expects a guitar on input 1 (§2.2); a plugin on a stereo track must pass
  // both channels, dry exact at Mix = 0 (§4.5). A saved session's mode wins over either.
  if (wrapperType == wrapperType_Standalone) {
    inputMode_.store(static_cast<int>(InputMode::Mono), std::memory_order_relaxed);
  }
}

BrainscapeProcessor::~BrainscapeProcessor() {
  {
    const std::lock_guard<std::mutex> lock(spareMutex_);
    spareQuit_ = true;
  }
  spareWake_.notify_all();
  if (spareThread_.joinable()) spareThread_.join();
}

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
  engineCalls_.store(0u, std::memory_order_relaxed);
  armRequest_.store(true, std::memory_order_relaxed);
}

void BrainscapeProcessor::setNonRealtime(bool isNonRealtime) noexcept {
  AudioProcessor::setNonRealtime(isNonRealtime);
  // The VST3, VST2 and LV2 wrappers call this before every block: only the switch to
  // offline arms a transport start.
  if (!offline_.exchange(isNonRealtime, std::memory_order_relaxed) && isNonRealtime) {
    armRequest_.store(true, std::memory_order_relaxed);
  }
}

void BrainscapeProcessor::InitEngine(double sampleRate) {
  // TODO(companion §4.2): at other host rates the engine must stay at 48 kHz behind an
  // r8brain-free-src resampler (reported latency). Until then it runs natively at the
  // host rate, and the status says it is not at the pedal's rate.
  const std::lock_guard<std::mutex> spareLock(spareMutex_);  // the worker is between passes
  FreeSpare();  // configured for the old rate; the worker prepares a new one
  spareState_.store(kSpareEmpty, std::memory_order_release);
  config_                 = EngineConfig{};
  config_.sampleRate      = sampleRate;
  config_.maxBlockSize    = kMaxChunk;
  config_.historyFrames   = 1u << 22;
  config_.looperFrames    = 0;
  config_.stereoInput     = true;
  config_.ditherRingWrite = true;
  if (live_ == nullptr) {
    slots_[0] = std::make_unique<EngineSlot>();
    live_     = slots_[0].get();
  }
  // Off the audio thread: 16.9 MiB, every page touched by Init's ring clear.
  auto arenas  = std::make_unique<host::HeapArenas>(PlanMemory(config_));
  engineReady_ = arenas->ok() && live_->engine.Init(config_, arenas->get());
  live_->arenas = std::move(arenas);
  engineRate_   = sampleRate;
  statusEngineRate_.store(engineReady_ ? sampleRate : 0.0, std::memory_order_relaxed);
  statusReady_.store(engineReady_, std::memory_order_relaxed);
  if (engineReady_) {
    // Every producer stores the mirror before it posts, so the mirrors read after this
    // generation step hold every event posted before it; those events are then dropped,
    // as is any restore posted so far, which the mirrors also hold.
    const uint32_t gen = NextGeneration();
    sink_.SetGeneration(gen);
    generationFloor_ = gen;
    // A unit still waiting is in the mirrors (and its tempo in liveNs_): a preset change among
    // them makes this restart one too (clock.md §10.1). Units are posted under controlMutex_.
    const bool unitPending  = stateSeq_.load(std::memory_order_acquire) != seqApplied_;
    const bool presetChange = unitPending && stateKind_.load(std::memory_order_relaxed) ==
                                                 static_cast<uint32_t>(UnitKind::PresetChange);
    seqApplied_      = stateSeq_.load(std::memory_order_acquire);
    loadPending_     = false;
    hostCount_ = uiCount_ = 0;
    for (size_t i = 0; i < kNumLeafParams; ++i) sent_[i] = params_[i]->Plain();
    activeMode_       = mode_;
    activeModeHash_   = ModeHash(activeMode_);
    sentEffectVolume_ = effectVolume_->Plain();
    resendVolume_     = false;
    // Init resets every parameter and clears freeze (Engine.cpp Init), so the wrapper's
    // preset goes back in with an Exact load (§4.1): the start state a render begins from.
    // The effect volume, a device setting the load keeps (mode-compiler.md §3.8), goes in
    // first, so the load's restart snaps its smoother too.
    live_->engine.SetParam(ParamId::EffectVolumeDb, sentEffectVolume_);
    ToPreset(sent_.data(), activeMode_, *restore_);
    live_->engine.LoadPreset(*restore_, LoadMode::Exact);
    live_->preset       = sent_;
    live_->mode         = activeMode_;
    live_->effectVolume = sentEffectVolume_;
    sentHash_.store(ValuesHash(sent_, activeModeHash_, sentEffectVolume_), std::memory_order_relaxed);
    engineRateInt_ = static_cast<uint32_t>(std::lround(sampleRate));
    afterLoad_     = {};
    LoadAfterRestart();
    // prepareToPlay and a change of device or rate restart the preset that plays: the next block
    // re-asserts the committed tempo (the last block's, or a restored session's) and rows 83 and
    // 84 (clock.md §10.1, P12).
    ScheduleReasserts(presetChange, liveNs_.load(std::memory_order_relaxed));
  }
  spareWake_.notify_all();
}

// What follows every restart, at Init, at a transport start or after an Exact load: a new
// timeline, and freeze as the host parameter has it.
void BrainscapeProcessor::LoadAfterRestart() noexcept {
  RestartTimeline();
  if (freeze_->get()) live_->engine.SetFreeze(true);
  recallSent_ = -1;  // rows 85 and 86 go out again: an Init'd or swapped-in engine has its default
  glideSent_  = -1;
}

void BrainscapeProcessor::ScheduleReasserts(bool presetChange, uint32_t ns) noexcept {
  reassert_.pending      = true;
  reassert_.presetChange = presetChange;
  reassert_.ns           = ns;
  // The host's tempo, when it is followed, is re-sent by the follower, and a start anchored.
  follower_.Reset();
  // What a second Exact load before the next block carries.
  const bool storedPlays = presetChange && tempoRecallPreset_.load(std::memory_order_relaxed) &&
                           lastSource_ == static_cast<uint8_t>(tempo::ClockSource::Internal);
  carryNs_ = storedPlays ? activeMode_.performance.usPerQuarter * 1000u : ns;
}

// The re-asserts of clock.md §2.5 and §10.1 at frame 0 of the new timeline, before anything else:
// 1. the committed tempo the load carried, unless the host's tempo is followed (the follower sends
//    it after them, then anchors a playing transport, §4.4) or a preset change under recall
//    Preset with the Internal source, which plays its stored tempo; 2. a MIDI master's position,
//    as the pedal re-asserts it (§2.5 item 2), when the Standalone's translator knows it: Locate
//    with AtNextTick to the next tick's position, then Continue while the master runs and the
//    engine followed it in the last block; 3. after a
//    restart of the same preset, rows 83 and 84, the live Subdiv and time mode.
void BrainscapeProcessor::EmitReasserts(uint32_t offset) noexcept {
  if (!reassert_.pending) return;
  reassert_.pending = false;
  const bool storedPlays = reassert_.presetChange && tempoRecallPreset_.load(std::memory_order_relaxed) &&
                           lastSource_ == static_cast<uint8_t>(tempo::ClockSource::Internal);
  if (!follower_.Following() && !storedPlays) Emit(TempoEventOf(reassert_.ns), offset);
  if (receiveMidiClock_.load(std::memory_order_relaxed) && midiClock_.PositionKnown()) {
    Emit(TransportEventOf(tempo::TransportKind::Locate, true, midiClock_.NextTickPosition()), offset);
    // Continue only while the engine still followed a running master in the last block: a master
    // gone without its Stop (a cable pulled, the app quit) left the translator running, and an
    // armed Continue would hold the grid for a tick that never comes (§3.4). The engine's gap rule
    // (§3.5) has reverted its source to Internal a second after the last tick.
    if (midiClock_.Running() && lastSource_ == static_cast<uint8_t>(tempo::ClockSource::ClockRunning)) {
      Emit(TransportEventOf(tempo::TransportKind::Continue, true), offset);
    }
  }
  if (!reassert_.presetChange) EmitRows(offset);
}

// A session restored while audio runs is a Spillover load, which plays the package's stored Subdiv
// and time mode and keeps the running tempo: the session's performance follows it at its frame.
void BrainscapeProcessor::EmitAfterLoad(uint32_t offset) noexcept {
  if (!afterLoad_.pending) return;
  afterLoad_.pending = false;
  if (afterLoad_.ns != 0u && !follower_.Following()) Emit(TempoEventOf(afterLoad_.ns), offset);
  EmitRows(offset);
}

// Rows 83 and 84 as Subdivision events: §5.1's code of the Subdiv knob's position, the time mode.
void BrainscapeProcessor::EmitRows(uint32_t offset) noexcept {
  sentSubdiv_   = subdiv_->Plain();
  sentTimeMode_ = timeMode_->Plain();
  Emit(SubdivisionEventOf(tempo::SubdivField::Subdivision,
                          tempo::SubdivCodeFromPosition(SubdivPositionOf(sentSubdiv_))),
       offset);
  Emit(SubdivisionEventOf(tempo::SubdivField::TimeMode, TimeModeOf(sentTimeMode_)), offset);
}

void BrainscapeProcessor::Emit(const TempoEvent& e, uint32_t offset) noexcept {
  Engine::BlockEvent b;
  b.offset = offset;
  b.type   = e.type;
  b.id     = e.id;
  b.value  = FloatOf(e.valueBits);
  Emit(b);
}

// The Standalone's MIDI clock (clock.md §10.2): every byte of a message, in order, through the one
// translator (§4.3); its events at the message's frame at the MIDI rank, or, for a message past
// the block, pending for the next block's first frame.
void BrainscapeProcessor::FeedMidiClock(const juce::MidiMessageMetadata& m, uint32_t offset, bool pend) noexcept {
  for (int i = 0; i < m.numBytes; ++i) {
    MidiClockEvent c;
    if (midiClock_.Feed(m.data[i], &c) != MidiClockParser::Result::Event) continue;
    if (pend) {
      WrapperEvent e{c.type == tempo::kEventClockTick ? WrapperEvent::Type::ClockTick : WrapperEvent::Type::Transport,
                     WrapperEvent::Source::Midi, c.id, FloatOf(c.valueBits)};
      e.frame = framePos_;
      InsertPending(e);
    } else {
      Emit({c.type == tempo::kEventClockTick ? Engine::EventType::ClockTick : Engine::EventType::Transport, c.id,
            c.valueBits},
           offset);
    }
  }
}

HostTransport BrainscapeProcessor::ReadTransport() const {
  HostTransport t;
  if (juce::AudioPlayHead* head = getPlayHead()) {
    if (const auto position = head->getPosition()) {
      t.playing = position->getIsPlaying();
      if (const auto bpm = position->getBpm()) {
        t.hasBpm = true;
        t.bpm    = *bpm;
      }
      if (const auto ppq = position->getPpqPosition()) {
        t.hasPpq = true;
        t.ppq    = *ppq;
      }
    }
  }
  return t;
}

void BrainscapeProcessor::RestartTimeline() noexcept {
  framePos_     = 0;
  pendingCount_ = 0;  // stamped for the old timeline
  timeline_.fetch_add(1u, std::memory_order_acq_rel);
}

uint32_t BrainscapeProcessor::NextGeneration() noexcept { return sink_.Generation() + 1u; }

void BrainscapeProcessor::reset() {
  if (engineReady_) live_->engine.Reset();  // keeps the ring and the counter (§4.9)
}

void BrainscapeProcessor::TriggerFromUi() noexcept {
  sink_.Post({WrapperEvent::Type::Trigger, WrapperEvent::Source::Ui,
              static_cast<uint32_t>(Engine::TriggerSource::Footswitch), 1.0f});
}

void BrainscapeProcessor::TapFromUi() noexcept {
  sink_.Post({WrapperEvent::Type::Tap, WrapperEvent::Source::Ui, 0u, 0.0f});
}

bool BrainscapeProcessor::SetTempoFromUi(double bpm) noexcept {
  const uint32_t ns = tempo::NsPerQuarterFromBpm(bpm);
  if (ns == 0u) return false;
  sink_.Post({WrapperEvent::Type::Tempo, WrapperEvent::Source::Ui, ns, 0.0f});
  return true;
}

BrainscapeProcessor::TempoDisplay BrainscapeProcessor::GetTempoDisplay() const noexcept {
  TempoDisplay d;
  d.nsPerQuarter  = liveNs_.load(std::memory_order_relaxed);
  d.source        = dispSource_.load(std::memory_order_relaxed);
  d.followingHost = dispFollowing_.load(std::memory_order_relaxed);
  d.hostOctaves   = dispHostOctaves_.load(std::memory_order_relaxed);
  d.subdiv        = dispSubdiv_.load(std::memory_order_relaxed);
  d.timeMode      = dispTimeMode_.load(std::memory_order_relaxed);
  d.rate          = dispRate_.load(std::memory_order_relaxed);
  d.hostClamped   = dispHostClamped_.load(std::memory_order_relaxed);
  const uint8_t flags = dispFlags_.load(std::memory_order_relaxed);
  d.running  = (flags & kTempoFlagRunning) != 0u;
  d.locked   = (flags & kTempoFlagLocked) != 0u;
  d.position = dispPosition_.load(std::memory_order_relaxed);
  return d;
}

PerformanceState BrainscapeProcessor::LivePerformance() const {
  PerformanceState p = CurrentMode().performance;
  p.usPerQuarter     = UsFromNs(liveNs_.load(std::memory_order_relaxed));
  p.timeMode         = static_cast<brainscape::TimeMode>(TimeModeOf(timeMode_->Plain()));
  p.subdiv           = static_cast<Subdivision>(tempo::SubdivCodeFromPosition(SubdivPositionOf(subdiv_->Plain())));
  return p;
}

TempoInfo BrainscapeProcessor::EngineTempo() const noexcept {
  return engineReady_ ? live_->engine.Tempo() : TempoInfo{};
}

TempoStats BrainscapeProcessor::EngineTempoCounts() const noexcept {
  return engineReady_ ? live_->engine.TempoCounts() : TempoStats{};
}

void BrainscapeProcessor::PostAt(uint64_t frame, WrapperEvent e) noexcept {
  // Events hold canonical values (profile §3.7), as every other producer's do: the value
  // sent to the engine becomes the parameter's mirror and the session state, so it must
  // be the bits the engine keeps, not the caller's -0, NaN or out-of-range value. A macro's
  // or the pedal's position is its row's value, canonicalized to [0, 1].
  if ((e.type == WrapperEvent::Type::Param &&
       (IsLeaf(e.id) || e.id == kEffectVolume || e.id == kPerfSubdiv || e.id == kPerfTimeMode)) ||
      (e.type == WrapperEvent::Type::Macro && IsMacroRow(static_cast<ParamId>(e.id)))) {
    e.value = Canonicalize(static_cast<ParamId>(e.id), e.value);
  } else if (e.type == WrapperEvent::Type::Expression) {
    e.value = Canonicalize(ParamId::PerfExpression, e.value);
  }
  e.frame    = frame;
  e.scripted = true;
  e.timeline = timeline_.load(std::memory_order_acquire);
  sink_.PostScripted(e);
}

void BrainscapeProcessor::SetSettings(const WrapperSettings& s) noexcept {
  inputMode_.store(static_cast<int>(s.inputMode), std::memory_order_relaxed);
  inputGainDb_.store(CanonicalGainDb(s.inputGainDb), std::memory_order_relaxed);
  outputGainDb_.store(CanonicalGainDb(s.outputGainDb), std::memory_order_relaxed);
  restartOnStart_.store(s.restartOnStart, std::memory_order_release);
  tempoSource_.store(static_cast<uint32_t>(s.tempoSource), std::memory_order_relaxed);
  receiveMidiClock_.store(s.receiveMidiClock, std::memory_order_relaxed);
  tempoRecallPreset_.store(s.tempoRecallPreset, std::memory_order_relaxed);
  tempoGlide_.store(s.tempoGlide, std::memory_order_relaxed);
  if (s.restartOnStart) EnsureSpareWorker();
  spareWake_.notify_all();  // the worker prepares or releases the spare
}

void BrainscapeProcessor::SetMonitorTrimDb(float db) noexcept {
  monitorTrimDb_.store(CanonicalGainDb(db), std::memory_order_relaxed);
}

WrapperSettings BrainscapeProcessor::GetSettings() const noexcept {
  WrapperSettings s;
  s.inputMode      = static_cast<InputMode>(inputMode_.load(std::memory_order_relaxed));
  s.inputGainDb    = inputGainDb_.load(std::memory_order_relaxed);
  s.outputGainDb   = outputGainDb_.load(std::memory_order_relaxed);
  s.restartOnStart = restartOnStart_.load(std::memory_order_relaxed);
  s.tempoSource    = tempoSource_.load(std::memory_order_relaxed) == static_cast<uint32_t>(TempoSource::Internal)
                         ? TempoSource::Internal
                         : TempoSource::Host;
  s.receiveMidiClock  = receiveMidiClock_.load(std::memory_order_relaxed);
  s.tempoRecallPreset = tempoRecallPreset_.load(std::memory_order_relaxed);
  s.tempoGlide        = tempoGlide_.load(std::memory_order_relaxed);
  return s;
}

BrainscapeProcessor::Status BrainscapeProcessor::GetStatus() const noexcept {
  Status s;
  s.hostRate        = hostRate_.load(std::memory_order_relaxed);
  s.engineRate      = statusEngineRate_.load(std::memory_order_relaxed);
  s.engineReady     = statusReady_.load(std::memory_order_relaxed);
  s.pedalRate       = s.engineReady && s.engineRate == kPedalRate;
  s.lastHostBlock   = lastHostBlock_.load(std::memory_order_relaxed);
  s.maxHostBlock    = maxHostBlock_.load(std::memory_order_relaxed);
  s.droppedEvents   = sink_.Lost();
  s.lastLoadInexact = lastLoadInexact_.load(std::memory_order_relaxed);
  s.restartOnStart  = restartOnStart_.load(std::memory_order_relaxed);
  s.spareReady      = spareState_.load(std::memory_order_acquire) == kSpareReady &&
                 spareHash_.load(std::memory_order_relaxed) == sentHash_.load(std::memory_order_relaxed);
  s.lastStart       = lastStart_.load(std::memory_order_relaxed);
  s.engineCalls     = engineCalls_.load(std::memory_order_relaxed);
  return s;
}

BrainscapeParam* BrainscapeProcessor::FindHostParam(ParamId id) noexcept {
  if (IsLeaf(id)) return params_[LeafIndex(id)];
  if (IsMacroRow(id)) return macros_[MacroIndex(id)];
  if (id == ParamId::PerfExpression) return expression_;
  if (id == ParamId::EffectVolumeDb) return effectVolume_;
  if (id == ParamId::PerfSubdiv) return subdiv_;
  if (id == ParamId::PerfTimeMode) return timeMode_;
  return nullptr;
}

ModeState BrainscapeProcessor::CurrentMode() const {
  const std::lock_guard<std::mutex> lock(modeMutex_);
  return mode_;
}

PresetSource BrainscapeProcessor::CurrentSource() const {
  const std::lock_guard<std::mutex> lock(controlMutex_);
  return source_;
}

std::unique_ptr<PresetState> BrainscapeProcessor::CurrentPreset() const {
  auto  preset = std::make_unique<PresetState>();
  float values[kNumLeafParams];
  for (size_t i = 0; i < kNumLeafParams; ++i) values[i] = params_[i]->Plain();
  ToPreset(values, CurrentMode(), *preset);
  preset->performance = LivePerformance();
  ControlState& c = preset->control;
  for (uint32_t k = 0; k < c.macroCount && k < kMaxMacros; ++k) {
    const auto id = static_cast<ParamId>(c.positions[k].macroId);
    if (IsMacroRow(id)) c.positions[k].position = macros_[MacroIndex(id)]->Plain();
  }
  return preset;
}

void BrainscapeProcessor::SetMacroMirrors(const ControlState& control) noexcept {
  for (size_t k = 0; k < kMaxMacros; ++k) {
    const auto id       = static_cast<ParamId>(static_cast<uint32_t>(ParamId::MacroActivity) + k);
    float      position = FindParam(id)->def;
    for (uint32_t j = 0; j < control.macroCount && j < kMaxMacros; ++j) {
      if (control.positions[j].macroId == static_cast<uint32_t>(id)) position = control.positions[j].position;
    }
    macros_[k]->StoreMirror(Canonicalize(id, position));
  }
}

// Outside every lock, since a host may call back in: each inner setValue sees its own
// normalised view and returns at once (companion §5.3).
void BrainscapeProcessor::NotifyHostOfMirrors() {
  for (BrainscapeParam* p : params_) p->setValueNotifyingHost(p->getValue());
  for (BrainscapeParam* p : macros_) p->setValueNotifyingHost(p->getValue());
  for (BrainscapeParam* p : {subdiv_, timeMode_}) p->setValueNotifyingHost(p->getValue());
}

bool BrainscapeProcessor::LoadPresetState(const PresetState& state, LoadReport* report,
                                          const PresetSource& source) {
  LoadReport r;
  CheckPreset(state, &r);
  if (report != nullptr) *report = r;
  if (r.invalidMode) return false;
  // Complete-state semantics, as LoadPreset applies them: a leaf the preset lacks loads its
  // default, an id that is not a Leaf row is ignored, and of repeated ids the first counts.
  float plain[kNumLeafParams];
  bool  seen[kNumLeafParams] = {};
  for (size_t i = 0; i < kNumLeafParams; ++i) plain[i] = Canonicalize(LeafId(i), FindParam(LeafId(i))->def);
  for (uint32_t k = 0; k < state.leafCount && k < PresetState::kMaxLeaves; ++k) {
    const size_t i = LeafIndex(state.leaves[k].id);
    if (i == kNumLeafParams || seen[i]) continue;
    plain[i] = Canonicalize(LeafId(i), state.leaves[k].value);
    seen[i]  = true;
  }
  ModeState mode;
  mode.mode        = state.mode;
  mode.control     = state.control;
  mode.performance = state.performance;
  mode.soundRev    = state.soundRev;
  {
    const std::lock_guard<std::mutex> lock(controlMutex_);
    {
      const std::lock_guard<std::mutex> modeLock(modeMutex_);
      mode_ = mode;
    }
    source_ = source;
    // A preset change plays the preset's stored Subdiv and time mode (clock.md §10.1): the rows
    // take them before the unit goes out, and its load applies them. Under recall Preset with the
    // internal source it plays the stored tempo too, which the display shows at once.
    subdiv_->StoreMirror(Canonicalize(ParamId::PerfSubdiv, static_cast<float>(tempo::SubdivPositionFromCode(
                                                               static_cast<uint8_t>(mode.performance.subdiv)))));
    timeMode_->StoreMirror(Canonicalize(ParamId::PerfTimeMode, static_cast<float>(static_cast<uint32_t>(mode.performance.timeMode))));
    if (tempoRecallPreset_.load(std::memory_order_relaxed) && !dispFollowing_.load(std::memory_order_relaxed) &&
        dispSource_.load(std::memory_order_relaxed) == static_cast<uint8_t>(tempo::ClockSource::Internal) &&
        mode.performance.usPerQuarter >= kMinUsPerQuarter && mode.performance.usPerQuarter <= kMaxUsPerQuarter) {
      liveNs_.store(mode.performance.usPerQuarter * 1000u, std::memory_order_relaxed);
    }
    PostStateUnit(plain, mode, UnitKind::PresetChange, 0u);
    for (size_t i = 0; i < kNumLeafParams; ++i) params_[i]->StoreMirror(plain[i]);
    SetMacroMirrors(mode.control);
    lastLoadInexact_.store(!r.exact, std::memory_order_relaxed);
    loadSerial_.fetch_add(1u, std::memory_order_relaxed);
  }
  NotifyHostOfMirrors();
  freeze_->setValueNotifyingHost(0.0f);  // the load turned it off
  spareWake_.notify_all();
  return true;
}

bool BrainscapeProcessor::StartAudition(const juce::File& wav, juce::String& error) {
  const std::unique_ptr<PresetState> preset = CurrentPreset();
  double                          rate  = 0.0;
  const juce::AudioBuffer<float>* audio = testInput_.GetSource() == TestInput::Source::FileLoop
                                              ? testInput_.LoadedAudio(&rate)
                                              : nullptr;
  AuditionInput input = audio != nullptr ? FileInput(*audio, rate, testInput_.LoadedName())
                                         : TestSignalInput();
  const auto mode = static_cast<InputMode>(inputMode_.load(std::memory_order_relaxed));
  if (!audition_.Start(wav, *preset, mode, std::move(input))) {
    error = "An audition is already rendering";
    return false;
  }
  return true;
}

// ── State ──────────────────────────────────────────────────────────────────────────

void BrainscapeProcessor::getStateInformation(juce::MemoryBlock& destData) {
  WrapperState state{};
  {
    const std::lock_guard<std::mutex> lock(controlMutex_);
    for (size_t i = 0; i < kNumLeafParams; ++i) state.plain[i] = params_[i]->Plain();
    state.settings          = GetSettings();
    state.effectVolumeDb    = effectVolume_->Plain();
    state.hasEffectVolume   = true;
    // The tempo core's performance (clock.md §10.1, §10.4): the committed tempo and rows 83, 84.
    state.tempoNs        = liveNs_.load(std::memory_order_relaxed);
    state.hasPerformance = true;
    state.subdivPosition = subdiv_->Plain();
    state.timeMode       = timeMode_->Plain();
    // While a factory mode plays, the session names its package and keeps the macro mirrors at
    // the positions its CTRL defines (StateCodec.h, FMOD).
    if (source_.factory >= 0 && static_cast<size_t>(source_.factory) < FactoryCount()) {
      const FactoryPackage& f = Factory(static_cast<size_t>(source_.factory));
      state.hasFactory        = true;
      state.factory.id        = f.id;
      std::memcpy(state.factory.packageHash, f.bytes + kPackageHashAt, sizeof state.factory.packageHash);
      const ControlState& c = mode_.control;
      for (uint32_t k = 0; k < c.macroCount && k < kMaxMacros; ++k) {
        const auto id = static_cast<ParamId>(c.positions[k].macroId);
        if (!IsMacroRow(id)) continue;
        state.factory.macroIds[state.factory.macroCount]  = static_cast<uint32_t>(id);
        state.factory.positions[state.factory.macroCount] = macros_[MacroIndex(id)]->Plain();
        ++state.factory.macroCount;
      }
    }
  }
  std::vector<uint8_t> bytes;
  EncodeState(state, bytes);
  destData.replaceAll(bytes.data(), bytes.size());
}

void BrainscapeProcessor::setStateInformation(const void* data, int sizeInBytes) {
  WrapperState state{};
  if (sizeInBytes <= 0 || !DecodeState(data, static_cast<size_t>(sizeInBytes), state)) return;
  // A v1 session holds leaves (StateCodec.h). One saved while a factory mode played names its
  // package (FMOD): it plays this build's package of that id, over the session's leaves, with
  // the session's macro mirrors as the CTRL positions. Any other plays the default mode, whose
  // CTRL positions the macro mirrors take, until session v2 carries a package (mode-compiler.md
  // §9.2); so does one whose factory mode this build lacks, inexact. The effect volume, a device
  // setting, goes out after the unit's load (ApplyStateUnit), so its mirror is stored first.
  ModeState    mode{};
  PresetSource source;
  bool         exact = state.unknownIds + state.missingIds == 0u && !state.unreadTail;
  if (state.hasFactory && !RestoreFactoryMode(state, state.plain, &mode, &source, &exact)) exact = false;
  {
    const std::lock_guard<std::mutex> lock(controlMutex_);
    {
      const std::lock_guard<std::mutex> modeLock(modeMutex_);
      mode_ = mode;
    }
    source_ = source;
    if (state.hasEffectVolume) effectVolume_->StoreMirror(state.effectVolumeDb);
    // A session restore restarts the session's preset with its performance (clock.md §10.1): the
    // tempo it saved (the internal tempo persists, P12) and rows 83 and 84, or the preset's
    // stored ones for a session without them. The mirrors go first: the unit's re-asserts read them.
    subdiv_->StoreMirror(state.hasPerformance
                             ? state.subdivPosition
                             : Canonicalize(ParamId::PerfSubdiv, static_cast<float>(tempo::SubdivPositionFromCode(
                                                                     static_cast<uint8_t>(mode.performance.subdiv)))));
    timeMode_->StoreMirror(state.hasPerformance
                               ? state.timeMode
                               : Canonicalize(ParamId::PerfTimeMode, static_cast<float>(static_cast<uint32_t>(mode.performance.timeMode))));
    if (state.tempoNs != 0u) liveNs_.store(state.tempoNs, std::memory_order_relaxed);
    PostStateUnit(state.plain, mode, UnitKind::SessionRestore, state.tempoNs);
    for (size_t i = 0; i < kNumLeafParams; ++i) params_[i]->StoreMirror(state.plain[i]);
    SetMacroMirrors(mode.control);
    SetSettings(state.settings);
    lastLoadInexact_.store(!exact, std::memory_order_relaxed);
    loadSerial_.fetch_add(1u, std::memory_order_relaxed);
  }
  // Outside the lock, since a host may call back in. Each inner setValue sees its own
  // normalised view and returns at once (companion §5.3). Freeze never loads engaged.
  NotifyHostOfMirrors();
  effectVolume_->setValueNotifyingHost(effectVolume_->getValue());
  freeze_->setValueNotifyingHost(0.0f);
}

void BrainscapeProcessor::PostStateUnit(const float* plain, const ModeState& mode, UnitKind kind,
                                        uint32_t tempoNs) noexcept {
  uint32_t words[kModeWords];
  std::memcpy(words, &mode, sizeof mode);
  const uint32_t gen = NextGeneration();
  const uint32_t seq = stateSeq_.load(std::memory_order_relaxed);
  stateSeq_.store(seq + 1u, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  for (size_t i = 0; i < kNumLeafParams; ++i) stateSlot_[i].store(plain[i], std::memory_order_relaxed);
  for (size_t i = 0; i < kModeWords; ++i) modeSlot_[i].store(words[i], std::memory_order_relaxed);
  stateKind_.store(static_cast<uint32_t>(kind), std::memory_order_relaxed);
  stateTempoNs_.store(tempoNs, std::memory_order_relaxed);
  stateGen_.store(gen, std::memory_order_relaxed);
  stateSeq_.store(seq + 2u, std::memory_order_release);
  // Only once the slot is complete: whoever pops an event stamped with this generation
  // then sees the whole unit.
  sink_.SetGeneration(gen);
}

// A restore applies as one unit (§4.7): the decoded preset becomes a Spillover load at the
// block's first frame, before the block's other events (profile §5.10, §5.11), so a preset
// recalled while playing keeps its trails. Before anything has played since the last Init
// or restart there are no trails, and the restore is the start state: an Exact load, which
// on an engine that has rendered nothing clears nothing (Engine::Restart), so it runs here.
void BrainscapeProcessor::ApplyStateUnit() noexcept {
  const uint32_t seq = stateSeq_.load(std::memory_order_acquire);
  // Odd: a newer restore is being written, and every event drained this block is older
  // than it; the next block takes it.
  if (seq == seqApplied_ || (seq & 1u) != 0u) return;
  float plain[kNumLeafParams];
  for (size_t i = 0; i < kNumLeafParams; ++i) plain[i] = stateSlot_[i].load(std::memory_order_relaxed);
  auto* unit = reinterpret_cast<unsigned char*>(&unitMode_);
  for (size_t i = 0; i < kModeWords; ++i) {
    const uint32_t w = modeSlot_[i].load(std::memory_order_relaxed);
    std::memcpy(unit + i * sizeof w, &w, sizeof w);
  }
  const uint32_t gen     = stateGen_.load(std::memory_order_relaxed);
  const bool     session = stateKind_.load(std::memory_order_relaxed) == static_cast<uint32_t>(UnitKind::SessionRestore);
  const uint32_t tempoNs = stateTempoNs_.load(std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_acquire);
  if (stateSeq_.load(std::memory_order_relaxed) != seq) return;
  for (size_t i = 0; i < kNumLeafParams; ++i) sent_[i] = plain[i];
  // The mode changes with the load, at the block's first frame: this block's macro moves,
  // all at or after it, fan out through the new one, as the engine's do.
  activeMode_     = unitMode_;
  activeModeHash_ = ModeHash(activeMode_);
  ToPreset(plain, activeMode_, *restore_);
  touched_.set();
  resendVolume_ = true;
  seqApplied_      = seq;
  generationFloor_ = gen;
  if (framePos_ != 0) {
    loadPending_ = true;
    afterLoad_   = {session, session ? tempoNs : 0u};
    return;
  }
  live_->engine.LoadPreset(*restore_, LoadMode::Exact);
  live_->preset = sent_;
  live_->mode   = activeMode_;
  afterLoad_    = {};
  ScheduleReasserts(!session, session && tempoNs != 0u ? tempoNs : carryNs_);
}

// ── Restart on transport start (§4.9) ──────────────────────────────────────────────

bool BrainscapeProcessor::CheckTransportStart(bool playing) noexcept {
  if (armRequest_.exchange(false, std::memory_order_relaxed)) startArmed_ = true;
  if (!playing) {
    startArmed_ = true;
    return false;
  }
  if (!startArmed_) return false;
  startArmed_ = false;
  if (restartOnStart_.load(std::memory_order_acquire)) RestartAtTransportStart();
  return true;
}

void BrainscapeProcessor::RestartAtTransportStart() noexcept {
  // The values in effect at the block's first frame, loaded Exact: those sent (a restore
  // this block included), then the block's live parameter events in their same-frame order
  // (§4.7). Sent after the load instead, they would glide the smoothers from whatever the
  // last playback left, and two bounces of one automated passage would differ. A stamp
  // made before the restart is void, so scripted events stay out.
  // A macro or expression move folds in as its fan-out through the active mode, which a
  // restore this block has already replaced; the effect volume is a Global row (§3.8).
  Values start       = sent_;
  float  startVolume = resendVolume_ ? effectVolume_->Plain() : sentEffectVolume_;
  // A unit this block folds into the restart: a preset change, or a session with its tempo.
  const bool     unitThisBlock = loadPending_;
  const AfterLoad session      = afterLoad_;
  for (const auto& live : {std::make_pair(hostEvents_.data(), hostCount_),
                           std::make_pair(uiEvents_.data(), uiCount_)}) {
    for (size_t k = 0; k < live.second; ++k) {
      const WrapperEvent& e = live.first[k];
      if (e.scripted || !Applies(e)) continue;
      if (e.type == WrapperEvent::Type::Param && e.id == kEffectVolume) {
        startVolume = e.value;
      } else {
        Track(e, start, nullptr);
      }
    }
  }
  if (resync_) {  // the queue overflowed: the mirrors are the latest word
    for (size_t i = 0; i < kNumLeafParams; ++i) start[i] = params_[i]->Plain();
    startVolume = effectVolume_->Plain();
  }
  bool restarted = false;
  // Offline, or with nothing played since the last Init or restart, restart in place (§4.9
  // a): about a millisecond offline, and on an engine that has rendered nothing the load
  // clears nothing (Engine::Restart), so real time can take it too. The effect volume goes in
  // first, so the restart snaps it.
  if (isNonRealtime() || framePos_ == 0) {
    live_->engine.SetParam(ParamId::EffectVolumeDb, startVolume);
    ToPreset(start.data(), activeMode_, *restore_);
    live_->engine.LoadPreset(*restore_, LoadMode::Exact);
    live_->preset       = start;
    live_->mode         = activeMode_;
    live_->effectVolume = startVolume;
    restarted           = true;
  } else {
    // Real time never blocks (§4.9 b): swap in the spare if it holds this preset.
    int ready = kSpareReady;
    if (spareState_.compare_exchange_strong(ready, kSpareSwapping, std::memory_order_acq_rel)) {
      if (SameBits(spare_->preset, start) && SameMode(spare_->mode, activeMode_) &&
          SameFloat(spare_->effectVolume, startVolume)) {
        std::swap(live_, spare_);
        restarted = true;
      }
      spareState_.store(restarted ? kSpareRetired : kSpareReady, std::memory_order_release);
    }
  }
  lastStart_.store(restarted ? TransportStart::Restarted : TransportStart::SpareNotReady,
                   std::memory_order_relaxed);
  if (!restarted) return;
  // The block's live events still go out at its first frame: the parameter ones repeat
  // what the load holds, and freeze is a level, so it lands as it would have.
  loadPending_      = false;  // the restore, if any, is part of the Exact load
  sentEffectVolume_ = startVolume;
  resendVolume_     = false;
  afterLoad_        = {};
  LoadAfterRestart();
  // The restart of the same preset (§10.1): the committed tempo it carries, rows 83 and 84; or a
  // preset change, or a session's tempo, that this block's unit brought.
  ScheduleReasserts(unitThisBlock && !session.pending,
                    session.pending && session.ns != 0u ? session.ns : carryNs_);
}

void BrainscapeProcessor::EnsureSpareWorker() {
  std::call_once(spareOnce_, [this] { spareThread_ = std::thread([this] { SpareWorkerLoop(); }); });
}

void BrainscapeProcessor::SpareWorkerLoop() {
  std::unique_lock<std::mutex> lock(spareMutex_);
  while (!spareQuit_) {
    PrepareSpare();
    // A spare still allocated is freed on a later pass once the audio thread lets go of it.
    if (restartOnStart_.load(std::memory_order_acquire) ||
        spareState_.load(std::memory_order_acquire) != kSpareEmpty) {
      spareWake_.wait_for(lock, kSparePoll);
    } else {
      spareWake_.wait(lock);
    }
  }
}

// One worker pass, under spareMutex_: keeps a spare restarted with the current preset while
// the option is on, and frees it when the option is off (§4.1, §4.7).
void BrainscapeProcessor::PrepareSpare() {
  int state = spareState_.load(std::memory_order_acquire);
  if (!engineReady_ || !restartOnStart_.load(std::memory_order_acquire)) {
    if ((state == kSpareReady || state == kSpareRetired) &&
        spareState_.compare_exchange_strong(state, kSparePreparing, std::memory_order_acq_rel)) {
      FreeSpare();
      spareState_.store(kSpareEmpty, std::memory_order_release);
    }
    return;
  }
  // One consistent snapshot is not needed: a spare that differs from what the live engine
  // plays at the transport start is never swapped in.
  Values snapshot;
  for (size_t i = 0; i < kNumLeafParams; ++i) snapshot[i] = params_[i]->Plain();
  ModeState mode;
  {
    const std::lock_guard<std::mutex> modeLock(modeMutex_);
    mode = mode_;
  }
  const float volume  = effectVolume_->Plain();
  const bool  settled = SameBits(snapshot, lastSnapshot_) && SameMode(mode, lastMode_) &&
                       SameFloat(volume, lastVolume_);
  lastSnapshot_ = snapshot;
  lastMode_     = mode;
  lastVolume_   = volume;
  if (!settled || (state == kSpareReady && SameBits(snapshot, spareSnapshot_) &&
                   SameMode(mode, spareMode_) && SameFloat(volume, spareVolume_))) {
    return;
  }
  if (state != kSpareEmpty && state != kSpareReady && state != kSpareRetired) return;
  if (!spareState_.compare_exchange_strong(state, kSparePreparing, std::memory_order_acq_rel)) return;
  if (spare_ == nullptr) {
    for (auto& slot : slots_) {
      if (slot == nullptr) {
        slot   = std::make_unique<EngineSlot>();
        spare_ = slot.get();
        break;
      }
    }
    auto arenas = std::make_unique<host::HeapArenas>(PlanMemory(config_));
    if (spare_ == nullptr || !arenas->ok() || !spare_->engine.Init(config_, arenas->get())) {
      FreeSpare();
      spareState_.store(kSpareEmpty, std::memory_order_release);
      return;
    }
    spare_->arenas = std::move(arenas);
  }
  auto preset = std::make_unique<PresetState>();
  ToPreset(snapshot.data(), mode, *preset);
  spare_->engine.SetParam(ParamId::EffectVolumeDb, volume);  // snapped by the load's restart
  spare_->engine.LoadPreset(*preset, LoadMode::Exact);
  spare_->preset       = snapshot;
  spare_->mode         = mode;
  spare_->effectVolume = volume;
  spareSnapshot_       = snapshot;
  spareMode_           = mode;
  spareVolume_         = volume;
  spareHash_.store(ValuesHash(snapshot, ModeHash(mode), volume), std::memory_order_relaxed);
  spareState_.store(kSpareReady, std::memory_order_release);
}

void BrainscapeProcessor::FreeSpare() noexcept {
  for (auto& slot : slots_) {
    if (slot != nullptr && slot.get() == spare_) slot.reset();
  }
  spare_ = nullptr;
}

// ── Events ─────────────────────────────────────────────────────────────────────────

void BrainscapeProcessor::DrainEvents() noexcept {
  const uint32_t timeline = timeline_.load(std::memory_order_relaxed);
  WrapperEvent   e;
  while (hostCount_ < hostEvents_.size() && uiCount_ < uiEvents_.size() && sink_.Queue().Pop(e)) {
    if (e.scripted) {
      if (e.timeline != timeline) continue;  // stamped for a timeline a restart ended
      if (e.frame > framePos_) {
        InsertPending(e);
        continue;
      }
    }
    if (e.source == WrapperEvent::Source::Host) {
      hostEvents_[hostCount_++] = e;
    } else {
      uiEvents_[uiCount_++] = e;
    }
  }
}

void BrainscapeProcessor::InsertPending(const WrapperEvent& e) noexcept {
  if (pendingCount_ == pending_.size()) {
    sink_.CountLost();
    return;
  }
  size_t at = pendingCount_;  // after every event stamped at or before e's frame
  for (; at > 0u && pending_[at - 1u].frame > e.frame; --at) pending_[at] = pending_[at - 1u];
  pending_[at] = e;
  ++pendingCount_;
}

void BrainscapeProcessor::Emit(const Engine::BlockEvent& e) noexcept {
  if (numBlockEvents_ == blockEvents_.size()) {
    sink_.CountLost();
    return;
  }
  blockEvents_[numBlockEvents_]     = e;
  blockEvents_[numBlockEvents_].seq = seq_++;
  ++numBlockEvents_;
}

void BrainscapeProcessor::Emit(const WrapperEvent& e, uint32_t offset) noexcept {
  Engine::BlockEvent b;
  b.offset = offset;
  b.value  = e.value;
  switch (e.type) {
    case WrapperEvent::Type::Param: {
      const size_t leaf = LeafIndex(e.id);
      if (leaf < kNumLeafParams) {
        sent_[leaf] = e.value;
        touched_.set(leaf);
      } else if (e.id == kEffectVolume) {
        sentEffectVolume_ = e.value;
        effectTouched_    = true;
      } else if (e.id == kPerfSubdiv) {  // row 83: §5.1's code of the knob's position
        sentSubdiv_    = e.value;
        subdivTouched_ = true;
        Emit(SubdivisionEventOf(tempo::SubdivField::Subdivision,
                                tempo::SubdivCodeFromPosition(SubdivPositionOf(e.value))),
             offset);
        return;
      } else if (e.id == kPerfTimeMode) {  // row 84
        sentTimeMode_    = e.value;
        timeModeTouched_ = true;
        Emit(SubdivisionEventOf(tempo::SubdivField::TimeMode, TimeModeOf(e.value)), offset);
        return;
      } else {
        return;  // only Leaf rows and the effect volume take SetParam
      }
      b.type = Engine::EventType::SetParam;
      b.id   = e.id;
      break;
    }
    case WrapperEvent::Type::Freeze: b.type = Engine::EventType::Freeze; break;
    case WrapperEvent::Type::Trigger:
      b.type = Engine::EventType::Trigger;
      b.id   = e.id;
      break;
    case WrapperEvent::Type::Macro:
      if (!IsMacroRow(static_cast<ParamId>(e.id))) return;
      Track(e, sent_, &touched_);
      b.type = Engine::EventType::MacroMove;
      b.id   = e.id;
      break;
    case WrapperEvent::Type::Expression:
      Track(e, sent_, &touched_);
      b.type = Engine::EventType::Expression;
      b.id   = 0;
      break;
    // The tempo core's events (clock.md §4.1), their payloads as given: an invalid one is the
    // engine's to ignore and count (§4.2). Taps and tempos are dropped while the host's tempo is
    // followed, before they reach the engine (§10.1).
    case WrapperEvent::Type::Tap:
      if (follower_.Following()) return;
      b.type = Engine::EventType::Tap;
      b.id   = e.id;
      break;
    case WrapperEvent::Type::Tempo:
      if (follower_.Following()) return;
      b.type = Engine::EventType::Tempo;
      b.id   = e.id;
      break;
    case WrapperEvent::Type::ClockTick:
      b.type = Engine::EventType::ClockTick;
      b.id   = e.id;
      break;
    case WrapperEvent::Type::Transport:
      b.type = Engine::EventType::Transport;
      b.id   = e.id;
      break;
    case WrapperEvent::Type::Subdivision:
      b.type = Engine::EventType::Subdivision;
      b.id   = e.id;
      break;
  }
  Emit(b);
}

// The engine applies a MacroMove's or an Expression's targets as SetParam events through the
// same evaluator (ModeEval.h, detail/ModeEvalBody.h), so the leaves it writes here are the bits
// the engine keeps: the mirrors follow the fan-out exactly. Rows that are not Leaf rows (a
// target the engine would ignore) are skipped.
void BrainscapeProcessor::Track(const WrapperEvent& e, Values& values,
                                std::bitset<kNumLeafParams>* touched) const noexcept {
  PresetLeaf out[kMaxTargets];
  size_t     n = 0;
  switch (e.type) {
    case WrapperEvent::Type::Param: {
      const size_t leaf = LeafIndex(e.id);
      if (leaf == kNumLeafParams) return;
      values[leaf] = e.value;
      if (touched != nullptr) touched->set(leaf);
      return;
    }
    case WrapperEvent::Type::Macro:
      n = EvalMacro(activeMode_.mode, static_cast<ParamId>(e.id), e.value, out, kMaxTargets);
      break;
    case WrapperEvent::Type::Expression:
      n = EvalExpression(activeMode_.mode, activeMode_.control, e.value, out, kMaxTargets);
      break;
    case WrapperEvent::Type::Freeze:
    case WrapperEvent::Type::Trigger:
    case WrapperEvent::Type::Tap:
    case WrapperEvent::Type::Tempo:
    case WrapperEvent::Type::ClockTick:
    case WrapperEvent::Type::Transport:
    case WrapperEvent::Type::Subdivision: return;
  }
  for (size_t k = 0; k < n; ++k) {
    const size_t leaf = LeafIndex(out[k].id);
    if (leaf == kNumLeafParams) continue;
    values[leaf] = out[k].value;
    if (touched != nullptr) touched->set(leaf);
  }
}

void BrainscapeProcessor::EmitPending(size_t from, size_t to, WrapperEvent::Source rank,
                                      uint32_t offset) noexcept {
  for (size_t k = from; k < to; ++k) {
    if (pending_[k].source == rank) Emit(pending_[k], offset);
  }
}

// A parameter or freeze event posted before the last applied restore or Init lost to it
// (§4.7: the restore arrived later), and a stamp made before a restart is void, late or not.
// Momentary events (a trigger, a tap, a clock tick, a transport) always apply. Events stamped
// for a later frame wait in pending_ and keep their frame order instead.
bool BrainscapeProcessor::Applies(const WrapperEvent& e) const noexcept {
  if (e.scripted && e.timeline != timeline_.load(std::memory_order_relaxed)) return false;
  return e.type == WrapperEvent::Type::Trigger || e.type == WrapperEvent::Type::Tap ||
         e.type == WrapperEvent::Type::ClockTick || e.type == WrapperEvent::Type::Transport ||
         static_cast<int32_t>(e.generation - generationFloor_) >= 0;
}

void BrainscapeProcessor::EmitLive(const WrapperEvent* events, size_t count, uint32_t offset) noexcept {
  for (size_t k = 0; k < count; ++k) {
    if (Applies(events[k])) Emit(events[k], offset);
  }
}

// The events due at `frame` (the block's first frame takes everything late too), in the
// fixed same-frame order (§4.7): state load, host automation, MIDI, UI. Within a rank,
// events stamped earlier come first.
void BrainscapeProcessor::EmitDue(uint64_t frame, uint64_t blockStart, uint64_t chunkStart,
                                  size_t* pending, const juce::MidiBuffer& midi,
                                  juce::MidiBufferIterator* midiIt, bool* frameZero) noexcept {
  const auto offset = static_cast<uint32_t>(frame - chunkStart);
  const bool first  = *frameZero && frame == blockStart;
  size_t     runEnd = *pending;
  while (runEnd < pendingCount_ && pending_[runEnd].frame <= frame) ++runEnd;
  if (first) {
    // The tempo core first (clock.md §10.1, §4.4): the re-asserts after an Exact load, then
    // rows 85 and 86 when they changed (before a load reads 85, and before the block's tempo
    // changes are classed by 86), the state load and a restored session's performance after it,
    // then the host's Tempo and Transport.
    EmitReasserts(offset);
    const auto device = [this, offset](int value, int* sent, uint32_t id) {
      if (value == *sent) return;
      *sent = value;
      Engine::BlockEvent b;
      b.offset = offset;
      b.type   = Engine::EventType::SetParam;
      b.id     = id;
      b.value  = value != 0 ? 1.0f : 0.0f;
      Emit(b);
    };
    device(tempoRecallPreset_.load(std::memory_order_relaxed) ? 1 : 0, &recallSent_, kTempoRecall);
    device(tempoGlide_.load(std::memory_order_relaxed) ? 1 : 0, &glideSent_, kTempoGlide);
  }
  if (first && loadPending_) {
    Engine::BlockEvent load;
    load.offset = offset;
    load.type   = Engine::EventType::SpilloverLoad;
    load.preset = restore_.get();
    Emit(load);
    loadPending_ = false;
  }
  if (first) {
    EmitAfterLoad(offset);
    for (size_t k = 0; k < hostTempoCount_; ++k) Emit(hostTempo_[k], offset);
    hostTempoCount_ = 0;
  }
  if (first && resendVolume_) {  // the device setting the restore kept (mode-compiler.md §3.8)
    resendVolume_  = false;
    const float v  = effectVolume_->Plain();
    if (!SameFloat(v, sentEffectVolume_)) Emit({WrapperEvent::Type::Param, WrapperEvent::Source::Ui, kEffectVolume, v}, offset);
  }
  EmitPending(*pending, runEnd, WrapperEvent::Source::Host, offset);
  if (first) EmitLive(hostEvents_.data(), hostCount_, offset);
  EmitPending(*pending, runEnd, WrapperEvent::Source::Midi, offset);
  // Note-on is a trigger event (companion §5.7). Raw bytes: building a MidiMessage can
  // allocate for long SysEx.
  for (; *midiIt != midi.cend(); ++*midiIt) {
    const juce::MidiMessageMetadata m = **midiIt;
    if (blockStart + static_cast<uint64_t>(std::max(0, m.samplePosition)) > frame) break;
    if (m.numBytes >= 3 && (m.data[0] & 0xF0u) == 0x90u && m.data[2] > 0u) {
      Emit({WrapperEvent::Type::Trigger, WrapperEvent::Source::Midi,
            static_cast<uint32_t>(Engine::TriggerSource::MidiNote), static_cast<float>(m.data[2]) / 127.0f},
           offset);
    }
    if (midiClockOn_) FeedMidiClock(m, offset, false);
  }
  EmitPending(*pending, runEnd, WrapperEvent::Source::Ui, offset);
  if (first) {
    EmitLive(uiEvents_.data(), uiCount_, offset);
    if (resync_) {  // the queue overflowed: re-send every mirror
      resync_ = false;
      for (size_t i = 0; i < kNumLeafParams; ++i) {
        Emit({WrapperEvent::Type::Param, WrapperEvent::Source::Ui, static_cast<uint32_t>(LeafId(i)),
              params_[i]->Plain()},
             offset);
      }
      Emit({WrapperEvent::Type::Param, WrapperEvent::Source::Ui, kEffectVolume, effectVolume_->Plain()}, offset);
      Emit({WrapperEvent::Type::Param, WrapperEvent::Source::Ui, kPerfSubdiv, subdiv_->Plain()}, offset);
      Emit({WrapperEvent::Type::Param, WrapperEvent::Source::Ui, kPerfTimeMode, timeMode_->Plain()}, offset);
      Emit({WrapperEvent::Type::Freeze, WrapperEvent::Source::Ui, 0u, freeze_->get() ? 1.0f : 0.0f}, offset);
    }
    *frameZero = false;
  }
  *pending = runEnd;
}

void BrainscapeProcessor::WriteBackMirrors() noexcept {
  // The mirrors follow what the engine was sent, so two producers racing on one
  // parameter settle on the engine's value within a block.
  for (size_t i = 0; touched_.any() && i < kNumLeafParams; ++i) {
    if (touched_.test(i)) params_[i]->StoreMirror(sent_[i]);
  }
  touched_.reset();
  if (effectTouched_) effectVolume_->StoreMirror(sentEffectVolume_);
  effectTouched_ = false;
  if (subdivTouched_) subdiv_->StoreMirror(sentSubdiv_);
  if (timeModeTouched_) timeMode_->StoreMirror(sentTimeMode_);
  subdivTouched_ = timeModeTouched_ = false;
}

// ── Audio ──────────────────────────────────────────────────────────────────────────

void BrainscapeProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) {
  // No juce::ScopedNoDenormals: the engine's own guard owns the FP control word around
  // every entry point (companion §4.6, profile §4.1).
  const int numSamples = buffer.getNumSamples();
  if (numSamples > 0) lastHostBlock_.store(numSamples, std::memory_order_relaxed);
  if (!engineReady_) {
    buffer.clear();
    return;
  }
  const int numOut = std::min(getTotalNumOutputChannels(), buffer.getNumChannels());
  const int numIn  = std::min(getTotalNumInputChannels(), buffer.getNumChannels());

  // Drained first, so the state unit read next is at least as new as every drained event.
  DrainEvents();
  // Zero-frame calls (VST3 sends them whenever buses exist) render nothing (§4.3): their
  // events wait for the next block, whose first frame is the frame they were posted at.
  if (numSamples <= 0) return;
  if (numOut == 0) {
    buffer.clear();
    return;
  }
  resync_ = sink_.TakeResync();
  ApplyStateUnit();
  // The host's tempo and transport (clock.md §4.4), read once: a transport start restarts first
  // (with the option on), so the follower's events come after the restart's re-asserts. A
  // Spillover load this block may play its stored tempo (recall Preset): the host's goes after it.
  const HostTransport transport = ReadTransport();
  const bool          startEdge = CheckTransportStart(transport.playing);
  if (loadPending_) follower_.ResendTempo();
  hostTempoCount_ = follower_.Block(
      transport, tempoSource_.load(std::memory_order_relaxed) == static_cast<uint32_t>(TempoSource::Host),
      startEdge, numSamples, engineRateInt_, hostTempo_.data());
  // Receive MIDI clock gates the translator: turned off, it forgets the master, whose Stop it will
  // not see (clock.md §11.16), so a later Exact load re-asserts no stale position or Continue.
  const bool midiClockOn = receiveMidiClock_.load(std::memory_order_relaxed);
  if (!midiClockOn && midiClockOn_) midiClock_.Reset();
  midiClockOn_ = midiClockOn;

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
  const float outDb = outputGainDb_.load(std::memory_order_relaxed) + monitorTrimDb_.load(std::memory_order_relaxed);
  if (outDb != outGainDb_) {
    outGainDb_ = outDb;
    outGain_   = GainFromDb(outDb);
  }

  // Chunks of at most 512 frames (§4.3), each one Process call with its events stamped at
  // their offsets: the engine splits at them (§4.10, profile §5.11).
  const uint64_t start     = framePos_;
  size_t         pending   = 0;
  auto           midiIt    = midi.cbegin();
  bool           frameZero = true;
  for (int frame = 0; frame < numSamples;) {
    const int      len        = std::min(numSamples - frame, static_cast<int>(kMaxChunk));
    const uint64_t chunkStart = start + static_cast<uint64_t>(frame);
    const uint64_t chunkEnd   = chunkStart + static_cast<uint64_t>(len);
    numBlockEvents_           = 0;
    seq_                      = 0;
    for (;;) {
      uint64_t due = frameZero ? start : std::numeric_limits<uint64_t>::max();
      if (pending < pendingCount_) due = std::min(due, std::max(pending_[pending].frame, start));
      if (midiIt != midi.cend()) {
        due = std::min(due, start + static_cast<uint64_t>(std::max(0, (*midiIt).samplePosition)));
      }
      if (due >= chunkEnd) break;
      EmitDue(due, start, chunkStart, &pending, midi, &midiIt, &frameZero);
    }
    RenderChunk(hostInL, hostInR, outL, outR, frame, len);
    frame += len;
  }
  // Pending events this block applied leave the list; note-ons stamped past the block
  // apply at the next block's first frame, after its host automation.
  std::copy(pending_.begin() + static_cast<std::ptrdiff_t>(pending),
            pending_.begin() + static_cast<std::ptrdiff_t>(pendingCount_), pending_.begin());
  pendingCount_ -= pending;
  framePos_ = start + static_cast<uint64_t>(numSamples);
  for (; midiIt != midi.cend(); ++midiIt) {
    const juce::MidiMessageMetadata m = *midiIt;
    if (m.numBytes >= 3 && (m.data[0] & 0xF0u) == 0x90u && m.data[2] > 0u) {
      WrapperEvent e{WrapperEvent::Type::Trigger, WrapperEvent::Source::Midi,
                     static_cast<uint32_t>(Engine::TriggerSource::MidiNote),
                     static_cast<float>(m.data[2]) / 127.0f};
      e.frame = framePos_;
      InsertPending(e);
    }
    if (midiClockOn_) FeedMidiClock(m, 0u, true);
  }
  hostCount_ = uiCount_ = 0;
  WriteBackMirrors();
  sentHash_.store(ValuesHash(sent_, activeModeHash_, sentEffectVolume_), std::memory_order_relaxed);

  // Engine::Tempo() is the audio thread's (§2.6): the BPM panel, the session and the re-asserts
  // read copies. The committed tempo waits while a newer unit (a restored session's tempo) has not
  // applied yet.
  const TempoInfo info = live_->engine.Tempo();
  carryNs_             = info.nsPerQuarter;
  lastSource_          = info.source;
  if (stateSeq_.load(std::memory_order_acquire) == seqApplied_) {
    liveNs_.store(info.nsPerQuarter, std::memory_order_relaxed);
  }
  dispSource_.store(info.source, std::memory_order_relaxed);
  dispSubdiv_.store(info.subdiv, std::memory_order_relaxed);
  dispTimeMode_.store(info.timeMode, std::memory_order_relaxed);
  dispRate_.store(engineRateInt_, std::memory_order_relaxed);
  dispFlags_.store(info.flags, std::memory_order_relaxed);
  dispPosition_.store(info.position, std::memory_order_relaxed);
  dispFollowing_.store(follower_.Following(), std::memory_order_relaxed);
  dispHostOctaves_.store(static_cast<int8_t>(follower_.Octaves()), std::memory_order_relaxed);
  dispHostClamped_.store(follower_.Clamped(), std::memory_order_relaxed);

  for (int c = 2; c < numOut; ++c) buffer.clear(c, 0, numSamples);
  onsets_.fetch_add(live_->engine.ConsumeOnsetCount(), std::memory_order_relaxed);
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
  // Input level, then the live input function, dsp/'s SanitizeInput (companion §4.8,
  // profile §3.7: NaN and ±inf -> +0, every finite value unchanged). At 0 dB the input
  // passes bit-exact.
  if (inGain_ != 1.f) {
    for (int i = 0; i < numFrames; ++i) {
      l[i] *= inGain_;
      r[i] *= inGain_;
    }
  }
  SanitizeInput(l, l, static_cast<size_t>(numFrames));
  SanitizeInput(r, r, static_cast<size_t>(numFrames));
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
  ctx.events    = blockEvents_.data();
  ctx.numEvents = static_cast<uint32_t>(numBlockEvents_);
  live_->engine.Process(ctx);
  engineCalls_.fetch_add(1u, std::memory_order_relaxed);

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
