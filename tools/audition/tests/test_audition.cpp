// The audition tooling's tests (tools/audition; docs/design/mode-compiler.md §11.3): the shared
// render against a hand-written engine loop, in every block pattern and with events delivered at
// their stamps; inputs, hashes, WAVs and the K-weighted loudness against known values; the
// scripts' plan; the pre-screen's thresholds at their edges; and a suite written to disk.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "Hash.h"
#include "Inputs.h"
#include "JsonWriter.h"
#include "Metrics.h"
#include "Recipe.h"
#include "Render.h"
#include "Suite.h"
#include "Wav.h"
#include "brainscape/Engine.h"
#include "brainscape/EventQueue.h"
#include "brainscape/HostArenas.h"
#include "brainscape/InputCondition.h"
#include "brainscape/ModeEval.h"
#include "brainscape/Preset.h"
#include "catch.hpp"

using namespace bsa;
namespace bs = brainscape;
using bs::ParamId;
using bs::PresetState;
using EventType = bs::Engine::EventType;

namespace {

constexpr double kPi = 3.14159265358979323846;

bool SameBits(const std::vector<float>& a, const std::vector<float>& b) {
  return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * 4) == 0);
}
bool SameBits(const Stereo& a, const Stereo& b) { return SameBits(a.l, b.l) && SameBits(a.r, b.r); }

std::unique_ptr<PresetState> Busy() {
  float v[bs::kNumLeafParams];
  for (size_t i = 0; i < bs::kNumLeafParams; ++i) v[i] = bs::FindParam(bs::LeafId(i))->def;
  auto set = [&](ParamId id, float x) { v[bs::LeafIndex(id)] = x; };
  set(ParamId::Mix, 0.6f);
  set(ParamId::Feedback, 0.5f);
  set(ParamId::Overlap, 0.6f);
  set(ParamId::SprayMs, 40.f);
  set(ParamId::TransposeSt, 12.f);
  set(ParamId::ReverbMix, 0.3f);
  set(ParamId::DelayMix, 0.2f);
  return LeafPreset(v);
}

std::shared_ptr<PresetState> Package(const char* name, bs::PackageInfo* info = nullptr) {
  std::ifstream     f(std::string(BSA_CORPUS_PRESETS) + "/" + name, std::ios::binary);
  const std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  auto              state = std::make_shared<PresetState>();
  bs::PackageInfo   local;
  REQUIRE(bs::DecodePreset(bytes.data(), bytes.size(), state.get(), nullptr, info != nullptr ? info : &local));
  return state;
}

// The engine driven by hand: canonical configuration, Init, LoadPreset(Exact), then
// Process in `block`-frame blocks over the conditioned input. Events split the blocks and go
// through the unstamped API (SetParam, SetFreeze, Trigger), which the engine's contract says
// renders what stamped events render.
Stereo Reference(const PresetState& preset, const Stereo& in, uint32_t block = 48,
                 const std::vector<ScriptEvent>& events = {}) {
  bs::EngineConfig cfg;
  cfg.historyFrames = 1u << 22;
  bs::host::HeapArenas arenas(bs::PlanMemory(cfg));
  auto                 engine = std::make_unique<bs::Engine>();
  REQUIRE(engine->Init(cfg, arenas.get()));
  REQUIRE(engine->LoadPreset(preset, bs::LoadMode::Exact));
  Stereo c = in;
  bs::ConditionInput24(c.l.data(), c.l.data(), c.l.size());
  bs::ConditionInput24(c.r.data(), c.r.data(), c.r.size());
  Stereo out{std::vector<float>(c.Frames()), std::vector<float>(c.Frames())};
  size_t next = 0;
  for (size_t pos = 0; pos < c.Frames();) {
    for (; next < events.size() && events[next].frame == static_cast<int64_t>(pos); ++next) {
      const ScriptEvent& e = events[next];
      if (e.type == EventType::SetParam) engine->SetParam(static_cast<ParamId>(e.id), e.value);
      if (e.type == EventType::Freeze) engine->SetFreeze(e.value != 0.f);
      if (e.type == EventType::Trigger) engine->Trigger();
    }
    size_t end = std::min(c.Frames(), pos + block);
    if (next < events.size()) end = std::min(end, static_cast<size_t>(events[next].frame));
    const auto             n       = static_cast<uint32_t>(end - pos);
    const float*           ins[2]  = {c.l.data() + pos, c.r.data() + pos};
    float*                 outs[2] = {out.l.data() + pos, out.r.data() + pos};
    bs::Engine::ProcessContext ctx;
    ctx.in        = ins;
    ctx.out       = outs;
    ctx.numFrames = n;
    engine->Process(ctx);
    pos = end;
  }
  return out;
}

Stereo Sine(double hz, double amplitude, size_t frames) {
  Stereo s{std::vector<float>(frames), std::vector<float>(frames)};
  for (size_t i = 0; i < frames; ++i) {
    s.l[i] = s.r[i] = static_cast<float>(amplitude * std::sin(2.0 * kPi * hz * static_cast<double>(i) / kRate));
  }
  return s;
}

std::string ReadAll(const std::filesystem::path& p) {
  std::ifstream     f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

Preset MakePreset(std::shared_ptr<PresetState> state, const char* id) {
  Preset p;
  p.identity.package = true;
  p.identity.id      = id;
  p.identity.name    = id;
  p.state            = std::move(state);
  return p;
}

size_t Count(const std::vector<Planned>& plan, const char* script) {
  return static_cast<size_t>(std::count_if(plan.begin(), plan.end(), [&](const Planned& p) {
    return p.script == script && p.role != Role::Determinism;
  }));
}

const Planned* Named(const std::vector<Planned>& plan, const std::string& name) {
  for (const Planned& p : plan) {
    if (p.name == name) return &p;
  }
  return nullptr;
}

}  // namespace

TEST_CASE("the shared render is the engine's from the exact-restart state, in any block pattern") {
  const Input  in     = VectorInput(Vector::Plucks, 2 * kRate, kRate);
  const auto   preset = Busy();
  const Stereo want   = Reference(*preset, in.audio);

  Renderer r;
  REQUIRE(r.ok());
  RenderRequest rq;
  rq.preset = preset.get();
  rq.input  = &in.audio;
  RenderResult got;
  REQUIRE(r.Render(rq, &got));
  CHECK(got.load.exact);
  CHECK(SameBits(got.out, want));
  // The input's onsets, per second of the render (2 s of plucks, then 1 s of silence).
  REQUIRE(got.onsetSeconds.size() == 3u);
  CHECK(got.onsets > 0u);
  CHECK(got.onsetSeconds[0] + got.onsetSeconds[1] + got.onsetSeconds[2] == got.onsets);
  CHECK(got.onsetSeconds[2] == 0u);
  // Engine::Stats() over the render: the free-running scheduler's births, per second too, and
  // counted from the load (a second render on the same engine counts the same).
  REQUIRE(got.birthSeconds.size() == 3u);
  CHECK(got.births > 0u);
  CHECK(uint64_t{got.birthSeconds[0]} + got.birthSeconds[1] + got.birthSeconds[2] == got.births);
  CHECK(got.birthSeconds[2] > 0u);  // periodic: it runs on in the silence
  for (const std::vector<uint32_t>& pattern :
       {std::vector<uint32_t>{48}, std::vector<uint32_t>{512}, std::vector<uint32_t>{1},
        kMixedPattern}) {
    rq.blockPattern = pattern;
    RenderResult again;
    REQUIRE(r.Render(rq, &again));  // the same engine, restarted by the Exact load
    CHECK(SameBits(again.out, want));
    CHECK(again.births == got.births);
  }
  Renderer fresh;
  RenderResult other;
  REQUIRE(fresh.Render(rq, &other));
  CHECK(SameBits(other.out, want));

  SECTION("mono plays R := L") {
    rq.mode = InputMode::Mono;
    RenderResult mono;
    REQUIRE(r.Render(rq, &mono));
    CHECK(SameBits(mono.out, Reference(*preset, Stereo{in.audio.l, in.audio.l})));
    CHECK_FALSE(SameBits(mono.out, want));
  }
}

