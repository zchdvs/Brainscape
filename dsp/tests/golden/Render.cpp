#include "Render.h"

#include <cstdlib>
#include <cstring>
#include <memory>

#include "../FpEnvTestUtil.h"
#include "EventScript.h"
#include "Sha256.h"
#include "brainscape/Engine.h"

namespace brainscape::golden {

namespace {

constexpr int64_t kSecond      = 48000;
constexpr int64_t kPedalBlock  = 48;
constexpr float   kActiveLevel = 0x1p-16f;  // about -96 dBFS

// The control word's mode bits: what a guarded call must hand back to its caller. The
// status bits change with the caller's own arithmetic (x86 MXCSR flags; FPSCR's cumulative
// flags and the NZCV of its comparisons on the M7).
#if defined(BRAINSCAPE_FPENV_ARM32)
constexpr detail::FpWord kModeMask = ~(detail::kFpFlagBits | 0xF0000000u);
#elif defined(BRAINSCAPE_FPENV_AARCH64)
constexpr detail::FpWord kModeMask = ~detail::FpWord{0};  // FPCR holds no status bits
#else
constexpr detail::FpWord kModeMask = ~detail::kFpFlagBits;
#endif

int64_t& At(RenderOutput* out, Counter c) { return out->counters[static_cast<size_t>(c)]; }

bool Nonzero(float x) {  // on the bits: DAZ makes a subnormal compare equal to 0
  uint32_t u;
  std::memcpy(&u, &x, sizeof u);
  return (u & 0x7FFFFFFFu) != 0;
}

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

// Streams frames into the whole-render hash, the per-second hashes and the hash of the
// output since the last restart.
class OutputHasher {
 public:
  void Add(const float* l, const float* r, uint32_t n) {
    PackFrames(l, r, n, bytes_);
    segment_.Update(bytes_, 8u * n);
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
  void Restart() { segment_.Reset(); }
  void Finish(RenderOutput* out) {
    if (frame_ % kSecond != 0) seconds_.push_back(second_.Hex());
    out->hash         = all_.Hex();
    out->secondHashes = std::move(seconds_);
    if (out->restartFrame >= 0) out->restartHash = segment_.Hex();
  }

 private:
  Sha256                   all_, second_, segment_;
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

Renderer::Renderer(const RenderConfig& cfg)
    : cfg_(cfg),
      queue_(std::make_unique<EventQueue>()),
      blockEvents_(EventQueue::kCapacity) {
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
                      const PresetCase& p, RenderOutput* out, Capture* capture,
                      int64_t inputStart) {
  *out = RenderOutput{};
  At(out, Counter::LastActiveFrame)  = -1;
  At(out, Counter::LastNonzeroFrame) = -1;
  if (!ok_) return false;
  if (cfg_.fpEnv == FpEnv::Clean) return RenderIn(v, notes, p, out, capture, inputStart);
  const testing::HostileFpScope scope(testing::kHostileFpWord);
  const bool rendered = RenderIn(v, notes, p, out, capture, inputStart);
  // Every guarded entry point restores the caller's word on return.
  return rendered &&
         (detail::ReadFpControl() & kModeMask) == (testing::kHostileFpWord & kModeMask);
}

bool Renderer::RenderIn(const VectorCase& v, const std::vector<testsignal::Note>& notes,
                        const PresetCase& p, RenderOutput* out, Capture* capture,
                        int64_t inputStart) {
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
  // Inexact presets are corpus bugs.
  if (!engine.LoadPreset(*CompletePreset(p.params), LoadMode::Exact)) return false;
  queue_->Clear();  // the load restarted the engine: a new timeline
  StagedPresets staged;
  for (const ParamList& s : p.script.Staged()) {
    staged.push_back(CompletePreset(s));
    if (!CheckPreset(*staged.back())) return false;
  }
  const auto stagedFeedback = [&staged](uint32_t i) {
    return staged[i]->leaves[static_cast<uint32_t>(ParamId::Feedback) - 1u].value;
  };

  const int64_t frames   = static_cast<int64_t>(v.frames) - inputStart;
  const auto&   restarts = p.script.Restarts();
  if (frames <= 0) return false;
  for (const RestartPoint& r : restarts) {
    if (r.frame <= 0 || r.frame >= frames) return false;
  }

  int32_t qL[512], qR[512];
  float   inL[512], inR[512], outL[512], outR[512];
  auto    gen = std::make_unique<testsignal::Generator>();
  gen->Start(notes.data(), static_cast<uint32_t>(notes.size()));
  for (int64_t skip = inputStart; skip > 0;) {
    const auto n = static_cast<uint32_t>(skip < 512 ? skip : 512);
    gen->RenderQ23(qL, qR, n);
    skip -= n;
  }
  EventCursor  cursor(p.script.Events());
  OutputHasher hasher;

  const int64_t ring        = cfg.historyFrames;
  const int64_t reanchorAge = ring - ring / 4;  // GranularCore::Process's 3/4-ring rule
  const int64_t slack       = RingSlackFrames();
  // The first re-anchor, or the far guard measured from the pin if that is sooner.
  const int64_t pinReach    = reanchorAge + 1 < ring - slack ? reanchorAge + 1 : ring - slack;
  bool          frozen      = false;
  bool          freezeLevel = false;  // what the events at the current frame leave
  int64_t       pinAbs      = 0;
  float         feedback    = engine.GetParam(ParamId::Feedback);  // the smoother's target
  int64_t       reach       = slack >= ring ? 0 : frames;
  auto          reachBy     = [&reach](int64_t f) { reach = f < reach ? f : reach; };
  int64_t       base        = 0;  // the render frame of the engine timeline's frame 0
  size_t        nextRestart = 0;

  size_t  patternIdx = 0;
  int64_t gridEnd    = cfg_.blockPattern[0];
  for (int64_t pos = 0; pos < frames;) {
    // A restart before the events stamped at its frame. It begins a new engine timeline, so
    // the queue is cleared with it and later stamps count from here.
    for (; nextRestart < restarts.size() && restarts[nextRestart].frame == pos; ++nextRestart) {
      const RestartPoint& r = restarts[nextRestart];
      if (r.load) {
        if (!engine.LoadPreset(*staged[r.staged], LoadMode::Exact)) return false;
        feedback = stagedFeedback(r.staged);
      } else {
        engine.Restart();
      }
      queue_->Clear();
      base        = pos;
      frozen      = false;
      freezeLevel = false;
      ++At(out, Counter::Restarts);
      out->restartFrame = pos;
      hasher.Restart();
    }
    int64_t end = gridEnd < frames ? gridEnd : frames;  // clamp to what remains
    if (nextRestart < restarts.size() && restarts[nextRestart].frame < end) {
      end = restarts[nextRestart].frame;
    }
    // Onsets are reported at hop boundaries (engine frames 256k - 1) and counted per block,
    // so a block whose hop boundaries straddle a freeze toggle could not say which were
    // frozen: it ends at the toggle. Output is block-split invariant, so only counting
    // sees this cut. Split delivery cuts at every event anyway.
    const int64_t enginePos = pos - base;
    const int64_t firstHop  = base + enginePos + 255 - (enginePos & 255);
    const int64_t lastHop   = base + ((end - base) & ~int64_t{255}) - 1;
    if (cfg_.delivery == Delivery::Split) {
      const int64_t next = cursor.NextAfter(pos);
      if (next < end) end = next;
    } else if (firstHop < lastHop) {
      const int64_t toggle = cursor.NextFreeze(firstHop, lastHop);
      if (toggle < end) end = toggle;
    }
    const auto n = static_cast<uint32_t>(end - pos);

    // The block's events: their counters, and the frozen and feedback state of each
    // stretch between them. Freeze is a level settled after each frame's events, as the
    // engine settles it; a Spillover load's freeze-off is immediate.
    const Event*   evs = nullptr;
    const uint32_t nev = cursor.Take(end, &evs);
    int64_t segStart        = pos;
    bool    frozenAtHop     = false;
    bool    frozenAtHopSeen = false;
    auto    settleFreeze    = [&] {
      if (freezeLevel && !frozen) {
        ++At(out, Counter::FreezeEngages);
        pinAbs = segStart;
      }
      frozen = freezeLevel;
    };
    auto closeSegment = [&](int64_t segEnd) {
      settleFreeze();
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
        ApplyUnstamped(engine, ev, staged);
      } else if (!queue_->Push(ToEngineEvent(ev, base, staged))) {
        return false;  // refused: outside the contract (profile §5.11)
      }
      ++At(out, Counter::Events);
      if (ev.frame % kPedalBlock != 0) ++At(out, Counter::OffGridEvents);
      switch (ev.type) {
        case EventType::SetParam:
          if (ev.id == static_cast<uint32_t>(ParamId::Feedback)) {
            feedback = Canonicalize(ParamId::Feedback, ev.value);
          }
          break;
        case EventType::Freeze: freezeLevel = ev.value != 0.f; break;
        case EventType::Trigger: ++At(out, Counter::Triggers); break;
        case EventType::SpilloverLoad:
          ++At(out, Counter::Loads);
          freezeLevel = false;
          frozen      = false;
          feedback    = stagedFeedback(ev.id);
          break;
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
    if (cfg_.delivery == Delivery::Engine) {
      // The transport hands the block every event it holds: the ones pushed above.
      ctx.events    = blockEvents_.data();
      ctx.numEvents = queue_->PopBlock(pos - base, n, blockEvents_.data(),
                                       static_cast<uint32_t>(blockEvents_.size()));
      if (ctx.numEvents != nev) return false;
    }
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
      if (Nonzero(l) || Nonzero(r)) At(out, Counter::LastNonzeroFrame) = pos + i;
    }
    hasher.Add(outL, outR, n);
    if (capture != nullptr) {
      capture->l.insert(capture->l.end(), outL, outL + n);
      capture->r.insert(capture->r.end(), outR, outR + n);
    }

    pos = end;
    if (pos == gridEnd) gridEnd += cfg_.blockPattern[++patternIdx % cfg_.blockPattern.size()];
  }
  if (queue_->ConsumeRefused() != 0) return false;

  At(out, Counter::Frames) = frames;
  out->ringReachFrame      = reach;
  hasher.Finish(out);
  return true;
}

}  // namespace brainscape::golden
