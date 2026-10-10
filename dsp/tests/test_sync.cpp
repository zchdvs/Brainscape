// Synced times (sound revision 9, docs/design/clock.md §2.3, §5.2-§5.4, §6.1, §6.2, §7, §8.2):
// the note values and their octave folds against a 128-bit reference, the producers' durations
// and display text, the post chain's crossfade, chain of fades and slew on the chain itself, and
// in the engine row 63 and base_sync on the exact frames, each change of the committed tempo in
// its class, global.tempo_glide, block-split invariance and Restart.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "WideInt.h"
#include "brainscape/Engine.h"
#include "brainscape/HostArenas.h"
#include "brainscape/Mode.h"
#include "brainscape/ParamDisplay.h"
#include "brainscape/Preset.h"
#include "brainscape/Tempo.h"
#include "brainscape/TestSignal.h"
#include "catch.hpp"
#include "detail/FpEnvGuard.h"
#include "detail/PostChain.h"
#include "detail/Smoother.h"
#include "detail/Tempo.h"

using namespace brainscape;

namespace {

using Ev     = Engine::Event;
using EvType = Engine::EventType;
using Params = std::vector<std::pair<ParamId, float>>;
using tempo::SubdivField;
using tempo::SyncTarget;
using tempo::TransportKind;

constexpr uint32_t kRate   = 48000;
constexpr uint32_t kUs120  = 500000;
constexpr uint32_t kUs140  = 428571;     // 140 BPM stored: 857.14 frames per tick
constexpr uint32_t kUs1375 = 436364;     // 137.5 BPM stored
constexpr uint32_t kNs90   = 666666667;  // 90 BPM

// The 128-bit reference of §2.3: round-half-up(Pc · ticks · s · 2^octaves / (576 · 2^32)).
uint64_t RefDuration(uint64_t pc, uint32_t ticks, uint32_t s, int32_t octaves) {
  uint64_t q = 0;
  const uint64_t ts = static_cast<uint64_t>(ticks) * s;
  if (octaves >= 0) {
    REQUIRE(testing::RefMulDivRoundU64(pc << octaves, ts, 576ull << 32, &q));
  } else {
    REQUIRE(testing::RefMulDivRoundU64(pc, ts, (576ull << 32) << -octaves, &q));
  }
  return q;
}

// §5.3's fold, by its words: halved while above the maximum, doubled while below the minimum,
// each octave from Pc.
uint64_t RefFolded(uint64_t pc, uint32_t ticks, uint32_t s, uint64_t lo, uint64_t hi, int32_t* k) {
  *k         = 0;
  uint64_t f = RefDuration(pc, ticks, s, 0);
  while (f > hi) f = RefDuration(pc, ticks, s, --*k);
  while (f < lo) f = RefDuration(pc, ticks, s, ++*k);
  return f;
}

// ── The engine side ─────────────────────────────────────────────────────────────────────────

struct Stereo {
  std::vector<float> l, r;
};

bool Same(const Stereo& a, const Stereo& b) {
  return a.l.size() == b.l.size() &&
         std::memcmp(a.l.data(), b.l.data(), a.l.size() * sizeof(float)) == 0 &&
         std::memcmp(a.r.data(), b.r.data(), a.r.size() * sizeof(float)) == 0;
}

// Plucks every 100 ms over a quiet noise floor (test_clock.cpp's input).
Stereo Plucks(size_t frames) {
  uint32_t x    = 0x2468ACE1u;
  auto     next = [&x] {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return static_cast<float>(x & 0xFFFFFFu) / 8388608.0f - 1.0f;
  };
  Stereo s{std::vector<float>(frames), std::vector<float>(frames)};
  for (size_t i = 0; i < frames; ++i) {
    const float a = next();
    const float b = next();
    s.l[i] = 0.001f * a;
    s.r[i] = 0.001f * b;
  }
  for (size_t at = 1200; at + 2400 < frames; at += 4800) {
    float env = 0.6f;
    for (size_t i = 0; i < 2400; ++i, env *= 0.998f) {
      const float v = env * next();
      s.l[at + i] += v;
      s.r[at + i] += 0.8f * v;
    }
  }
  return s;
}

Stereo Impulse(size_t frames) {
  Stereo s{std::vector<float>(frames, 0.0f), std::vector<float>(frames, 0.0f)};
  s.l[0] = 1.0f;
  s.r[0] = 1.0f;
  return s;
}

EngineConfig Config(double sampleRate = 48000.0) {
  EngineConfig cfg;
  cfg.sampleRate      = sampleRate;
  cfg.historyFrames   = 1u << 16;
  cfg.ditherRingWrite = false;
  return cfg;
}

struct Rig {
  EngineConfig     cfg;
  host::HeapArenas arenas;
  Engine           engine;
  explicit Rig(const EngineConfig& c = Config()) : cfg(c), arenas(PlanMemory(c)) {
    REQUIRE(arenas.ok());
    REQUIRE(engine.Init(cfg, arenas.get()));
  }
};

// A complete preset of the default mode, `params` over the defaults, layer 0's base_sync and the
// stored performance state given.
std::unique_ptr<PresetState> Preset(const Params& params, uint32_t us = kUs120,
                                    uint8_t baseSync = 0, uint8_t subdiv = 0, uint8_t mode = 0) {
  auto s = std::make_unique<PresetState>();
  for (size_t i = 0; i < kNumLeafParams; ++i) {
    s->leaves[i] = {static_cast<uint32_t>(LeafId(i)), FindParam(LeafId(i))->def};
  }
  s->leafCount = static_cast<uint32_t>(kNumLeafParams);
  for (const auto& p : params) s->leaves[LeafIndex(p.first)].value = p.second;
  s->mode.layers[0].baseSync  = baseSync;
  s->mode.features            = RequiredModeFeatures(s->mode);
  s->performance.usPerQuarter = us;
  s->performance.subdiv       = static_cast<Subdivision>(subdiv);
  s->performance.timeMode     = static_cast<TimeMode>(mode);
  REQUIRE(ComputeModeHash(s->mode, &s->mode.modeHash));
  PresetDiagnostic d;
  REQUIRE(ValidateMode(*s, &d));
  LoadReport report;
  REQUIRE(CheckPreset(*s, &report));
  return s;
}

// The grain path as a clean delay (test_engine.cpp's DegenerateDelay): one rectangular 10 ms
// grain at a time, no randomness, so an impulse comes out whole `delayMs` later.
Params Clean(float delayMs) {
  return {{ParamId::DelayMs, delayMs},    {ParamId::Mix, 1.0f},          {ParamId::Feedback, 0.0f},
          {ParamId::GrainSizeMs, 10.0f},  {ParamId::Overlap, 0.25f},     {ParamId::SprayMs, 0.0f},
          {ParamId::SpreadCents, 0.0f},   {ParamId::ReverseProb, 0.0f},  {ParamId::Jitter, 0.0f},
          {ParamId::WindowSustain, 1.0f}, {ParamId::WindowSmooth, 0.0f}, {ParamId::PanSpread, 0.0f}};
}

Params With(Params p, const Params& more) {
  for (const auto& kv : more) p.push_back(kv);
  return p;
}

Ev Event(int64_t frame, uint32_t seq, EvType type, uint32_t id = 0, float value = 0.f) {
  Ev e;
  e.frame = frame;
  e.seq   = seq;
  e.type  = type;
  e.id    = id;
  e.value = value;
  return e;
}
Ev Tap(int64_t f) { return Event(f, 0, EvType::Tap); }
Ev TempoNs(int64_t f, uint32_t ns) { return Event(f, 0, EvType::Tempo, ns); }
Ev Tick(int64_t f) { return Event(f, 0, EvType::ClockTick); }
Ev Subdiv(int64_t f, SubdivField field, uint8_t code) {
  return Event(f, 0, EvType::Subdivision, tempo::SubdivisionId(field, code));
}
Ev Set(int64_t f, ParamId id, float v) {
  return Event(f, 0, EvType::SetParam, static_cast<uint32_t>(id), v);
}
Ev Load(int64_t f, const PresetState* preset) {
  Ev e     = Event(f, 0, EvType::SpilloverLoad, static_cast<uint32_t>(SwitchStyle::Trails));
  e.preset = preset;
  return e;
}

// Sorted by frame, sequence numbers in that order.
std::vector<Ev> Sorted(std::vector<Ev> ev) {
  std::stable_sort(ev.begin(), ev.end(), [](const Ev& a, const Ev& b) { return a.frame < b.frame; });
  for (uint32_t i = 0; i < ev.size(); ++i) ev[i].seq = i;
  return ev;
}

// Renders `in` from the engine's frame in blocks of `pattern` (repeated), with `events` stamped.
Stereo Render(Engine& e, const Stereo& in, const std::vector<Ev>& events,
              const std::vector<uint32_t>& pattern = {48}) {
  const int64_t start = e.SampleCounter();
  Stereo        out{std::vector<float>(in.l.size()), std::vector<float>(in.l.size())};
  std::vector<Engine::BlockEvent> block;
  size_t next = 0, bi = 0;
  for (size_t pos = 0; pos < in.l.size();) {
    const size_t  n  = std::min<size_t>(pattern[bi++ % pattern.size()], in.l.size() - pos);
    const int64_t f0 = start + static_cast<int64_t>(pos);
    block.clear();
    while (next < events.size() && events[next].frame < f0 + static_cast<int64_t>(n)) {
      const Ev&          ev = events[next++];
      Engine::BlockEvent b;
      b.offset = static_cast<uint32_t>(ev.frame - f0);
      b.seq    = ev.seq;
      b.type   = ev.type;
      b.id     = ev.id;
      b.value  = ev.value;
      b.preset = ev.preset;
      block.push_back(b);
    }
    const float* ins[2]  = {in.l.data() + pos, in.r.data() + pos};
    float*       outs[2] = {out.l.data() + pos, out.r.data() + pos};
    Engine::ProcessContext ctx;
    ctx.in        = ins;
    ctx.out       = outs;
    ctx.numFrames = static_cast<uint32_t>(n);
    ctx.events    = block.data();
    ctx.numEvents = static_cast<uint32_t>(block.size());
    e.Process(ctx);
    pos += n;
  }
  return out;
}

std::vector<uint32_t> RandomPattern(uint32_t seed) {
  std::vector<uint32_t> p;
  for (uint32_t i = 0; i < 257; ++i) p.push_back(1u + testsignal::SplitMix32(seed, i) % 512u);
  return p;
}

// The first frame whose left sample is not exactly 0, or -1.
int64_t FirstNonzero(const std::vector<float>& x) {
  for (size_t i = 0; i < x.size(); ++i) {
    if (x[i] != 0.0f) return static_cast<int64_t>(i);
  }
  return -1;
}

// ── The post chain alone ──────────────────────────────────────────────────────────────────────

struct Post {
  std::vector<float> warm, bulk;
  detail::PostChain  chain;
  explicit Post(double sr = 48000.0)
      : warm(detail::PostChain::WarmFloats(sr)), bulk(detail::PostChain::BulkFloats(sr)) {
    const detail::FpEnvGuard guard;
    chain.Init(warm.data(), bulk.data(), sr);
  }
};

// The delay stage alone, wet only, no feedback.
detail::PostParams DelayOnly(uint32_t syncFrames, float fb = 0.0f) {
  detail::PostParams p;
  p.delayMix        = 1.0f;
  p.delayFb         = fb;
  p.delaySyncFrames = syncFrames;
  return p;
}

// The chain over `in` (mono, both channels) with `params[i].second` from frame params[i].first,
// in blocks of `pattern` that also split at every change, as the engine's spans do.
std::vector<float> RenderPost(const std::vector<std::pair<int64_t, detail::PostParams>>& params,
                              const std::vector<float>& in, const std::vector<uint32_t>& pattern) {
  Post post;
  const detail::FpEnvGuard guard;
  post.chain.Reset(params[0].second);
  std::vector<float> l = in, r = in;
  size_t             next = 1, bi = 0;
  detail::PostParams p = params[0].second;
  for (int64_t pos = 0; pos < static_cast<int64_t>(in.size());) {
    while (next < params.size() && params[next].first <= pos) p = params[next++].second;
    int64_t end = pos + pattern[bi++ % pattern.size()];
    if (end > static_cast<int64_t>(in.size())) end = static_cast<int64_t>(in.size());
    if (next < params.size() && params[next].first < end) end = params[next].first;
    post.chain.Process(p, static_cast<uint32_t>(end - pos), l.data() + pos, r.data() + pos);
    pos = end;
  }
  return l;
}

}  // namespace