TEST_CASE("script events apply at their stamps, through the engine's transport") {
  const Input  in     = VectorInput(Vector::Strums, 2 * kRate, kRate / 2);
  const auto   preset = Busy();
  std::vector<ScriptEvent> events = {
      {4801, EventType::SetParam, static_cast<uint32_t>(ParamId::Feedback), 0.85f, 0},
      {24000, EventType::Freeze, 0, 1.f, 0},
      {50000, EventType::Trigger, 0, 1.f, 0},
      {50007, EventType::SetParam, static_cast<uint32_t>(ParamId::TransposeSt), -12.f, 0},
      {72001, EventType::Freeze, 0, 0.f, 0},
  };
  Renderer      r;
  RenderRequest rq;
  rq.preset = preset.get();
  rq.input  = &in.audio;
  rq.events = events;
  for (const std::vector<uint32_t>& pattern : {std::vector<uint32_t>{48}, kMixedPattern}) {
    rq.blockPattern = pattern;
    RenderResult got;
    REQUIRE(r.Render(rq, &got));
    CHECK(got.events == events.size());
    CHECK(SameBits(got.out, Reference(*preset, in.audio, 48, events)));
  }
  RenderResult plain;
  rq.events.clear();
  REQUIRE(r.Render(rq, &plain));
  CHECK_FALSE(SameBits(plain.out, Reference(*preset, in.audio, 48, events)));

  SECTION("a malformed request is refused with a reason") {
    RenderResult bad;
    rq.events = {{100, EventType::Freeze, 0, 1.f, 0}, {99, EventType::Freeze, 0, 0.f, 0}};
    CHECK_FALSE(r.Render(rq, &bad));
    CHECK(bad.error.find("order") != std::string::npos);
    rq.events = {{100, EventType::SpilloverLoad, 0, 0.f, 0}};
    CHECK_FALSE(r.Render(rq, &bad));
    rq.events.clear();
    rq.blockPattern = {513};
    CHECK_FALSE(r.Render(rq, &bad));
  }
  SECTION("a Spillover load of a staged preset") {
    const auto other = Package("macro_sweep.bsp");
    rq.events        = {{kRate, EventType::SpilloverLoad, 1, 0.f, 0}};  // FastCut
    rq.staged        = {other.get()};
    RenderResult a, b;
    REQUIRE(r.Render(rq, &a));
    rq.blockPattern = {512};
    REQUIRE(r.Render(rq, &b));
    CHECK(SameBits(a.out, b.out));
    CHECK_FALSE(SameBits(a.out, plain.out));
    // Before the load the render is the first preset's.
    CHECK(std::memcmp(a.out.l.data(), plain.out.l.data(), kRate * 4) == 0);
  }
}

TEST_CASE("inputs: the test-signal vectors, looped and conditioned") {
  const Input v = VectorInput(Vector::Plucks);
  CHECK(v.name == "plucks");
  CHECK(v.signalFrames == kSignalFrames);
  CHECK(v.audio.Frames() == kSignalFrames + kTailFrames);
  CHECK(std::all_of(v.audio.l.begin() + kSignalFrames, v.audio.l.end(), [](float x) { return x == 0.f; }));
  // On the codec's grid already: conditioning changes no bit.
  CHECK(SameBits(Conditioned(v.audio, InputMode::Stereo), v.audio));
  const Input loop = LoopedInput(Vector::Plucks, kSweepFrames);
  CHECK(loop.signalFrames == kSweepFrames);
  CHECK(loop.audio.Frames() == kSweepFrames);
  CHECK(std::memcmp(loop.audio.l.data(), v.audio.l.data(), kSignalFrames * 4) == 0);
  CHECK(std::memcmp(loop.audio.r.data() + kSignalFrames, v.audio.r.data(), (kSweepFrames - kSignalFrames) * 4) == 0);
  Stereo off{{0.1234567f, -0.987654f, 2.f}, {0.5f, 1e-9f, -3.f}};
  const Stereo c = Conditioned(off, InputMode::Mono);
  std::vector<float> want = off.l;
  bs::ConditionInput24(want.data(), want.data(), want.size());
  CHECK(SameBits(c.l, want));
  CHECK(SameBits(c.r, want));
  for (const Vector x : kVectors) {
    Vector back;
    REQUIRE(VectorByName(bs::testsignal::VectorName(x), &back));
    CHECK(back == x);
  }
  Vector none;
  CHECK_FALSE(VectorByName("nope", &none));
}

TEST_CASE("render hashes: SHA-256 of the interleaved float32, whole and per second") {
  RenderHashes one = HashRender(std::vector<float>{0.f}, std::vector<float>{0.f});
  CHECK(one.whole == "af5570f5a1810b7af78caf4bc70a660f0df51e42baf91d4de5b2328de0e83dfc");  // 8 zero bytes
  REQUIRE(one.seconds.size() == 1u);
  CHECK(one.seconds[0] == one.whole);
  const Stereo s = Sine(441.0, 0.25, 2 * kRate + 7);
  const RenderHashes h = HashRender(s);
  REQUIRE(h.seconds.size() == 3u);
  std::vector<uint8_t> bytes;
  for (size_t i = 0; i < s.Frames(); ++i) {
    for (const float x : {s.l[i], s.r[i]}) {
      uint32_t u;
      std::memcpy(&u, &x, 4);
      for (int b = 0; b < 4; ++b) bytes.push_back(static_cast<uint8_t>(u >> (8 * b)));
    }
  }
  CHECK(h.whole == Sha256Hex(bytes.data(), bytes.size()));
  CHECK(h.seconds[1] == Sha256Hex(bytes.data() + kRate * 8, kRate * 8));
  CHECK(h.seconds[2] == Sha256Hex(bytes.data() + 2 * kRate * 8, 7 * 8));
}

TEST_CASE("16-bit and float WAVs") {
  CHECK(ToPcm16(0.f) == 0);
  CHECK(ToPcm16(1.f / 65536.f) == 1);  // half a step rounds away from zero
  CHECK(ToPcm16(-1.f / 65536.f) == -1);
  CHECK(ToPcm16(0.99999f) == 32767);
  CHECK(ToPcm16(1.f) == 32767);
  CHECK(ToPcm16(-1.f) == -32768);
  CHECK(ToPcm16(-7.f) == -32768);
  CHECK(ToPcm16(std::nanf("")) == 0);
  CHECK(ToPcm16(0.5f) == 16384);
  const Stereo               s = Sine(1000.0, 0.5, 1000);
  const std::vector<uint8_t> w = WavPcm16(s);
  REQUIRE(w.size() == 44u + 1000u * 4u);
  CHECK(std::memcmp(w.data(), "RIFF", 4) == 0);
  CHECK(std::memcmp(w.data() + 8, "WAVEfmt ", 8) == 0);
  CHECK(w[20] == 1);   // PCM
  CHECK(w[22] == 2);   // stereo
  CHECK(w[34] == 16);  // bits
  Stereo   back;
  uint32_t rate = 0;
  uint16_t bits = 0;
  REQUIRE(ReadWav(w, &back, &rate, &bits));
  CHECK(rate == kRate);
  CHECK(bits == 16);
  for (size_t i = 0; i < s.Frames(); ++i) CHECK(std::fabs(back.l[i] - s.l[i]) <= 0.5f / 32768.f);
  REQUIRE(ReadWav(WavFloat32(s), &back, &rate, &bits));
  CHECK(bits == 32);
  CHECK(SameBits(back, s));
}

TEST_CASE("K-weighted loudness (ITU-R BS.1770-4)") {
  // A 997 Hz sine in both channels: the K filter's gain there is what -0.691 cancels, so a
  // full-scale one reads 0 LKFS (BS.1770-4: -3.01 LKFS in one channel).
  CHECK(Loudness(Sine(997.0, 1.0, 10 * kRate)).Integrated(0, 10 * kRate) == Approx(0.0).margin(0.02));
  CHECK(Loudness(Sine(997.0, 0.1, 10 * kRate)).Integrated(0, 10 * kRate) == Approx(-20.0).margin(0.02));
  // The absolute gate drops what is under -70 LKFS, the relative gate what is 10 LU under the
  // gated mean (-23 LUFS here, so the -40 LUFS half goes). What stays: 47 blocks of the loud 5 s
  // and the three straddling the change, 3/4, 1/2 and 1/4 loud: 48.5 / 50 of the loud level.
  const double straddled = -20.0 + 10.0 * std::log10(48.5 / 50.0);
  Stereo quietTail = Sine(997.0, 0.1, 10 * kRate);
  for (size_t i = 5 * kRate; i < 10 * kRate; ++i) quietTail.l[i] = quietTail.r[i] *= 0.0001f;
  CHECK(Loudness(quietTail).Integrated(0, 10 * kRate) == Approx(straddled).margin(0.01));
  Stereo relTail = Sine(997.0, 0.1, 10 * kRate);
  for (size_t i = 5 * kRate; i < 10 * kRate; ++i) relTail.l[i] = relTail.r[i] *= 0.01f;
  CHECK(Loudness(relTail).Integrated(0, 10 * kRate) == Approx(straddled).margin(0.01));
  CHECK(Loudness(relTail).Integrated(0, 5 * kRate) == Approx(-20.0).margin(0.02));  // a span
  const Stereo silent{std::vector<float>(4 * kRate), std::vector<float>(4 * kRate)};
  CHECK(Loudness(silent).Integrated(0, 4 * kRate) == kSilentDb);
  const Loudness k(Sine(997.0, 0.1, 5 * kRate));
  CHECK(k.ShortTerm(kRate) == Approx(-20.0).margin(0.05));
  CHECK(k.ShortTerm(3 * kRate) == kSilentDb);  // the 3 s window does not fit
}

