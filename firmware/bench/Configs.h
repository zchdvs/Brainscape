#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Corpus.h"
#include "EventScript.h"
#include "brainscape/Engine.h"
#include "brainscape/EventQueue.h"
#include "brainscape/Params.h"
#include "brainscape/Preset.h"
#include "brainscape/TestSignal.h"

// The bench images' configurations (firmware/README.md; docs/design/cpu-budget.md §8.2), built
// and rendered the same way on the Seed and on the host. The bench (bench/main.cpp) times each
// block with DWT; the host tool brainscape_bench_configs (firmware/tests/bench_configs.cpp)
// renders them with the same engine code for the output hashes the device must match (§8.3, G1).
// Nothing here measures or depends on the platform: a block is handed to a hook that runs it.
namespace brainscape::fw::bench {

inline constexpr uint32_t kBlock     = 48;
inline constexpr uint32_t kMaxBlocks = 124u * 1000u;  // 124 s of 48-frame blocks

using golden::ParamList;
using golden::PresetSource;
using P = ParamId;

// The event stream a configuration runs, stamped into block b of the engine timeline in (frame,
// sequence) order and delivered as the live image delivers events, through an EventQueue. Raw
// streams: the firmware's event contract (platform/EventBudget.h) would carry or drop part of the
// floods, and the bench measures the engine under what the plugin or a careless producer sends.
enum class Events : uint8_t {
  None,
  Player,         // session 1's "events": every block cutoff, pitch and size sweeps; every 250 ms
                  // four footswitch triggers 12 frames apart; every 1.5 s a freeze toggle; every
                  // second a Spillover load switching mode (FastCut and Trails in turn)
  TriggerFlood,   // 48 footswitch triggers at each block's first frame: one fires every frame
  Every50ms,      // a footswitch trigger every 2,400 frames, at the block it falls in
  Midi48,         // 48 MIDI-note triggers at each block's first frame for 2 s, then 2 s quiet
  MacroSweep,     // the eight macros moving every block, a footswitch trigger every 300 blocks
  ActivityMax,    // macro.activity to 1 at frame 0
  Sweep,          // EventPlan::param moving every block, a triangle over `period` blocks
  EveryN,         // one EventPlan::type event at block 16k + 8 (the per-event micro)
  Fuzz,           // seeded random parameter and event streams (EventPlan::seed)
};

struct EventPlan {
  Events            kind   = Events::None;
  uint32_t          seed   = 0;              // Fuzz
  ParamId           param  = ParamId::Mix;   // Sweep
  float             lo     = 0.f, hi = 0.f;  // Sweep: the triangle's ends
  uint32_t          period = 0;              // Sweep: blocks per triangle
  Engine::EventType type   = Engine::EventType::SetParam;  // EveryN
  uint32_t          id     = 0;              // EveryN: SetParam's row or MacroMove's macro
  float             value  = 0.f;            // EveryN: alternates with `lo` each time
};

struct Spec {
  std::string                   config;   // what the result line calls it
  std::string                   input;    // which notes
  PresetSource                  preset;   // loaded Exact: a corpus package or the defaults,
                                          // with parameters over them
  const char*                   structure = nullptr;  // a package's mode and CTRL instead
  const PresetState*            state     = nullptr;  // or a complete preset (a factory one)
  std::vector<testsignal::Note> notes;
  uint32_t                      frames = 0;
  bool                          cold       = false;  // the bench: clean+invalidate D and I caches per block
  bool                          forceFlush = false;  // the hooks build: FZ inside the guard
  EventPlan                     events;
  // > 0: statistics for the active part [0, tail) and the silent tail [tail, end) too, the tail
  // starting at the first block after the last input frame.
  uint32_t                      tailStartFrame = 0;
  // The births suite: equal voice counts at two grain lengths.
  uint32_t                      voices = 0, grainFrames = 0;
};

// The corpus and the presets every configuration shares, built once (the bench at boot).
class Bench {
 public:
  // False with *why when a package the configurations need does not decode or load.
  bool Setup(Engine* engine, const char** why);

  Engine&                               engine() { return *engine_; }
  const std::vector<golden::VectorCase>& corpus() const { return corpus_; }

  // ---- The suites' configurations --------------------------------------------------------
  // session 1's suites (blocks warm and cold, stages, births, tail) and the "restart" render
  std::vector<Spec> Blocks(bool quick) const;
  std::vector<Spec> Stages(bool quick) const;
  std::vector<Spec> Births(bool quick) const;
  std::vector<Spec> Tail() const;
  Spec              RestartRender() const;
  // §8.2: corners, wave 1's stressors, CLOCK and the fuzz sets
  std::vector<Spec> Stress(bool quick) const;
  // §8.2's per-stage and per-event costs by difference: each post stage alone with its mix
  // settled and moving (and the delay's head gliding), and each event type every 16 blocks
  std::vector<Spec> Post(bool quick) const;
  std::vector<Spec> EventCosts(bool quick) const;
  // The configurations whose blocks the ledger suite logs one by one
  std::vector<Spec> LedgerSet(bool quick) const;

  // ---- One render ------------------------------------------------------------------------
  // Runs block b: the bench times engine.Process(ctx) there, the host just calls it.
  using BlockFn = void (*)(void* user, uint32_t b, const Engine::ProcessContext& ctx);
  struct Rendered {
    bool        loaded = false;  // the Exact load applied exactly
    uint32_t    blocks = 0;
    uint32_t    eventsPushed = 0, eventsRefused = 0;
    std::string hash;            // SHA-256 of the output, frame by frame (L, R as binary32 LE)
  };
  Rendered Render(const Spec& s, BlockFn fn, void* user);

  // The spec's preset, built (also what SuiteRestart loads).
  std::unique_ptr<PresetState> Build(const Spec& s) const;

 private:
  Spec Make(const std::string& config, const char* vector, PresetSource preset, uint32_t seconds,
            const char* structure = nullptr) const;
  Spec MakePessimistic(const std::string& config, const char* vector, float grainMs, uint32_t seconds,
                       const ParamList& changes = {}, const char* structure = "strum_marks") const;
  PresetSource Corpus(const char* vector, const char* preset) const;
  const golden::VectorCase* FindVector(const char* name) const;
  void Push(uint32_t b, const EventPlan& plan, uint32_t* seq, Rendered* r);

  Engine*                         engine_ = nullptr;
  std::vector<golden::VectorCase> corpus_;
  std::unique_ptr<PresetState>    loadA_, loadB_;   // the Player stream's Spillover payloads
  std::unique_ptr<PresetState>    macro8_;          // a macro with eight curved targets
  std::unique_ptr<PresetState>    murmuration_;     // the factory preset (firmware/factory)
  std::vector<std::unique_ptr<PresetState>> fuzzLoads_;  // the fuzz streams' loads
  EventQueue                      queue_;
  Engine::BlockEvent              blockEvents_[EventQueue::kCapacity];
  uint32_t                        fuzzState_ = 0;
};

// Session 1's configurations, for SuiteRestart and the ledger suite.
ParamList Nominal();
ParamList Pessimistic(float grainMs);
ParamList With(ParamList base, const ParamList& changes);

}  // namespace brainscape::fw::bench