// ── §2.3, §5.2, §5.3: durations and folds ───────────────────────────────────────────────────────

TEST_CASE("synced durations: every note value and Subdiv at six tempos against a 128-bit "
          "reference, folded (§2.3, §5.3)",
          "[sync]") {
  const uint32_t ticks[] = {3, 4, 6, 8, 9, 12, 16, 18, 24, 32, 36, 48, 64, 72, 96, 192};
  for (uint8_t code = 1; code <= 16; ++code) CHECK(tempo::NoteTicks(code) == ticks[code - 1]);
  CHECK(tempo::NoteTicks(0) == 0u);
  CHECK(tempo::NoteTicks(17) == 0u);
  testing::I128::Overflow() = false;
  for (const uint32_t rate : {8000u, 44100u, 48000u, 96000u, 384000u}) {
    for (const uint32_t bpm : {20u, 30u, 60u, 119u, 120u, 300u}) {
      const uint32_t ns = (60000000000ull + bpm / 2) / bpm;
      const uint64_t pc = tempo::PFromNs(ns, rate);
      for (uint8_t code = 1; code <= 16; ++code) {
        for (uint8_t subdiv = 0; subdiv < tempo::kSubdivCodes; ++subdiv) {
          INFO(rate << " Hz, " << bpm << " BPM, code " << int(code) << ", subdiv " << int(subdiv));
          const uint32_t s = tempo::SubdivTicks(subdiv);
          for (const bool post : {true, false}) {
            const uint32_t lo = post ? tempo::PostSyncMinFrames(rate) : tempo::BaseSyncMinFrames(rate);
            const uint32_t hi = post ? tempo::PostSyncMaxFrames(rate) : tempo::BaseSyncMaxFrames(rate);
            int32_t        k = 0, refK = 0;
            const uint32_t got = tempo::FoldedFrames(pc, ticks[code - 1], s, lo, hi, &k);
            const uint64_t want = RefFolded(pc, ticks[code - 1], s, lo, hi, &refK);
            REQUIRE(got == want);
            REQUIRE(k == refK);
            REQUIRE(got >= lo);
            REQUIRE(got <= hi);
            REQUIRE(k >= -5);
            REQUIRE(k <= 3);
            // The producers' form, from the tempo in ns (exact here: the tempo is a whole ns).
            const tempo::SyncedTime d = tempo::SyncedDuration(
                post ? SyncTarget::PostDelay : SyncTarget::BaseDelay, code, ns, subdiv, 0, rate);
            REQUIRE(d.frames == got);
            REQUIRE(d.octaves == k);
          }
        }
      }
    }
  }
  REQUIRE_FALSE(testing::I128::Overflow());
}

