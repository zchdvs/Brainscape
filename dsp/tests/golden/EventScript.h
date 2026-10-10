#pragma once
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "brainscape/Engine.h"
#include "brainscape/EventQueue.h"
#include "brainscape/Preset.h"
#include "brainscape/Tempo.h"

// Frame-stamped event scripts (docs/design/determinism-profile.md §5.11): the engine's
// own events, (absolute frame, sequence number, type, id, value). Frames before an
// event's stamp use the old state, the event applies from its frame, and same-frame
// events apply in sequence order. Besides events, a script can restart the engine
// mid-render (§5.8), which Process cannot do: a restart runs with Process stopped.
namespace brainscape::golden {

using ParamList = std::vector<std::pair<ParamId, float>>;
using Event     = Engine::Event;
using EventType = Engine::EventType;

// A preset as the corpus writes it (docs/design/mode-compiler.md §10.3): a committed package of
// the corpus (presets/NAME.bsp, compiled by bspc from presets/NAME.json and decoded through
// DecodePreset), or without one the default mode, with `params` over its leaves. Structure
// comes only from packages, never from assigning ModeBlob fields (§4.4); `params` over a
// package serve the ablations.
struct PresetSource {
  const char* package = nullptr;
  ParamList   params;
};

// What a load stages: the preset, and a Spillover load's switch style (an Exact load restarts).
struct StagedLoad {
  PresetSource preset;
  SwitchStyle  style = SwitchStyle::Trails;
};

// Structure an ablation switches off (Corpus.h), in every preset a render loads.
enum Strip : uint16_t {
  kStripOnset   = 1u << 0,  // `onset` leaves scheduler.sources
  kStripMark    = 1u << 1,  // layer 0 reads the live position, not marks
  kStripMode    = 1u << 2,  // the default mode and CTRL (Mode.h, PresetState.h); leaves kept
  kKeepMode     = 1u << 3,  // every load keeps the starting preset's mode and CTRL
  kStripSources = 1u << 4,  // the default sources join scheduler.sources (sound revision 4)
  kStripPitchSet    = 1u << 5,  // layer 0 plays the default set {0: 1} by `cycle` (revision 5)
  kStripPitchSelect = 1u << 6,  // layer 0's `random` selection becomes `cycle` (revision 5)
  kStripClock       = 1u << 7,  // `clock` leaves scheduler.sources (sound revision 8)
  kStripSubdiv      = 1u << 8,  // the stored subdivision is TAP (sound revision 8)
};

// A restart at `frame`, before the events stamped there: Engine::Restart, which keeps the
// parameter values, or with `load` a LoadPreset(..., Exact) of a staged preset. Either
// begins a new engine timeline, whose frame 0 is `frame`.
struct RestartPoint {
  int64_t  frame  = 0;
  bool     load   = false;
  uint32_t staged = 0;  // load: its preset in Script::Staged()
};

class Script {
 public:
  void Param(int64_t frame, ParamId id, float value) {
    Add(frame, EventType::SetParam, static_cast<uint32_t>(id), value);
  }
  void Freeze(int64_t frame, bool on) { Add(frame, EventType::Freeze, 0, on ? 1.f : 0.f); }
  // A trigger from `src`, which fires only if the playing mode lists its source (sound revision
  // 4, mode-compiler.md §7.5).
  void Trigger(int64_t frame, Engine::TriggerSource src = Engine::TriggerSource::Footswitch) {
    Add(frame, EventType::Trigger, static_cast<uint32_t>(src), 1.f);
  }
  // A macro move or an expression-pedal move (mode-compiler.md §3.4, events 4 and 5).
  void Macro(int64_t frame, ParamId macro, float position) {
    Add(frame, EventType::MacroMove, static_cast<uint32_t>(macro), position);
  }
  void Expression(int64_t frame, float position) {
    Add(frame, EventType::Expression, 0, position);
  }
  // A Spillover load of a complete preset, `params` over the defaults (profile §5.10), or of a
  // committed package: a mode change when its mode differs (mode-compiler.md §7.3).
  void Spillover(int64_t frame, ParamList params, SwitchStyle style = SwitchStyle::Trails) {
    Add(frame, EventType::SpilloverLoad, Stage({{nullptr, std::move(params)}, style}), 0.f);
  }
  void SpilloverPackage(int64_t frame, const char* package,
                        SwitchStyle style = SwitchStyle::Trails) {
    Add(frame, EventType::SpilloverLoad, Stage({{package, {}}, style}), 0.f);
  }
  void Restart(int64_t frame) { AddRestart({frame, false, 0}); }
  void ExactLoad(int64_t frame, ParamList params) {
    AddRestart({frame, true, Stage({{nullptr, std::move(params)}, SwitchStyle::Trails})});
  }
  void ExactLoadPackage(int64_t frame, const char* package) {
    AddRestart({frame, true, Stage({{package, {}}, SwitchStyle::Trails})});
  }
  // The tempo core's events (sound revision 8, docs/design/clock.md §4.1, §8.3), payloads as
  // Tempo.h builds them: a tap; a tempo in ns per quarter; a MIDI clock tick; a transport (Start
  // and Locate carry a position in 24-ppqn ticks, an exact binary32; AtNextTick for MIDI's, an
  // anchor offset for the host's); a Subdivision event setting the subdivision (§5.1's code) or
  // the time mode. No unstamped call exists for them (§2.6): split delivery hands each to the
  // block that starts at its frame (Render.cpp).
  void Tap(int64_t frame) { Add(frame, EventType::Tap, 0, 0.f); }
  void Tempo(int64_t frame, uint32_t nsPerQuarter) {
    Add(frame, EventType::Tempo, nsPerQuarter, 0.f);
  }
  void Tick(int64_t frame) { Add(frame, EventType::ClockTick, 0, 0.f); }
  void Transport(int64_t frame, tempo::TransportKind kind, uint32_t position = 0,
                 bool atNextTick = false, uint32_t offset = 0) {
    Add(frame, EventType::Transport, tempo::TransportId(kind, atNextTick, offset),
        static_cast<float>(position));  // exact: positions are below 2^24
  }
  void Subdivision(int64_t frame, tempo::SubdivField field, uint8_t code) {
    Add(frame, EventType::Subdivision, tempo::SubdivisionId(field, code), 0.f);
  }
  // Any event, raw: an invalid payload or an unknown type (§4.2).
  void Raw(int64_t frame, EventType type, uint32_t id, float value) { Add(frame, type, id, value); }

