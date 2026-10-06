#include "Render.h"

#include <cstdlib>
#include <cstring>
#include <memory>

#include "EventScript.h"
#include "Sha256.h"
#include "brainscape/Engine.h"

namespace brainscape::golden {

namespace {

constexpr int64_t kSecond      = 48000;
constexpr int64_t kPedalBlock  = 48;
constexpr float   kActiveLevel = 0x1p-16f;  // about -96 dBFS

int64_t& At(RenderOutput* out, Counter c) { return out->counters[static_cast<size_t>(c)]; }

// How far behind its position's reference a grain can read, at the parameter maxima
// (profile §6.4): base delay, spray, an attack offset and the read span L * (1 + r)
// at r = 4, plus the 64-frame guard margin, §5.7's 512-frame write-ahead and one
// 256-frame onset hop (a mark can precede the block that reports its onset).
int64_t RingSlackFrames() {
  auto ms = [](ParamId id) { return static_cast<int64_t>(FindParam(id)->max) * 48; };
  return ms(ParamId::DelayMs) + ms(ParamId::SprayMs) + 6 * ms(ParamId::GrainSizeMs) + 64 + 512 +
         256;
}

// Interleaved little-endian float32, the byte stream every hash is taken over.
void PackFrames(const float* l, const float* r, uint32_t n, uint8_t* bytes) {
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t u[2];
    std::memcpy(&u[0], &l[i], 4);
    std::memcpy(&u[1], &r[i], 4);
    for (int c = 0; c < 2; ++c) {
      for (int b = 0; b < 4; ++b) {
        bytes[8u * i + 4u * c + b] = static_cast<uint8_t>(u[c] >> (8 * b));
      }
    }
  }
}

// Streams frames into the whole-render hash and the per-second hashes.
class OutputHasher {
 public:
  void Add(const float* l, const float* r, uint32_t n) {
    PackFrames(l, r, n, bytes_);
    uint32_t done = 0;
    while (done < n) {
      const int64_t toSecond = kSecond - frame_ % kSecond;
      const auto    take     = static_cast<uint32_t>(toSecond < n - done ? toSecond : n - done);
      all_.Update(bytes_ + 8u * done, 8u * take);
      second_.Update(bytes_ + 8u * done, 8u * take);
      done += take;
      frame_ += take;
      if (frame_ % kSecond == 0) seconds_.push_back(second_.Hex());
    }
  }
  void Finish(RenderOutput* out) {
    if (frame_ % kSecond != 0) seconds_.push_back(second_.Hex());
    out->hash         = all_.Hex();
    out->secondHashes = std::move(seconds_);
  }

 private:
  Sha256                   all_, second_;
  std::vector<std::string> seconds_;
  int64_t                  frame_ = 0;
  uint8_t                  bytes_[512 * 8];
};

}  // namespace

std::vector<testsignal::Note> VectorNotes(const VectorCase& v) {
  const uint32_t count = testsignal::BuildVector(v.source, v.activeFrames, nullptr, 0);
  std::vector<testsignal::Note> notes(count);
  testsignal::BuildVector(v.source, v.activeFrames, notes.data(), count);
  return notes;
}

std::string InputHash(const VectorCase& v, const std::vector<testsignal::Note>& notes) {
  auto gen = std::make_unique<testsignal::Generator>();
  gen->Start(notes.data(), static_cast<uint32_t>(notes.size()));
  float   l[512], r[512];
  uint8_t bytes[512 * 8];
  Sha256  sha;
  for (uint32_t pos = 0; pos < v.frames;) {
    const uint32_t n = v.frames - pos < 512u ? v.frames - pos : 512u;
    gen->Render(l, r, n);
    PackFrames(l, r, n, bytes);
    sha.Update(bytes, 8u * n);
    pos += n;
  }
  return sha.Hex();
}

Renderer::Renderer(const RenderConfig& cfg) : cfg_(cfg) {
  EngineConfig ec;
  ec.historyFrames      = 1u << cfg_.historyLog2;
  const MemoryPlan plan = PlanMemory(ec);
  ok_                   = !cfg_.blockPattern.empty();
  for (uint32_t b : cfg_.blockPattern) ok_ = ok_ && b >= 1 && b <= ec.maxBlockSize;
  for (size_t t = 0; t < kNumTiers; ++t) {
    arenas_.bytes[t] = plan.bytes[t];
    if (plan.bytes[t] == 0) continue;
    const size_t align = plan.align[t] > 64 ? plan.align[t] : 64;
    raw_[t]            = std::malloc(plan.bytes[t] + align);
    if (raw_[t] == nullptr) { ok_ = false; continue; }
    const auto a =
        (reinterpret_cast<uintptr_t>(raw_[t]) + align - 1) & ~(static_cast<uintptr_t>(align) - 1);
    arenas_.base[t] = reinterpret_cast<void*>(a);
  }
}