TEST_CASE("metrics: peak, tail, steps and the numbers checks") {
  // 1 s of sine, then a decay of 60 dB per 0.5 s, then silence to 4 s.
  Stereo s = Sine(500.0, 0.5, 4 * kRate);
  for (size_t i = kRate; i < s.Frames(); ++i) {
    const double t = static_cast<double>(i - kRate) / kRate;
    s.l[i] = s.r[i] = static_cast<float>(s.l[i] * std::pow(10.0, -6.0 * t));
  }
  Metrics m = Measure(s, kRate);
  CHECK(m.peakDbfs == Approx(Db20(0.5)).margin(0.01));
  // -70 dBFS from 0.5 amplitude (-6.02 dBFS) is 64 dB down: 0.533 s at 120 dB per second.
  CHECK(m.tailSeconds == Approx(64.0 / 120.0).margin(0.01));
  CHECK(m.tailFinite);
  CHECK_FALSE(m.tailEstimated);
  CHECK(TailText(m) == "0.53 s");
  CHECK(m.nonFinite == 0u);
  CHECK(m.subnormal == 0u);
  CHECK(m.overFull == 0u);
  Stereo hum = Sine(500.0, 0.01, 3 * kRate);
  m          = Measure(hum, kRate);
  CHECK_FALSE(m.tailFinite);  // a level that does not fall
  CHECK(m.tailSeconds == Approx(2.0).margin(0.01));
  CHECK(std::fabs(m.tailDecayDbPerS) < 0.05);
  CHECK(TailText(m) == "unending (over 2.0 s)");
  hum.l[100]  = std::nanf("");
  hum.r[200]  = 1e-40f;  // subnormal
  hum.l[300]  = 1.5f;
  m           = Measure(hum, kRate);
  CHECK(m.nonFinite == 1u);
  CHECK(m.subnormal == 1u);
  CHECK(m.overFull == 1u);
  CHECK(m.peakDbfs == Approx(Db20(1.5)));
  CHECK((m.maxStepFrame == 300u || m.maxStepFrame == 301u));  // into or out of the 1.5
  CHECK(Round1(4.04) == Approx(4.0));

  // A tail still sounding when the render ends is not finite until a probe measures it.
  auto decaying = [](double dbPerSecond, size_t silent, double floorAmp = 0.0) {
    Stereo d = Sine(500.0, 0.5, kRate + silent);
    for (size_t i = kRate; i < d.Frames(); ++i) {
      const double t   = static_cast<double>(i - kRate) / kRate;
      const double amp = std::max(0.5 * std::pow(10.0, -dbPerSecond * t / 20.0), floorAmp);
      d.l[i] = d.r[i] = static_cast<float>(amp * std::sin(2.0 * kPi * 500.0 * static_cast<double>(i) / kRate));
    }
    return d;
  };
  m = Measure(decaying(6.0, 4 * kRate), kRate);
  CHECK_FALSE(m.tailFinite);
  CHECK_FALSE(m.tailEstimated);
  CHECK(m.tailSeconds == Approx(4.0).margin(0.01));
  CHECK(TailText(m) == "unending (over 4.0 s)");
  // A fall that reaches -70 dBFS inside the last 0.5 s is not measured either.
  m = Measure(decaying(30.0, 5 * kRate / 2), kRate);
  CHECK_FALSE(m.tailFinite);
  m = Measure(decaying(30.0, 3 * kRate), kRate);
  CHECK(m.tailFinite);
  CHECK(m.tailSeconds == Approx(64.0 / 30.0).margin(0.02));
  CHECK(Round1(-1.06) == Approx(-1.1));

  // The probe: the same render with 60 s of silence (kTailProbeFrames).
  // 6 dB per second: ends inside the probe, 10.7 s after the input.
  Metrics t = Measure(decaying(6.0, 4 * kRate), kRate);
  MeasureTail(decaying(6.0, kTailProbeFrames), kRate, &t);
  CHECK(t.tailFinite);
  CHECK_FALSE(t.tailEstimated);
  CHECK(t.tailProbeSeconds == Approx(60.0));
  CHECK(t.tailSeconds == Approx(64.0 / 6.0).margin(0.01));
  CHECK(TailText(t).rfind("10.6", 0) == 0);
  CHECK(TailText(t).find("(60 s probe)") != std::string::npos);
  // 0.8 dB per second: still sounding at 60 s, extrapolated from the fitted fall (80 s).
  MeasureTail(decaying(0.8, kTailProbeFrames), kRate, &t);
  CHECK(t.tailFinite);
  CHECK(t.tailEstimated);
  CHECK(t.tailDecayDbPerS == Approx(0.8).margin(0.02));
  CHECK(t.tailSeconds == Approx(64.0 / 0.8).margin(1.0));
  CHECK(TailText(t).rfind("about 80", 0) == 0);
  // Slower than 0.2 dB per second, flat, rising, or a fall that stops: unending.
  MeasureTail(decaying(0.1, kTailProbeFrames), kRate, &t);
  CHECK_FALSE(t.tailFinite);
  CHECK(t.tailDecayDbPerS == Approx(0.1).margin(0.02));
  CHECK(TailText(t) == "unending (over 60.0 s)");
  MeasureTail(decaying(-0.3, kTailProbeFrames), kRate, &t);  // growing
  CHECK_FALSE(t.tailFinite);
  MeasureTail(decaying(1.0, kTailProbeFrames, 0.01), kRate, &t);  // falls to -40 dBFS, then holds
  CHECK_FALSE(t.tailFinite);
  // A sustained level whose 100 ms peaks wander by several dB (the review's case: a fit over a
  // few seconds of peaks found a fall in it): unending.
  Stereo wander = Sine(500.0, 0.5, kRate + kTailProbeFrames);
  uint32_t seed = 12345u;
  for (size_t a = kRate; a < wander.Frames(); a += kRate / 10) {
    seed = seed * 1664525u + 1013904223u;
    const double g = std::pow(10.0, (static_cast<double>(seed >> 8) / 16777216.0 - 0.5) * 6.0 / 20.0);
    for (size_t i = a; i < std::min(a + kRate / 10, wander.Frames()); ++i) {
      wander.l[i] = wander.r[i] = static_cast<float>(wander.l[i] * g);
    }
  }
  MeasureTail(wander, kRate, &t);
  CHECK_FALSE(t.tailFinite);
  CHECK(std::fabs(t.tailDecayDbPerS) < kMinDecayDbPerS);
}

TEST_CASE("clicks: a step scored against the steps around it") {
  // A smooth waveform scores under 2 at any pitch, so a pitch shift is no click.
  for (const double hz : {220.0, 880.0, 3520.0}) {
    const Metrics m = Measure(Sine(hz, 0.5, kRate), kRate);
    CHECK(m.click > 1.0);
    CHECK(m.click < 2.0);
  }
  // A splice (the waveform jumps 3.7 ms ahead) scores in the tens; a step under -40 dBFS is not
  // scored.
  auto jumped = [](double amplitude) {
    Stereo s = Sine(220.0, amplitude, kRate);
    for (size_t i = kRate / 2; i < s.Frames(); ++i) {
      s.l[i] = static_cast<float>(amplitude * std::sin(2.0 * kPi * 220.0 * static_cast<double>(i + 177) / kRate));
    }
    return s;
  };
  Metrics m = Measure(jumped(0.5), kRate);
  CHECK(m.click > 20.0);
  CHECK(m.clickFrame == kRate / 2);
  CHECK(m.clickStep > 0.1);
  CHECK(Measure(jumped(0.004), kRate).click == 0.0);
}

