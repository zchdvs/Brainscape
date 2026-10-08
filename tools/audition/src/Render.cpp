#include "Render.h"

#include <algorithm>
#include <cstring>

#include "brainscape/EventQueue.h"
#include "brainscape/HostArenas.h"
#include "brainscape/InputCondition.h"
#include "brainscape/ModeEval.h"
#include "brainscape/Params.h"

namespace bsa {

using brainscape::Engine;
using brainscape::EngineConfig;
using brainscape::ParamId;
using brainscape::PresetState;

namespace {

EngineConfig CanonicalConfig() {
  EngineConfig cfg;  // determinism profile §2.3
  cfg.sampleRate      = kRate;
  cfg.maxBlockSize    = kMaxBlock;
  cfg.historyFrames   = 1u << 22;
  cfg.stereoInput     = true;
  cfg.ditherRingWrite = true;
  return cfg;
}

}  // namespace

struct Renderer::Impl {
  Impl() : arenas(brainscape::PlanMemory(CanonicalConfig())) {
    engine = std::make_unique<Engine>();
    queue  = std::make_unique<brainscape::EventQueue>();
    ok     = arenas.ok() && engine->Init(CanonicalConfig(), arenas.get());
    blockEvents.resize(brainscape::EventQueue::kCapacity);
  }