TEST_CASE("synced durations: §5.2's and §5.3's worked values and the fold edges", "[sync]") {
  const auto post = [](uint8_t code, uint32_t ns, uint8_t subdiv = 0) {
    return tempo::SyncedDuration(SyncTarget::PostDelay, code, ns, subdiv, 0, kRate);
  };
  // 1/1 at 120 BPM is 2 s and 2/1 is 4 s exactly, under the maximum: neither folds (with draft
  // v1's 2 s line both folded at the default tempo, record §6, P5).
  CHECK(post(15, 500000000).frames == 96000u);
  CHECK(post(15, 500000000).octaves == 0);
  CHECK(post(16, 500000000).frames == 192000u);
  CHECK(post(16, 500000000).octaves == 0);
  // A 2/1 at 119 BPM folds to an exact whole note, 96,807 frames, never a clamp (§5.3).
  const uint32_t ns119 = 504201681;  // 6e10 / 119, rounded
  CHECK(post(16, ns119).frames == 96807u);
  CHECK(post(16, ns119).octaves == -1);
  // At 50 BPM, 1/2d plays 172,800 frames and 1/1, folded, 115,200 (§5.2's sweep).
  CHECK(post(14, 1200000000).frames == 172800u);
  CHECK(post(15, 1200000000).frames == 115200u);
  CHECK(post(15, 1200000000).octaves == -1);
  // A 1/32 under ×8 at 300 BPM is 150 frames and plays as 600 (12.5 ms), two doublings.
  CHECK(post(1, 200000000, 5).frames == 600u);
  CHECK(post(1, 200000000, 5).octaves == 2);
  // Tempo mode forces TAP (§2.4): the same 1/32 at 300 BPM, 1,200 frames, unfolded.
  CHECK(tempo::SyncedDuration(SyncTarget::PostDelay, 1, 200000000, 5, 2, kRate).frames == 1200u);
  CHECK(tempo::SyncedDuration(SyncTarget::PostDelay, 1, 200000000, 5, 2, kRate).subdiv == 0u);
  // The edges: a duration of exactly the maximum stays, one frame over halves; exactly the
  // minimum stays, one under doubles. 2/1 at TAP is 8 quarters, 8 · Pc frames.
  const uint64_t at = 24000ull << 32;  // 24,000 frames a quarter: 2/1 at 192,000
  int32_t        k  = 9;
  CHECK(tempo::FoldedFrames(at, 192, 24, 480, 192000, &k) == 192000u);
  CHECK(k == 0);
  CHECK(tempo::FoldedFrames(at + (1ull << 29), 192, 24, 480, 192000, &k) == 96001u);  // 192,001
  CHECK(k == -1);
  const uint64_t low = 480ull << 32;  // 1/4 at 480 frames a quarter
  CHECK(tempo::FoldedFrames(low, 24, 24, 480, 192000, &k) == 480u);
  CHECK(k == 0);
  CHECK(tempo::FoldedFrames(low - (1ull << 32), 24, 24, 480, 192000, &k) == 958u);
  CHECK(k == 1);
  // The ranges at 48 kHz: 10 ms to 4 s and 2^-7 of it, 1 ms to 5 s and 2^-7 of it (§11.16).
  CHECK(tempo::PostSyncMinFrames(kRate) == 480u);
  CHECK(tempo::PostSyncMaxFrames(kRate) == 193500u);
  CHECK(tempo::BaseSyncMinFrames(kRate) == 48u);
  CHECK(tempo::BaseSyncMaxFrames(kRate) == 241875u);
  CHECK(tempo::PostSyncMaxFrames(44100) == 177778u);
  CHECK(tempo::PostSyncMaxFrames(384000) == 1548000u);
  // The headroom keeps 120 BPM off the fold boundary: a 2/1 a little slower than 120 BPM, as a
  // clock's fit may commit it (0.05 % slow: 192,096 frames), plays unfolded, as does one at the
  // maximum itself (1/2D at 44.65 BPM, 3 · 64,500 frames); at 119 BPM (above) it folds.
  CHECK(post(16, 500250000).frames == 192096u);
  CHECK(post(16, 500250000).octaves == 0);
  CHECK(post(14, 1343750000).frames == 193500u);
  CHECK(post(14, 1343750000).octaves == 0);
  CHECK(post(14, 1343753500).frames == 96750u);  // 193,501 frames, halved from Pc
  CHECK(post(14, 1343753500).octaves == -1);
  // ... and a synced base 2/1 at 96 BPM (5 s) likewise.
  const tempo::SyncedTime base96 =
      tempo::SyncedDuration(SyncTarget::BaseDelay, 16, 625000000, 0, 0, kRate);
  CHECK(base96.frames == 240000u);
  CHECK(base96.octaves == 0);
  // Inputs out of range give no duration.
  CHECK(post(0, 500000000).frames == 0u);
  CHECK(post(17, 500000000).frames == 0u);
  CHECK(post(9, 199999999).frames == 0u);
  CHECK(tempo::SyncedDuration(SyncTarget::PostDelay, 9, 500000000, 6, 0, kRate).frames == 0u);
  CHECK(tempo::SyncedDuration(SyncTarget::PostDelay, 9, 500000000, 0, 0, 7999).frames == 0u);
}