TEST_CASE("clicks on a render: SoftNotes shows a splice that Plucks hides") {
  // The review's case: under the Mix law the dry plays at unity, so a Plucks render's own
  // attacks are the largest steps and the old 4x rule could not fail. On SoftNotes the static
  // render is smooth; the same splice injected into each shows the difference.
  const auto state = Package("macro_sweep.bsp");
  Renderer   renderer;
  auto       render = [&](Vector v) {
    const Input   in = LoopedInput(v, kSweepFrames);
    RenderRequest rq;
    rq.preset = state.get();
    rq.input  = &in.audio;
    RenderResult r;
    REQUIRE(renderer.Render(rq, &r));
    return r.out;
  };
  auto splice = [](Stereo s, size_t at) {
    for (size_t i = at; i + 300 < s.Frames(); ++i) {
      s.l[i] = s.l[i + 300];
      s.r[i] = s.r[i + 300];
    }
    return s;
  };
  const Stereo  soft = render(Vector::SoftNotes);
  const Metrics ref  = Measure(soft, kSweepFrames);
  CHECK(ref.click < 4.0);
  int caught = 0;
  for (const size_t at : {3 * kRate + 1234, 7 * kRate + 77, 11 * kRate + 4321, 13 * kRate + 999}) {
    caught += Measure(splice(soft, at), kSweepFrames).click > 4.0 * std::max(ref.click, 2.0) ? 1 : 0;
  }
  CHECK(caught >= 3);
  const Stereo  plucks = render(Vector::Plucks);
  const Metrics pref   = Measure(plucks, kSweepFrames);
  const Metrics pcut   = Measure(splice(plucks, 7 * kRate + 77), kSweepFrames);
  CHECK(pref.maxStep > 0.5);                 // the dry's attacks
  CHECK(pcut.maxStep < 4.0 * pref.maxStep);  // the old rule: no splice can fail it
}

TEST_CASE("macro positions applied as stored ones") {
  auto state = Package("macro_sweep.bsp");
  CHECK(MacroDefined(*state, ParamId::MacroRepeats));
  const float stored = StoredPosition(*state, ParamId::MacroRepeats);
  CHECK(stored >= 0.f);
  PresetState moved = *state;
  AtPosition(&moved, ParamId::MacroRepeats, 1.f);
  bs::PresetLeaf out[bs::kMaxMacroTargets];
  const size_t n = bs::EvalMacro(state->mode, ParamId::MacroRepeats, 1.f, out, bs::kMaxMacroTargets);
  REQUIRE(n == 1u);
  CHECK(LeafValue(moved, static_cast<ParamId>(out[0].id)) == out[0].value);
  CHECK(LeafValue(moved, ParamId::Feedback) == Approx(0.9f));
  CHECK(StoredPosition(moved, ParamId::MacroRepeats) == 1.f);
  CHECK(LeafValue(moved, ParamId::Mix) == LeafValue(*state, ParamId::Mix));
  bs::LoadReport report;
  CHECK(bs::CheckPreset(moved, &report));
  // A leaf-only preset's default macros: defined, at 0.5.
  const auto leafOnly = Busy();
  CHECK(StoredPosition(*leafOnly, ParamId::MacroActivity) == 0.5f);
  CHECK(StoredPosition(*leafOnly, ParamId::MacroAux1) == -1.f);
  // SetLeaf keeps the ids ascending.
  PresetState sparse;
  SetLeaf(&sparse, ParamId::Mix, 0.25f);
  SetLeaf(&sparse, ParamId::DelayMs, 100.f);
  SetLeaf(&sparse, ParamId::Mix, 0.75f);
  REQUIRE(sparse.leafCount == 2u);
  CHECK(sparse.leaves[0].id == static_cast<uint32_t>(ParamId::DelayMs));
  CHECK(sparse.leaves[1].value == 0.75f);
}

TEST_CASE("the scripts' plan") {
  const auto      state = Package("macro_sweep.bsp");
  Preset          a = MakePreset(state, "factory.a"), b = MakePreset(state, "factory.b"),
                  c = MakePreset(state, "factory.c");
  const std::vector<const Preset*> set = {&a, &b, &c};
  std::vector<std::string>         all(std::begin(kScriptNames), std::end(kScriptNames));
  std::vector<std::string>         skipped;
  const std::vector<Planned>       plan = Plan(a, set, all, &skipped);
  CHECK(skipped.empty());
  CHECK(Count(plan, "S0") == 9u);
  // An attack mode's sweeps and S11 again on SoftNotes, for the Clicks check.
  // The reference and the sweep, on plucks and soft_notes, and Activity's five rungs on plucks.
  CHECK(Count(plan, "S1") == 4u + 5u);
  const Planned* arung = Named(plan, "S1.activity-0.75.plucks");
  REQUIRE(arung != nullptr);
  CHECK(arung->role == Role::ActivityRung);
  CHECK(arung->tailFrames == 0u);
  CHECK_FALSE(arung->writeWav);
  REQUIRE(arung->positions.size() == 1u);
  CHECK(arung->positions[0].first == ParamId::MacroActivity);
  CHECK(arung->positions[0].second == 0.75f);
  CHECK(Named(plan, "S1.activity-0.75.soft_notes") == nullptr);  // the class input only
  for (const char* s : {"S2", "S3", "S4", "S5", "S6"}) CHECK(Count(plan, s) == 2u);
  for (const char* s : {"S8", "S9"}) CHECK(Count(plan, s) == 1u);
  CHECK(Count(plan, "S7") == 5u);
  CHECK(Count(plan, "S10") == 2u);
  CHECK(Count(plan, "S11") == 2u * (2u + 16u));
  CHECK(Named(plan, "S1.reference.soft_notes") != nullptr);
  CHECK(Named(plan, "S4.time.soft_notes") != nullptr);
  CHECK(Named(plan, "S11.corner-a1r0s1t0.soft_notes") != nullptr);
  CHECK(Named(plan, "S11.at-factory.c.soft_notes") != nullptr);
  CHECK(std::count_if(plan.begin(), plan.end(), [](const Planned& p) { return p.role == Role::Determinism; }) == 3);

  const Planned* sweep = Named(plan, "S2.repeats.plucks");
  REQUIRE(sweep != nullptr);
  REQUIRE(sweep->events.size() == kSweepFrames / kPedalBlock);
  const float p = StoredPosition(*state, ParamId::MacroRepeats);
  CHECK(sweep->events.front().value == p);
  CHECK(sweep->events.front().frame == 0);
  CHECK(sweep->events.back().frame == kSweepFrames - kPedalBlock);
  float lo = 1, hi = 0;
  for (const ScriptEvent& e : sweep->events) {
    CHECK(e.frame % kPedalBlock == 0);
    lo = std::min(lo, e.value);
    hi = std::max(hi, e.value);
  }
  CHECK(lo == 0.f);
  CHECK(hi == 1.f);
  CHECK(sweep->events.back().value == Approx(p).margin(0.001));

  const Planned* rung = Named(plan, "S7.repeats-0.25.plucks");
  REQUIRE(rung != nullptr);
  CHECK(rung->tailFrames == kRepeatsTail);
  const Planned* load = Named(plan, "S10.trails.from-factory.c.plucks");  // the set's previous: cyclic
  REQUIRE(load != nullptr);
  CHECK(load->from == &c);
  CHECK(Named(plan, "S11.at-factory.b.plucks") != nullptr);
  CHECK(Named(plan, "S11.corner-a1r0s1t0.plucks") != nullptr);
  const Planned* trig = Named(plan, "S9.footswitch.plucks");
  REQUIRE(trig != nullptr);
  CHECK(trig->events.size() == 16u);
  CHECK(trig->events.front().frame == kRate + kRate / 4);

  // A pad mode plays its sweeps on SoftNotes; a set of one loads S10 from the default preset.
  Preset pad = MakePreset(state, "factory.pad");
  pad.declare.inputClass = InputClass::Pad;
  const std::vector<Planned> alone = Plan(pad, {&pad}, all);
  CHECK(Named(alone, "S1.activity.soft_notes") != nullptr);
  CHECK(Count(alone, "S1") == 2u + 5u);  // the class input is the Clicks input
  CHECK(Named(alone, "S1.activity-0.soft_notes") != nullptr);
  const Planned* fromDefault = Named(alone, "S10.fastcut.from-default.soft_notes");
  REQUIRE(fromDefault != nullptr);
  CHECK(fromDefault->from == nullptr);
  CHECK(Count(alone, "S11") == 16u);

  // A macro the mode leaves undefined is skipped and said so.
  auto noSpace = std::make_shared<PresetState>(*state);
  for (auto& m : noSpace->mode.macros.macros) {
    if (m.id == static_cast<uint32_t>(ParamId::MacroSpace)) m.count = 0;
  }
  Preset ns = MakePreset(noSpace, "factory.ns");
  skipped.clear();
  const std::vector<Planned> part = Plan(ns, {&ns}, {"S5", "S7"}, &skipped);
  REQUIRE(skipped.size() == 1u);
  CHECK(skipped[0].rfind("S5:", 0) == 0);
  CHECK(Count(part, "S5") == 0u);
  CHECK(Count(part, "S1") == 2u);  // the sweeps' references, on plucks and soft_notes
  CHECK(Count(part, "S7") == 5u);
}

