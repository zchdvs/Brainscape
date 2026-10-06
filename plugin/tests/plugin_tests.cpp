// Headless checks of the wrapper obligations (docs/design/companion-app.md §4.12, §5):
// the processor is driven the way hosts drive it and compared bit for bit with the engine
// driven directly from the exact-restart state in 48-frame blocks (the pedal's grid), its
// events stamped at their frames.
#define CATCH_CONFIG_RUNNER
#include "catch.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "Audition.h"
#include "PlainAttachment.h"
#include "PluginProcessor.h"
#include "StateCodec.h"
#include "brainscape/Engine.h"
#include "brainscape/HostArenas.h"
#include "TestSupport.h"
#include "brainscape/InputCondition.h"
#include "brainscape/ParamDisplay.h"
#include "brainscape/SoundRevision.h"
#include "golden/Sha256.h"

#if defined(_MSC_VER)
#include <crtdbg.h>
#include <malloc.h>
#endif

// ── Allocation audit ──────────────────────────────────────────────────────────────────
// Every global operator new (and, in MSVC Debug builds, every CRT heap call) made on
// this thread while tAudit is set is counted; processBlock runs with it set.

namespace {
thread_local bool     tAudit = false;
std::atomic<uint64_t> gAuditHits{0};

void NoteAlloc() noexcept {
  if (tAudit) gAuditHits.fetch_add(1u, std::memory_order_relaxed);
}

void* Alloc(std::size_t n) noexcept {
  NoteAlloc();
  return std::malloc(n != 0 ? n : 1);
}

void* AlignedAlloc(std::size_t n, std::size_t align) noexcept {
  NoteAlloc();
#if defined(_MSC_VER)
  return _aligned_malloc(n != 0 ? n : 1, align);
#else
  void* p = nullptr;
  return posix_memalign(&p, align < sizeof(void*) ? sizeof(void*) : align, n != 0 ? n : 1) == 0 ? p : nullptr;
#endif
}

void AlignedFree(void* p) noexcept {
#if defined(_MSC_VER)
  _aligned_free(p);
#else
  std::free(p);
#endif
}

void* OrThrow(void* p) {
  if (p == nullptr) throw std::bad_alloc();
  return p;
}

#if defined(_MSC_VER) && defined(_DEBUG)
int CrtAllocHook(int type, void*, size_t, int, long, const unsigned char*, int) {
  if (type == _HOOK_ALLOC || type == _HOOK_REALLOC) NoteAlloc();
  return 1;
}
#endif
}  // namespace