  // Sorted by (frame, seq); frames are the render's. A SpilloverLoad event carries its
  // staged preset's index in `id` and no `preset`: the render binds the decoded preset and
  // puts the load's switch style in `id`.
  const std::vector<Event>&        Events() const { return events_; }
  std::vector<Event>&              MutableEvents() { return events_; }
  const std::vector<RestartPoint>& Restarts() const { return restarts_; }
  std::vector<RestartPoint>&       MutableRestarts() { return restarts_; }
  const std::vector<StagedLoad>&   Staged() const { return staged_; }
  std::vector<StagedLoad>&         MutableStaged() { return staged_; }

 private:
  void     Add(int64_t frame, EventType type, uint32_t id, float value);
  void     AddRestart(const RestartPoint& r);
  uint32_t Stage(StagedLoad load) {
    staged_.push_back(std::move(load));
    return static_cast<uint32_t>(staged_.size() - 1u);
  }

  std::vector<Event>        events_;
  std::vector<RestartPoint> restarts_;  // sorted by frame
  std::vector<StagedLoad>   staged_;
  uint32_t                  seq_ = 0;
};

// The clock generators (docs/design/clock.md §8.3), integer-only and seeded through TestSignal.h's
// SplitMix32. ClockTicks adds `count` ticks at `nsPerQuarter`: the ideal tick i at
// start + floor(i·ns·48,000 / (24·10^9)), exactly, then the model's jitter: None plays the ideal
// frame; Hardware stamps it on the pedal's 48-frame block grid (the first block boundary at or
// after it, uniform over a block); Computer adds the sum of four uniform draws in [-141, 141]
// (σ ≈ 163 frames, 3.4 ms) and holds every 500th tick 1,843 frames (38.4 ms), the ticks after it
// bunching behind it. Frames never fall below the script's latest tick. Ticks whose ideal frame
// lies in [dropFrom, dropTo) are lost (a dropout). Returns the ideal frame of tick `count`, where
// a continuing run starts.
enum class TickModel : uint8_t { None, Hardware, Computer };
int64_t ClockTicks(Script& script, int64_t start, uint32_t nsPerQuarter, uint32_t count,
                   TickModel model, uint32_t seed, int64_t dropFrom = -1, int64_t dropTo = -1);
// Taps at `start` and then each interval after the last, each moved by a draw in
// [-spread, spread] frames. Returns the last tap's frame.
int64_t TapSeries(Script& script, int64_t start, const std::vector<int64_t>& intervals,
                  uint32_t spread, uint32_t seed);

// A complete preset (companion §6.1): every leaf, `params` over the defaults.
std::unique_ptr<PresetState> CompletePreset(const ParamList& params);

// The committed package presets/NAME.bsp, decoded; false when it cannot be read or decoded.
// A program that links the embedded packages (EmbeddedPackages.h: the firmware images and
// brainscape_parity_stream) reads its compiled-in copy of the file instead.
bool LoadPackage(const char* name, PresetState* out, PackageInfo* info = nullptr);

// A preset source, complete: the package's state (or the default mode's leaves) with `params`
// over its leaves, then `strip`'s structure switched off, and the mode and CTRL of `modeFrom`
// when given (kKeepMode). Null when the package cannot be loaded. *info receives the
// package's header.
std::unique_ptr<PresetState> CompletePreset(const PresetSource& source, uint16_t strip = 0,
                                            const PresetState* modeFrom = nullptr,
                                            PackageInfo* info = nullptr);

// A script's staged presets, decoded for the render, with each load's switch style.
struct StagedState {
  std::unique_ptr<PresetState> state;
  SwitchStyle                  style = SwitchStyle::Trails;
};
using StagedPresets = std::vector<StagedState>;

// How events reach the engine: through the engine's transport (Engine, the contract's
// delivery: each block's events go into an EventQueue stamped with engine frames, and
// Process takes PopBlock's offsets), or through SetParam, SetFreeze, Trigger and
// LoadPreset with every block split at the events' frames (Split, what a wrapper does
// without engine-side events). Both must render the same bits.
enum class Delivery : uint8_t { Engine, Split };

// Walks a script block by block.
class EventCursor {
 public:
  explicit EventCursor(const std::vector<Event>& events) : events_(events) {}