TEST_CASE("the pre-screen's thresholds, at their edges") {
  Preset preset = MakePreset(Package("macro_sweep.bsp"), "factory.t");
  auto make = [](const std::string& script, const std::string& name, Role role, double loud, double peak) {
    Rendered r;
    r.plan.script = script;
    r.plan.name   = name;
    r.plan.role   = role;
    r.ok = r.exact     = true;
    r.hashes.whole     = "h-" + name;
    r.metrics.loudness = loud;
    r.metrics.peakDbfs = peak;
    r.metrics.maxStep  = 0.1;
    r.metrics.click    = 1.5;
    return r;
  };
  const double dp = DryLevel(Vector::Plucks), ds = DryLevel(Vector::Strums);
  CHECK(dp == Approx(-18.6).margin(0.06));  // the Mix-law probe's dry level (record §2.2)
  auto verdict = [](const std::vector<Check>& cs, const char* name) {
    for (const Check& c : cs) {
      if (c.name == name) return c.verdict;
    }
    return std::string("missing");
  };
  std::vector<Rendered> rs = {make("S0", "S0.engaged.plucks", Role::Engaged, dp + 4.04, -1.04),
                              make("S0", "S0.engaged.strums", Role::Engaged, ds - 1.04, -3.0),
                              make("S0", "S0.wet.plucks", Role::Wet, dp + 2.04, -3.0),
                              make("S0", "S0.wet.strums", Role::Wet, ds - 2.04, -3.0)};
  std::vector<Check> cs = PreScreen(preset, rs);
  CHECK(verdict(cs, "Engaged") == "pass");
  CHECK(verdict(cs, "Level") == "pass");
  CHECK(verdict(cs, "Peak") == "pass");
  CHECK(verdict(cs, "Tail") == "pass");
  CHECK(verdict(cs, "Renders") == "pass");
  CHECK(verdict(cs, "Fallback") == "n/a");  // not an onset mode
  rs.push_back(make("S0", "S0.engaged.saturation", Role::Engaged, dp, 6.0));
  rs.back().plan.vector = Vector::Saturation;
  cs = PreScreen(preset, rs);
  CHECK(verdict(cs, "Peak") == "pass");  // judged on the class inputs only
  CHECK(verdict(cs, "Peak (other)") == "info");  // the other vectors and the wet: reported
  for (const Check& c : cs) {
    if (c.name == "Peak (other)") CHECK(c.detail.find("6.0 dBFS (S0.engaged.saturation)") != std::string::npos);
  }
  rs.pop_back();
  // Peak is judged on the class inputs only (the owner's decision, 2026-10-08): an attack mode
  // that clips only on SoftNotes passes, its stored peak there reported under Peak (other) by
  // name, never as a failure.
  rs.push_back(make("S0", "S0.engaged.soft_notes", Role::Engaged, DryLevel(Vector::SoftNotes), 0.5));
  rs.back().plan.vector         = Vector::SoftNotes;
  rs.back().metrics.overFull    = 3;
  cs = PreScreen(preset, rs);
  CHECK(verdict(cs, "Peak") == "pass");
  CHECK(verdict(cs, "Peak (other)") == "info");
  for (const Check& c : cs) {
    CHECK_FALSE(c.verdict == "FAIL");
    if (c.name == "Peak") CHECK(c.detail.find("soft_notes") == std::string::npos);
    if (c.name == "Peak (other)") {
      CHECK(c.detail.rfind("soft_notes at the stored positions 0.50 dBFS, 3 samples over full scale", 0) == 0);
    }
  }
  // A class input that clips still fails, with the other input still reported.
  rs[1].metrics.peakDbfs = -0.5;
  cs = PreScreen(preset, rs);
  CHECK(verdict(cs, "Peak") == "FAIL");
  CHECK(verdict(cs, "Peak (other)") == "info");
  for (const Check& c : cs) {
    if (c.name == "Peak") {
      CHECK(c.detail.find("strums -0.50 dBFS (FAIL)") != std::string::npos);
      CHECK(c.detail.find("soft_notes") == std::string::npos);
    }
    if (c.name == "Peak (other)") CHECK(c.detail.find("soft_notes at the stored positions 0.50 dBFS") != std::string::npos);
  }
  rs[1].metrics.peakDbfs = -3.0;
  {  // A pad mode: judged on SoftNotes; Plucks and Strums reported.
    Preset pad              = preset;
    pad.declare.inputClass  = InputClass::Pad;
    rs[0].metrics.peakDbfs  = 0.5;  // plucks over full scale: reported
    cs = PreScreen(pad, rs);
    CHECK(verdict(cs, "Peak") == "FAIL");  // soft_notes, its class input, at +0.50 dBFS
    for (const Check& c : cs) {
      if (c.name == "Peak") CHECK(c.detail.find("soft_notes 0.50 dBFS, 3 samples over full scale (FAIL)") != std::string::npos);
      if (c.name == "Peak (other)") {
        CHECK(c.detail.rfind("plucks at the stored positions 0.50 dBFS; strums at the stored positions -3.00 dBFS", 0) == 0);
      }
    }
    rs.back().metrics.peakDbfs = -1.0;
    rs.back().metrics.overFull = 0;
    CHECK(verdict(PreScreen(pad, rs), "Peak") == "pass");  // plucks' clip does not fail it
    rs[0].metrics.peakDbfs = -1.04;
  }
  rs.pop_back();
  rs[0].metrics.loudness = dp + 4.06;
  rs[1].metrics.loudness = ds - 1.0;
  CHECK(verdict(PreScreen(preset, rs), "Engaged") == "FAIL");
  rs[0].metrics.loudness = dp + 4.0;
  rs[1].metrics.loudness = ds - 1.06;
  CHECK(verdict(PreScreen(preset, rs), "Engaged") == "FAIL");
  rs[1].metrics.loudness = ds;
  rs[2].metrics.loudness = dp + 2.06;
  CHECK(verdict(PreScreen(preset, rs), "Level") == "FAIL");
  rs[2].metrics.loudness = dp;
  rs[0].metrics.peakDbfs = -0.94;
  CHECK(verdict(PreScreen(preset, rs), "Peak") == "FAIL");
  rs[0].metrics.peakDbfs = -0.99;  // peaks are compared unrounded
  CHECK(verdict(PreScreen(preset, rs), "Peak") == "FAIL");
  rs[0].metrics.peakDbfs = -1.0;
  CHECK(verdict(PreScreen(preset, rs), "Peak") == "pass");
  rs[0].metrics.peakDbfs = -3.0;
  rs[0].metrics.tailEstimated = true;  // still sounding at the render's end, but falling
  rs[0].metrics.tailSeconds   = 40.0;
  CHECK(verdict(PreScreen(preset, rs), "Tail") == "pass");
  rs[0].metrics.tailEstimated = false;
  rs[0].metrics.tailFinite    = false;
  CHECK(verdict(PreScreen(preset, rs), "Tail") == "FAIL");
  preset.declare.selfOscillating = true;
  CHECK(verdict(PreScreen(preset, rs), "Tail") == "pass");
  preset.declare.selfOscillating = false;
  rs[0].metrics.tailFinite = true;
  rs[3].metrics.subnormal  = 2;
  CHECK(verdict(PreScreen(preset, rs), "Denormals") == "FAIL");
  rs[3].metrics.subnormal = 0;
  rs[3].metrics.nonFinite = 1;
  CHECK(verdict(PreScreen(preset, rs), "Finite") == "FAIL");
  rs[3].metrics.nonFinite = 0;
  rs[3].exact             = false;
  CHECK(verdict(PreScreen(preset, rs), "Renders") == "FAIL");
  rs[3].exact = true;

  SECTION("determinism") {
    Rendered again   = rs[0];
    again.plan.name  = "S0.engaged.plucks@again";
    again.plan.role  = Role::Determinism;
    again.plan.repeatOf = "S0.engaged.plucks";
    rs.push_back(again);
    CHECK(verdict(PreScreen(preset, rs), "Determinism") == "pass");
    rs.back().hashes.whole = "other";
    CHECK(verdict(PreScreen(preset, rs), "Determinism") == "FAIL");
  }
  SECTION("fallback, for an onset mode") {
    Preset onset = MakePreset(Package("mode_onset_marks.bsp"), "factory.o");
    REQUIRE(OnsetMode(*onset.state));
    const double dsn = DryLevel(Vector::SoftNotes);
    rs.push_back(make("S0", "S0.wet.soft_notes", Role::Wet, dsn - 12.0, -3.0));
    rs[2].metrics.loudness = dp;
    CHECK(verdict(PreScreen(onset, rs), "Fallback") == "pass");
    rs.back().metrics.loudness = dsn - 12.06;
    CHECK(verdict(PreScreen(onset, rs), "Fallback") == "FAIL");
    onset.declare.needsAttacks = true;
    CHECK(verdict(PreScreen(onset, rs), "Fallback") == "declared");
  }
  SECTION("sweeps, clicks and the Repeats rule") {
    Rendered ref = make("S1", "S1.reference.plucks", Role::SweepRef, -18.0, -3.0);
    ref.metrics.shortTerm.assign(14, -18.0);
    Rendered act = make("S1", "S1.activity.plucks", Role::Sweep, -18.0, -3.0);
    act.plan.macro = ParamId::MacroActivity;
    act.metrics.shortTerm.assign(14, -18.0);
    act.metrics.shortTerm[5] = -18.0 + 3.04;
    Rendered space = act;
    space.plan.name = "S5.space.plucks";
    space.plan.macro = ParamId::MacroSpace;
    space.metrics.shortTerm[5] = -30.0;  // not judged
    Rendered rep = act;
    rep.plan.name   = "S2.repeats.plucks";
    rep.plan.macro  = ParamId::MacroRepeats;
    rep.plan.stored = 0.25f;  // the rising leg: 2 s to 10 s, windows 2..7
    for (size_t k = 0; k < 14; ++k) rep.metrics.shortTerm[k] = -18.0 + 0.5 * static_cast<double>(k);
    // The Clicks renders: the reference and a sweep on SoftNotes.
    Rendered kref = make("S1", "S1.reference.soft_notes", Role::SweepRef, -20.0, -6.0);
    kref.plan.vector   = Vector::SoftNotes;
    kref.metrics.click = 3.0;
    Rendered kact      = make("S1", "S1.activity.soft_notes", Role::Sweep, -20.0, 3.0);  // peak: not judged
    kact.plan.vector   = Vector::SoftNotes;
    kact.plan.macro    = ParamId::MacroActivity;
    kact.metrics.click = 12.0;
    kact.metrics.maxStep = 1.9;
    rs.insert(rs.end(), {ref, act, space, rep, kref, kact});
    const size_t iAct = rs.size() - 5, iRep = rs.size() - 3, iKref = rs.size() - 2, iKact = rs.size() - 1;
    cs = PreScreen(preset, rs);
    CHECK(verdict(cs, "Sweeps") == "pass");
    CHECK(verdict(cs, "Clicks") == "pass");  // 12.0 = 4x the reference's 3.0
    CHECK(verdict(cs, "Repeats") == "pass");
    CHECK(verdict(cs, "Peak (moved)") == "pass");  // the class sweeps only
    rs[iAct].metrics.shortTerm[5] = -18.0 + 3.06;
    CHECK(verdict(PreScreen(preset, rs), "Sweeps") == "FAIL");
    rs[iAct].metrics.shortTerm[5] = -18.0;
    rs[iKact].metrics.click = 12.01;
    cs = PreScreen(preset, rs);
    CHECK(verdict(cs, "Clicks") == "FAIL");
    rs[iKact].metrics.click   = 12.0;
    rs[iAct].metrics.maxStep  = 5.0;  // the class sweep's steps are not the Clicks check's
    rs[iAct].metrics.click    = 50.0;
    CHECK(verdict(PreScreen(preset, rs), "Clicks") == "pass");
    rs[iKref].metrics.click = 0.0;  // a static render with no scored step: the base is 2
    CHECK(verdict(PreScreen(preset, rs), "Clicks") == "FAIL");
    rs[iKact].metrics.click = 8.0;
    CHECK(verdict(PreScreen(preset, rs), "Clicks") == "pass");
    rs[iAct].metrics.peakDbfs = 0.01;
    CHECK(verdict(PreScreen(preset, rs), "Peak (moved)") == "FAIL");  // over 0 dBFS, unrounded
    rs[iAct].metrics.peakDbfs = -3.0;
    rs[iRep].metrics.shortTerm[5] = rs[iRep].metrics.shortTerm[4] - 1.1;  // the rising leg falls
    CHECK(verdict(PreScreen(preset, rs), "Repeats") == "FAIL");
    rs[iRep].metrics.shortTerm[5] = rs[iRep].metrics.shortTerm[4] - 0.9;  // within 1 LU
    CHECK(verdict(PreScreen(preset, rs), "Repeats") == "pass");
    // Response: Shape judged; Activity reported, and judged only when nothing moves.
    rs[iAct].response.measured      = true;
    rs[iAct].response.brightnessPct = 0.0;
    rs[iAct].response.envelopeDb    = 0.0;
    rs[iAct].response.heard         = {5, 6, 6};
    rs[iAct].response.refHeard      = {5, 6, 6};
    rs[iAct].metrics.shortTerm.assign(14, -18.0);
    CHECK(verdict(PreScreen(preset, rs), "Response (Activity)") == "FAIL");  // a dead knob
    rs[iAct].response.heard = {5, 7, 6};
    CHECK(verdict(PreScreen(preset, rs), "Response (Activity)") == "info");
    rs[iAct].response.heard         = {5, 6, 6};
    rs[iAct].metrics.shortTerm[6]   = -18.0 + 0.2;  // the level moves
    CHECK(verdict(PreScreen(preset, rs), "Response (Activity)") == "info");
    rs[iAct].metrics.shortTerm[6]   = -18.0;
    rs[iAct].response.brightnessPct = 5.0;
    CHECK(verdict(PreScreen(preset, rs), "Response (Activity)") == "info");
    Rendered shape = act;
    shape.plan.name             = "S3.shape.plucks";
    shape.plan.macro            = ParamId::MacroShape;
    shape.metrics.shortTerm.assign(14, -18.0);
    shape.response.measured     = true;
    shape.response.brightnessPct = 4.9;
    shape.response.envelopeDb   = 0.9;
    rs.push_back(shape);
    CHECK(verdict(PreScreen(preset, rs), "Response (Shape)") == "FAIL");
    rs.back().response.envelopeDb = -1.0;
    CHECK(verdict(PreScreen(preset, rs), "Response (Shape)") == "pass");
    rs.pop_back();
    // S7's ladder: levels and tails non-decreasing, at most +10 LU over the stored position.
    const double stored = rs[0].metrics.loudness;
    for (int i = 0; i < 5; ++i) {
      Rendered r = make("S7", "S7.repeats-" + std::to_string(i) + ".plucks", Role::RepeatsRung,
                        stored + 2.0 * i, -3.0);
      r.metrics.tailSeconds = 1.0 + i;
      rs.push_back(r);
    }
    CHECK(verdict(PreScreen(preset, rs), "Repeats") == "pass");
    CHECK(verdict(PreScreen(preset, rs), "Response (Repeats)") == "pass");  // 5 s against 1 s
    rs.back().metrics.tailSeconds = 1.49;  // under 1.5 times the tail at 0: named, not judged
    cs = PreScreen(preset, rs);
    CHECK(verdict(cs, "Response (Repeats)") == "listen");
    CHECK(verdict(cs, "Repeats") == "FAIL");  // and shorter than the rung before it
    CHECK_FALSE(verdict(cs, "Response (Repeats)") == "FAIL");
    for (int i = 0; i < 5; ++i) rs[rs.size() - 1 - i].metrics.tailSeconds = 0.0;  // no tail at all
    CHECK(verdict(PreScreen(preset, rs), "Response (Repeats)") == "listen");
    for (int i = 0; i < 5; ++i) rs[rs.size() - 5 + i].metrics.tailSeconds = 1.0 + i;
    rs.back().metrics.tailSeconds = 5.0;
    rs.back().metrics.tailFinite = false;  // an unending tail at maximum is the largest
    CHECK(verdict(PreScreen(preset, rs), "Repeats") == "pass");
    CHECK(verdict(PreScreen(preset, rs), "Response (Repeats)") == "pass");
    rs.back().metrics.loudness = stored + 10.06;
    CHECK(verdict(PreScreen(preset, rs), "Repeats") == "FAIL");
    rs.back().metrics.loudness = stored + 8.0;
    rs[rs.size() - 2].metrics.loudness = stored + 8.6;  // falls by 0.6 LU
    CHECK(verdict(PreScreen(preset, rs), "Repeats") == "FAIL");
  }
  SECTION("Activity's response: births per second at S1's rungs") {
    auto rung = [&](float pos, uint32_t perSecond) {
      Rendered r = make("S1", "S1.activity-" + std::to_string(pos) + ".plucks", Role::ActivityRung, -18.0, -3.0);
      r.plan.position = pos;
      r.metrics.span  = kSignalFrames;
      r.birthSeconds.assign(10, perSecond);
      r.births = 10u * perSecond;
      return r;
    };
    const size_t first = rs.size();
    for (const auto& [pos, n] : std::vector<std::pair<float, uint32_t>>{{0.f, 40}, {0.25f, 40}, {0.5f, 44}, {0.75f, 48}, {1.f, 50}}) {
      rs.push_back(rung(pos, n));
    }
    cs = PreScreen(preset, rs);
    CHECK(verdict(cs, "Response (Activity)") == "pass");  // 40 to 50: 1.25x, never falling
    for (const Check& c : cs) {
      if (c.name == "Response (Activity)") CHECK(c.detail.find("40.0 40.0 44.0 48.0 50.0 (1.25x)") != std::string::npos);
    }
    rs[first + 4].birthSeconds.assign(10, 49);  // 1.225x: under a quarter
    CHECK(verdict(PreScreen(preset, rs), "Response (Activity)") == "FAIL");
    rs[first + 4].birthSeconds.assign(10, 60);
    rs[first + 3].birthSeconds.assign(10, 41);  // 48 to 41, over 5 % against the direction
    CHECK(verdict(PreScreen(preset, rs), "Response (Activity)") == "FAIL");
    rs[first + 3].birthSeconds.assign(10, 42);  // 44 to 42: within 5 %
    CHECK(verdict(PreScreen(preset, rs), "Response (Activity)") == "pass");
    for (size_t k = 0; k < 5; ++k) rs[first + k].birthSeconds.assign(10, 40);  // flat: a knob that adds no grains
    CHECK(verdict(PreScreen(preset, rs), "Response (Activity)") == "FAIL");
    const uint32_t falling[5] = {80, 70, 60, 50, 40};  // fewer grains as Activity rises: monotonic
    for (size_t k = 0; k < 5; ++k) rs[first + k].birthSeconds.assign(10, falling[k]);
    CHECK(verdict(PreScreen(preset, rs), "Response (Activity)") == "pass");
    const uint32_t none[5] = {0, 0, 0, 0, 0};  // a mode with no births at all
    for (size_t k = 0; k < 5; ++k) rs[first + k].birthSeconds.assign(10, none[k]);
    CHECK(verdict(PreScreen(preset, rs), "Response (Activity)") == "FAIL");
    // Load: births per second over the input's seconds, from S0's class renders.
    rs[0].metrics.span = kSignalFrames;
    rs[0].birthSeconds.assign(20, 0);
    for (size_t k = 0; k < 10; ++k) rs[0].birthSeconds[k] = 12;
    rs[0].births = 150;
    for (const Check& c : PreScreen(preset, rs)) {
      if (c.name == "Load") {
        CHECK(c.verdict == "info");
        CHECK(c.detail.find("plucks at the stored positions 12.0 births per second") != std::string::npos);
      }
    }
  }
  SECTION("combinations hold peak, tail and clicks") {
    Rendered soft = make("S0", "S0.engaged.soft_notes", Role::Engaged, -20.0, -6.0);
    soft.plan.vector   = Vector::SoftNotes;
    soft.metrics.click = 3.0;
    rs.push_back(soft);
    Rendered c = make("S11", "S11.corner-a1r1s1t1.plucks", Role::Combination, -18.0, 0.0);
    rs.push_back(c);
    Rendered k = make("S11", "S11.corner-a1r1s1t1.soft_notes", Role::Combination, -20.0, 2.0);  // not judged
    k.plan.vector   = Vector::SoftNotes;
    k.metrics.click = 12.0;
    rs.push_back(k);
    const size_t iC = rs.size() - 2, iK = rs.size() - 1;
    CHECK(verdict(PreScreen(preset, rs), "Combinations") == "pass");
    rs[iC].metrics.peakDbfs = 0.04;  // the review's case: +0.04 dBFS rounded to 0.0 passed
    rs[iC].metrics.overFull = 3;
    cs = PreScreen(preset, rs);
    CHECK(verdict(cs, "Combinations") == "FAIL");
    CHECK(verdict(cs, "Peak (moved)") == "FAIL");
    for (const Check& x : cs) {
      if (x.name == "Peak (moved)") CHECK(x.detail.find("0.04 dBFS, 3 samples over full scale") != std::string::npos);
    }
    rs[iC].metrics.peakDbfs   = -2.0;
    rs[iC].metrics.overFull   = 0;
    rs[iC].metrics.tailFinite = false;
    CHECK(verdict(PreScreen(preset, rs), "Combinations") == "FAIL");
    preset.declare.selfOscillating = true;
    CHECK(verdict(PreScreen(preset, rs), "Combinations") == "pass");
    rs[iK].metrics.click = 12.1;  // over 4x S0's 3.0 on SoftNotes
    cs = PreScreen(preset, rs);
    CHECK(verdict(cs, "Combinations") == "FAIL");
    for (const Check& x : cs) {
      if (x.name == "Combinations") CHECK(x.detail.find("S11.corner-a1r1s1t1.soft_notes (step 4.0x") != std::string::npos);
    }
    rs[iK].metrics.click   = 3.0;
    rs[iC].metrics.click   = 99.0;  // the class render's steps are not judged
    rs[iC].metrics.maxStep = 1.9;
    CHECK(verdict(PreScreen(preset, rs), "Combinations") == "pass");
  }
}

