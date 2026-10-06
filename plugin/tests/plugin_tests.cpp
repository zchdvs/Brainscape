// Headless checks of the wrapper obligations (docs/design/companion-app.md §4.12, §5):
// the processor is driven the way hosts drive it and compared bit for bit with the engine
// driven directly in 48-frame blocks (the pedal's grid).
#define CATCH_CONFIG_RUNNER
#include "catch.hpp"

#include <atomic>
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
#include <utility>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PlainAttachment.h"
#include "PluginProcessor.h"
#include "StateCodec.h"
#include "brainscape/Engine.h"
#include "brainscape/HostArenas.h"
#include "TestSupport.h"
#include "brainscape/ParamDisplay.h"

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
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.8e", static_cast<double>(plain));  // 9 digits: round-trips
  std::string s(buf);
  const size_t e        = s.find('e');
  const int    exponent = std::atoi(s.c_str() + e + 1);
  const bool   percent  = FindParamDisplay(id)->kind == DisplayKind::Percent;
  return s.substr(0, e) + "e" + std::to_string(exponent + (percent ? 2 : 0)) + (percent ? "%" : "");
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
    REQUIRE(st.pedalExact == (rate == 48000.0));
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
    REQUIRE(proc->GetStatus().pedalExact);
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
      const float want = CanonicalizePlain(d.id, v);
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
    REQUIRE(Bits(p.Plain()) == Bits(CanonicalizePlain(d.id, 0.0f)));
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
    return std::vector<RefEvent>{{change, [pitch, morph](Engine& e) {
                                    e.SetParam(ParamId::PitchSt, pitch);
                                    e.SetParam(ParamId::FilterMorph, morph);
                                  }}};
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

  // Every value typed as text lands on exactly that binary32, in every parameter's units.
  for (const ParamDescriptor& d : kParamTable) {
    if (FindParamDisplay(d.id)->steps >= 2) continue;
    INFO(d.name);
    juce::Slider              knob;
    BrainscapePlainAttachment att(proc.Param(d.id), knob);
    for (float v : SampleValues(d.id, static_cast<uint32_t>(d.id) * 104729u, 300)) {
      const float       want = CanonicalizePlain(d.id, v);
      const std::string text = ExactText(d.id, want);
      INFO(text);
      REQUIRE(att.CommitText(text));
      REQUIRE(Bits(proc.Param(d.id).Plain()) == Bits(want));
      REQUIRE(att.DisplayText() == FormatPlainText(d.id, want));
    }
    att.ResetToDefault();
    REQUIRE(Bits(proc.Param(d.id).Plain()) == Bits(CanonicalizePlain(d.id, d.def)));
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
  s.inputMode    = InputMode::Stereo;
  s.inputGainDb  = -3.5f;
  s.outputGainDb = 2.25f;
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

TEST_CASE("a state restore while running applies as one unit at the next block") {
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
  const Stereo got = RenderProcessor(*proc, in, {}, r);
  const Preset all = Complete(Busy());
  const Stereo ref = RenderReference({}, in, kRate, {{change, [&all](Engine& e) {
                                                         for (const auto& v : all) e.SetParam(v.first, v.second);
                                                         e.SetFreeze(false);
                                                       }}});
  RequireSame(got, ref, "restore at frame 48000");
}

TEST_CASE("MIDI note-on fires a grain at its sample offset") {
  Preset preset = Busy();
  preset.push_back({ParamId::OnsetTrigger, 0.0f});
  const Stereo in    = MakeInput(72000);
  const int    notes[] = {30037, 30038, 40192, 51200, 51201};  // odd offsets, a block start, pairs
  auto         trig  = [](std::vector<int> frames) {
    std::vector<RefEvent> ev;
    for (int f : frames) ev.push_back({f, [](Engine& e) { e.Trigger(Engine::TriggerSource::MidiNote); }});
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
  RequireSame(got, RenderReference(Busy(), in, kRate, {{change, [](Engine& e) { e.SetParam(ParamId::Mix, 0.25f); }}}),
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
  RequireSame(got, RenderReference(Busy(), in, kRate, {{engage, [](Engine& e) { e.SetFreeze(true); }}}),
              "freeze at frame 19200");
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
#if defined(_MSC_VER) && defined(_DEBUG)
  _CrtSetAllocHook(CrtAllocHook);
#endif
  juce::ScopedJuceInitialiser_GUI juce;  // the attachment tests create sliders
  return Catch::Session().run(argc, argv);
}