TEST_CASE("synced durations: the effective value as the editor and the plugin show it (§5.3, "
          "§5.4)",
          "[sync]") {
  const auto text = [](SyncTarget t, uint8_t code, uint32_t ns, uint8_t subdiv, uint8_t mode) {
    char buf[96];
    const size_t n = FormatSyncedTime(t, code, ns, subdiv, mode, kRate, buf, sizeof buf);
    REQUIRE(n == std::strlen(buf));
    return std::string(buf);
  };
  const SyncTarget P = SyncTarget::PostDelay, B = SyncTarget::BaseDelay;
  CHECK(text(P, 0, 500000000, 0, 0) == "Off");
  CHECK(text(P, 9, 500000000, 0, 0) == "1/4 \xC2\xB7 500 ms");
  CHECK(text(P, 8, 500000000, 0, 0) == "1/8D \xC2\xB7 375 ms");
  CHECK(text(P, 15, 500000000, 0, 0) == "1/1 \xC2\xB7 2.00 s");
  // A fold: 2/1 at 119 BPM plays a whole note.
  CHECK(text(P, 16, 504201681, 0, 0) == "2/1 \xE2\x86\x92 1/1 \xC2\xB7 2.02 s");
  // The Subdiv scales it (D12): a quarter at ×1/2 is a half note.
  CHECK(text(P, 9, 500000000, 2, 0) ==
        "1/4 \xC2\xB7 Subdiv \xC3\x97" "1/2 \xE2\x86\x92 1/2 \xC2\xB7 1.00 s");
  // Tempo mode forces TAP.
  CHECK(text(P, 9, 500000000, 2, 2) == "1/4 \xC2\xB7 500 ms");
  // Triplets and dotted values keep their kind through the Subdiv and the fold: 1/16T at ×8 at
  // 300 BPM is 200 frames, doubled twice to 800 (16.7 ms): a 1/32T.
  CHECK(text(P, 2, 200000000, 5, 0) ==
        "1/16T \xC2\xB7 Subdiv \xC3\x97" "8 \xE2\x86\x92 1/32T \xC2\xB7 16.7 ms");
  CHECK(text(P, 5, 500000000, 3, 0) ==
        "1/16D \xC2\xB7 Subdiv \xC3\x97" "2 \xE2\x86\x92 1/32D \xC2\xB7 93.8 ms");
  // 2/1 at ×1/4 and 20 BPM: 8/1, folded five octaves to a 1/4 (3 s... 1/4 at 20 BPM).
  CHECK(text(P, 16, 3000000000u, 1, 0) ==
        "2/1 \xC2\xB7 Subdiv \xC3\x97" "1/4 \xE2\x86\x92 1/4 \xC2\xB7 3.00 s");
  // The base delay's range: 1 ms-5 s, so a 2/1 at 60 BPM (8 s) halves once.
  CHECK(text(B, 16, 1000000000, 0, 0) == "2/1 \xE2\x86\x92 1/1 \xC2\xB7 4.00 s");
  CHECK(text(B, 1, 200000000, 5, 0) ==
        "1/32 \xC2\xB7 Subdiv \xC3\x97" "8 \xE2\x86\x92 1/256 \xC2\xB7 3.12 ms");
  // Out of range: no text.
  CHECK(text(P, 9, 1u, 0, 0).empty());
  char small[4];
  CHECK(FormatSyncedTime(P, 9, 500000000, 0, 0, kRate, small, sizeof small) == 3u);
  CHECK(std::string(small) == "1/4");
}

// ── §7.3: the post chain's crossfade, chain of fades and slew ──────────────────────────────────

TEST_CASE("post chain: a synced echo lands on its exact frame, to the line's end (§6.1)",
          "[sync]") {
  for (const uint32_t frames : {480u, 20571u, 20945u, 95999u, 96000u, 191999u, 192000u, 193499u,
                                193500u}) {
    INFO(frames << " frames");
    std::vector<float> in(frames + 64, 0.0f);
    in[0]                      = 1.0f;
    const std::vector<float> o = RenderPost({{0, DelayOnly(frames)}}, in, {512});
    CHECK(FirstNonzero(o) == static_cast<int64_t>(frames));
    CHECK(o[frames] == 1.0f);
  }
  // post.delay.time_ms keeps its clamp at round(2·R) - 1: 2,000 ms plays 95,999 frames.
  detail::PostParams p = DelayOnly(0);
  p.delayFrames        = 96000.0f;
  std::vector<float> in(96100, 0.0f);
  in[0] = 1.0f;
  CHECK(FirstNonzero(RenderPost({{0, p}}, in, {512})) == 95999);
}

TEST_CASE("post chain: a jump crossfades two heads over 1,024 frames with gains n/1024 (§7.3)",
          "[sync]") {
  // A ramp in the line, so every read says where it came from.
  std::vector<float> in(60000);
  for (size_t i = 0; i < in.size(); ++i) in[i] = static_cast<float>(i % 4096) * (1.0f / 4096.0f);
  const uint32_t     a = 12000, b = 9000;
  const int64_t      at = 30011;
  detail::PostParams pa = DelayOnly(a), pb = DelayOnly(b);
  pb.delayJump          = 1;
  const std::vector<float> o = RenderPost({{0, pa}, {at, pb}}, in, {48});
  for (int64_t f = at - 100; f < at; ++f) REQUIRE(o[f] == in[f - a]);
  for (uint32_t k = 1; k <= detail::PostChain::kXfadeFrames; ++k) {
    const int64_t f  = at + k - 1;
    const float   gi = static_cast<float>(k) * (1.0f / 1024.0f);
    const float   go = static_cast<float>(1024u - k) * (1.0f / 1024.0f);
    const float   xo = in[f - a], xi = in[f - b];
    const float   want = xo * go + xi * gi;
    REQUIRE(o[f] == want);
  }
  for (int64_t f = at + 1024; f < at + 5000; ++f) REQUIRE(o[f] == in[f - b]);
  // Without the jump the same change glides: the head moves at most 0.5 frames a frame.
  detail::PostParams glide = pb;
  glide.delayJump          = 0;
  const std::vector<float> g = RenderPost({{0, pa}, {at, glide}}, in, {48});
  CHECK(g[at + 1023] != o[at + 1023]);
}

TEST_CASE("post chain: a chain of fades lands at the same frames for every split (E11)",
          "[sync]") {
  // A Subdiv knob turned through all six zones: six jumps 200 frames apart, inside each other's
  // fades, then Steps and a Drift during the last, then the mix to 0 and back (a silent stage
  // jumps), with feedback recirculating the mixed read.
  std::vector<float> in(140000);
  for (size_t i = 0; i < in.size(); ++i) {
    const float a = static_cast<float>(testsignal::SplitMix32(5, static_cast<uint32_t>(i)) >> 8);
    in[i]         = (a * (1.0f / 8388608.0f) - 1.0f) * (i < 30000 ? 0.5f : 0.0f);
  }
  std::vector<std::pair<int64_t, detail::PostParams>> sched;
  detail::PostParams p = DelayOnly(24000, 0.6f);
  sched.push_back({0, p});
  const uint32_t targets[] = {12000, 6000, 3000, 48000, 96000, 24000};
  int64_t        f         = 31001;
  for (const uint32_t t : targets) {
    p.delaySyncFrames = t;
    ++p.delayJump;
    sched.push_back({f, p});
    f += 200;
  }
  p.delaySyncFrames = 24100;  // a Step while the chain runs
  sched.push_back({f + 37, p});
  p.delaySyncFrames = 24196;  // a Drift: the slew
  p.delaySlow       = true;
  sched.push_back({f + 4999, p});
  p.delayMix = 0.0f;  // silent: the head jumps, no fade waits
  sched.push_back({f + 9001, p});
  p.delayMix        = 1.0f;
  p.delaySyncFrames = 30000;
  ++p.delayJump;
  sched.push_back({f + 60003, p});  // the mix long settled at an exact 0
  const std::vector<float> ref = RenderPost(sched, in, {1});
  for (const std::vector<uint32_t>& pattern :
       {std::vector<uint32_t>{512}, {48}, {441}, {48, 1, 127, 32}, {300, 512, 5, 64},
        RandomPattern(11)}) {
    INFO("pattern starting " << pattern[0]);
    const std::vector<float> got = RenderPost(sched, in, pattern);
    REQUIRE(std::memcmp(got.data(), ref.data(), ref.size() * sizeof(float)) == 0);
  }
  Post post;
  {
    const detail::FpEnvGuard guard;
    post.chain.Reset(sched[0].second);
  }
  // Two fades: the first jump's, and the one the latest waiting target starts the frame after it
  // ends (the five jumps during the first fade replace each other); none after the silent stage,
  // whose head jumps.
  std::vector<float> l = in, r = in;
  int64_t            pos = 0;
  size_t             next = 1;
  detail::PostParams cur  = sched[0].second;
  {
    const detail::FpEnvGuard guard;
    while (pos < static_cast<int64_t>(in.size())) {
      while (next < sched.size() && sched[next].first <= pos) cur = sched[next++].second;
      int64_t end = std::min<int64_t>(pos + 512, static_cast<int64_t>(in.size()));
      if (next < sched.size() && sched[next].first < end) end = sched[next].first;
      post.chain.Process(cur, static_cast<uint32_t>(end - pos), l.data() + pos, r.data() + pos);
      pos = end;
    }
  }
  CHECK(post.chain.Crossfades() == 2u);
}