TEST_CASE("a suite writes its renders, recipes and index") {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "bsa-suite-test";
  std::filesystem::remove_all(dir);
  Preset preset = MakePreset(Package("macro_sweep.bsp"), "corpus.macro_sweep");
  SuiteOptions o;
  o.scripts = {"S0", "S8"};
  o.metrics = true;
  o.outDir  = dir.u8string();
  Renderer    renderer;
  SuiteResult r = RunSuite(renderer, preset, {&preset}, o);
  REQUIRE(r.ok);
  CHECK(r.renders.size() == 9u + 2u + 1u);
  const std::filesystem::path pd = dir / "corpus.macro_sweep";
  const Rendered* e = nullptr;
  for (const Rendered& x : r.renders) {
    if (x.plan.name == "S0.engaged.plucks") e = &x;
    if (x.plan.role == Role::Determinism) {
      CHECK(x.wavPath.empty());
      CHECK_FALSE(std::filesystem::exists(pd / (x.plan.name + ".json")));
    }
  }
  REQUIRE(e != nullptr);
  CHECK(e->exact);
  CHECK(std::filesystem::file_size(pd / "S0.engaged.plucks.wav") == 44u + (kSignalFrames + kTailFrames) * 4u);
  const std::string recipe = ReadAll(pd / "S0.engaged.plucks.json");
  CHECK(recipe.find("\"format\": \"brainscape-audition/2\"") != std::string::npos);
  CHECK(recipe.find("\"outputSha256\": \"" + e->hashes.whole + "\"") != std::string::npos);
  CHECK(recipe.find("\"id\": \"corpus.macro_sweep\"") != std::string::npos);
  CHECK(recipe.find("\"exact\": true") != std::string::npos);
  const std::string wavBytes = ReadAll(pd / "S0.engaged.plucks.wav");
  CHECK(recipe.find(Sha256Hex(wavBytes.data(), wavBytes.size())) != std::string::npos);
  const std::string index = ReadAll(pd / "audition.json");
  CHECK(index.find("\"format\": \"brainscape-audition-index/1\"") != std::string::npos);
  CHECK(index.find("\"S8\"") != std::string::npos);
  CHECK(std::filesystem::exists(pd / "prescreen.txt"));
  // The 16-bit WAV is the float render, rounded.
  Stereo   wav;
  uint32_t rate;
  uint16_t bits;
  REQUIRE(ReadWav(std::vector<uint8_t>(wavBytes.begin(), wavBytes.end()), &wav, &rate, &bits));
  RenderRequest rq;
  rq.preset = preset.state.get();
  const Input in = VectorInput(Vector::Plucks);
  rq.input = &in.audio;
  RenderResult again;
  REQUIRE(renderer.Render(rq, &again));
  CHECK(HashRender(again.out).whole == e->hashes.whole);
  bool rounded = true;
  for (size_t i = 0; i < wav.Frames(); i += 97) {
    rounded = rounded && wav.l[i] == static_cast<float>(ToPcm16(again.out.l[i])) / 32768.f;
  }
  CHECK(rounded);
  bool sawDeterminism = false;
  for (const Check& c : r.checks) {
    if (c.name == "Determinism") {
      sawDeterminism = true;
      CHECK(c.verdict == "pass");
    }
  }
  CHECK(sawDeterminism);
  // Without WAVs the hashes are the same.
  o.wav = false;
  o.outDir = (dir / "nowav").u8string();
  SuiteResult q = RunSuite(renderer, preset, {&preset}, o);
  REQUIRE(q.renders.size() == r.renders.size());
  for (size_t i = 0; i < q.renders.size(); ++i) CHECK(q.renders[i].hashes.whole == r.renders[i].hashes.whole);
  CHECK_FALSE(std::filesystem::exists(dir / "nowav" / "corpus.macro_sweep" / "S0.engaged.plucks.wav"));
  std::filesystem::remove_all(dir);
}

