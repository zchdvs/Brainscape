#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Corpus.h"
#include "brainscape/Engine.h"
#include "brainscape/EventQueue.h"
#include "brainscape/Memory.h"

// Renders one (vector, preset) pair through the public Engine API from the exact-restart
// state, hashing the output as it goes (docs/design/determinism-profile.md §6.1: SHA-256
// of the interleaved little-endian float32 output, plus one per second) and counting
// coverage.
namespace brainscape::golden {

// The calling thread's floating-point control word during a render (profile §6.4). Clean
// leaves the thread's default; Hostile installs FTZ|DAZ (Arm FZ|DN) with round toward zero
// for the whole render, so every engine entry point, Init included, is entered from it, as
// from a host thread that set it. The engine's guard must make that change nothing.
enum class FpEnv : uint8_t { Clean, Hostile };

struct RenderConfig {
  std::vector<uint32_t> blockPattern{48};  // grid from frame 0, repeated; each <= 512
  uint32_t              historyLog2 = 22;  // 2^22 is the canonical ring (profile §2.3)
  // EngineConfig::maxBlockSize: 512 in every golden leg, 48 in the pedal's own engine (the
  // firmware's live configuration). It sizes buffers only, so it must not change a bit.
  uint32_t              maxBlockSize = 512;
  Delivery              delivery    = Delivery::Engine;
  // Init a new engine for every render instead of restarting one engine (Restart, by
  // way of LoadPreset(..., Exact)). Both are the exact-restart state, so both must
  // render the same bits.
  bool                  freshEngine = false;
  FpEnv                 fpEnv       = FpEnv::Clean;
};

struct RenderOutput {
  std::string              hash;
  std::vector<std::string> secondHashes;
  int64_t                  counters[static_cast<size_t>(Counter::kCount)] = {};
  int64_t                  firstOnsetBlock = -1;  // block start, so only block-accurate
  // The earliest frame at which the ring length can reach the output (profile §6.4:
  // the re-anchor, mark staleness, the far guard), or the render length if never.
  int64_t                  ringReachFrame = 0;
  // The last restart mid-render and the hash of the output from it on, or -1.
  int64_t                  restartFrame = -1;
  std::string              restartHash;
  // The committed package the preset starts from: its sound_hash and control_hash, 64 hex
  // digits as bspc prints them (the package rule, mode-compiler.md §8.3); empty without one.
  std::string              soundHash, controlHash;
};

// The output samples, kept only to write a WAV for a preset that misses its golden.
struct Capture {
  std::vector<float> l, r;
};

class Renderer {
 public:
  // The engine arenas and the Engine object in caller-owned memory instead of the heap: the
  // firmware places them as the pedal does (DTCM, AXI SRAM, SDRAM; grain-engine.md §7). Each
  // arena holds at least PlanMemory's bytes at its alignment; `engine` holds sizeof(Engine)
  // bytes aligned to alignof(Engine).
  struct Placement {
    Arenas arenas{};
    void*  engine = nullptr;
  };

  explicit Renderer(const RenderConfig& cfg);
  Renderer(const RenderConfig& cfg, const Placement& placement);
  ~Renderer();
  Renderer(const Renderer&) = delete;
  Renderer& operator=(const Renderer&) = delete;

  bool ok() const { return ok_; }
  const RenderConfig& Config() const { return cfg_; }
  uint32_t HistoryFrames() const { return 1u << cfg_.historyLog2; }

  // Renders the vector's input from frame `inputStart` on; the script's frames count from
  // there.
  bool Render(const VectorCase& v, const std::vector<testsignal::Note>& notes,
              const PresetCase& p, RenderOutput* out, Capture* capture = nullptr,
              int64_t inputStart = 0);

 private:
  bool RenderIn(const VectorCase& v, const std::vector<testsignal::Note>& notes,
                const PresetCase& p, RenderOutput* out, Capture* capture, int64_t inputStart);
  EngineConfig MakeEngineConfig() const;
  Engine*      NewEngine();

  RenderConfig                     cfg_;
  Arenas                           arenas_{};
  void*                            raw_[kNumTiers] = {};
  bool                             ok_ = false;
  Engine*                          engine_ = nullptr;  // Init'd once unless freshEngine
  std::unique_ptr<Engine>          ownedEngine_;       // the heap engine without a Placement
  void*                            engineStorage_ = nullptr;
  std::unique_ptr<EventQueue>      queue_;   // the engine's event transport
  std::vector<Engine::BlockEvent>  blockEvents_;
};

// SHA-256 of the vector's input, interleaved little-endian float32.
std::string InputHash(const VectorCase& v, const std::vector<testsignal::Note>& notes);

std::vector<testsignal::Note> VectorNotes(const VectorCase& v);

}  // namespace brainscape::golden