TEST_CASE("post chain: a Drift slews over about a second, bending the repeats at most 0.1 % "
          "(§7.2, §7.4)",
          "[sync]") {
  // A ramp in, so the read gives the head: Catmull-Rom is exact on a line, so out = (n - head)/8.
  std::vector<float> in(1200000);
  for (size_t i = 0; i < in.size(); ++i) in[i] = static_cast<float>(i) * 0.125f;
  const uint32_t     from = 24000, to = 24096;  // a 0.4 % commit on a 500 ms delay
  detail::PostParams a = DelayOnly(from), slew = DelayOnly(to), glide = DelayOnly(to);
  slew.delaySlow       = true;
  const int64_t at     = 30000;
  const std::vector<float> s = RenderPost({{0, a}, {at, slew}}, in, {512});
  const std::vector<float> g = RenderPost({{0, a}, {at, glide}}, in, {512});
  const auto head = [&](const std::vector<float>& o, int64_t f) {
    return static_cast<double>(f) - static_cast<double>(o[f]) * 8.0;
  };
  // 100 ms in: the glide is more than half the way, the slew has barely begun.
  CHECK(head(g, at + 4800) - from > 0.5 * (to - from));
  CHECK(head(s, at + 4800) - from < 0.01 * (to - from));
  // One second in: the slew is about a quarter of the way (1 - 2/e).
  const double oneSecond = (head(s, at + 48000) - from) / (to - from);
  CHECK(oneSecond > 0.2);
  CHECK(oneSecond < 0.3);
  // Its speed, frames of head movement per frame, is the bend: under 0.1 %. Averaged over 100 ms
  // (the ramp's float reads resolve the head to about 0.1 frame).
  double maxSpeed = 0;
  for (int64_t f = at + 4800; f < at + 400000; f += 997) {
    maxSpeed = std::max(maxSpeed, std::fabs(head(s, f) - head(s, f - 4800)) / 4800.0);
  }
  CHECK(maxSpeed < 0.001);
  CHECK(maxSpeed > 0.0005);
  // And it lands exactly, reading the integer tap again.
  const int64_t end = static_cast<int64_t>(in.size()) - 1;
  CHECK(s[end] == in[end - to]);
}

// ── §6.1, §6.2: the engine ──────────────────────────────────────────────────────────────────────

TEST_CASE("row 63: the post delay's echo on the synced frame at 140 and 137.5 BPM; time_ms waits "
          "(§6.1)",
          "[sync]") {
  struct Case {
    uint32_t us;
    uint8_t  code;
    int64_t  frames;
  };
  // 1/4 at 140 BPM: Pc = 20,571.408 frames, 20,571; 1/8D at 137.5: 15,709.1, 15,709; 1/1T at 140:
  // 54,857.09, 54,857; 2/1 at 137.5: 8 · 20,945.47 = 167,563.8, 167,564.
  for (const Case c : {Case{kUs140, 9, 20571}, Case{kUs1375, 8, 15709}, Case{kUs140, 13, 54857},
                       Case{kUs1375, 16, 167564}}) {
    INFO("code " << int(c.code) << " at " << c.us << " us");
    const uint64_t pc = tempo::PFromNs(c.us * 1000u, kRate);
    REQUIRE(tempo::DurationFrames(pc, tempo::NoteTicks(c.code), 24) == static_cast<uint64_t>(c.frames));
    auto preset = Preset(With(Clean(100.0f), {{ParamId::DelayMix, 1.0f},
                                               {ParamId::DelayFb, 0.0f},
                                               {ParamId::DelayTimeMs, 90.0f},
                                               {ParamId::DelaySync, static_cast<float>(c.code)}}),
                         c.us);
    Rig rig;
    REQUIRE(rig.engine.LoadPreset(*preset, LoadMode::Exact));
    const Stereo out = Render(rig.engine, Impulse(4800 + c.frames + 2400), {}, {256});
    CHECK(FirstNonzero(out.l) == 4800 + c.frames);
    CHECK(std::fabs(out.l[4800 + c.frames]) > 0.5f);
    CHECK(rig.engine.TempoCounts().crossfades == 0u);  // the load primes the head
  }
  // At 120 BPM a synced 1/8 is post.delay.time_ms 250 bit for bit.
  const Stereo in = Plucks(48000);
  auto         ms = Preset(With(Clean(100.0f), {{ParamId::DelayMix, 0.6f}, {ParamId::DelayFb, 0.5f},
                                                {ParamId::DelayTimeMs, 250.0f}}));
  auto sync = Preset(With(Clean(100.0f), {{ParamId::DelayMix, 0.6f}, {ParamId::DelayFb, 0.5f},
                                          {ParamId::DelayTimeMs, 1999.0f},
                                          {ParamId::DelaySync, 6.0f}}));
  Rig a, b;
  REQUIRE(a.engine.LoadPreset(*ms, LoadMode::Exact));
  REQUIRE(b.engine.LoadPreset(*sync, LoadMode::Exact));
  CHECK(Same(Render(a.engine, in, {}), Render(b.engine, in, {})));
}

TEST_CASE("base_sync: the first grain tap on the division's frame (§6.2, Q11)", "[sync]") {
  struct Case {
    uint32_t us;
    uint8_t  code;
    uint8_t  subdiv;
    int64_t  frames;
  };
  // 1/8 at 140 BPM: 10,285.7, 10,286; 1/16T at 137.5 BPM ×1/2: 3,490.9 · 2, 6,982; 1/1 at
  // 137.5: 83,781.9, 83,782 (inside base_ms's 5 s).
  for (const Case c : {Case{kUs140, 6, 0, 10286}, Case{kUs1375, 2, 2, 6982},
                       Case{kUs1375, 15, 0, 83782}}) {
    INFO("base_sync " << int(c.code) << " at " << c.us << " us");
    auto preset = Preset(With(Clean(333.0f), {{ParamId::DelayMix, 0.0f}}), c.us, c.code, c.subdiv);
    REQUIRE((preset->mode.features & kModeFeatureTempoSync) != 0u);
    EngineConfig cfg  = Config();
    cfg.historyFrames = 1u << 17;
    Rig rig(cfg);
    REQUIRE(rig.engine.LoadPreset(*preset, LoadMode::Exact));
    const Stereo out = Render(rig.engine, Impulse(c.frames + 2400), {}, {256});
    CHECK(FirstNonzero(out.l) == c.frames);
    CHECK(std::fabs(out.l[c.frames]) > 0.5f);
  }
}