TEST_CASE("a tail the render does not see end is measured on a probe") {
  // The review's case: feedback.amount over 1 sustains (or grows) for as long as it is heard,
  // and the S0 render's 10 s of silence cannot show it; the probe's 60 s and its fit call it
  // unending, and the Tail check fails.
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "bsa-tail-test";
  std::filesystem::remove_all(dir);
  // The review's seed `review.grow` on this package's mode: the grain feedback over 1 into a
  // short post delay at 0.9.
  auto grow = std::make_shared<PresetState>(*Package("macro_sweep.bsp"));
  for (const auto& leaf : std::vector<std::pair<ParamId, float>>{{ParamId::Feedback, 1.1f},
                                                                 {ParamId::DelayFb, 0.9f},
                                                                 {ParamId::DelayMix, 1.f},
                                                                 {ParamId::DelayTimeMs, 40.f},
                                                                 {ParamId::Overlap, 0.f},
                                                                 {ParamId::SprayMs, 0.f},
                                                                 {ParamId::WindowSustain, 0.25f},
                                                                 {ParamId::WindowSmooth, 1.f},
                                                                 {ParamId::Mix, 0.35f},
                                                                 {ParamId::Jitter, 0.f},
                                                                 {ParamId::GrainSizeMs, 100.f},
                                                                 {ParamId::PanSpread, 0.f},
                                                                 {ParamId::ModRateHz, 0.6f},
                                                                 {ParamId::ModDepth, 0.05f},
                                                                 {ParamId::ReverbTime, 0.4f},
                                                                 {ParamId::ReverbMix, 0.12f}}) {
    SetLeaf(grow.get(), leaf.first, leaf.second);
  }
  Preset       preset = MakePreset(grow, "corpus.grow");
  SuiteOptions o;
  o.scripts = {"S0"};
  o.metrics = true;
  o.wav     = false;
  o.outDir  = dir.u8string();
  Renderer    renderer;
  SuiteResult r = RunSuite(renderer, preset, {&preset}, o);
  REQUIRE(r.ok);
  auto find = [](const SuiteResult& x, const char* name) -> const Rendered* {
    for (const Rendered& y : x.renders) {
      if (y.plan.name == name) return &y;
    }
    return nullptr;
  };
  auto verdict = [](const SuiteResult& x, const char* name) {
    for (const Check& c : x.checks) {
      if (c.name == name) return c.verdict;
    }
    return std::string("missing");
  };
  const Rendered* e = find(r, "S0.engaged.plucks");
  REQUIRE(e != nullptr);
  CHECK_FALSE(e->metrics.tailFinite);
  CHECK(e->metrics.tailProbeSeconds == Approx(60.0));
  CHECK(e->metrics.tailSeconds > 59.0);
  CHECK(e->metrics.tailDecayDbPerS < kMinDecayDbPerS);
  CHECK(verdict(r, "Tail") == "FAIL");
  // The probe measures; it is not one of the renders, so the hashes are the render's own.
  const Rendered* silence = find(r, "S0.engaged.silence");
  REQUIRE(silence != nullptr);
  CHECK(silence->metrics.tailProbeSeconds == 0.0);  // no tail judged on Silence: no probe
  RenderRequest rq;
  rq.preset      = grow.get();
  const Input in = VectorInput(Vector::Plucks);
  rq.input       = &in.audio;
  RenderResult again;
  REQUIRE(renderer.Render(rq, &again));
  CHECK(HashRender(again.out).whole == e->hashes.whole);
  // Declared self-oscillating, the same tail passes.
  preset.declare.selfOscillating = true;
  CHECK(verdict(RunSuite(renderer, preset, {&preset}, o), "Tail") == "pass");
  // Without the feedback the tail ends inside the render: no probe.
  SetLeaf(grow.get(), ParamId::Feedback, 0.f);
  SetLeaf(grow.get(), ParamId::DelayFb, 0.3f);
  preset.declare.selfOscillating = false;
  const SuiteResult q = RunSuite(renderer, preset, {&preset}, o);
  const Rendered*   f = find(q, "S0.engaged.plucks");
  REQUIRE(f != nullptr);
  CHECK(f->metrics.tailFinite);
  CHECK(f->metrics.tailProbeSeconds == 0.0);
  CHECK(verdict(q, "Tail") == "pass");
  std::filesystem::remove_all(dir);
}