Renderer::~Renderer() {
  for (void* p : raw_) std::free(p);
}

bool Renderer::Render(const VectorCase& v, const std::vector<testsignal::Note>& notes,
                      const PresetCase& p, RenderOutput* out, Capture* capture) {
  *out = RenderOutput{};
  At(out, Counter::LastActiveFrame)  = -1;
  At(out, Counter::LastNonzeroFrame) = -1;
  if (!ok_) return false;

  // Exact-restart state (profile §2.3 #3), then LoadPreset(P, Exact). The engine is
  // Init'd once and every render restarts it, so each render after the first checks
  // that Restart returns a used engine to the state Init leaves.
  EngineConfig cfg;
  cfg.historyFrames = 1u << cfg_.historyLog2;
  if (engine_ == nullptr || cfg_.freshEngine) {
    for (size_t t = 0; t < kNumTiers; ++t) {
      if (arenas_.base[t] != nullptr) std::memset(arenas_.base[t], 0, arenas_.bytes[t]);
    }
    engine_ = std::make_unique<Engine>();
    if (!engine_->Init(cfg, arenas_)) {
      engine_.reset();
      return false;
    }
  }
  Engine& engine = *engine_;
  // A complete preset (companion §6.1): every leaf, the preset's values over the defaults.
  auto preset = std::make_unique<PresetState>();
  for (uint32_t i = 0; i < kNumParams; ++i) {
    preset->leaves[i] = {static_cast<uint32_t>(kParamTable[i].id), kParamTable[i].def};
  }
  preset->leafCount = static_cast<uint32_t>(kNumParams);
  for (const auto& kv : p.params) {
    preset->leaves[static_cast<uint32_t>(kv.first) - 1u].value = kv.second;
  }
  if (!engine.LoadPreset(*preset, LoadMode::Exact)) return false;  // inexact: a corpus bug

  auto gen = std::make_unique<testsignal::Generator>();
  gen->Start(notes.data(), static_cast<uint32_t>(notes.size()));
  EventCursor  cursor(p.script.Events());
  OutputHasher hasher;

  const int64_t ring        = cfg.historyFrames;
  const int64_t reanchorAge = ring - ring / 4;  // GranularCore::Process's 3/4-ring rule
  const int64_t slack       = RingSlackFrames();
  // The first re-anchor, or the far guard measured from the pin if that is sooner.
  const int64_t pinReach    = reanchorAge + 1 < ring - slack ? reanchorAge + 1 : ring - slack;
  const int64_t frames      = v.frames;
  bool          frozen      = false;
  int64_t       pinAbs      = 0;
  float         feedback    = engine.GetParam(ParamId::Feedback);  // the smoother's target
  int64_t       reach       = slack >= ring ? 0 : frames;
  auto          reachBy     = [&reach](int64_t f) { reach = f < reach ? f : reach; };

  int32_t qL[512], qR[512];
  float   inL[512], inR[512], outL[512], outR[512];
  std::vector<Engine::BlockEvent> blockEvents;
  size_t  patternIdx = 0;
  int64_t gridEnd    = cfg_.blockPattern[0];
  for (int64_t pos = 0; pos < frames;) {
    int64_t end = gridEnd < frames ? gridEnd : frames;  // clamp to what remains
    // Onsets are reported at hop boundaries (frames 256k - 1) and counted per block, so
    // a block whose hop boundaries straddle a freeze toggle could not say which were
    // frozen: it ends at the toggle. Output is block-split invariant, so only counting
    // sees this cut. Split delivery cuts at every event anyway.
    const int64_t firstHop = pos + 255 - (pos & 255);
    const int64_t lastHop  = (end & ~int64_t{255}) - 1;
    if (cfg_.delivery == Delivery::Split) {
      const int64_t next = cursor.NextAfter(pos);
      if (next < end) end = next;
    } else if (firstHop < lastHop) {
      const int64_t toggle = cursor.NextFreeze(firstHop, lastHop);
      if (toggle < end) end = toggle;
    }
    const auto n = static_cast<uint32_t>(end - pos);

    // The block's events: their counters, and the frozen and feedback state of each
    // stretch between them.
    const Event*   evs = nullptr;
    const uint32_t nev = cursor.Take(end, &evs);
    blockEvents.clear();
    int64_t segStart        = pos;
    bool    frozenAtHop     = false;
    bool    frozenAtHopSeen = false;
    auto    closeSegment    = [&](int64_t segEnd) {
      if (frozen) {
        At(out, Counter::FrozenFrames) += segEnd - segStart;
        if (pinAbs + pinReach < segEnd) reachBy(pinAbs + pinReach);
      }
      if (feedback > 1.0f) At(out, Counter::FbAbove1Frames) += segEnd - segStart;
      segStart = segEnd;
    };
    for (uint32_t i = 0; i < nev; ++i) {
      const Event& ev = evs[i];
      if (ev.frame > segStart) closeSegment(ev.frame);
      if (ev.frame > firstHop && !frozenAtHopSeen) {
        frozenAtHop     = frozen;
        frozenAtHopSeen = true;
      }
      if (cfg_.delivery == Delivery::Split) {
        ApplyUnstamped(engine, ev);
      } else {
        blockEvents.push_back(ToBlockEvent(ev, pos));
      }
      ++At(out, Counter::Events);
      if (ev.frame % kPedalBlock != 0) ++At(out, Counter::OffGridEvents);
      if (ev.type == EventType::Trigger) ++At(out, Counter::Triggers);
      if (ev.type == EventType::Freeze) {
        const bool on = ev.value != 0.f;
        if (on && !frozen) {
          ++At(out, Counter::FreezeEngages);
          pinAbs = ev.frame;
        }
        frozen = on;
      }
      if (ev.type == EventType::SetParam && ev.id == ParamId::Feedback) {
        feedback = Canonicalize(ev.id, ev.value);
      }
    }
    closeSegment(end);
    if (!frozenAtHopSeen) frozenAtHop = frozen;

    gen->RenderQ23(qL, qR, n);
    for (uint32_t i = 0; i < n; ++i) {
      inL[i] = testsignal::Q23ToFloat(qL[i]);
      inR[i] = testsignal::Q23ToFloat(qR[i]);
      if (qL[i] == testsignal::kQ23Max || qL[i] == testsignal::kQ23Min ||
          qR[i] == testsignal::kQ23Max || qR[i] == testsignal::kQ23Min) {
        ++At(out, Counter::InClipFrames);
      }
    }

    const float* ins[2]  = {inL, inR};
    float*       outs[2] = {outL, outR};
    Engine::ProcessContext ctx;
    ctx.in        = ins;
    ctx.out       = outs;
    ctx.numFrames = n;
    ctx.events    = blockEvents.data();
    ctx.numEvents = static_cast<uint32_t>(blockEvents.size());
    engine.Process(ctx);

    const uint32_t onsets = engine.ConsumeOnsetCount();
    if (onsets > 0) {
      At(out, Counter::Onsets) += onsets;
      if (frozenAtHop) At(out, Counter::FrozenOnsets) += onsets;
      if (out->firstOnsetBlock < 0) {
        out->firstOnsetBlock = pos;
        reachBy(pos + ring - slack);  // no mark is older than the first onset
      }
    }

    for (uint32_t i = 0; i < n; ++i) {
      const float l = outL[i], r = outR[i];
      const bool  active =
          l > kActiveLevel || l < -kActiveLevel || r > kActiveLevel || r < -kActiveLevel;
      const bool silent = qL[i] == 0 && qR[i] == 0;
      if (silent) ++At(out, Counter::SilentInFrames);
      if (active) {
        ++At(out, Counter::OutActiveFrames);
        if (silent) ++At(out, Counter::TailActiveFrames);
        At(out, Counter::LastActiveFrame) = pos + i;
      }
      if (l != 0.f || r != 0.f) At(out, Counter::LastNonzeroFrame) = pos + i;
    }
    hasher.Add(outL, outR, n);
    if (capture != nullptr) {
      capture->l.insert(capture->l.end(), outL, outL + n);
      capture->r.insert(capture->r.end(), outR, outR + n);
    }

    pos = end;
    if (pos == gridEnd) gridEnd += cfg_.blockPattern[++patternIdx % cfg_.blockPattern.size()];
  }

  At(out, Counter::Frames) = frames;
  out->ringReachFrame      = reach;
  hasher.Finish(out);
  return true;
}

}  // namespace brainscape::golden