TEST_CASE("synced times: each change of the committed tempo in its class (§7.1); tempo_glide "
          "glides jumps (D22)",
          "[sync]") {
  const Stereo in = Plucks(5 * 48000);
  // A quarter-synced echo over the plucks.
  const Params base = With(Clean(50.0f), {{ParamId::DelayMix, 0.5f}, {ParamId::DelayFb, 0.6f},
                                          {ParamId::DelaySync, 9.0f}});
  auto preset = Preset(base);
  struct Case {
    const char*     name;
    std::vector<Ev> events;
    uint64_t        crossfades, jumps;
  };
  const std::vector<Case> cases = {
      // A tap starting a new chain at 90 BPM: a Jump.
      {"tap jump", {Tap(48001), Tap(80001)}, 1, 1},
      // A host tempo step within Pc >> 5, 120 to 121 BPM: a Step, which glides.
      {"tempo step", {TempoNs(24007, 495867769)}, 0, 0},
      // A host tempo jump at a section change, 120 to 90 BPM.
      {"tempo jump", {TempoNs(24007, kNs90)}, 1, 1},
      // A Subdiv change: discrete, a crossfade though Pc does not move.
      {"subdiv", {Subdiv(24007, SubdivField::Subdivision, 3)}, 1, 0},
      // The code moved by a macro or a SetParam: discrete.
      {"code", {Set(24007, ParamId::DelaySync, 12.0f)}, 1, 0},
      // Sync off and on: discrete each way.
      {"sync off and on", {Set(24007, ParamId::DelaySync, 0.0f), Set(72011, ParamId::DelaySync, 9.0f)}, 2, 0},
      // A tempo change that changes the fold: 1/1 at 120 BPM (2 s) then 2/1 at 119 folds.
      {"time mode Tempo (forces TAP; nothing moves at TAP)", {Subdiv(24007, SubdivField::TimeMode, 2)}, 0, 0},
  };
  for (const Case& c : cases) {
    INFO(c.name);
    Rig rig;
    REQUIRE(rig.engine.LoadPreset(*preset, LoadMode::Exact));
    const TempoStats before = rig.engine.TempoCounts();
    Render(rig.engine, in, Sorted(c.events));
    const TempoStats after = rig.engine.TempoCounts();
    CHECK(after.crossfades - before.crossfades == c.crossfades);
    CHECK(after.jumps - before.jumps == c.jumps);
  }
  // global.tempo_glide On: the Jump glides, the discrete change still crossfades.
  {
    Rig rig;
    REQUIRE(rig.engine.LoadPreset(*preset, LoadMode::Exact));
    rig.engine.SetParam(ParamId::TempoGlide, 1.0f);
    Render(rig.engine, in,
           Sorted({TempoNs(24007, kNs90), Subdiv(96013, SubdivField::Subdivision, 2)}));
    const TempoStats s = rig.engine.TempoCounts();
    CHECK(s.jumps == 1u);
    CHECK(s.crossfades == 1u);
  }
  // A fold change caused by a tempo Step crosses an octave: a crossfade, glide or not (§5.3).
  {
    auto whole = Preset(With(base, {{ParamId::DelaySync, 16.0f}}));  // 2/1 at 120: 4 s, unfolded
    Rig  rig;
    REQUIRE(rig.engine.LoadPreset(*whole, LoadMode::Exact));
    rig.engine.SetParam(ParamId::TempoGlide, 1.0f);
    Render(rig.engine, in, {TempoNs(24007, 504201681)});  // 119 BPM: a Step, and a fold
    const TempoStats s = rig.engine.TempoCounts();
    CHECK(s.jumps == 0u);
    CHECK(s.crossfades == 1u);
    CHECK(s.folds == 1u);
  }
  // Unsynced, nothing crossfades.
  {
    auto plain = Preset(With(base, {{ParamId::DelaySync, 0.0f}}));
    Rig  rig;
    REQUIRE(rig.engine.LoadPreset(*plain, LoadMode::Exact));
    Render(rig.engine, in, Sorted({TempoNs(24007, kNs90), Subdiv(48013, SubdivField::Subdivision, 2)}));
    CHECK(rig.engine.TempoCounts().crossfades == 0u);
  }
}

TEST_CASE("synced times: a clock's deadband commit slews the post delay, no crossfade (§7.1, "
          "§7.2)",
          "[sync]") {
  // A clock at the stored 1,024 frames a tick, acquired, then at 1,027 (0.29 %, outside rule 3.1's
  // Pc >> 9): once the fit crosses the band, its 48th fitted tick outside commits a Drift.
  const Stereo in    = Plucks(6 * 48000);
  const uint32_t us  = 1024u * 24u * 1000u / 48u;  // 512,000 µs: 1,024 frames a tick
  auto         preset =
      Preset(With(Clean(50.0f), {{ParamId::DelayMix, 0.5f}, {ParamId::DelaySync, 9.0f}}), us);
  std::vector<Ev> ev;
  for (int64_t k = 0; k < 40; ++k) ev.push_back(Tick(1000 + k * 1024));
  for (int64_t k = 40; k < 240; ++k) ev.push_back(Tick(1000 + 40 * 1024 + (k - 40) * 1027));
  Rig rig;
  REQUIRE(rig.engine.LoadPreset(*preset, LoadMode::Exact));
  Render(rig.engine, in, Sorted(ev));
  const TempoStats s = rig.engine.TempoCounts();
  CHECK(s.commits >= 2u);   // the acquisition and a deadband commit
  CHECK(s.slews >= 1u);     // ... a Drift
  CHECK(s.crossfades == 0u);
}

TEST_CASE("synced times: block-split invariance with every event type, fades chained by a Subdiv "
          "sweep (§1.4, E11)",
          "[sync]") {
  const Stereo in    = Plucks(6 * 48000);
  auto         preset = Preset(With(Clean(80.0f), {{ParamId::DelayMix, 0.5f},
                                                    {ParamId::DelayFb, 0.7f},
                                                    {ParamId::Feedback, 0.4f},
                                                    {ParamId::DelaySync, 9.0f}}),
                               kUs1375, 6, 3, 1);  // base_sync 1/8, ×2, Subdiv time mode
  auto other = Preset(With(Clean(80.0f), {{ParamId::DelayMix, 0.4f}, {ParamId::DelaySync, 11.0f}}),
                      600000, 9, 1);  // 100 BPM, ×1/4: a recall
  std::vector<Ev> ev = {Tap(10001), Tap(42001), Tap(74001)};
  // The Subdiv knob through all six zones 200 frames apart: a chain of fades.
  const uint8_t zones[] = {1, 2, 0, 3, 4, 5};
  for (int k = 0; k < 6; ++k) ev.push_back(Subdiv(90001 + 200 * k, SubdivField::Subdivision, zones[k]));
  ev.push_back(TempoNs(101011, 436363636));  // a Step back to 137.5 BPM
  ev.push_back(Set(110007, ParamId::DelaySync, 16.0f));
  ev.push_back(Set(120013, ParamId::TempoRecall, 1.0f));
  ev.push_back(Load(130001, other.get()));
  ev.push_back(Set(140003, ParamId::TempoGlide, 1.0f));
  ev.push_back(TempoNs(150001, 300000000));  // a Jump, glided
  ev.push_back(Subdiv(160007, SubdivField::TimeMode, 2));
  for (int64_t k = 0; k < 60; ++k) ev.push_back(Tick(170000 + k * 857 + (k % 3)));
  ev.push_back(Set(230011, ParamId::DelaySync, 0.0f));
  ev.push_back(Subdiv(240007, SubdivField::TimeMode, 1));    // Subdiv mode: ×1/4 again
  ev.push_back(Set(250009, ParamId::DelaySync, 16.0f));      // 2/1 at ×1/4: folds twice
  ev = Sorted(ev);

  Rig rig;
  REQUIRE(rig.engine.LoadPreset(*preset, LoadMode::Exact));
  const Stereo     ref = Render(rig.engine, in, ev, {1});
  const TempoStats c   = rig.engine.TempoCounts();
  CHECK(c.crossfades >= 8u);
  CHECK(c.folds >= 1u);
  for (const std::vector<uint32_t>& pattern :
       {std::vector<uint32_t>{512}, {48}, {441}, {48, 1, 127, 32}, {300, 512, 5, 64},
        RandomPattern(3)}) {
    INFO("pattern starting " << pattern[0]);
    Rig r2;
    REQUIRE(r2.engine.LoadPreset(*preset, LoadMode::Exact));
    CHECK(Same(Render(r2.engine, in, ev, pattern), ref));
    const TempoStats c2 = r2.engine.TempoCounts();
    CHECK(c2.crossfades == c.crossfades);
    CHECK(c2.folds == c.folds);
  }
}