TEST_CASE("a cancelled render and suite stop") {
  const auto        state = Package("macro_sweep.bsp");
  std::atomic<bool> cancel{true};
  Renderer          renderer;
  const Input       in = VectorInput(Vector::Plucks);
  RenderRequest     rq;
  rq.preset = state.get();
  rq.input  = &in.audio;
  rq.cancel = &cancel;
  RenderResult r;
  CHECK_FALSE(renderer.Render(rq, &r));
  CHECK(r.error == "cancelled");
  cancel = false;
  REQUIRE(renderer.Render(rq, &r));  // a flag never set changes nothing
  rq.cancel = nullptr;
  RenderResult plain;
  REQUIRE(renderer.Render(rq, &plain));
  CHECK(HashRender(plain.out).whole == HashRender(r.out).whole);
  // A suite cancelled from another thread returns at once, cancelled, and writes no index.
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "bsa-cancel-test";
  std::filesystem::remove_all(dir);
  Preset       preset = MakePreset(state, "corpus.cancel");
  SuiteOptions o;
  for (const char* s : kScriptNames) o.scripts.push_back(s);
  o.metrics = true;
  o.wav     = false;
  o.outDir  = dir.u8string();
  o.cancel  = &cancel;
  std::thread stopper([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    cancel = true;
  });
  const auto        start = std::chrono::steady_clock::now();
  const SuiteResult s     = RunSuite(renderer, preset, {&preset}, o);
  const double      took  = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  stopper.join();
  CHECK(s.cancelled);
  CHECK_FALSE(s.ok);
  CHECK(took < 2.0);
  CHECK_FALSE(std::filesystem::exists(dir / "corpus.cancel" / "audition.json"));
  std::filesystem::remove_all(dir);
}

TEST_CASE("the JSON writer") {
  JsonWriter w;
  w.BeginObject().Key("a").String("q\"\\\n\x01").Key("b").BeginArray().Int(-3).Uint(7).EndArray();
  w.Key("c").Double(-0.0001, 2).Key("d").Float(100.f).Key("e").Float(0.35f).Key("f").Null();
  w.Key("g").BeginObject().EndObject().Key("h").Bool(true).EndObject();
  CHECK(w.Text() ==
        "{\n  \"a\": \"q\\\"\\\\\\n\\u0001\",\n  \"b\": [\n    -3,\n    7\n  ],\n  \"c\": 0.00,\n"
        "  \"d\": 100,\n  \"e\": 0.35,\n  \"f\": null,\n  \"g\": {},\n  \"h\": true\n}\n");
  CHECK(FloatText(1e-9f) == "1e-09");
  CHECK(FloatText(20000.f) == "20000");
  CHECK(BitsHex(1.f) == "3f800000");
}