  brainscape::host::HeapArenas    arenas;
  std::unique_ptr<Engine>         engine;
  std::unique_ptr<brainscape::EventQueue> queue;
  std::vector<Engine::BlockEvent> blockEvents;
  bool                            ok = false;
};

Renderer::Renderer() : impl_(std::make_unique<Impl>()) {}
Renderer::~Renderer() = default;

bool Renderer::ok() const { return impl_->ok; }

bool Renderer::Render(const RenderRequest& rq, RenderResult* result) {
  *result = RenderResult{};
  auto fail = [result](const char* why) {
    result->error = why;
    return false;
  };
  if (!impl_->ok) return fail("the engine could not be set up");
  if (rq.preset == nullptr || rq.input == nullptr) return fail("no preset or no input");
  if (rq.input->l.size() != rq.input->r.size()) return fail("the input's channels differ in length");
  if (rq.blockPattern.empty()) return fail("an empty block pattern");
  for (const uint32_t n : rq.blockPattern) {
    if (n == 0 || n > kMaxBlock) return fail("a block size outside 1..512");
  }
  for (size_t i = 0; i < rq.events.size(); ++i) {
    const ScriptEvent& e = rq.events[i];
    if (e.frame < 0 || (i > 0 && e.frame < rq.events[i - 1].frame)) {
      return fail("script events out of frame order");
    }
    if (e.type == Engine::EventType::SpilloverLoad &&
        (e.staged >= rq.staged.size() || rq.staged[e.staged] == nullptr)) {
      return fail("a SpilloverLoad without its staged preset");
    }
  }

  Engine& engine = *impl_->engine;
  // The device settings (mode-compiler.md §3.8) are part of the recipe: every render starts
  // from their defaults, whatever an earlier render left. A load keeps Global rows.
  for (const brainscape::ParamDescriptor& d : brainscape::kParamTable) {
    if (d.kind == brainscape::ParamKind::Global) engine.SetParam(d.id, d.def);
  }
  engine.LoadPreset(*rq.preset, brainscape::LoadMode::Exact, &result->load);
  if (!result->load.applied) return fail("the preset did not load (an invalid mode)");
  impl_->queue->Clear();       // the Exact load restarted the engine: a new timeline
  engine.ConsumeOnsetCount();  // zeroed by the restart; read so the count starts here
  const Engine::GrainStats first = engine.Stats();  // the counts run on: the render's are deltas
  uint64_t                 born  = first.births;

  const Stereo   in     = Conditioned(*rq.input, rq.mode);
  const size_t   frames = in.Frames();
  Stereo&        out    = result->out;
  out.l.assign(frames, 0.f);
  out.r.assign(frames, 0.f);
  result->onsetSeconds.assign((frames + kRate - 1) / kRate, 0u);
  result->birthSeconds.assign((frames + kRate - 1) / kRate, 0u);

  uint32_t seq       = 0;
  size_t   next      = 0;  // the next script event to push
  size_t   blockIdx  = 0;
  for (size_t pos = 0; pos < frames;) {
    if (rq.cancel != nullptr && rq.cancel->load(std::memory_order_relaxed)) {
      impl_->queue->Clear();  // no staged preset stays referenced
      return fail("cancelled");
    }
    const auto n = static_cast<uint32_t>(
        std::min<size_t>(rq.blockPattern[blockIdx++ % rq.blockPattern.size()], frames - pos));
    const auto end = static_cast<int64_t>(pos + n);
    // The block's events, stamped on the engine timeline (render frame = engine frame).
    for (; next < rq.events.size() && rq.events[next].frame < end; ++next) {
      const ScriptEvent& e = rq.events[next];
      Engine::Event      ev;
      ev.frame = e.frame;
      ev.seq   = seq++;
      ev.type  = e.type;
      ev.id    = e.id;
      ev.value = e.value;
      if (e.type == Engine::EventType::SpilloverLoad) ev.preset = rq.staged[e.staged];
      if (!impl_->queue->Push(ev)) {
        return fail("the event transport refused an event (more than its capacity in one block)");
      }
      ++result->events;
    }
    Engine::ProcessContext ctx;
    const float*           ins[2]  = {in.l.data() + pos, in.r.data() + pos};
    float*                 outs[2] = {out.l.data() + pos, out.r.data() + pos};
    ctx.in        = ins;
    ctx.out       = outs;
    ctx.numFrames = n;
    ctx.events    = impl_->blockEvents.data();
    ctx.numEvents = impl_->queue->PopBlock(static_cast<int64_t>(pos), n, impl_->blockEvents.data(),
                                           static_cast<uint32_t>(impl_->blockEvents.size()));
    engine.Process(ctx);
    const uint32_t onsets = engine.ConsumeOnsetCount();
    result->onsets += onsets;
    result->onsetSeconds[pos / kRate] += onsets;
    const uint64_t births = engine.Stats().births;
    result->birthSeconds[pos / kRate] += static_cast<uint32_t>(births - born);
    born = births;
    pos += n;
  }
  const Engine::GrainStats last = engine.Stats();
  result->births      = last.births - first.births;
  result->burstBirths = last.burstBirths - first.burstBirths;
  result->steals      = last.steals - first.steals;
  // Retire the last block's events, so no staged preset is referenced after the render.
  impl_->queue->PopBlock(static_cast<int64_t>(frames), 0, impl_->blockEvents.data(), 0);
  impl_->queue->Clear();
  return true;
}

Stereo Conditioned(const Stereo& in, InputMode mode) {
  Stereo c;
  c.l.resize(in.l.size());
  c.r.resize(in.r.size());
  brainscape::ConditionInput24(in.l.data(), c.l.data(), in.l.size());
  brainscape::ConditionInput24(in.r.data(), c.r.data(), in.r.size());
  if (mode == InputMode::Mono) c.r = c.l;
  return c;
}

std::unique_ptr<PresetState> LeafPreset(const float* values) {
  auto state = std::make_unique<PresetState>();
  for (size_t i = 0; i < brainscape::kNumLeafParams; ++i) {
    state->leaves[i] = {static_cast<uint32_t>(brainscape::LeafId(i)), values[i]};
  }
  state->leafCount = static_cast<uint32_t>(brainscape::kNumLeafParams);
  return state;
}

float LeafValue(const PresetState& preset, ParamId id) {
  for (uint32_t i = 0; i < preset.leafCount && i < PresetState::kMaxLeaves; ++i) {
    if (preset.leaves[i].id == static_cast<uint32_t>(id)) return preset.leaves[i].value;
  }
  const brainscape::ParamDescriptor* d = brainscape::FindParam(id);
  return d != nullptr ? d->def : 0.f;
}

void SetLeaf(PresetState* preset, ParamId id, float value) {
  const auto raw = static_cast<uint32_t>(id);
  uint32_t   at  = 0;
  for (; at < preset->leafCount; ++at) {
    if (preset->leaves[at].id == raw) {
      preset->leaves[at].value = value;
      return;
    }
    if (preset->leaves[at].id > raw) break;
  }
  if (preset->leafCount >= PresetState::kMaxLeaves) return;
  for (uint32_t k = preset->leafCount; k > at; --k) preset->leaves[k] = preset->leaves[k - 1];
  preset->leaves[at] = {raw, value};
  ++preset->leafCount;
}

bool MacroDefined(const PresetState& preset, ParamId macro) {
  const brainscape::MacroTable& t = preset.mode.macros;
  for (uint32_t i = 0; i < t.macroCount && i < brainscape::kMaxMacros; ++i) {
    if (t.macros[i].id == static_cast<uint32_t>(macro)) return t.macros[i].count > 0;
  }
  return false;
}

float StoredPosition(const PresetState& preset, ParamId macro) {
  if (!MacroDefined(preset, macro)) return -1.f;
  const brainscape::ControlState& c = preset.control;
  if (c.present != 0) {
    for (uint32_t i = 0; i < c.macroCount && i < brainscape::kMaxMacros; ++i) {
      if (c.positions[i].macroId == static_cast<uint32_t>(macro)) return c.positions[i].position;
    }
  }
  return 0.5f;
}

void AtPosition(PresetState* preset, ParamId macro, float position) {
  if (!MacroDefined(*preset, macro)) return;
  brainscape::PresetLeaf out[brainscape::kMaxMacroTargets];
  const size_t           n =
      brainscape::EvalMacro(preset->mode, macro, position, out, brainscape::kMaxMacroTargets);
  for (size_t k = 0; k < n; ++k) SetLeaf(preset, static_cast<ParamId>(out[k].id), out[k].value);
  brainscape::ControlState& c = preset->control;
  if (c.present == 0) return;
  for (uint32_t i = 0; i < c.macroCount && i < brainscape::kMaxMacros; ++i) {
    if (c.positions[i].macroId == static_cast<uint32_t>(macro)) {
      // CTRL holds canonical positions: what the macro row's Canonicalize gives.
      c.positions[i].position = brainscape::Canonicalize(macro, position);
    }
  }
}

}  // namespace bsa