TEST_CASE("synced times: Restart equals Init and an Exact load of the active preset", "[sync]") {
  const Stereo in     = Plucks(3 * 48000);
  auto         preset = Preset(With(Clean(80.0f), {{ParamId::DelayMix, 0.5f},
                                                    {ParamId::DelayFb, 0.6f},
                                                    {ParamId::DelaySync, 12.0f}}),
                               kUs140, 9, 2);
  Rig a;
  REQUIRE(a.engine.LoadPreset(*preset, LoadMode::Exact));
  // History first: a tempo jump, a Subdiv mid-fade, taps: Restart plays the stored tempo and
  // Subdiv again (clock.md §2.5) and keeps the leaves, which nothing here moves.
  Render(a.engine, in, Sorted({TempoNs(20011, kNs90), Subdiv(20311, SubdivField::Subdivision, 4),
                               Tap(50003), Tap(70003)}));
  a.engine.Restart();
  Rig b;
  REQUIRE(b.engine.LoadPreset(*preset, LoadMode::Exact));
  CHECK(Same(Render(a.engine, in, {}), Render(b.engine, in, {})));
}

// ── The reviews' amendments (clock.md §11.16) ───────────────────────────────────────────────────

TEST_CASE("post chain: a Drift's slew is capped at 2^-10 frames a frame on long echoes, within "
          "§7.4's 0.1 % (§7.2)",
          "[sync]") {
  // The head alone, stepped with the slew's pair as PostChain computes it: its position is exact,
  // so its speed is the bend. Measured commit sizes (§7.1: at most 0.34 % after lock, 0.58 %
  // before the window fills) on 1 s, 2 s, 4 s and the longest echoes.
  const float coef = -static_cast<float>(std::expm1(-1.0 / 48000.0));
  const float keep = 1.0f - coef;
  struct Case {
    uint32_t from, to;
  };
  for (const Case c : {Case{48000, 48163}, Case{96000, 96326}, Case{96000, 96557},
                       Case{191300, 191950}, Case{192000, 193113}, Case{24000, 24096}}) {
    INFO(c.from << " to " << c.to);
    detail::TapGlide g;
    g.Prime(c.from);
    g.Retarget(c.to);
    g.slow = true;
    double  prev = c.from, maxSpeed = 0.0;
    int64_t n    = 0;
    for (; g.moving && n < 4000000; ++n) {
      g.Step(coef, keep);
      const double head = static_cast<double>(g.base) + static_cast<double>(g.frac);
      maxSpeed          = std::max(maxSpeed, std::fabs(head - prev));
      prev              = head;
    }
    CHECK_FALSE(g.moving);  // landed on the integer tap, exactly
    CHECK(g.base == c.to);
    // The head's float fraction rounds each step by at most 2^-25 of a frame.
    CHECK(maxSpeed <= 0x1p-10 + 0x1p-20);
    CHECK(maxSpeed < 0.001);
    // Without the cap the τ = 1 s pair would bend a change of Δ frames by Δ/(e·τ), past the cap
    // from 128 frames: the long ones run at the cap.
    if (c.to - c.from > 160u) CHECK(maxSpeed > 0x1p-10 - 0x1p-20);
  }
}

TEST_CASE("synced times: a 2/1 at 120 BPM under a block-stamped MIDI clock never folds (D23, "
          "§5.3)",
          "[sync]") {
  // The hardware model of §8.3: ticks of an exact 120 BPM clock stamped at the next 48-frame block
  // boundary, at 16 phases of the grid. The fit commits a tempo a few ppm off 120 BPM, slow at
  // some phases, where an inclusive 4·R maximum folded the 2/1 to a whole note (§11.16); with
  // the headroom it never folds and nothing crossfades.
  auto preset = Preset(With(Clean(50.0f), {{ParamId::DelayMix, 1.0f}, {ParamId::DelayFb, 0.0f},
                                           {ParamId::DelaySync, 16.0f}}));
  const Stereo in{std::vector<float>(450000, 0.0f), std::vector<float>(450000, 0.0f)};
  int slowPhases = 0;
  for (int64_t ph = 0; ph < 48; ph += 3) {
    INFO("phase " << ph);
    std::vector<Ev> ev = {Event(4000, 0, EvType::Transport,
                                tempo::TransportId(TransportKind::Start, true), 0.0f)};
    for (int64_t i = 0; i < 440; ++i) ev.push_back(Tick((4800 + ph + 1000 * i + 47) / 48 * 48));
    Rig rig;
    REQUIRE(rig.engine.LoadPreset(*preset, LoadMode::Exact));
    Render(rig.engine, in, Sorted(ev), {48});
    const TempoStats s = rig.engine.TempoCounts();
    CHECK(s.ticks == 440u);
    CHECK(s.folds == 0u);
    CHECK(s.crossfades == 0u);
    const uint32_t ns = rig.engine.Tempo().nsPerQuarter;
    CHECK(ns > 499500000u);
    CHECK(ns < 500500000u);
    if (tempo::DurationFrames(tempo::PFromNs(ns, kRate), 192, 24) > 4u * kRate) ++slowPhases;
  }
  CHECK(slowPhases > 0);  // the boundary the headroom moved was crossed
}

TEST_CASE("synced times: at a non-integer sample rate the longest synced echo plays its exact "
          "frames (§5.3)",
          "[sync]") {
  // The line is sized from the tempo core's integer rate R, as the synced targets are: a 1/2D at
  // 44.65 BPM (stored 1,343,750 µs) is 3 · 64,500 = 193,500 frames at R = 48,000, the maximum.
  for (const double sr : {48000.0, 47999.6, 47999.5, 48000.4}) {
    INFO(sr << " Hz");
    REQUIRE(detail::PostChain::BulkFloats(sr) == 2u * (193500u + 2u));
    auto dry = Preset(With(Clean(100.0f), {{ParamId::DelayMix, 0.0f}, {ParamId::DelaySync, 14.0f}}),
                      1343750);
    auto wet = Preset(With(Clean(100.0f), {{ParamId::DelayMix, 1.0f}, {ParamId::DelayFb, 0.0f},
                                           {ParamId::DelaySync, 14.0f}}),
                      1343750);
    Rig a(Config(sr)), b(Config(sr));
    REQUIRE(a.engine.LoadPreset(*dry, LoadMode::Exact));
    REQUIRE(b.engine.LoadPreset(*wet, LoadMode::Exact));
    const Stereo  in = Impulse(193500 + 12000);
    const int64_t g  = FirstNonzero(Render(a.engine, in, {}, {256}).l);
    const int64_t e  = FirstNonzero(Render(b.engine, in, {}, {256}).l);
    REQUIRE(g > 0);
    CHECK(e - g == 193500);
  }
  REQUIRE(detail::PostChain::BulkFloats(44099.7) == 2u * (177778u + 2u));
}