  // The frame of the first event not yet taken that is stamped after `frame`, or
  // INT64_MAX.
  int64_t NextAfter(int64_t frame) const;
  // The first freeze or Spillover event not yet taken with a frame in (after, upTo], or
  // INT64_MAX.
  int64_t NextFreeze(int64_t after, int64_t upTo) const;
  // Takes the events stamped before `end` as [*first, *first + count) of the script.
  uint32_t Take(int64_t end, const Event** first);

 private:
  const std::vector<Event>& events_;
  size_t                    next_ = 0;
};

// Split delivery: applies one event through the unstamped API, which the next Process
// call applies at its first frame. A macro or expression move has no unstamped call, so it is
// what a wrapper without engine-side events would send: its leaves, evaluated with
// EvalMacro or EvalExpression on `active` (the preset whose mode the engine plays), as
// SetParams. Events 6-10 have none at all (docs/design/clock.md §2.6): the caller hands them to
// the block that starts at their frame, at offset 0 (IsTempoEvent).
void ApplyUnstamped(Engine& engine, const Event& e, const StagedPresets& staged,
                    const PresetState& active);
// Events 6-10 and any later type: what split delivery passes as block events.
inline bool IsTempoEvent(const Event& e) {
  return static_cast<uint8_t>(e.type) >= tempo::kEventTap;
}
// Engine delivery: the event on the engine timeline that began at render frame `base`,
// with a SpilloverLoad's staged preset bound.
Event ToEngineEvent(const Event& e, int64_t base, const StagedPresets& staged);

}  // namespace brainscape::golden
