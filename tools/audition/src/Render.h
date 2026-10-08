#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "brainscape/Engine.h"
#include "brainscape/PresetState.h"

// The shared offline render (docs/design/companion-app.md §4.9; mode-compiler.md §8.1, §9.1):
// one preset loaded Exact on an engine in the canonical configuration, an input conditioned as
// the pedal's codec hands it over, and an event script delivered through the engine's own
// transport at its stamps. The app, `bspc render` and the audition scripts all render through
// it, so a render is the same bits wherever it is made. No JUCE (companion §3.5), and nothing
// in namespace brainscape (the symbol-scan rule, companion §3.4): namespace bsa.
namespace bsa {

inline constexpr uint32_t kRate       = 48000;  // the only rate a render runs at
inline constexpr uint32_t kPedalBlock = 48;     // the pedal's block
inline constexpr uint32_t kMaxBlock   = 512;    // EngineConfig::maxBlockSize of the render

// Planar stereo at 48 kHz.
struct Stereo {
  std::vector<float> l, r;
  size_t Frames() const { return l.size(); }
};

// How the input jacks hand the input over (companion §4.8): Mono plays R := L.
enum class InputMode : uint8_t { Mono = 0, Stereo = 1 };

// One event of a render's script, at a render frame (frame 0 is the first frame after the Exact
// load). Events at one frame apply in list order. A SpilloverLoad's `id` is its SwitchStyle and
// `staged` the index of its preset in RenderRequest::staged.
struct ScriptEvent {
  int64_t                       frame  = 0;
  brainscape::Engine::EventType type   = brainscape::Engine::EventType::SetParam;
  uint32_t                      id     = 0;
  float                         value  = 0.f;
  uint32_t                      staged = 0;
};

struct RenderRequest {
  const brainscape::PresetState* preset = nullptr;  // loaded Exact before frame 0
  // 48 kHz; the render conditions a copy (ConditionInput24 on each channel, then the mode).
  const Stereo*                  input  = nullptr;
  InputMode                      mode   = InputMode::Stereo;
  std::vector<ScriptEvent>       events;  // by frame; same-frame events in order
  std::vector<const brainscape::PresetState*> staged;  // SpilloverLoad presets
  // Block sizes from frame 0, repeated; each in 1..kMaxBlock. Any pattern renders the same bits
  // (the engine's contract); the pedal's is {48}.
  std::vector<uint32_t>          blockPattern{kPedalBlock};
  // Read before every block: once true, the render stops and fails ("cancelled"). It changes no
  // bits of a render it lets finish.
  const std::atomic<bool>*       cancel = nullptr;
};

struct RenderResult {
  Stereo                 out;
  brainscape::LoadReport load;     // the Exact load's report
  uint64_t               onsets  = 0;  // ConsumeOnsetCount over the render
  // The same per 1 s of the render, each block's count in the second it starts in.
  std::vector<uint32_t>  onsetSeconds;
  uint32_t               events  = 0;  // events delivered
  std::string            error;        // why a render failed
};

// An engine in the canonical configuration (determinism profile §2.3: 48 kHz, a 2^22-frame
// ring, stereo input, dither on the ring write), reused across renders: every render starts
// with the device settings at their defaults and LoadPreset(Exact), which restarts the engine,
// so each render begins from the exact-restart state, as one on a new engine would.
class Renderer {
 public:
  Renderer();
  ~Renderer();
  Renderer(const Renderer&)            = delete;
  Renderer& operator=(const Renderer&) = delete;

  bool ok() const;
  // False, with result->error, when the request is malformed, the preset does not load, or
  // the engine's transport refuses an event (a render outside the parity contract).
  bool Render(const RenderRequest& request, RenderResult* result);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// The input as the render conditions it: ConditionInput24 on each channel, then R := L for Mono.
Stereo Conditioned(const Stereo& in, InputMode mode);

// A complete leaf-only preset: the default mode and CTRL, every Leaf row from values[ordinal]
// (Params.h kLeafParams order, kNumLeafParams values), as the plugin's parameter list holds it.
std::unique_ptr<brainscape::PresetState> LeafPreset(const float* values);

// The value a preset stores for a leaf, or the row's default when it stores none.
float LeafValue(const brainscape::PresetState& preset, brainscape::ParamId id);
// Sets a leaf's stored value, adding the leaf when the preset lacks it (ids kept ascending).
void SetLeaf(brainscape::PresetState* preset, brainscape::ParamId id, float value);

// The stored position of a macro (CTRL's pickup reference), or 0.5, the compiler's default for
// an omitted one; -1 when the mode leaves the macro undefined.
float StoredPosition(const brainscape::PresetState& preset, brainscape::ParamId macro);
bool  MacroDefined(const brainscape::PresetState& preset, brainscape::ParamId macro);
// The preset at other macro positions, as a stored one: each targeted leaf set to EvalMacro at
// its position (the derive rule, mode-compiler.md §3.5) and CTRL's position updated. A macro
// the mode leaves undefined is skipped.
void AtPosition(brainscape::PresetState* preset, brainscape::ParamId macro, float position);

}  // namespace bsa