TEST_CASE("synced times: Init on a used engine equals a fresh one after a Drift (§2.5)", "[sync]") {
  // A Drift leaves the post parameters' slew flag set; Init clears the synced cache, and must
  // clear the flag with it, or a later post.delay.time_ms move slews instead of gliding.
  const uint32_t us     = 1024u * 24u * 1000u / 48u;  // 1,024 frames a tick
  auto           synced = Preset(
      With(Clean(50.0f), {{ParamId::DelayMix, 0.5f}, {ParamId::DelaySync, 9.0f}}), us);
  auto unsynced = Preset(With(Clean(50.0f), {{ParamId::DelayMix, 0.6f}, {ParamId::DelayFb, 0.4f},
                                             {ParamId::DelayTimeMs, 300.0f}}));
  std::vector<Ev> drift;
  for (int64_t k = 0; k < 40; ++k) drift.push_back(Tick(1000 + k * 1024));
  for (int64_t k = 40; k < 240; ++k) drift.push_back(Tick(1000 + 40 * 1024 + (k - 40) * 1027));
  const std::vector<Ev> moves = Sorted({Set(24007, ParamId::DelayTimeMs, 700.0f),
                                        Set(72011, ParamId::DelayTimeMs, 150.0f)});
  const Stereo in = Plucks(3 * 48000);
  Rig fresh;
  REQUIRE(fresh.engine.LoadPreset(*unsynced, LoadMode::Exact));
  const Stereo want = Render(fresh.engine, in, moves);

  Rig used;
  REQUIRE(used.engine.LoadPreset(*synced, LoadMode::Exact));
  Render(used.engine, Plucks(6 * 48000), Sorted(drift));
  REQUIRE(used.engine.TempoCounts().slews >= 1u);
  REQUIRE(used.engine.Init(used.cfg, used.arenas.get()));
  REQUIRE(used.engine.LoadPreset(*unsynced, LoadMode::Exact));
  CHECK(Same(Render(used.engine, in, moves), want));
}

TEST_CASE("synced times: a Spillover load's recalled tempo is a Jump, crossfaded (§2.5, §7.1)",
          "[sync]") {
  // The same preset stored at 90 BPM: code, Subdiv and fold unchanged, so only the recall's Jump
  // of Pc can crossfade it.
  const Params p = With(Clean(50.0f), {{ParamId::DelayMix, 0.5f}, {ParamId::DelayFb, 0.6f},
                                       {ParamId::DelaySync, 9.0f}});
  auto a = Preset(p, kUs140);
  auto b = Preset(p, 666667);
  Rig  rig;
  REQUIRE(rig.engine.LoadPreset(*a, LoadMode::Exact));
  Render(rig.engine, Plucks(2 * 48000),
         Sorted({Set(1001, ParamId::TempoRecall, 1.0f), Load(24007, b.get())}));
  const TempoStats s = rig.engine.TempoCounts();
  CHECK(s.jumps == 1u);
  CHECK(s.crossfades == 1u);
  // Under Keep the running tempo stays: nothing to fade.
  Rig keep;
  REQUIRE(keep.engine.LoadPreset(*a, LoadMode::Exact));
  Render(keep.engine, Plucks(2 * 48000), Sorted({Load(24007, b.get())}));
  CHECK(keep.engine.TempoCounts().jumps == 0u);
  CHECK(keep.engine.TempoCounts().crossfades == 0u);
}

TEST_CASE("synced times: two changes of Pc at one frame take the strongest class (§7.1)", "[sync]") {
  auto a = Preset(With(Clean(50.0f), {{ParamId::DelayMix, 0.5f}, {ParamId::DelaySync, 9.0f}}),
                  kUs140);
  Rig  rig;
  REQUIRE(rig.engine.LoadPreset(*a, LoadMode::Exact));
  // 140 to 90 BPM (a Jump), then 90 to 91 (a Step), both at frame 24007: one crossfade.
  Render(rig.engine, Plucks(48000), Sorted({TempoNs(24007, kNs90), TempoNs(24007, 659340659)}));
  CHECK(rig.engine.TempoCounts().jumps == 1u);
  CHECK(rig.engine.TempoCounts().crossfades == 1u);
}

TEST_CASE("post chain: a stage that falls silent mid-fade drops the fade and the jump waiting "
          "(§7.3)",
          "[sync]") {
  detail::PostParams p0 = DelayOnly(12000);
  p0.delayMix           = 0.5f;
  detail::PostParams off = p0;
  off.delayMix           = 0.0f;
  // The mix goes to 0 at frame F; the chain's smoother (τ = 10 ms) reaches 0 n frames later, the
  // per-sample gate closing there, and the next span start finds the stage silent.
  detail::Smoother sm;
  sm.SetTau(10.0f, 48000.0);
  sm.Prime(0.5f);
  sm.target = 0.0f;
  int64_t n = 0;
  while (sm.Next() != 0.0f) ++n;
  const int64_t F = 40000, close = F + n + 1;
  std::vector<float> in(200000);
  for (size_t i = 0; i < in.size(); ++i) {
    const uint32_t u = testsignal::SplitMix32(9, static_cast<uint32_t>(i)) >> 8;
    in[i]            = (static_cast<float>(u) * (1.0f / 8388608.0f) - 1.0f) * 0.5f;
  }
  detail::PostParams jump = off;
  jump.delaySyncFrames    = 30000;
  jump.delayJump          = 1;
  detail::PostParams wait = jump;
  wait.delaySyncFrames    = 31000;
  wait.delayJump          = 2;
  detail::PostParams on   = wait;
  on.delayMix             = 0.5f;
  // A jump 500 frames before the gate closes and another waiting behind it, so a fade is under
  // way and one waits when it does; against the same target set once the stage is silent.
  const std::vector<float> x = RenderPost(
      {{0, p0}, {F, off}, {close - 500, jump}, {close - 400, wait}, {close + 40000, on}}, in, {64});
  const std::vector<float> y =
      RenderPost({{0, p0}, {F, off}, {close + 10000, wait}, {close + 40000, on}}, in, {64});
  INFO("gate closes at " << close);
  CHECK(std::memcmp(x.data() + close + 40000, y.data() + close + 40000,
                    (in.size() - static_cast<size_t>(close + 40000)) * sizeof(float)) == 0);
}

TEST_CASE("synced times: folds count every octave (§5.3)", "[sync]") {
  // 1/32 under ×8 at 300 BPM: 150 frames, doubled twice to 600.
  auto a = Preset(With(Clean(50.0f), {{ParamId::DelayMix, 0.5f}, {ParamId::DelaySync, 1.0f}}),
                  200000, 0, 5);
  Rig  rig;
  REQUIRE(rig.engine.LoadPreset(*a, LoadMode::Exact));
  CHECK(rig.engine.TempoCounts().folds == 2u);
}