void* operator new(std::size_t n) { return OrThrow(Alloc(n)); }
void* operator new[](std::size_t n) { return OrThrow(Alloc(n)); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return Alloc(n); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return Alloc(n); }
void  operator delete(void* p) noexcept { std::free(p); }
void  operator delete[](void* p) noexcept { std::free(p); }
void  operator delete(void* p, std::size_t) noexcept { std::free(p); }
void  operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void  operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void  operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
void* operator new(std::size_t n, std::align_val_t a) { return OrThrow(AlignedAlloc(n, static_cast<std::size_t>(a))); }
void* operator new[](std::size_t n, std::align_val_t a) { return OrThrow(AlignedAlloc(n, static_cast<std::size_t>(a))); }
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
  return AlignedAlloc(n, static_cast<std::size_t>(a));
}
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
  return AlignedAlloc(n, static_cast<std::size_t>(a));
}
void operator delete(void* p, std::align_val_t) noexcept { AlignedFree(p); }
void operator delete[](void* p, std::align_val_t) noexcept { AlignedFree(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { AlignedFree(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { AlignedFree(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { AlignedFree(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { AlignedFree(p); }

using namespace brainscape;
using namespace brainscape::plugin;
using namespace brainscape::testing;

namespace {

constexpr double kRate = 48000.0;

bool AllFinite(const std::vector<float>& v) {
  for (float x : v) {
    if (!std::isfinite(x)) return false;
  }
  return true;
}

double Energy(const std::vector<float>& v) {
  double e = 0.0;
  for (float x : v) e += static_cast<double>(x) * x;
  return e;
}

// ── The processor, driven as a host drives it ─────────────────────────────────────────

struct HostSetup {
  int       numIn  = 2;  // 0 = disabled input bus
  int       numOut = 2;
  double    rate   = kRate;
  InputMode mode   = InputMode::Stereo;
};

std::unique_ptr<BrainscapeProcessor> MakeProcessor(const Preset& preset, const HostSetup& h,
                                                   int maxBlock = 512) {
  auto p = std::make_unique<BrainscapeProcessor>();
  juce::AudioProcessor::BusesLayout layout;
  layout.inputBuses.add(h.numIn == 0   ? juce::AudioChannelSet::disabled()
                        : h.numIn == 1 ? juce::AudioChannelSet::mono()
                                       : juce::AudioChannelSet::stereo());
  layout.outputBuses.add(h.numOut == 1 ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo());
  REQUIRE(p->setBusesLayout(layout));
  for (const auto& v : preset) p->Param(v.first).SetPlainNotifyingHost(v.second);
  WrapperSettings s;
  s.inputMode = h.mode;
  p->SetSettings(s);
  p->setRateAndBufferSizeDetails(h.rate, maxBlock);
  p->prepareToPlay(h.rate, maxBlock);
  return p;
}

struct HostRender {
  std::vector<int>              pattern{512};
  std::vector<int>              noteOns;      // absolute frames
  std::function<void(int)>      beforeBlock;  // called with the block's first frame
};

// In place, as JUCE hosts process: channel c of the buffer is both input c and output c.
// With a mono input and stereo output, channel 1 starts as a sentinel the processor must
// not read as input.
Stereo RenderProcessor(BrainscapeProcessor& p, const Stereo& in, const HostSetup& h,
                       const HostRender& r = {}) {
  const int frames = static_cast<int>(in.l.size());
  Stereo    io;
  io.l = h.numIn == 0 ? std::vector<float>(in.l.size(), 0.25f) : in.l;
  io.r = h.numIn >= 2 ? in.r : std::vector<float>(in.l.size(), 1000.0f);
  const int numCh = std::max({h.numIn, h.numOut, 1});
  juce::MidiBuffer midi;
  midi.ensureSize(4096);
  size_t   k = 0, ni = 0;
  int      pos  = 0;
  uint64_t hits = gAuditHits.load();
  while (pos < frames) {
    const int want = r.pattern[k++ % r.pattern.size()];
    const int n    = std::min(want, frames - pos);  // never past the end
    if (r.beforeBlock) r.beforeBlock(pos);
    float*                   chans[2] = {io.l.data() + pos, io.r.data() + pos};
    juce::AudioBuffer<float> buffer(chans, numCh, n);
    midi.clear();
    for (; ni < r.noteOns.size() && r.noteOns[ni] < pos + n; ++ni) {
      midi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), r.noteOns[ni] - pos);
    }
    tAudit = true;
    p.processBlock(buffer, midi);
    tAudit = false;
    pos += n;
  }
  CHECK(gAuditHits.load() - hits == 0u);  // processBlock never allocated
  if (h.numOut == 1) io.r = io.l;
  return io;
}

// Process calls a wrapper that never splits a host block itself makes: one per chunk of at
// most 512 frames, none for a zero-frame block.
uint64_t ChunkCalls(const std::vector<int>& pattern, int frames) {
  uint64_t calls = 0;
  for (size_t k = 0, pos = 0; static_cast<int>(pos) < frames; ++k) {
    const int n = std::min(pattern[k % pattern.size()], frames - static_cast<int>(pos));
    calls += static_cast<uint64_t>((n + 511) / 512);
    pos += static_cast<size_t>(n);
  }
  return calls;
}

// A host transport the tests start and stop.
class TestPlayHead final : public juce::AudioPlayHead {
 public:
  bool playing = false;
  juce::Optional<PositionInfo> getPosition() const override {
    PositionInfo info;
    info.setIsPlaying(playing);
    return info;
  }
};

Stereo Slice(const Stereo& s, int from, int to) {
  return {std::vector<float>(s.l.begin() + from, s.l.begin() + to),
          std::vector<float>(s.r.begin() + from, s.r.begin() + to)};
}

Stereo Join(const std::vector<Stereo>& parts) {
  Stereo all;
  for (const Stereo& p : parts) {
    all.l.insert(all.l.end(), p.l.begin(), p.l.end());
    all.r.insert(all.r.end(), p.r.begin(), p.r.end());
  }
  return all;
}

// Waits for the spare engine's worker to have a spare restarted with the current preset.
bool WaitForSpare(BrainscapeProcessor& p) {
  for (int i = 0; i < 500; ++i) {
    if (p.GetStatus().spareReady) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

void RequireSame(const Stereo& got, const Stereo& want, const char* what) {
  INFO(what << ": first differing frame L " << FirstDiff(got.l, want.l) << ", R "
            << FirstDiff(got.r, want.r));
  REQUIRE(SameBits(got.l, want.l));
  REQUIRE(SameBits(got.r, want.r));
}

std::string PatternName(const std::vector<int>& p) {
  std::string s = "{";
  for (size_t i = 0; i < p.size(); ++i) s += (i ? "," : "") + std::to_string(p[i]);
  return s + "}";
}

// Text a user could type for exactly this plain value, in the display's units.
std::string ExactText(ParamId id, float plain) {
  const DisplayKind kind = FindParamDisplay(id)->kind;
  char              buf[64];
  if (kind == DisplayKind::Balance) {  // (p - 0.5) * 200 %; 17 digits pin p for p >= 2^-24
    std::snprintf(buf, sizeof buf, "%.17g%%", (static_cast<double>(plain) - 0.5) * 200.0);
    return buf;
  }
  std::snprintf(buf, sizeof buf, "%.8e", static_cast<double>(plain));  // 9 digits: round-trips
  std::string s(buf);
  const size_t e        = s.find('e');
  const int    exponent = std::atoi(s.c_str() + e + 1);
  const bool   percent  = kind == DisplayKind::Percent;
  const bool   hundreds = percent || kind == DisplayKind::Amount;
  return s.substr(0, e) + "e" + std::to_string(exponent + (hundreds ? 2 : 0)) + (percent ? "%" : "");
}

std::vector<float> SampleValues(ParamId id, uint32_t seed, int count) {
  const ParamDescriptor* d = FindParam(id);
  std::vector<float>     v = {d->min, d->max, d->def, 7.02f, 0.4f, 0.55f, 1234.5f, -3.3f, 0.1f};
  for (int i = 0; i < count; ++i) {
    const double u = (Xorshift(seed) & 0xFFFFFFu) / 16777216.0;
    float        x = static_cast<float>(d->min + (static_cast<double>(d->max) - d->min) * u);
    const int    nudge = static_cast<int>(Xorshift(seed) % 7u) - 3;  // a few ULPs either way
    uint32_t     bits  = Bits(x);
    if (x > 0.f) bits = static_cast<uint32_t>(static_cast<int64_t>(bits) + nudge);
    std::memcpy(&x, &bits, sizeof x);
    v.push_back(x);
  }
  return v;
}

}  // namespace

// ── Tests ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("the allocation audit sees allocations") {  // so its zero counts mean something
  const uint64_t before = gAuditHits.load();
  tAudit                = true;
  auto* p               = new int(7);
  std::vector<float> v(64);
  juce::String       s("grain");
  s << 1234567.0;
  tAudit = false;
  delete p;
  REQUIRE(gAuditHits.load() - before >= 3u);
}

TEST_CASE("host blocks of any size reproduce the 48-frame engine reference bit for bit") {
  const Stereo in  = MakeInput(2 * 48000);
  const Stereo ref = RenderReference(Busy(), in);
  REQUIRE(AllFinite(ref.l));
  REQUIRE(AllFinite(ref.r));
  REQUIRE(Energy(ref.l) > 1.0);
  std::printf("reference (engine, 48-frame blocks): %016llx\n", static_cast<unsigned long long>(Hash(ref)));

  const std::vector<std::vector<int>> patterns = {
      {1}, {7}, {512}, {513}, {4096}, {0, 480}, {0, 0, 1, 0, 7, 512, 513, 0, 4096, 37, 0, 441}};
  for (const auto& pattern : patterns) {
    auto         proc = MakeProcessor(Busy(), {});
    HostRender   r;
    r.pattern         = pattern;
    const Stereo got  = RenderProcessor(*proc, in, {}, r);
    std::printf("processor, host blocks %-40s %016llx\n", PatternName(pattern).c_str(),
                static_cast<unsigned long long>(Hash(got)));
    REQUIRE(AllFinite(got.l));
    REQUIRE(AllFinite(got.r));
    RequireSame(got, ref, PatternName(pattern).c_str());
    REQUIRE(proc->GetStatus().engineCalls == ChunkCalls(pattern, 2 * 48000));
  }
}

TEST_CASE("mono and stereo layouts process in place") {
  const Stereo in = MakeInput(48000);
  const Stereo monoIn{in.l, in.l};
  const Stereo refMono = RenderReference(Busy(), monoIn);

  SECTION("stereo bus, mono input mode: R copies L") {
    HostSetup h;
    h.mode    = InputMode::Mono;
    auto proc = MakeProcessor(Busy(), h);
    RequireSame(RenderProcessor(*proc, in, h, {{480}}), refMono, "stereo bus, mono mode");
  }
  SECTION("mono in, stereo out: the second channel's stale contents are never input") {
    HostSetup h;
    h.numIn   = 1;
    auto proc = MakeProcessor(Busy(), h);
    RequireSame(RenderProcessor(*proc, in, h, {{441}}), refMono, "mono -> stereo");
  }
  SECTION("mono in, mono out") {
    HostSetup h;
    h.numIn = h.numOut = 1;
    auto         proc  = MakeProcessor(Busy(), h);
    const Stereo got   = RenderProcessor(*proc, in, h, {{256}});
    REQUIRE(SameBits(got.l, refMono.l));
  }
  SECTION("disabled input bus feeds zeros, never a null pointer") {
    HostSetup h;
    h.numIn   = 0;
    auto proc = MakeProcessor(Busy(), h);
    REQUIRE(proc->getTotalNumInputChannels() == 0);
    const Stereo silence{std::vector<float>(in.l.size(), 0.f), std::vector<float>(in.l.size(), 0.f)};
    RequireSame(RenderProcessor(*proc, in, h, {{512}}), RenderReference(Busy(), silence),
                "disabled input");
  }
}

TEST_CASE("non-finite live input is sanitized to +0 and nothing else changes") {
  Stereo in    = MakeInput(48000);
  Stereo clean = in;
  for (size_t n : {100u, 5000u, 20000u, 20001u, 47999u}) {
    in.l[n]    = std::numeric_limits<float>::quiet_NaN();
    in.r[n]    = n % 2 ? std::numeric_limits<float>::infinity() : -std::numeric_limits<float>::infinity();
    clean.l[n] = 0.f;
    clean.r[n] = 0.f;
  }
  auto         proc = MakeProcessor(Busy(), {});
  const Stereo got  = RenderProcessor(*proc, in, {}, {{333}});
  REQUIRE(AllFinite(got.l));
  REQUIRE(AllFinite(got.r));
  RequireSame(got, RenderReference(Busy(), clean), "sanitized input");
}

TEST_CASE("prepareToPlay at 44.1, 48 and 96 kHz") {
  for (double rate : {44100.0, 48000.0, 96000.0}) {
    INFO("host rate " << rate);
    HostSetup h;
    h.rate      = rate;
    auto   proc = MakeProcessor(Busy(), h, 1024);
    auto   st   = proc->GetStatus();
    REQUIRE(st.engineReady);
    REQUIRE(st.hostRate == rate);
    REQUIRE(st.engineRate == rate);  // native at the host rate until the resampled mode lands
    REQUIRE(st.pedalRate == (rate == 48000.0));
    REQUIRE(proc->getLatencySamples() == 0);
    const Stereo in  = MakeInput(static_cast<int>(rate));
    const Stereo got = RenderProcessor(*proc, in, h, {{static_cast<int>(rate / 100.0)}});
    REQUIRE(AllFinite(got.l));
    REQUIRE(Energy(got.l) > 1.0);
    RequireSame(got, RenderReference(Busy(), in, rate), "native-rate engine");
  }
}

TEST_CASE("a rate change re-initialises; a same-rate re-prepare keeps the running engine") {
  {
    const Stereo in = MakeInput(96000);
    const Stereo first{std::vector<float>(in.l.begin(), in.l.begin() + 48000),
                       std::vector<float>(in.r.begin(), in.r.begin() + 48000)};
    const Stereo second{std::vector<float>(in.l.begin() + 48000, in.l.end()),
                        std::vector<float>(in.r.begin() + 48000, in.r.end())};
    HostSetup h96;
    h96.rate  = 96000.0;
    auto proc = MakeProcessor(Busy(), h96);
    RenderProcessor(*proc, first, h96);
    proc->prepareToPlay(kRate, 512);  // back to the pedal's rate: a fresh engine
    REQUIRE(proc->GetStatus().pedalRate);
    const Stereo a = RenderProcessor(*proc, first, {});
    proc->prepareToPlay(kRate, 256);  // same rate: ring and engine state kept (§4.1)
    const Stereo b = RenderProcessor(*proc, second, {});
    const Stereo ref = RenderReference(Busy(), in);
    Stereo joined = a;
    joined.l.insert(joined.l.end(), b.l.begin(), b.l.end());
    joined.r.insert(joined.r.end(), b.r.begin(), b.r.end());
    RequireSame(joined, ref, "re-prepared");
  }
}

TEST_CASE("plain values round-trip exactly through BrainscapeParam") {
  BrainscapeProcessor proc;
  for (const ParamDescriptor& d : kParamTable) {
    INFO(d.name);
    BrainscapeParam& p = proc.Param(d.id);
    for (float v : SampleValues(d.id, static_cast<uint32_t>(d.id) * 7919u, 2000)) {
      const float want = Canonicalize(d.id, v);
      p.SetPlainNotifyingHost(v);
      REQUIRE(Bits(p.Plain()) == Bits(want));
      p.setValue(p.getValue());  // a host echoing the normalised view it read
      REQUIRE(Bits(p.Plain()) == Bits(want));
      p.setValueNotifyingHost(p.getValue());
      REQUIRE(Bits(p.Plain()) == Bits(want));
      REQUIRE(p.getCurrentValueAsText() == FormatPlainText(d.id, want));
    }
    p.SetPlainNotifyingHost(std::numeric_limits<float>::quiet_NaN());
    REQUIRE(p.Plain() == d.min);
    p.SetPlainNotifyingHost(-0.0f);
    REQUIRE(Bits(p.Plain()) == Bits(Canonicalize(d.id, 0.0f)));
    p.setValue(0.3f);  // a lossy host set maps through the shared taper
    REQUIRE(Bits(p.Plain()) == Bits(PlainFromNormalized(d.id, 0.3f)));
  }
  REQUIRE(proc.getParameters().size() == static_cast<int>(kNumParams) + 1);  // + freeze
  REQUIRE(proc.Param(ParamId::DelayMs).getParameterID() == "layer0.position.base_ms");
  REQUIRE(proc.Param(ParamId::OnsetTrigger).isDiscrete());
  REQUIRE(proc.Param(ParamId::OnsetTrigger).getNumSteps() == 2);
}

TEST_CASE("edits after prepare reach the engine as exact bits at the next block") {
  // The two values the record's APVTS probe changed (companion-app-record §2.5).
  const int    change = 24000;
  const Stereo in     = MakeInput(48000);
  auto         proc   = MakeProcessor(Busy(), {});
  HostRender   r;
  r.pattern     = {480};
  r.beforeBlock = [&](int pos) {
    if (pos != change) return;
    proc->Param(ParamId::PitchSt).SetPlainNotifyingHost(7.02f);
    proc->Param(ParamId::FilterMorph).SetPlainNotifyingHost(0.4f);
  };
  const Stereo got = RenderProcessor(*proc, in, {}, r);
  const auto   at  = [&](float pitch, float morph) {
    return std::vector<RefEvent>{RefParam(change, ParamId::PitchSt, pitch),
                                 RefParam(change, ParamId::FilterMorph, morph)};
  };
  RequireSame(got, RenderReference(Busy(), in, kRate, at(7.02f, 0.4f)), "UI edits");
  // One ULP of one value changes the output, so exactness is not vacuous here.
  const Stereo ulp = RenderReference(Busy(), in, kRate, at(7.02f, std::nextafter(0.4f, 1.0f)));
  REQUIRE_FALSE(SameBits(got.l, ulp.l));
}

TEST_CASE("the plain slider attachment edits and shows exact values") {
  BrainscapeProcessor proc;
  juce::Slider        pitchKnob, morphKnob, mixKnob, delayKnob, cutoffKnob, posKnob;
  BrainscapePlainAttachment pitch(proc.Param(ParamId::PitchSt), pitchKnob);
  BrainscapePlainAttachment morph(proc.Param(ParamId::FilterMorph), morphKnob);
  BrainscapePlainAttachment mix(proc.Param(ParamId::Mix), mixKnob);
  BrainscapePlainAttachment delay(proc.Param(ParamId::DelayMs), delayKnob);
  BrainscapePlainAttachment cutoff(proc.Param(ParamId::FilterCutoffHz), cutoffKnob);
  BrainscapePlainAttachment pos(proc.Param(ParamId::PositionSource), posKnob);

  pitchKnob.setValue(0.6, juce::sendNotificationSync);  // a knob turn: the pot path
  REQUIRE(Bits(proc.Param(ParamId::PitchSt).Plain()) == Bits(PlainFromNormalized(ParamId::PitchSt, 0.6f)));

  REQUIRE(pitch.CommitText("7.02"));
  REQUIRE(Bits(proc.Param(ParamId::PitchSt).Plain()) == Bits(7.02f));
  REQUIRE(pitch.CommitText("-0.37 st"));
  REQUIRE(Bits(proc.Param(ParamId::PitchSt).Plain()) == Bits(-0.37f));
  REQUIRE(morph.CommitText("0.4"));
  REQUIRE(Bits(proc.Param(ParamId::FilterMorph).Plain()) == Bits(0.4f));
  REQUIRE(morph.CommitText("hp"));
  REQUIRE(proc.Param(ParamId::FilterMorph).Plain() == 2.0f);
  REQUIRE(mix.CommitText("55.55%"));
  REQUIRE(Bits(proc.Param(ParamId::Mix).Plain()) == Bits(0.5555f));
  REQUIRE(delay.CommitText("1.2 s"));
  REQUIRE(proc.Param(ParamId::DelayMs).Plain() == 1200.0f);
  REQUIRE(delay.CommitText("250ms"));
  REQUIRE(proc.Param(ParamId::DelayMs).Plain() == 250.0f);
  REQUIRE(cutoff.CommitText("2.5k"));
  REQUIRE(proc.Param(ParamId::FilterCutoffHz).Plain() == 2500.0f);
  REQUIRE(cutoff.CommitText("Off"));
  REQUIRE(proc.Param(ParamId::FilterCutoffHz).Plain() == 20000.0f);
  REQUIRE(pos.CommitText("Mark"));
  REQUIRE(proc.Param(ParamId::PositionSource).Plain() == 1.0f);
  REQUIRE_FALSE(delay.CommitText("fast"));
  REQUIRE_FALSE(delay.CommitText("12 st"));
  REQUIRE(proc.Param(ParamId::DelayMs).Plain() == 250.0f);

  // A centred balance (Window skew) reads -100..+100 %; reverb time reads 0-100.
  juce::Slider              skewKnob, reverbKnob;
  BrainscapePlainAttachment skew(proc.Param(ParamId::WindowSkew), skewKnob);
  BrainscapePlainAttachment reverb(proc.Param(ParamId::ReverbTime), reverbKnob);
  const auto skewAfter = [&](const char* text) {
    REQUIRE(skew.CommitText(text));
    return proc.Param(ParamId::WindowSkew).Plain();
  };
  REQUIRE(skewAfter("-50%") == 0.25f);
  REQUIRE(skewAfter("+25") == 0.625f);
  REQUIRE(skewAfter("0") == 0.5f);
  REQUIRE(skewAfter("-100") == 0.0f);
  REQUIRE(skewAfter("100%") == 1.0f);
  REQUIRE(skewAfter("-37.5%") == 0.3125f);
  REQUIRE(Bits(skewAfter("-40%")) == Bits(0.3f));
  REQUIRE(Bits(skewAfter("33.3333%")) == Bits(0.6666665f));
  REQUIRE(skewAfter("1e-20%") == 0.5f);
  REQUIRE(skewAfter("-1e3") == 0.0f);
  REQUIRE(skewAfter("250%") == 1.0f);
  REQUIRE(skew.DisplayText() == "+100%");
  REQUIRE_FALSE(skew.CommitText("12 ms"));
  REQUIRE(reverb.CommitText("60"));
  REQUIRE(Bits(proc.Param(ParamId::ReverbTime).Plain()) == Bits(0.6f));
  REQUIRE(reverb.DisplayText() == "60");
  // The balance's decimal arithmetic against an independent one: b = M * 1e-6 % is plain
  // (1e8 + M) * 5e-9, parsed by the plain-unit path of another parameter.
  uint32_t seed = 0x5EEDu;
  for (int i = 0; i < 4000; ++i) {
    const int64_t m = static_cast<int64_t>(Xorshift(seed) % 200000001u) - 100000000;
    const int64_t a = m < 0 ? -m : m;
    char          typed[48], exact[48];
    std::snprintf(typed, sizeof typed, "%s%lld.%06lld%%", m < 0 ? "-" : "", static_cast<long long>(a / 1000000),
                  static_cast<long long>(a % 1000000));
    std::snprintf(exact, sizeof exact, "%llde-9", static_cast<long long>((100000000 + m) * 5));
    float want = 0.f;
    REQUIRE(ParsePlainText(ParamId::FilterMorph, exact, want));  // no unit shift, range 0..3
    INFO(typed);
    REQUIRE(Bits(skewAfter(typed)) == Bits(want));
  }

  // Every value typed as text lands on exactly that binary32, in every parameter's units.
  for (const ParamDescriptor& d : kParamTable) {
    if (FindParamDisplay(d.id)->steps >= 2) continue;
    INFO(d.name);
    juce::Slider              knob;
    BrainscapePlainAttachment att(proc.Param(d.id), knob);
    for (float v : SampleValues(d.id, static_cast<uint32_t>(d.id) * 104729u, 300)) {
      const float       want = Canonicalize(d.id, v);
      const std::string text = ExactText(d.id, want);
      INFO(text);
      REQUIRE(att.CommitText(text));
      REQUIRE(Bits(proc.Param(d.id).Plain()) == Bits(want));
      REQUIRE(att.DisplayText() == FormatPlainText(d.id, want));
    }
    att.ResetToDefault();
    REQUIRE(Bits(proc.Param(d.id).Plain()) == Bits(Canonicalize(d.id, d.def)));
  }

  // A host-side change shows up on the next refresh, and the knob follows the taper.
  proc.Param(ParamId::DelayMs).setValueNotifyingHost(0.25f);
  REQUIRE(delay.Refresh());
  REQUIRE(static_cast<float>(delayKnob.getValue()) ==
          NormalizedFromPlain(ParamId::DelayMs, proc.Param(ParamId::DelayMs).Plain()));
  REQUIRE_FALSE(delay.Refresh());
}

TEST_CASE("session state round-trips bit for bit") {
  BrainscapeProcessor a;
  uint32_t            seed = 42u;
  for (const ParamDescriptor& d : kParamTable) {
    const auto values = SampleValues(d.id, seed += 977u, 1);
    a.Param(d.id).SetPlainNotifyingHost(values.back());
  }
  a.Param(ParamId::PitchSt).SetPlainNotifyingHost(7.02f);
  WrapperSettings s;
  s.inputMode      = InputMode::Stereo;
  s.inputGainDb    = -3.5f;
  s.outputGainDb   = 2.25f;
  s.restartOnStart = true;
  a.SetSettings(s);
  a.Freeze().setValueNotifyingHost(1.0f);
  juce::MemoryBlock blob;
  a.getStateInformation(blob);

  BrainscapeProcessor b;
  b.Freeze().setValueNotifyingHost(1.0f);
  b.setStateInformation(blob.getData(), static_cast<int>(blob.getSize()));
  for (const ParamDescriptor& d : kParamTable) {
    INFO(d.name);
    REQUIRE(Bits(b.Param(d.id).Plain()) == Bits(a.Param(d.id).Plain()));
  }
  REQUIRE(b.GetSettings().inputMode == InputMode::Stereo);
  REQUIRE(Bits(b.GetSettings().inputGainDb) == Bits(-3.5f));
  REQUIRE(Bits(b.GetSettings().outputGainDb) == Bits(2.25f));
  REQUIRE(b.GetSettings().restartOnStart);
  REQUIRE_FALSE(b.Freeze().get());  // never restored engaged
  REQUIRE_FALSE(b.GetStatus().lastLoadInexact);
  juce::MemoryBlock again;
  b.getStateInformation(again);
  REQUIRE(again == blob);

  SECTION("malformed or newer blobs change nothing") {
    const float before = b.Param(ParamId::DelayMs).Plain();
    const char  junk[] = "not a brainscape state at all";
    b.setStateInformation(junk, sizeof junk);
    b.setStateInformation(blob.getData(), static_cast<int>(blob.getSize()) - 3);
    juce::MemoryBlock newer(blob);
    static_cast<uint8_t*>(newer.getData())[4] = 2;  // format version 2
    b.setStateInformation(newer.getData(), static_cast<int>(newer.getSize()));
    REQUIRE(Bits(b.Param(ParamId::DelayMs).Plain()) == Bits(before));
  }
  SECTION("ids this build lacks are ignored, ids it has but the blob lacks load defaults") {
    WrapperState st{};
    REQUIRE(DecodeState(blob.getData(), blob.getSize(), st));
    std::vector<uint8_t> bytes;
    EncodeState(st, bytes);
    bytes[8]              = 2;  // claim 2 parameters...
    const uint32_t odd[2] = {999u, Bits(1.0f)};
    std::memcpy(bytes.data() + 12 + 8, odd, sizeof odd);  // ...the second one unknown
    std::vector<uint8_t> cut(bytes.begin(), bytes.begin() + 12 + 16);
    cut.insert(cut.end(), bytes.begin() + 12 + 8 * static_cast<long>(kNumParams), bytes.end());
    WrapperState partial{};
    REQUIRE(DecodeState(cut.data(), cut.size(), partial));
    REQUIRE(partial.unknownIds == 1u);
    REQUIRE(partial.missingIds == kNumParams - 1u);
    REQUIRE(Bits(partial.plain[0]) == Bits(st.plain[0]));
    REQUIRE(partial.plain[1] == kParamTable[1].def);
  }
}

TEST_CASE("a state restore while running is a Spillover load at the next block's first frame") {
  const Stereo in     = MakeInput(96000);
  const int    change = 48000;
  BrainscapeProcessor source;
  for (const auto& v : Busy()) source.Param(v.first).SetPlainNotifyingHost(v.second);
  WrapperSettings stereo;
  stereo.inputMode = InputMode::Stereo;  // the session carries the input mode too
  source.SetSettings(stereo);
  juce::MemoryBlock blob;
  source.getStateInformation(blob);

  auto       proc = MakeProcessor({}, {});
  HostRender r;
  r.pattern     = {480};
  r.beforeBlock = [&](int pos) {
    if (pos == change) proc->setStateInformation(blob.getData(), static_cast<int>(blob.getSize()));
  };
  const Stereo got      = RenderProcessor(*proc, in, {}, r);
  const auto   restored = CompleteState(Busy());
  RequireSame(got, RenderReference({}, in, kRate, {RefLoad(change, restored.get())}), "restore at frame 48000");
  // A Spillover load restarts the random-number epoch; 28 separate parameter stores do not.
  std::vector<RefEvent> stores;
  for (const auto& v : Complete(Busy())) stores.push_back(RefParam(change, v.first, v.second));
  REQUIRE_FALSE(SameBits(got.l, RenderReference({}, in, kRate, stores).l));
}

TEST_CASE("a restore before anything has played is an Exact load") {
  BrainscapeProcessor source;
  for (const auto& v : Busy()) source.Param(v.first).SetPlainNotifyingHost(v.second);
  juce::MemoryBlock blob;
  source.getStateInformation(blob);
  const Stereo in   = MakeInput(24000);
  auto         proc = MakeProcessor({}, {});  // prepared: the engine Init'd with the defaults
  proc->setStateInformation(blob.getData(), static_cast<int>(blob.getSize()));
  const Stereo got = RenderProcessor(*proc, in, {}, {{480}});
  RequireSame(got, RenderReference(Busy(), in), "restored after prepareToPlay, before the first block");
  // A Spillover load there would glide the smoothers from the defaults instead.
  const auto restored = CompleteState(Busy());
  REQUIRE_FALSE(SameBits(got.l, RenderReference({}, in, kRate, {RefLoad(0, restored.get())}).l));
}

TEST_CASE("MIDI note-on fires a grain at its sample offset") {
  Preset preset = Busy();
  preset.push_back({ParamId::OnsetTrigger, 0.0f});
  const Stereo in    = MakeInput(72000);
  const int    notes[] = {30037, 30038, 40192, 51200, 51201};  // odd offsets, a block start, pairs
  auto         trig  = [](std::vector<int> frames) {
    std::vector<RefEvent> ev;
    for (int f : frames) ev.push_back(RefTrigger(f, Engine::TriggerSource::MidiNote, 100.0f / 127.0f));
    return ev;
  };
  auto       proc = MakeProcessor(preset, {});
  HostRender r;
  r.pattern        = {256};
  r.noteOns        = std::vector<int>(std::begin(notes), std::end(notes));
  const Stereo got = RenderProcessor(*proc, in, {}, r);
  RequireSame(got, RenderReference(preset, in, kRate, trig(r.noteOns)), "note-ons at their frames");
  const Stereo early = RenderReference(preset, in, kRate, trig({30036, 30038, 40192, 51200, 51201}));
  const Stereo none  = RenderReference(preset, in, kRate);
  REQUIRE_FALSE(SameBits(got.l, early.l));
  REQUIRE_FALSE(SameBits(got.l, none.l));
}

TEST_CASE("a zero-frame call applies parameter changes without processing") {
  const Stereo in     = MakeInput(24000);
  const int    change = 9600;
  auto         proc   = MakeProcessor(Busy(), {});
  HostRender   r;
  r.pattern     = {480};
  r.beforeBlock = [&](int pos) {
    if (pos != change) return;
    proc->Param(ParamId::Mix).SetPlainNotifyingHost(0.25f);
    juce::AudioBuffer<float> empty(2, 0);
    juce::MidiBuffer         midi;
    proc->processBlock(empty, midi);
  };
  const Stereo got = RenderProcessor(*proc, in, {}, r);
  RequireSame(got, RenderReference(Busy(), in, kRate, {RefParam(change, ParamId::Mix, 0.25f)}),
              "change applied by a zero-frame call");
}

TEST_CASE("freeze is a host-automatable toggle that reaches the engine") {
  const Stereo in     = MakeInput(48000);
  const int    engage = 19200;
  auto         proc   = MakeProcessor(Busy(), {});
  REQUIRE(proc->Freeze().isAutomatable());
  REQUIRE(proc->Freeze().getParameterID() == "perf.freeze");
  HostRender r;
  r.pattern     = {480};
  r.beforeBlock = [&](int pos) {
    if (pos == engage) proc->Freeze().setValueNotifyingHost(1.0f);
  };
  const Stereo got = RenderProcessor(*proc, in, {}, r);
  RequireSame(got, RenderReference(Busy(), in, kRate, {RefFreeze(engage, true)}), "freeze at frame 19200");
}

TEST_CASE("a plugin insert passes both channels by default; the Standalone starts in mono") {
  auto proc = std::make_unique<BrainscapeProcessor>();
  REQUIRE(proc->GetSettings().inputMode == InputMode::Stereo);
  proc->Param(ParamId::Mix).SetPlainNotifyingHost(0.0f);
  proc->setRateAndBufferSizeDetails(kRate, 512);
  proc->prepareToPlay(kRate, 512);
  const Stereo in = MakeInput(9600);
  RequireSame(RenderProcessor(*proc, in, {}, {{480}}), in, "stereo bus, default settings, Mix 0");

  juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_Standalone);
  auto standalone = std::make_unique<BrainscapeProcessor>();
  juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_Undefined);
  REQUIRE(standalone->GetSettings().inputMode == InputMode::Mono);  // a guitar on input 1
}

TEST_CASE("a state restore wins over edits posted before it and loses to edits after it") {
  Preset restored = Busy();
  restored.push_back({ParamId::Mix, 0.8f});
  auto source = std::make_unique<BrainscapeProcessor>();
  for (const auto& v : restored) source->Param(v.first).SetPlainNotifyingHost(v.second);
  juce::MemoryBlock blob;
  source->getStateInformation(blob);
  const auto restore = [&blob](BrainscapeProcessor& p) {
    p.setStateInformation(blob.getData(), static_cast<int>(blob.getSize()));
  };
  const auto savedMix = [](BrainscapeProcessor& p) {
    juce::MemoryBlock saved;
    p.getStateInformation(saved);
    WrapperState st{};
    REQUIRE(DecodeState(saved.getData(), saved.getSize(), st));
    return st.plain[static_cast<size_t>(ParamId::Mix) - 1u];
  };
  const auto   all    = CompleteState(restored);
  const Stereo in     = MakeInput(48000);
  const int    change = 24000;
  const RefEvent load = RefLoad(change, all.get());

  SECTION("running: an edit and a freeze just before the restore lose to it") {
    auto       proc = MakeProcessor({}, {});
    HostRender r;
    r.pattern     = {480};
    r.beforeBlock = [&](int pos) {
      if (pos != change) return;
      proc->Param(ParamId::Mix).SetPlainNotifyingHost(0.3f);
      proc->Freeze().setValueNotifyingHost(1.0f);
      restore(*proc);
    };
    RequireSame(RenderProcessor(*proc, in, {}, r), RenderReference({}, in, kRate, {load}), "edit, then restore");
    REQUIRE(proc->Param(ParamId::Mix).Plain() == 0.8f);
    REQUIRE(savedMix(*proc) == 0.8f);
    REQUIRE_FALSE(proc->Freeze().get());
  }
  SECTION("running: an edit just after the restore wins") {
    auto       proc = MakeProcessor({}, {});
    HostRender r;
    r.pattern     = {480};
    r.beforeBlock = [&](int pos) {
      if (pos != change) return;
      restore(*proc);
      proc->Param(ParamId::Mix).SetPlainNotifyingHost(0.3f);
    };
    RequireSame(RenderProcessor(*proc, in, {}, r),
                RenderReference({}, in, kRate, {load, RefParam(change, ParamId::Mix, 0.3f)}), "restore, then edit");
    REQUIRE(proc->Param(ParamId::Mix).Plain() == 0.3f);
    REQUIRE(savedMix(*proc) == 0.3f);
  }
  SECTION("not yet prepared: edits before the restore stay lost after prepareToPlay") {
    auto proc = std::make_unique<BrainscapeProcessor>();
    proc->Param(ParamId::Mix).SetPlainNotifyingHost(0.3f);
    proc->Freeze().setValueNotifyingHost(1.0f);
    restore(*proc);
    proc->setRateAndBufferSizeDetails(kRate, 512);
    proc->prepareToPlay(kRate, 512);
    RequireSame(RenderProcessor(*proc, in, {}, {{512}}), RenderReference(restored, in), "inactive, then prepared");
    REQUIRE(savedMix(*proc) == 0.8f);
  }
  SECTION("suspended (no blocks, no prepare): the restore still wins at the next block") {
    auto         proc = MakeProcessor({}, {});
    const Stereo first{std::vector<float>(in.l.begin(), in.l.begin() + change),
                       std::vector<float>(in.r.begin(), in.r.begin() + change)};
    const Stereo second{std::vector<float>(in.l.begin() + change, in.l.end()),
                        std::vector<float>(in.r.begin() + change, in.r.end())};
    Stereo got = RenderProcessor(*proc, first, {}, {{480}});
    for (int i = 0; i < 100; ++i) proc->Param(ParamId::Mix).SetPlainNotifyingHost(0.01f * static_cast<float>(i));
    restore(*proc);
    const Stereo rest = RenderProcessor(*proc, second, {}, {{480}});
    got.l.insert(got.l.end(), rest.l.begin(), rest.l.end());
    got.r.insert(got.r.end(), rest.r.begin(), rest.r.end());
    RequireSame(got, RenderReference({}, in, kRate, {load}), "suspended, then resumed");
  }
}

TEST_CASE("only events the resync cannot restore count as lost") {
  // While no block runs, every edit waits for the same frame, where the resync re-sends
  // the mirrors: an overflow there loses nothing. A trigger cannot be re-sent.
  auto proc = std::make_unique<BrainscapeProcessor>();
  for (int i = 0; i < 3000; ++i) {
    proc->Param(ParamId::DelayMs).SetPlainNotifyingHost(10.0f + static_cast<float>(i) * 0.5f);
  }
  proc->setRateAndBufferSizeDetails(kRate, 512);
  proc->prepareToPlay(kRate, 512);
  const Stereo in = MakeInput(9600);
  RequireSame(RenderProcessor(*proc, in, {}, {{512}}), RenderReference({{ParamId::DelayMs, 1509.5f}}, in),
              "after 3000 edits before prepare");
  REQUIRE(proc->GetStatus().droppedEvents == 0u);
  for (int i = 0; i < 3000; ++i) proc->TriggerFromUi();
  const uint32_t lostTriggers = 3000u - static_cast<uint32_t>(WrapperQueue::capacity());
  REQUIRE(proc->GetStatus().droppedEvents == lostTriggers);
  // A scripted event has no mirror behind it: an overflow loses it, even at frame 0.
  proc->PostAt(0, {WrapperEvent::Type::Param, WrapperEvent::Source::Ui, static_cast<uint32_t>(ParamId::Mix), 0.3f});
  proc->PostAt(9, {WrapperEvent::Type::Param, WrapperEvent::Source::Ui, static_cast<uint32_t>(ParamId::Mix), 0.4f});
  REQUIRE(proc->GetStatus().droppedEvents == lostTriggers + 2u);
}

TEST_CASE("scripted events reach the engine stamped at their frames, host blocks unsplit") {
  using E         = WrapperEvent;
  const Stereo in = MakeInput(48000);
  const auto   id = [](ParamId p) { return static_cast<uint32_t>(p); };
  const auto   script = [&](BrainscapeProcessor& p) {
    p.PostAt(0, {E::Type::Param, E::Source::Ui, id(ParamId::Feedback), 0.5f});  // the first block
    p.PostAt(10007, {E::Type::Param, E::Source::Ui, id(ParamId::PitchSt), 5.0f});
    p.PostAt(20011, {E::Type::Trigger, E::Source::Ui, 0u, 1.0f});
    p.PostAt(30000, {E::Type::Param, E::Source::Ui, id(ParamId::Mix), 0.45f});  // after the host's
    p.PostAt(30000, {E::Type::Param, E::Source::Host, id(ParamId::Mix), 0.4f});
    p.PostAt(40013, {E::Type::Freeze, E::Source::Ui, 0u, 1.0f});
  };
  const auto ref = [&](int triggerAt) {
    return RenderReference(Busy(), in, kRate,
                           {RefParam(0, ParamId::Feedback, 0.5f), RefParam(10007, ParamId::PitchSt, 5.0f),
                            RefTrigger(triggerAt), RefParam(30000, ParamId::Mix, 0.4f),
                            RefParam(30000, ParamId::Mix, 0.45f), RefFreeze(40013, true)});
  };
  const Stereo want = ref(20011);
  for (const std::vector<int>& pattern : std::vector<std::vector<int>>{{512}, {441}, {4096}, {0, 37, 1}}) {
    auto proc = MakeProcessor(Busy(), {});
    script(*proc);
    HostRender r;
    r.pattern = pattern;
    RequireSame(RenderProcessor(*proc, in, {}, r), want, PatternName(pattern).c_str());
    REQUIRE(proc->Param(ParamId::PitchSt).Plain() == 5.0f);  // mirrors follow once applied
    REQUIRE(proc->Param(ParamId::Mix).Plain() == 0.45f);
    // The engine splits at the stamps: one Process call per chunk, as with no events.
    REQUIRE(proc->GetStatus().engineCalls == ChunkCalls(pattern, 48000));
  }
  REQUIRE_FALSE(SameBits(want.l, ref(20012).l));  // one frame later is audible: not vacuous

  SECTION("stamps count from the latest Init") {
    HostSetup h96;
    h96.rate  = 96000.0;
    auto proc = MakeProcessor(Busy(), h96);
    RenderProcessor(*proc, MakeInput(9600), h96);
    proc->PostAt(2000, {E::Type::Param, E::Source::Ui, id(ParamId::Mix), 0.9f});  // void after the Init
    proc->prepareToPlay(kRate, 512);  // a rate change: a fresh engine at frame 0
    proc->PostAt(4801, {E::Type::Param, E::Source::Ui, id(ParamId::Mix), 0.25f});
    const Stereo short48 = MakeInput(9600);
    RequireSame(RenderProcessor(*proc, short48, {}, {{480}}),
                RenderReference(Busy(), short48, kRate, {RefParam(4801, ParamId::Mix, 0.25f)}), "stamp after a re-Init");
  }
}

TEST_CASE("scripted parameter values are canonicalized like every other producer's") {
  // The mirror and the session state hold what the engine keeps, never the caller's bits.
  using E           = WrapperEvent;
  const Stereo   in = MakeInput(480);
  const uint32_t mix = static_cast<uint32_t>(ParamId::Mix);
  for (const float v : {2.0f, -0.0f, std::numeric_limits<float>::quiet_NaN(), -1.0f,
                        std::numeric_limits<float>::infinity(), 1e-40f, 0.25f}) {
    INFO("value bits " << Bits(v));
    auto proc = MakeProcessor({}, {});
    proc->PostAt(0, {E::Type::Param, E::Source::Ui, mix, v});
    proc->PostAt(240, {E::Type::Param, E::Source::Ui, mix, v});  // applied inside the block
    RenderProcessor(*proc, in, {});
    REQUIRE(Bits(proc->Param(ParamId::Mix).Plain()) == Bits(Canonicalize(ParamId::Mix, v)));
    // The saved session is byte for byte the one an editor edit to the canonical value saves.
    auto ref = MakeProcessor({}, {});
    ref->Param(ParamId::Mix).SetPlainNotifyingHost(Canonicalize(ParamId::Mix, v));
    juce::MemoryBlock got, want;
    proc->getStateInformation(got);
    ref->getStateInformation(want);
    REQUIRE(got == want);
  }
}

namespace {

juce::File WriteWav(const juce::File& file, double rate, const std::vector<std::vector<float>>& channels) {
  file.deleteFile();
  std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
  REQUIRE(stream != nullptr);
  juce::WavAudioFormat format;
  const auto           options = juce::AudioFormatWriterOptions{}
                               .withSampleRate(rate)
                               .withNumChannels(static_cast<int>(channels.size()))
                               .withBitsPerSample(32)
                               .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
  std::unique_ptr<juce::AudioFormatWriter> writer = format.createWriterFor(stream, options);
  REQUIRE(writer != nullptr);
  const int                frames = static_cast<int>(channels[0].size());
  juce::AudioBuffer<float> buffer(static_cast<int>(channels.size()), frames);
  for (size_t c = 0; c < channels.size(); ++c) buffer.copyFrom(static_cast<int>(c), 0, channels[c].data(), frames);
  REQUIRE(writer->writeFromAudioSampleBuffer(buffer, 0, frames));
  return file;
}

}  // namespace

TEST_CASE("the test input loops a file through the engine") {
  const juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("brainscape-test-input");
  REQUIRE(dir.createDirectory());
  constexpr int      kLength = 1000;
  std::vector<float> a(kLength), b(kLength);
  for (int i = 0; i < kLength; ++i) {  // exact in binary32, never zero
    a[static_cast<size_t>(i)] = static_cast<float>(i + 1) * 0x1p-12f;
    b[static_cast<size_t>(i)] = -static_cast<float>(i + 1) * 0x1p-13f;
  }
  const juce::File stereo48 = WriteWav(dir.getChildFile("stereo48.wav"), 48000.0, {a, b});
  const juce::File mono48   = WriteWav(dir.getChildFile("mono48.wav"), 48000.0, {a});
  const juce::File stereo44 = WriteWav(dir.getChildFile("stereo44.wav"), 44100.0, {a, b});
  const juce::File bad      = dir.getChildFile("bad.wav");
  REQUIRE(bad.replaceWithText("not audio"));

  auto proc = MakeProcessor({{ParamId::Mix, 0.0f}}, {});  // dry exact at Mix 0
  TestInput& input = proc->GetTestInput();
  juce::String error;
  const Stereo silence{std::vector<float>(2600, 0.f), std::vector<float>(2600, 0.f)};

  SECTION("a 48 kHz file plays bit for bit and wraps") {
    REQUIRE(input.LoadFile(stereo48, error));
    input.SetSource(TestInput::Source::FileLoop);
    const Stereo got = RenderProcessor(*proc, silence, {}, {{480}});
    for (size_t k = 0; k < got.l.size(); ++k) {
      INFO("frame " << k);
      REQUIRE(Bits(got.l[k]) == Bits(a[k % kLength]));
      REQUIRE(Bits(got.r[k]) == Bits(b[k % kLength]));
    }
  }
  SECTION("a mono file feeds both channels") {
    REQUIRE(input.LoadFile(mono48, error));
    input.SetSource(TestInput::Source::FileLoop);
    const Stereo got = RenderProcessor(*proc, silence, {}, {{512}});
    REQUIRE(SameBits(got.l, got.r));
    REQUIRE(Bits(got.l[kLength + 3]) == Bits(a[3]));
  }
  SECTION("a 44.1 kHz file is stepped at its own rate across the wrap") {
    REQUIRE(input.LoadFile(stereo44, error));
    input.SetSource(TestInput::Source::FileLoop);
    const Stereo got   = RenderProcessor(*proc, silence, {}, {{333}});
    double       phase = 0.0;
    for (size_t k = 0; k < got.l.size(); ++k) {
      const auto  i0   = static_cast<size_t>(phase);
      const auto  i1   = i0 + 1 < static_cast<size_t>(kLength) ? i0 + 1 : 0u;
      const auto  frac = static_cast<float>(phase - static_cast<double>(i0));
      const float want = a[i0] + (a[i1] - a[i0]) * frac;
      INFO("frame " << k);
      REQUIRE(std::fabs(got.l[k] - want) <= 1e-6f);
      phase += 44100.0 / 48000.0;
      if (phase >= kLength) phase = std::fmod(phase, static_cast<double>(kLength));
    }
  }
  SECTION("a bad file is refused and the loaded loop stays") {
    REQUIRE(input.LoadFile(stereo48, error));
    REQUIRE_FALSE(input.LoadFile(bad, error));
    REQUIRE(error.isNotEmpty());
    REQUIRE(input.LoadedName() == "stereo48.wav");
  }
  SECTION("loading while the audio thread plays never frees the loop it reads") {
    REQUIRE(input.LoadFile(stereo48, error));
    input.SetSource(TestInput::Source::FileLoop);
    std::atomic<bool> stop{false}, finite{true};
    std::thread       audio([&] {
      std::vector<float> l(256), r(256);
      juce::MidiBuffer   midi;
      while (!stop.load()) {
        float*                   chans[2] = {l.data(), r.data()};
        juce::AudioBuffer<float> buffer(chans, 2, 256);
        proc->processBlock(buffer, midi);
        for (int i = 0; i < 256; ++i) {
          if (!std::isfinite(l[static_cast<size_t>(i)]) || !std::isfinite(r[static_cast<size_t>(i)])) finite = false;
        }
      }
    });
    bool loaded = true;  // no REQUIRE while the audio thread runs: a throw would skip the join
    for (int i = 0; i < 40; ++i) {
      loaded = input.LoadFile(i % 2 == 0 ? stereo44 : mono48, error) && loaded;
      input.CollectGarbage();
    }
    stop = true;
    audio.join();
    REQUIRE(loaded);
    REQUIRE(finite.load());
    REQUIRE(input.LoadedName() == "mono48.wav");
  }
  dir.deleteRecursively();
}

TEST_CASE("restart on transport start: bounces start from the exact-restart state") {
  const Stereo source  = MakeInput(4 * 48000);
  const Stereo preroll = Slice(source, 0, 24000);  // the transport stopped
  const Stereo take    = Slice(source, 48000, 120000);
  const Stereo gap     = Slice(source, 130000, 140000);
  const Stereo ref     = RenderReference(Busy(), take);
  TestPlayHead head;
  const auto   withOption = [&](bool on) {
    auto            proc = MakeProcessor(Busy(), {});
    WrapperSettings s    = proc->GetSettings();
    s.restartOnStart     = on;
    proc->SetSettings(s);
    proc->setPlayHead(&head);
    return proc;
  };
  const auto play = [&](BrainscapeProcessor& p, const Stereo& in, std::vector<int> pattern) {
    head.playing     = true;
    const Stereo out = RenderProcessor(p, in, {}, {std::move(pattern)});
    head.playing     = false;
    return out;
  };

  SECTION("offline: the engine restarts in place, every bounce the same") {
    auto proc = withOption(true);
    proc->setNonRealtime(true);
    RenderProcessor(*proc, preroll, {}, {{441}});
    const Stereo first = play(*proc, take, {441});
    RenderProcessor(*proc, gap, {}, {{441}});
    const Stereo second = play(*proc, take, {512, 37});
    RequireSame(first, ref, "first offline bounce");
    RequireSame(second, ref, "second offline bounce");
    REQUIRE(proc->GetStatus().lastStart == BrainscapeProcessor::TransportStart::Restarted);
    proc->setPlayHead(nullptr);
  }
  SECTION("real time: the prepared spare engine is swapped in") {
    auto proc = withOption(true);
    RenderProcessor(*proc, preroll, {}, {{441}});
    REQUIRE(WaitForSpare(*proc));
    const Stereo first = play(*proc, take, {441});
    REQUIRE(proc->GetStatus().lastStart == BrainscapeProcessor::TransportStart::Restarted);
    RenderProcessor(*proc, gap, {}, {{441}});
    REQUIRE(WaitForSpare(*proc));  // the worker restarted the retired engine
    const Stereo second = play(*proc, take, {256});
    RequireSame(first, ref, "first real-time bounce");
    RequireSame(second, ref, "second real-time bounce");
    proc->setPlayHead(nullptr);
  }
  SECTION("real time: a spare that holds another preset is not used, and is replaced") {
    auto proc = withOption(true);
    REQUIRE(WaitForSpare(*proc));
    Preset edited = Busy();
    edited.push_back({ParamId::Mix, 0.5f});
    {
      const auto paused = proc->PauseSpareWorker();
      proc->Param(ParamId::Mix).SetPlainNotifyingHost(0.5f);
      RenderProcessor(*proc, preroll, {}, {{441}});
      const Stereo late = play(*proc, take, {441});
      REQUIRE(proc->GetStatus().lastStart == BrainscapeProcessor::TransportStart::SpareNotReady);
      REQUIRE_FALSE(SameBits(late.l, RenderReference(edited, take).l));  // the engine ran on
    }
    RenderProcessor(*proc, gap, {}, {{441}});  // stopped: the next playing block is a start
    REQUIRE(WaitForSpare(*proc));
    RequireSame(play(*proc, take, {441}), RenderReference(edited, take), "after the spare caught up");
    proc->setPlayHead(nullptr);
  }
  SECTION("off: the second bounce goes on from the first") {
    auto proc = withOption(false);
    proc->setNonRealtime(true);
    RenderProcessor(*proc, preroll, {}, {{441}});
    const Stereo first = play(*proc, take, {441});
    RenderProcessor(*proc, gap, {}, {{441}});
    const Stereo second = play(*proc, take, {441});
    REQUIRE_FALSE(SameBits(first.l, second.l));
    REQUIRE(proc->GetStatus().lastStart == BrainscapeProcessor::TransportStart::None);
    proc->setPlayHead(nullptr);
  }
}

TEST_CASE("reset() is Engine::Reset: the ring and the counter run on") {
  static constexpr int at = 24000;  // static: the reference's lambdas use it uncaptured
  const Stereo         in = MakeInput(48000);
  auto                 proc = MakeProcessor(Busy(), {});
  HostRender   r;
  r.pattern     = {480};
  r.beforeBlock = [&](int pos) {
    if (pos == at) proc->reset();
  };
  const Stereo got = RenderProcessor(*proc, in, {}, r);
  RequireSame(got, RenderReference(Busy(), in, kRate, {}, [](Engine& e, int pos) {
                if (pos == at) e.Reset();
              }),
              "reset at frame 24000");
  REQUIRE_FALSE(SameBits(got.l, RenderReference(Busy(), in, kRate, {}, [](Engine& e, int pos) {
                                  if (pos == at) e.Restart();
                                }).l));
}

TEST_CASE("the offline audition renders the input from the exact-restart state") {
  const Preset preset = Busy();
  const Preset all    = Complete(preset);
  float        values[kNumParams];
  for (size_t i = 0; i < kNumParams; ++i) values[i] = all[i].second;

  SECTION("the test signal, in both input modes") {
    for (const InputMode mode : {InputMode::Stereo, InputMode::Mono}) {
      AuditionInput input = TestSignalInput();
      REQUIRE(input.l.size() == static_cast<size_t>((kAuditionSignalSeconds + kAuditionTailSeconds) * 48000.0));
      Stereo engineIn{input.l, mode == InputMode::Mono ? input.l : input.r};
      AuditionOutput out;
      REQUIRE(RenderAudition(values, mode, input, out));
      RequireSame({out.l, out.r}, RenderReference(preset, engineIn), mode == InputMode::Mono ? "mono" : "stereo");
    }
  }
  SECTION("a file off the 24-bit grid goes through ConditionInput24") {
    juce::AudioBuffer<float> audio(2, 4801);
    for (int i = 0; i < audio.getNumSamples(); ++i) {
      audio.setSample(0, i, static_cast<float>(i % 97 - 48) * 0.0123f);
      audio.setSample(1, i, static_cast<float>(i % 89 - 44) * -0.0071f);
    }
    AuditionInput input = FileInput(audio, 48000.0, "offgrid.wav");
    REQUIRE(input.l.size() == 4801u + static_cast<size_t>(kAuditionTailSeconds * 48000.0));
    Stereo conditioned{input.l, input.r};
    ConditionInput24(conditioned.l.data(), conditioned.l.data(), conditioned.l.size());
    ConditionInput24(conditioned.r.data(), conditioned.r.data(), conditioned.r.size());
    REQUIRE_FALSE(SameBits(conditioned.l, input.l));
    AuditionOutput out;
    REQUIRE(RenderAudition(values, InputMode::Stereo, input, out));
    RequireSame({out.l, out.r}, RenderReference(preset, conditioned), "conditioned file");
  }
  SECTION("the job writes the WAV bit for bit, with its hash in the recipe") {
    const juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("brainscape-audition");
    REQUIRE(dir.createDirectory());
    const juce::File wav  = dir.getChildFile("take.wav");
    auto             proc = MakeProcessor(preset, {});
    proc->GetTestInput().SetSource(TestInput::Source::Pluck);  // renders the test signal
    juce::String error;
    REQUIRE(proc->StartAudition(wav, error));
    AuditionJob::Result result = proc->GetAudition();
    for (int i = 0; i < 3000 && result.state == AuditionJob::State::Running; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      result = proc->GetAudition();
    }
    INFO(result.message);
    REQUIRE(result.state == AuditionJob::State::Done);

    AuditionInput  input = TestSignalInput();
    AuditionOutput want;
    REQUIRE(RenderAudition(values, InputMode::Stereo, input, want));
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(wav));
    REQUIRE(reader != nullptr);
    REQUIRE(reader->sampleRate == 48000.0);
    REQUIRE(reader->numChannels == 2u);
    REQUIRE(reader->usesFloatingPointData);
    const int                frames = static_cast<int>(reader->lengthInSamples);
    juce::AudioBuffer<float> read(2, frames);
    REQUIRE(reader->read(&read, 0, frames, 0, true, true));
    RequireSame({std::vector<float>(read.getReadPointer(0), read.getReadPointer(0) + frames),
                 std::vector<float>(read.getReadPointer(1), read.getReadPointer(1) + frames)},
                {want.l, want.r}, "the WAV's samples");

    golden::Sha256 sha;  // an independent SHA-256 of the interleaved little-endian float32
    for (size_t i = 0; i < want.l.size(); ++i) {
      uint8_t bytes[8];
      for (int c = 0; c < 2; ++c) {
        const uint32_t u = Bits(c == 0 ? want.l[i] : want.r[i]);
        for (int b = 0; b < 4; ++b) bytes[4 * c + b] = static_cast<uint8_t>(u >> (8 * b));
      }
      sha.Update(bytes, 8);
    }
    REQUIRE(result.outputSha256.toStdString() == sha.Hex());
    const juce::var recipe = juce::JSON::parse(dir.getChildFile("take.recipe.json"));
    REQUIRE(recipe["outputSha256"].toString() == result.outputSha256);
    REQUIRE(recipe["preset"].size() == static_cast<int>(kNumParams));
    REQUIRE(recipe["preset"][1]["bits"].toString() == juce::String::toHexString(static_cast<juce::int64>(Bits(values[1]))).paddedLeft('0', 8));
    REQUIRE(static_cast<int>(recipe["soundRevision"]) == static_cast<int>(kSoundRevision));
    reader.reset();
    dir.deleteRecursively();
  }
}

TEST_CASE("latency, tail and supported layouts") {
  BrainscapeProcessor proc;
  REQUIRE(proc.getLatencySamples() == 0);
  REQUIRE(std::isinf(proc.getTailLengthSeconds()));
  using Set    = juce::AudioChannelSet;
  auto layout  = [](Set in, Set out) {
    juce::AudioProcessor::BusesLayout l;
    l.inputBuses.add(in);
    l.outputBuses.add(out);
    return l;
  };
  REQUIRE(proc.checkBusesLayoutSupported(layout(Set::stereo(), Set::stereo())));
  REQUIRE(proc.checkBusesLayoutSupported(layout(Set::mono(), Set::stereo())));
  REQUIRE(proc.checkBusesLayoutSupported(layout(Set::mono(), Set::mono())));
  REQUIRE(proc.checkBusesLayoutSupported(layout(Set::disabled(), Set::stereo())));
  REQUIRE_FALSE(proc.checkBusesLayoutSupported(layout(Set::stereo(), Set::mono())));
  REQUIRE_FALSE(proc.checkBusesLayoutSupported(layout(Set::create5point1(), Set::create5point1())));
}

int main(int argc, char* argv[]) {
  ReportCrtErrorsOnStderr();
#if defined(_MSC_VER) && defined(_DEBUG)
  _CrtSetAllocHook(CrtAllocHook);
#endif
  juce::ScopedJuceInitialiser_GUI juce;  // the attachment tests create sliders
  return Catch::Session().run(argc, argv);
}
