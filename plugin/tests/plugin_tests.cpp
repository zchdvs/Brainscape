// Headless checks of the wrapper obligations (docs/design/companion-app.md §4.12, §5):
// the processor is driven the way hosts drive it and compared bit for bit with the engine
// driven directly from the exact-restart state in 48-frame blocks (the pedal's grid), its
// events stamped at their frames.
#define CATCH_CONFIG_RUNNER
#include "catch.hpp"

#include <algorithm>
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
#include "Compile.h"  // brainscape_compiler: the test documents (compiler/tests/data)
#include "Curation.h"
#include "FactoryModes.h"
#include "Lint.h"  // brainscape_compiler: derive, for the saved documents
#include "PlainAttachment.h"
#include "gui/CurationViews.h"
#include "gui/ModeMenu.h"
#include "gui/TempoPanel.h"
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "StateCodec.h"
#include "brainscape/Engine.h"
#include "brainscape/HostArenas.h"
#include "TestSupport.h"
#include "brainscape/InputCondition.h"
#include "brainscape/ModeEval.h"
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

// A host transport the tests start and stop, with a tempo and a position when they set them
// (docs/design/clock.md §8.4).
class TestPlayHead final : public juce::AudioPlayHead {
 public:
  bool   playing = false;
  bool   hasBpm  = false;
  double bpm     = 120.0;
  bool   hasPpq  = false;
  double ppq     = 0.0;
  juce::Optional<PositionInfo> getPosition() const override {
    PositionInfo info;
    info.setIsPlaying(playing);
    if (hasBpm) info.setBpm(bpm);
    if (hasPpq) info.setPpqPosition(ppq);
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

// A test document (compiler/tests/data), compiled and decoded as a package loads.
std::unique_ptr<PresetState> CompiledState(const char* name) {
  const juce::File file = juce::File(BRAINSCAPE_TEST_DATA).getChildFile(name);
  const std::string text = file.loadFileAsString().toStdString();
  const bsc::CompileResult r = bsc::Compile(text);
  REQUIRE(r.ok);
  bsc::DecodedPackage p = bsc::DecodePackage(r.package.data(), r.package.size());
  REQUIRE(p.ok);
  return std::move(p.state);
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
  for (const ParamDescriptor& d : LeafRows()) {
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
  // + freeze, the eight macros, the expression pedal, the effect volume, perf.subdiv and
  // perf.time_mode
  REQUIRE(proc.getParameters().size() == static_cast<int>(kNumLeafParams) + 13);
  REQUIRE(proc.Param(ParamId::DelayMs).getParameterID() == "layer0.position.base_ms");
  // Sound revision 2 retired rows 27 and 28 into mode structure: no longer registered.
  REQUIRE_FALSE(IsLeaf(ParamId::OnsetTrigger));
  REQUIRE_FALSE(IsLeaf(ParamId::PositionSource));
  REQUIRE(proc.Param(ParamId::FilterMorph).getParameterID() == "post.filter.morph");
}

// The ID table (mode-compiler.md §4, §9.2): one host parameter per Leaf row, then freeze, the
// eight Macro rows, perf.expression and the effect volume, each keyed on its stable name;
// Reserved and Retired rows are not registered. Automation follows host model (b) (§3.6, Q12)
// through the shared flags: the macros, Mix, the effect volume and the performance rows are
// automatable, every other leaf is registered but not, so a host records the knobs a player
// turns, not the leaves they fan out to.
TEST_CASE("the plugin registers the rows of host model (b)") {
  BrainscapeProcessor proc;
  const auto&         params = proc.getParameters();
  REQUIRE(params.size() == static_cast<int>(kNumLeafParams) + 13);
  for (size_t i = 0; i < kNumLeafParams; ++i) {
    const ParamDescriptor& d = *FindParam(LeafId(i));
    INFO(d.name);
    auto* p = dynamic_cast<BrainscapeParam*>(params[static_cast<int>(i)]);
    REQUIRE(p != nullptr);
    REQUIRE(p->Id() == d.id);
    REQUIRE(&proc.Param(d.id) == p);
    REQUIRE(proc.FindHostParam(d.id) == p);
    REQUIRE(p->getParameterID() == juce::String(d.name));
    REQUIRE(p->isAutomatable() == ((FindParamDisplay(d.id)->flags & kParamAutomatable) != 0u));
    REQUIRE(p->isAutomatable() == (d.id == ParamId::Mix));
  }
  REQUIRE(proc.Param(ParamId::WetTrimDb).getParameterID() == "wet_trim_db");
  REQUIRE(proc.Param(ParamId::TransposeSt).getParameterID() == "layer0.pitch.transpose_st");
  REQUIRE(params[static_cast<int>(kNumLeafParams)] == &proc.Freeze());
  REQUIRE(proc.Freeze().isAutomatable());
  REQUIRE(proc.Freeze().getParameterID() == FindParam(ParamId::PerfFreeze)->name);
  for (size_t k = 0; k < kMaxMacros; ++k) {
    const auto id = static_cast<ParamId>(static_cast<uint32_t>(ParamId::MacroActivity) + k);
    INFO(FindParam(id)->name);
    auto* p = dynamic_cast<BrainscapeParam*>(params[static_cast<int>(kNumLeafParams + 1 + k)]);
    REQUIRE(p == &proc.Macro(id));
    REQUIRE(proc.FindHostParam(id) == p);
    REQUIRE(p->getParameterID() == juce::String(FindParam(id)->name));
    REQUIRE(p->isAutomatable());
    REQUIRE(BrainscapeParam::EventTypeFor(id) == WrapperEvent::Type::Macro);
  }
  REQUIRE(params[static_cast<int>(kNumLeafParams) + 9] == &proc.Expression());
  REQUIRE(proc.Expression().getParameterID() == "perf.expression");
  REQUIRE(proc.Expression().isAutomatable());
  REQUIRE(BrainscapeParam::EventTypeFor(ParamId::PerfExpression) == WrapperEvent::Type::Expression);
  REQUIRE(params[static_cast<int>(kNumLeafParams) + 10] == &proc.EffectVolume());
  REQUIRE(proc.EffectVolume().getParameterID() == "global.effect_volume_db");
  REQUIRE(proc.EffectVolume().isAutomatable());
  REQUIRE(BrainscapeParam::EventTypeFor(ParamId::EffectVolumeDb) == WrapperEvent::Type::Param);
  // The tempo core's Performance rows, appended after 82 (clock.md §10.4): automatable, each a
  // Subdivision event; row 85, global.tempo_recall, is a device setting and not registered.
  REQUIRE(params[static_cast<int>(kNumLeafParams) + 11] == &proc.SubdivParam());
  REQUIRE(proc.SubdivParam().getParameterID() == "perf.subdiv");
  REQUIRE(proc.FindHostParam(ParamId::PerfSubdiv) == &proc.SubdivParam());
  REQUIRE(params[static_cast<int>(kNumLeafParams) + 12] == &proc.TimeModeParam());
  REQUIRE(proc.TimeModeParam().getParameterID() == "perf.time_mode");
  REQUIRE(proc.FindHostParam(ParamId::PerfTimeMode) == &proc.TimeModeParam());
  for (BrainscapeParam* p : {&proc.SubdivParam(), &proc.TimeModeParam()}) {
    REQUIRE(p->isAutomatable());
    REQUIRE(p->isDiscrete());
  }
  REQUIRE(proc.SubdivParam().getNumSteps() == 6);
  REQUIRE(proc.TimeModeParam().getNumSteps() == 3);
  REQUIRE(proc.FindHostParam(ParamId::TempoRecall) == nullptr);
  REQUIRE(proc.FindHostParam(ParamId::LevelDb) == nullptr);      // Reserved
  REQUIRE(proc.FindHostParam(ParamId::OnsetTrigger) == nullptr); // Retired
  REQUIRE(proc.FindHostParam(ParamId::PerfLoopLevel) == nullptr);
  for (const auto* p : params) {
    const auto* w = dynamic_cast<const juce::AudioProcessorParameterWithID*>(p);
    REQUIRE(w != nullptr);
    INFO(w->getParameterID());
    for (const ParamDescriptor& d : kParamTable) {
      if (d.kind == ParamKind::Reserved || d.kind == ParamKind::Retired) {
        REQUIRE((d.name == nullptr || w->getParameterID() != juce::String(d.name)));
      }
    }
  }
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
    proc->Param(ParamId::TransposeSt).SetPlainNotifyingHost(7.02f);
    proc->Param(ParamId::FilterMorph).SetPlainNotifyingHost(0.4f);
  };
  const Stereo got = RenderProcessor(*proc, in, {}, r);
  const auto   at  = [&](float pitch, float morph) {
    return std::vector<RefEvent>{RefParam(change, ParamId::TransposeSt, pitch),
                                 RefParam(change, ParamId::FilterMorph, morph)};
  };
  RequireSame(got, RenderReference(Busy(), in, kRate, at(7.02f, 0.4f)), "UI edits");
  // One ULP of one value changes the output, so exactness is not vacuous here.
  const Stereo ulp = RenderReference(Busy(), in, kRate, at(7.02f, std::nextafter(0.4f, 1.0f)));
  REQUIRE_FALSE(SameBits(got.l, ulp.l));
}

TEST_CASE("the plain slider attachment edits and shows exact values") {
  BrainscapeProcessor proc;
  juce::Slider        pitchKnob, morphKnob, mixKnob, delayKnob, cutoffKnob;
  BrainscapePlainAttachment pitch(proc.Param(ParamId::TransposeSt), pitchKnob);
  BrainscapePlainAttachment morph(proc.Param(ParamId::FilterMorph), morphKnob);
  BrainscapePlainAttachment mix(proc.Param(ParamId::Mix), mixKnob);
  BrainscapePlainAttachment delay(proc.Param(ParamId::DelayMs), delayKnob);
  BrainscapePlainAttachment cutoff(proc.Param(ParamId::FilterCutoffHz), cutoffKnob);

  pitchKnob.setValue(0.6, juce::sendNotificationSync);  // a knob turn: the pot path
  REQUIRE(Bits(proc.Param(ParamId::TransposeSt).Plain()) == Bits(PlainFromNormalized(ParamId::TransposeSt, 0.6f)));

  REQUIRE(pitch.CommitText("7.02"));
  REQUIRE(Bits(proc.Param(ParamId::TransposeSt).Plain()) == Bits(7.02f));
  REQUIRE(pitch.CommitText("-0.37 st"));
  REQUIRE(Bits(proc.Param(ParamId::TransposeSt).Plain()) == Bits(-0.37f));
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
  REQUIRE(cutoff.CommitText("Kill"));  // the minimum, the wet kill, as it is shown
  REQUIRE(proc.Param(ParamId::FilterCutoffHz).Plain() == 40.0f);
  REQUIRE(cutoff.DisplayText() == "Kill");
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
  for (const ParamDescriptor& d : LeafRows()) {
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

TEST_CASE("typed text is read by the compiler's exact reader, typed forms included") {
  // mode-compiler.md §6.5, §10.4: text looser than JSON, with JSON's exact rounding.
  float v = 0.f;
  REQUIRE(ParsePlainText(ParamId::DelayMs, ".5 s", v));
  REQUIRE(v == 500.0f);
  REQUIRE(ParsePlainText(ParamId::DelayMs, "5. ms", v));
  REQUIRE(v == 5.0f);
  REQUIRE(ParsePlainText(ParamId::DelayMs, "05", v));
  REQUIRE(v == 5.0f);
  REQUIRE(ParsePlainText(ParamId::DelayMs, "+25", v));
  REQUIRE(v == 25.0f);
  REQUIRE(ParsePlainText(ParamId::DelayMs, "0.5e3", v));
  REQUIRE(v == 500.0f);
  REQUIRE(ParsePlainText(ParamId::DelayMs, "-.5", v));  // canonicalized: the minimum
  REQUIRE(v == 1.0f);
  REQUIRE(ParsePlainText(ParamId::Mix, "55%", v));
  REQUIRE(Bits(v) == 0x3F0CCCCDu);  // the bits canonical JSON writes as 0.55
  REQUIRE(ParsePlainText(ParamId::TransposeSt, "7.02 st", v));
  REQUIRE(Bits(v) == Bits(7.02f));
  // At a binary32 midpoint, ties go to even, as the compiler reads them: 1 + 2^-24.
  REQUIRE(ParsePlainText(ParamId::GrainSizeMs, "1.000000059604644775390625", v));
  REQUIRE(Bits(v) == 0x3F800000u);
  REQUIRE(ParsePlainText(ParamId::GrainSizeMs, "1.000000059604644775390626", v));
  REQUIRE(Bits(v) == 0x3F800001u);
  REQUIRE_FALSE(ParsePlainText(ParamId::DelayMs, ".", v));
  REQUIRE_FALSE(ParsePlainText(ParamId::DelayMs, "1..5", v));
  REQUIRE_FALSE(ParsePlainText(ParamId::DelayMs, "4e38 ms", v));  // past FLT_MAX
}

TEST_CASE("the text shown at a parameter's ends and default can be typed back") {
  // What the host shows can be typed in and shows the same: the named ends (Off, Kill, the
  // filter types) included, which a missing name left unparsable (review finding).
  for (const ParamDescriptor& d : LeafRows()) {
    for (const float v : {d.min, d.max, d.def}) {
      const juce::String shown = FormatPlainText(d.id, Canonicalize(d.id, v));
      INFO(d.name << ": " << shown);
      float back = 0.f;
      REQUIRE(ParsePlainText(d.id, shown, back));
      REQUIRE(FormatPlainText(d.id, back) == shown);
    }
  }
  float v = 0.f;
  REQUIRE(ParsePlainText(ParamId::FilterCutoffHz, "Kill", v));
  REQUIRE(Bits(v) == Bits(40.0f));
  REQUIRE(ParsePlainText(ParamId::FilterCutoffHz, "off", v));
  REQUIRE(Bits(v) == Bits(20000.0f));
}

TEST_CASE("session state round-trips bit for bit") {
  BrainscapeProcessor a;
  uint32_t            seed = 42u;
  for (const ParamDescriptor& d : LeafRows()) {
    const auto values = SampleValues(d.id, seed += 977u, 1);
    a.Param(d.id).SetPlainNotifyingHost(values.back());
  }
  a.Param(ParamId::TransposeSt).SetPlainNotifyingHost(7.02f);
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
  for (const ParamDescriptor& d : LeafRows()) {
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
    cut.insert(cut.end(), bytes.begin() + 12 + 8 * static_cast<long>(kNumLeafParams), bytes.end());
    WrapperState partial{};
    REQUIRE(DecodeState(cut.data(), cut.size(), partial));
    REQUIRE(partial.unknownIds == 1u);
    REQUIRE(partial.missingIds == kNumLeafParams - 1u);
    REQUIRE(Bits(partial.plain[0]) == Bits(st.plain[0]));
    REQUIRE(partial.plain[1] == FindParam(LeafId(1))->def);
  }
  // This build writes its Leaf rows in ascending order: IDs 1-26 and the wave-1 leaves the
  // sound revisions since 4 made Leaf rows (mode-compiler.md §7.5). Sessions saved at sound
  // revision 1 hold IDs 1-28: 27 and 28 retired into mode structure at sound revision 2, which
  // the plugin does not load yet (session v2 migrates them, mode-compiler.md §4.4, lane D), so
  // they are unknown, the wave-1 leaves they lack are missing (their defaults), and every other
  // value decodes unchanged. Rows of other kinds in a session (a macro, the effect volume, a
  // Reserved row) are unknown too.
  SECTION("v1 sessions of sound revision 1 decode; 27, 28 and other kinds are unknown") {
    WrapperState st{};
    REQUIRE(DecodeState(blob.getData(), blob.getSize(), st));
    uint32_t r1Leaves = 0;  // sound revision 1's rows that are still leaves: IDs 1-26
    for (size_t i = 0; i < kNumLeafParams; ++i) {
      if (FindParam(LeafId(i))->sinceRev == 1u) {
        REQUIRE(static_cast<uint32_t>(LeafId(i)) == i + 1u);
        ++r1Leaves;
      }
    }
    REQUIRE(r1Leaves == 26u);
    const auto put = [](std::vector<uint8_t>* out, uint32_t u) {
      for (int k = 0; k < 4; ++k) out->push_back(static_cast<uint8_t>(u >> (8 * k)));
    };
    std::vector<uint8_t> v2 = {'B', 'S', 'W', 'S', 1, 0, 0, 0,
                               static_cast<uint8_t>(kNumLeafParams), 0, 0, 0};
    for (size_t i = 0; i < kNumLeafParams; ++i) {
      put(&v2, static_cast<uint32_t>(LeafId(i)));
      put(&v2, Bits(st.plain[i]));
    }
    std::vector<uint8_t> now;
    EncodeState(st, now);
    REQUIRE(std::equal(v2.begin(), v2.end(), now.begin()));
    const std::vector<uint8_t> settings(now.begin() + static_cast<long>(v2.size()), now.end());

    std::vector<uint8_t> v1 = {'B', 'S', 'W', 'S', 1, 0, 0, 0, 28, 0, 0, 0};
    for (uint32_t id = 1; id <= 28; ++id) {
      put(&v1, id);
      put(&v1, id <= 26u ? Bits(st.plain[id - 1u]) : Bits(1.0f));  // onset and marks on
    }
    v1.insert(v1.end(), settings.begin(), settings.end());
    WrapperState old{};
    REQUIRE(DecodeState(v1.data(), v1.size(), old));
    REQUIRE(old.unknownIds == 2u);
    REQUIRE(old.missingIds == kNumLeafParams - r1Leaves);
    // Sound revision 1's leaves decode unchanged; the later leaves take their defaults.
    const auto sameOrDefault = [&](const WrapperState& got) {
      for (size_t i = 0; i < kNumLeafParams; ++i) {
        const ParamDescriptor& d = *FindParam(LeafId(i));
        INFO(d.name);
        REQUIRE(Bits(got.plain[i]) == (d.sinceRev == 1u ? Bits(st.plain[i]) : Bits(d.def)));
      }
    };
    sameOrDefault(old);

    std::vector<uint8_t> extra(v1.begin(), v1.begin() + 12 + 8 * 28);
    extra[8] = 31;  // three more leaves, of other kinds
    put(&extra, static_cast<uint32_t>(ParamId::LevelDb));  // Reserved (W3)
    put(&extra, Bits(-3.0f));
    put(&extra, static_cast<uint32_t>(ParamId::MacroTime));  // Macro
    put(&extra, Bits(0.25f));
    put(&extra, static_cast<uint32_t>(ParamId::EffectVolumeDb));  // Global
    put(&extra, Bits(-6.0f));
    extra.insert(extra.end(), settings.begin(), settings.end());
    WrapperState mixed{};
    REQUIRE(DecodeState(extra.data(), extra.size(), mixed));
    REQUIRE(mixed.unknownIds == 5u);
    REQUIRE(mixed.missingIds == kNumLeafParams - r1Leaves);
    sameOrDefault(mixed);
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

TEST_CASE("MIDI note-on fires a grain at its sample offset, at every host block pattern") {
  Preset preset = Busy();  // the default mode: no onset source
  const Stereo in = MakeInput(72000);
  // Chunk and block edges, odd offsets and same-frame pairs.
  const std::vector<int> notes = {0, 1, 511, 512, 513, 4095, 4096, 30037, 30038, 40192, 51200, 51200, 51201};
  auto trig = [](const std::vector<int>& frames) {
    std::vector<RefEvent> ev;
    for (int f : frames) ev.push_back(RefTrigger(f, Engine::TriggerSource::MidiNote, 100.0f / 127.0f));
    return ev;
  };
  const Stereo want = RenderReference(preset, in, kRate, trig(notes));
  for (const auto& pattern : std::vector<std::vector<int>>{
           {1}, {7}, {256}, {512}, {513}, {4096}, {0, 480}, {0, 0, 1, 0, 7, 512, 513, 0, 4096, 37, 0, 441}}) {
    auto       proc = MakeProcessor(preset, {});
    HostRender r;
    r.pattern = pattern;
    r.noteOns = notes;
    RequireSame(RenderProcessor(*proc, in, {}, r), want, PatternName(pattern).c_str());
  }
  std::vector<int> early = notes;
  early[7]               = 30036;
  REQUIRE_FALSE(SameBits(want.l, RenderReference(preset, in, kRate, trig(early)).l));
  REQUIRE_FALSE(SameBits(want.l, RenderReference(preset, in, kRate).l));
}

TEST_CASE("host automation applies at the first frame of its block, at every host block pattern") {
  const Stereo in = MakeInput(48000);
  // Mix: automatable under every host model (mode-compiler.md §3.6).
  REQUIRE(BrainscapeProcessor().Param(ParamId::Mix).isAutomatable());
  for (const auto& pattern : std::vector<std::vector<int>>{
           {1}, {7}, {256}, {512}, {513}, {4096}, {0, 480}, {0, 0, 1, 0, 7, 512, 513, 0, 4096, 37, 0, 441}}) {
    auto       proc      = MakeProcessor(Busy(), {});
    int        appliedAt = -1;
    HostRender r;
    r.pattern     = pattern;
    r.beforeBlock = [&](int pos) {  // a VST3 parameter change, as the wrapper delivers it
      if (appliedAt >= 0 || pos < 20011) return;
      proc->Param(ParamId::Mix).setValue(0.73f);
      appliedAt = pos;
    };
    const Stereo got  = RenderProcessor(*proc, in, {}, r);
    const float  sent = PlainFromNormalized(ParamId::Mix, 0.73f);
    REQUIRE(Bits(proc->Param(ParamId::Mix).Plain()) == Bits(sent));
    RequireSame(got, RenderReference(Busy(), in, kRate, {RefParam(appliedAt, ParamId::Mix, sent)}),
                PatternName(pattern).c_str());
  }
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
    p.PostAt(10007, {E::Type::Param, E::Source::Ui, id(ParamId::TransposeSt), 5.0f});
    p.PostAt(20011, {E::Type::Trigger, E::Source::Ui, 0u, 1.0f});
    p.PostAt(30000, {E::Type::Param, E::Source::Ui, id(ParamId::Mix), 0.45f});  // after the host's
    p.PostAt(30000, {E::Type::Param, E::Source::Host, id(ParamId::Mix), 0.4f});
    p.PostAt(40013, {E::Type::Freeze, E::Source::Ui, 0u, 1.0f});
  };
  const auto ref = [&](int triggerAt) {
    return RenderReference(Busy(), in, kRate,
                           {RefParam(0, ParamId::Feedback, 0.5f), RefParam(10007, ParamId::TransposeSt, 5.0f),
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
    REQUIRE(proc->Param(ParamId::TransposeSt).Plain() == 5.0f);  // mirrors follow once applied
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

// The per-leaf touched set that replaced the 32-bit mask (mode-compiler.md §10.4): an event on
// any Leaf row, the last ordinal included, writes that leaf's mirror back once applied, and
// SetParam events on rows of other kinds (Reserved, Macro) reach no mirror and no engine state.
// (A macro moves by its own event; the effect volume, a Global row, takes SetParam.)
TEST_CASE("every leaf's mirror follows its applied event; rows of other kinds change nothing") {
  using E           = WrapperEvent;
  const Stereo in   = MakeInput(4800);
  auto         proc = MakeProcessor({}, {});
  std::vector<float>    sent(kNumLeafParams);
  std::vector<RefEvent> ref;
  for (size_t i = 0; i < kNumLeafParams; ++i) {
    const ParamDescriptor& d = *FindParam(LeafId(i));
    float v = Canonicalize(d.id, d.min + 0.25f * (d.max - d.min));
    if (Bits(v) == Bits(d.def)) v = Canonicalize(d.id, d.min + 0.75f * (d.max - d.min));
    INFO(d.name);
    REQUIRE(Bits(v) != Bits(proc->Param(d.id).Plain()));
    sent[i] = v;
    proc->PostAt(0, {E::Type::Param, E::Source::Ui, static_cast<uint32_t>(d.id), v});
    ref.push_back(RefParam(0, d.id, v));
  }
  for (const ParamId other : {ParamId::LevelDb, ParamId::MacroActivity, ParamId::PerfLoopLevel}) {
    REQUIRE_FALSE(IsLeaf(other));
    proc->PostAt(0, {E::Type::Param, E::Source::Ui, static_cast<uint32_t>(other), FindParam(other)->min});
  }
  HostRender r;
  r.pattern        = {480};
  const Stereo got = RenderProcessor(*proc, in, {}, r);
  for (size_t i = 0; i < kNumLeafParams; ++i) {
    INFO(FindParam(LeafId(i))->name);
    REQUIRE(Bits(proc->Param(LeafId(i)).Plain()) == Bits(sent[i]));
  }
  RequireSame(got, RenderReference({}, in, kRate, ref), "the leaf events alone");
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
  SECTION("offline, the mode passed before every block: only the switch to offline arms") {
    // The VST3, VST2 and LV2 wrappers call setNonRealtime before every block. The export
    // starts while the transport already plays in real time; re-arming at every block
    // restarted the engine every 10.7 ms.
    auto       proc = withOption(true);
    HostRender offline;
    offline.pattern     = {512};
    offline.beforeBlock = [&](int) { proc->setNonRealtime(true); };
    head.playing        = true;
    RenderProcessor(*proc, preroll, {}, {{441}});
    // Nothing had played since prepareToPlay, so real time restarted in place, no spare.
    REQUIRE(proc->GetStatus().lastStart == BrainscapeProcessor::TransportStart::Restarted);
    const Stereo first = RenderProcessor(*proc, take, {}, offline);
    head.playing       = false;
    RenderProcessor(*proc, gap, {}, offline);
    head.playing        = true;
    const Stereo second = RenderProcessor(*proc, take, {}, offline);
    head.playing        = false;
    RequireSame(first, ref, "an export starting mid-play");
    RequireSame(second, ref, "a second export");
    proc->setPlayHead(nullptr);
  }
}

// A host automation lane on Mix, 0.2 -> 0.9 over the take, sent per block (as VST3 hands
// over a block's parameter changes). Each bounce must start from the values in effect at its
// first frame, whatever the last playback left: the restart folds the first block's
// automation into its Exact load.
TEST_CASE("restart on transport start: an automated passage bounces the same every time") {
  const Stereo source = MakeInput(4 * 48000);
  const Stereo take   = Slice(source, 48000, 120000);
  const Stereo gap    = Slice(source, 130000, 140000);
  const auto   value  = [&](int pos) {
    return NormalizedFromPlain(ParamId::Mix, 0.2f + 0.7f * static_cast<float>(pos) / static_cast<float>(take.l.size()));
  };
  // The reference: the Exact load with the lane's first value, then each block's change.
  Preset                start = Busy();
  std::vector<RefEvent> changes;
  {
    BrainscapeProcessor lane;
    lane.Param(ParamId::Mix).setValue(value(0));
    start.push_back({ParamId::Mix, lane.Param(ParamId::Mix).Plain()});
    for (int pos = 480; pos < static_cast<int>(take.l.size()); pos += 480) {
      const float before = lane.Param(ParamId::Mix).Plain();
      lane.Param(ParamId::Mix).setValue(value(pos));
      if (Bits(lane.Param(ParamId::Mix).Plain()) != Bits(before)) {
        changes.push_back(RefParam(pos, ParamId::Mix, lane.Param(ParamId::Mix).Plain()));
      }
    }
  }
  REQUIRE(changes.size() > 100u);
  const Stereo ref = RenderReference(start, take, kRate, changes);
  TestPlayHead head;
  auto         proc = MakeProcessor(Busy(), {});
  WrapperSettings s = proc->GetSettings();
  s.restartOnStart  = true;
  proc->SetSettings(s);
  proc->setPlayHead(&head);
  HostRender automated;
  automated.pattern     = {480};
  automated.beforeBlock = [&](int pos) { proc->Param(ParamId::Mix).setValue(value(pos)); };
  const auto bounce = [&]() {
    head.playing     = true;
    const Stereo out = RenderProcessor(*proc, take, {}, automated);
    head.playing     = false;
    return out;
  };
  HostRender locate;  // stopped at the passage's start: the host sends the lane's value there
  locate.pattern     = {480};
  locate.beforeBlock = [&](int) { proc->Param(ParamId::Mix).setValue(value(0)); };

  SECTION("offline") {
    proc->setNonRealtime(true);
    RenderProcessor(*proc, gap, {}, {{441}});  // stopped on another Mix
    RequireSame(bounce(), ref, "first offline bounce");
    RenderProcessor(*proc, gap, {}, {{441}});  // stopped on the lane's last value
    RequireSame(bounce(), ref, "second offline bounce");
    REQUIRE(proc->GetStatus().lastStart == BrainscapeProcessor::TransportStart::Restarted);
  }
  SECTION("real time: the spare must hold the first block's values") {
    RenderProcessor(*proc, gap, {}, locate);
    REQUIRE(WaitForSpare(*proc));
    RequireSame(bounce(), ref, "first real-time bounce");
    REQUIRE(proc->GetStatus().lastStart == BrainscapeProcessor::TransportStart::Restarted);
    // Stopped on the lane's last value, the spare restarts with it: the next start moves
    // Mix, so the engine runs on and says so.
    RenderProcessor(*proc, gap, {}, {{441}});
    REQUIRE(WaitForSpare(*proc));
    REQUIRE_FALSE(SameBits(bounce().l, ref.l));
    REQUIRE(proc->GetStatus().lastStart == BrainscapeProcessor::TransportStart::SpareNotReady);
    RenderProcessor(*proc, gap, {}, locate);
    REQUIRE(WaitForSpare(*proc));
    RequireSame(bounce(), ref, "after a locate");
    REQUIRE(proc->GetStatus().lastStart == BrainscapeProcessor::TransportStart::Restarted);
  }
  proc->setPlayHead(nullptr);
}

TEST_CASE("restart on transport start: bounces with MIDI match the reference") {
  Preset preset = Busy();  // the default mode: no onset source
  const Stereo           source = MakeInput(4 * 48000);
  const Stereo           pre    = Slice(source, 0, 24000);
  const Stereo           take   = Slice(source, 48000, 120000);
  const std::vector<int> notes  = {0, 333, 24001};
  std::vector<RefEvent>  ev;
  for (int f : notes) ev.push_back(RefTrigger(f, Engine::TriggerSource::MidiNote, 100.0f / 127.0f));
  const Stereo want = RenderReference(preset, take, kRate, ev);
  for (const bool offline : {true, false}) {
    for (const auto& pattern : std::vector<std::vector<int>>{{441}, {4096}, {0, 37, 1}}) {
      INFO((offline ? "offline " : "real time ") << PatternName(pattern));
      TestPlayHead    head;
      auto            proc = MakeProcessor(preset, {});
      WrapperSettings s    = proc->GetSettings();
      s.restartOnStart     = true;
      proc->SetSettings(s);
      proc->setNonRealtime(offline);
      proc->setPlayHead(&head);
      RenderProcessor(*proc, pre, {}, {{441}});
      if (!offline) REQUIRE(WaitForSpare(*proc));
      HostRender r;
      r.pattern        = pattern;
      r.noteOns        = notes;
      head.playing     = true;
      const Stereo got = RenderProcessor(*proc, take, {}, r);
      head.playing     = false;
      proc->setPlayHead(nullptr);
      RequireSame(got, want, "bounce with note-ons");
    }
  }
}

// PostAt's contract: a stamp counts on the timeline it was made on, so one made before a
// restart is void, even when its frame has already passed (it would apply at once).
TEST_CASE("a scripted stamp made before a transport-start restart is void, late or not") {
  using E             = WrapperEvent;
  const Stereo source = MakeInput(3 * 48000);
  const Stereo pre    = Slice(source, 0, 4800);
  const Stereo take   = Slice(source, 48000, 96000);
  const Stereo clean  = RenderReference(Busy(), take);
  REQUIRE_FALSE(SameBits(clean.l, RenderReference(Busy(), take, kRate, {RefParam(0, ParamId::Mix, 0.25f)}).l));
  for (const uint64_t stamp : {uint64_t{100}, uint64_t{4800}, uint64_t{4801}}) {
    for (const bool offline : {true, false}) {
      INFO("stamp " << stamp << (offline ? " offline" : " real time"));
      TestPlayHead    head;
      auto            proc = MakeProcessor(Busy(), {});
      WrapperSettings s    = proc->GetSettings();
      s.restartOnStart     = true;
      proc->SetSettings(s);
      proc->setNonRealtime(offline);
      proc->setPlayHead(&head);
      RenderProcessor(*proc, pre, {}, {{480}});  // stopped: the timeline is at frame 4800
      if (!offline) REQUIRE(WaitForSpare(*proc));
      proc->PostAt(stamp, {E::Type::Param, E::Source::Ui, static_cast<uint32_t>(ParamId::Mix), 0.25f});
      head.playing     = true;
      const Stereo got = RenderProcessor(*proc, take, {}, {{480}});
      head.playing     = false;
      proc->setPlayHead(nullptr);
      REQUIRE(proc->GetStatus().lastStart == BrainscapeProcessor::TransportStart::Restarted);
      RequireSame(got, clean, "a void stamp");
    }
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
  float        values[kNumLeafParams];
  for (size_t i = 0; i < kNumLeafParams; ++i) values[i] = all[i].second;

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

    // The same preset and input through `bspc render`'s S0 (tools/audition): one hash.
    {
      bsa::Preset leafOnly;
      leafOnly.state = std::shared_ptr<const PresetState>(proc->CurrentPreset());
      bsa::SuiteOptions o;
      o.scripts = {"S0"};
      o.wav     = false;
      o.outDir  = dir.getChildFile("s0").getFullPathName().toStdString();
      bsa::Renderer          renderer;
      const bsa::SuiteResult s0 = bsa::RunSuite(renderer, leafOnly, {&leafOnly}, o);
      bool                   found = false;
      for (const bsa::Rendered& x : s0.renders) {
        if (x.plan.name != "S0.engaged.plucks") continue;
        found = true;
        REQUIRE(juce::String(x.hashes.whole) == result.outputSha256);
      }
      REQUIRE(found);
    }

    // Independent SHA-256s of the interleaved little-endian float32: the whole render and
    // each 1 s segment, as the golden harness hashes them.
    golden::Sha256           sha, segment;  // Hex() finishes a digest and starts the next
    std::vector<std::string> segments;
    for (size_t i = 0; i < want.l.size(); ++i) {
      uint8_t bytes[8];
      for (int c = 0; c < 2; ++c) {
        const uint32_t u = Bits(c == 0 ? want.l[i] : want.r[i]);
        for (int b = 0; b < 4; ++b) bytes[4 * c + b] = static_cast<uint8_t>(u >> (8 * b));
      }
      sha.Update(bytes, 8);
      segment.Update(bytes, 8);
      if ((i + 1) % 48000 == 0 || i + 1 == want.l.size()) segments.push_back(segment.Hex());
    }
    REQUIRE(segments.size() == 20u);  // 10 s of signal, a 10 s tail: S0's input
    REQUIRE(result.outputSha256.toStdString() == sha.Hex());
    const juce::var recipe = juce::JSON::parse(dir.getChildFile("take.recipe.json"));
    REQUIRE(recipe["outputSha256"].toString() == result.outputSha256);
    REQUIRE(recipe["outputSegmentSha256"].size() == static_cast<int>(segments.size()));
    for (size_t k = 0; k < segments.size(); ++k) {
      INFO("segment " << k);
      REQUIRE(recipe["outputSegmentSha256"][static_cast<int>(k)].toString().toStdString() == segments[k]);
    }
    REQUIRE(recipe["preset"].size() == static_cast<int>(kNumLeafParams));
    REQUIRE(recipe["preset"][1]["bits"].toString() == juce::String::toHexString(static_cast<juce::int64>(Bits(values[1]))).paddedLeft('0', 8));
    REQUIRE(static_cast<int>(recipe["soundRevision"]) == static_cast<int>(kSoundRevision));
    reader.reset();
    dir.deleteRecursively();
  }
}

// ── Modes, macros and device settings (mode-compiler.md §3.4-§3.8, §9.2) ─────────────────

TEST_CASE("a document loaded before anything has played is an Exact load of its mode") {
  const auto   state = CompiledState("engram.json");
  const Stereo in    = MakeInput(48000);
  auto         proc  = MakeProcessor(Busy(), {});
  LoadReport   report;
  REQUIRE(proc->LoadPresetState(*state, &report));
  REQUIRE(report.exact);
  REQUIRE_FALSE(proc->GetStatus().lastLoadInexact);
  const Stereo got = RenderProcessor(*proc, in, {}, {{441}});
  RequireSame(got, RenderStateReference(*state, in), "engram, Exact");
  // The mirrors hold the document: its leaves, and its CTRL positions as the macros'.
  for (uint32_t k = 0; k < state->leafCount; ++k) {
    INFO(FindParam(static_cast<ParamId>(state->leaves[k].id))->name);
    REQUIRE(Bits(proc->Param(static_cast<ParamId>(state->leaves[k].id)).Plain()) == Bits(state->leaves[k].value));
  }
  REQUIRE(Bits(proc->Macro(ParamId::MacroTime).Plain()) == Bits(0.5f));
  REQUIRE(Bits(proc->Macro(ParamId::MacroSpace).Plain()) == Bits(0.24f));
  REQUIRE(Bits(proc->Macro(ParamId::MacroFilter).Plain()) == Bits(1.0f));
  // What the wrapper plays, as a preset: the same state.
  const auto now = proc->CurrentPreset();
  REQUIRE(now->leafCount == state->leafCount);
  for (uint32_t k = 0; k < now->leafCount; ++k) REQUIRE(Bits(now->leaves[k].value) == Bits(state->leaves[k].value));
  REQUIRE(std::memcmp(&now->mode, &state->mode, sizeof(ModeBlob)) == 0);
  REQUIRE(std::memcmp(&now->control, &state->control, sizeof(ControlState)) == 0);
}

TEST_CASE("a document loaded while running is a Spillover load with Trails at the next block") {
  const auto   state  = CompiledState("engram.json");
  const Stereo in     = MakeInput(96000);
  const int    change = 48000;
  auto         proc   = MakeProcessor(Busy(), {});
  proc->Freeze().setValueNotifyingHost(1.0f);
  HostRender r;
  r.pattern     = {480};
  r.beforeBlock = [&](int pos) {
    if (pos == change) REQUIRE(proc->LoadPresetState(*state));
  };
  const Stereo got = RenderProcessor(*proc, in, {}, r);
  RequireSame(got, RenderReference(Busy(), in, kRate, {RefFreeze(0, true), RefLoad(change, state.get()),
                                                       RefFreeze(change, false)}),
              "engram over Busy at frame 48000");
  REQUIRE_FALSE(proc->Freeze().get());  // the load turned it off
}

TEST_CASE("a mode whose structure is invalid is refused and changes nothing") {
  auto state = CompiledState("engram.json");
  state->mode.macros.macroCount = 9;  // over kMaxMacros
  BrainscapeProcessor proc;
  const float before = proc.Param(ParamId::Mix).Plain();
  LoadReport  report;
  REQUIRE_FALSE(proc.LoadPresetState(*state, &report));
  REQUIRE(report.invalidMode);
  REQUIRE(Bits(proc.Param(ParamId::Mix).Plain()) == Bits(before));
}

// A host's macro move (§3.6) is a MacroMove: the engine applies the mode's targets, and the
// wrapper mirrors the same fan-out on the leaves, computed by the same evaluator, so a session
// saved after the move holds what plays. At every host block pattern.
TEST_CASE("a host macro move is a MacroMove; the leaf mirrors follow its fan-out") {
  const auto   state = CompiledState("engram.json");
  const Stereo in    = MakeInput(48000);
  for (const auto& pattern : std::vector<std::vector<int>>{{480}, {1, 7, 513}, {0, 4096, 37}}) {
    auto proc = MakeProcessor({}, {});
    REQUIRE(proc->LoadPresetState(*state));
    int        appliedAt = -1;
    HostRender r;
    r.pattern     = pattern;
    r.beforeBlock = [&](int pos) {
      if (appliedAt >= 0 || pos < 20011) return;
      proc->Macro(ParamId::MacroTime).setValue(0.8f);
      proc->Macro(ParamId::MacroActivity).setValue(0.3f);
      appliedAt = pos;
    };
    const Stereo got  = RenderProcessor(*proc, in, {}, r);
    const float  time = PlainFromNormalized(ParamId::MacroTime, 0.8f);
    const float  act  = PlainFromNormalized(ParamId::MacroActivity, 0.3f);
    RequireSame(got, RenderStateReference(*state, in, kRate,
                                          {RefMacro(appliedAt, ParamId::MacroTime, time),
                                           RefMacro(appliedAt, ParamId::MacroActivity, act)}),
                PatternName(pattern).c_str());
    for (const auto& move : {std::make_pair(ParamId::MacroTime, time), std::make_pair(ParamId::MacroActivity, act)}) {
      REQUIRE(Bits(proc->Macro(move.first).Plain()) == Bits(move.second));
      PresetLeaf   out[kMaxMacroTargets];
      const size_t n = EvalMacro(state->mode, move.first, move.second, out, kMaxMacroTargets);
      REQUIRE(n > 0u);
      for (size_t k = 0; k < n; ++k) {
        INFO(FindParam(static_cast<ParamId>(out[k].id))->name);
        REQUIRE(Bits(proc->Param(static_cast<ParamId>(out[k].id)).Plain()) == Bits(out[k].value));
      }
    }
    REQUIRE(proc->Param(ParamId::DelayTimeMs).Plain() > 405.0f);  // Time 0.8 is later than 0.5
  }
}

TEST_CASE("a macro the mode leaves undefined moves nothing") {
  const auto   state = CompiledState("engram.json");  // no aux macros
  const Stereo in    = MakeInput(9600);
  auto         proc  = MakeProcessor({}, {});
  REQUIRE(proc->LoadPresetState(*state));
  proc->PostAt(4800, {WrapperEvent::Type::Macro, WrapperEvent::Source::Ui,
                      static_cast<uint32_t>(ParamId::MacroAux1), 0.9f});
  const Stereo got = RenderProcessor(*proc, in, {}, {{480}});
  RequireSame(got, RenderStateReference(*state, in), "aux1 undefined");
}

TEST_CASE("an expression move fans out through CTRL's assignments") {
  const auto   state = CompiledState("controls.json");
  const Stereo in    = MakeInput(48000);
  auto         proc  = MakeProcessor({}, {});
  REQUIRE(proc->LoadPresetState(*state));
  REQUIRE(state->control.exprCount > 0u);
  int        appliedAt = -1;
  HostRender r;
  r.pattern     = {441};
  r.beforeBlock = [&](int pos) {
    if (appliedAt >= 0 || pos < 12000) return;
    proc->Expression().setValue(0.6f);
    appliedAt = pos;
  };
  const Stereo got = RenderProcessor(*proc, in, {}, r);
  const float  pos = PlainFromNormalized(ParamId::PerfExpression, 0.6f);
  RequireSame(got, RenderStateReference(*state, in, kRate, {RefExpression(appliedAt, pos)}), "expression 0.6");
  PresetLeaf   out[kMaxTargets];
  const size_t n = EvalExpression(state->mode, state->control, pos, out, kMaxTargets);
  REQUIRE(n > 0u);
  for (size_t k = 0; k < n; ++k) {
    if (!IsLeaf(out[k].id)) continue;
    // A leaf two assignments write ends on the later one.
    bool later = false;
    for (size_t j = k + 1; j < n; ++j) later = later || out[j].id == out[k].id;
    if (later) continue;
    INFO(FindParam(static_cast<ParamId>(out[k].id))->name);
    REQUIRE(Bits(proc->Param(static_cast<ParamId>(out[k].id)).Plain()) == Bits(out[k].value));
  }
}

// global.effect_volume_db (§3.8): a Global row the engine keeps across loads and restarts, so
// the wrapper restores it after Init and keeps it in the session, never in a preset.
TEST_CASE("the effect volume is a device setting: kept across loads, re-prepares and sessions") {
  const auto   state = CompiledState("engram.json");
  const Stereo in    = MakeInput(48000);
  SECTION("an edit applies at the next block, and a document load keeps it") {
    auto proc = MakeProcessor({}, {});
    proc->EffectVolume().SetPlainNotifyingHost(-6.0f);
    HostRender r;
    r.pattern     = {480};
    r.beforeBlock = [&](int pos) {
      if (pos == 24000) REQUIRE(proc->LoadPresetState(*state));
    };
    const Stereo got = RenderProcessor(*proc, in, {}, r);
    RequireSame(got, RenderReference({}, in, kRate, {RefParam(0, ParamId::EffectVolumeDb, -6.0f),
                                                     RefLoad(24000, state.get())}),
                "-6 dB, then engram");
    REQUIRE(Bits(proc->EffectVolume().Plain()) == Bits(-6.0f));
  }
  SECTION("Init restores it before the start state's Exact load") {
    auto proc = std::make_unique<BrainscapeProcessor>();
    proc->EffectVolume().SetPlainNotifyingHost(-9.5f);
    REQUIRE(proc->LoadPresetState(*state));
    proc->setRateAndBufferSizeDetails(kRate, 512);
    proc->prepareToPlay(kRate, 512);
    const Stereo got = RenderProcessor(*proc, in, {}, {{512}});
    RequireSame(got, RenderStateReference(*state, in, kRate, {}, {}, -9.5f), "-9.5 dB from Init");
  }
  SECTION("the session keeps it; a v1 session plays the default mode") {
    BrainscapeProcessor a;
    REQUIRE(a.LoadPresetState(*state));
    a.EffectVolume().SetPlainNotifyingHost(-3.0f);
    juce::MemoryBlock blob;
    a.getStateInformation(blob);
    auto b = MakeProcessor({}, {});
    REQUIRE(b->LoadPresetState(*CompiledState("controls.json")));
    b->setStateInformation(blob.getData(), static_cast<int>(blob.getSize()));
    REQUIRE(Bits(b->EffectVolume().Plain()) == Bits(-3.0f));
    const ModeState mode = b->CurrentMode();
    const ModeState none{};
    REQUIRE(std::memcmp(&mode.mode, &none.mode, sizeof(ModeBlob)) == 0);
    REQUIRE(Bits(b->Macro(ParamId::MacroTime).Plain()) == Bits(0.5f));
    const Stereo got = RenderProcessor(*b, in, {}, {{480}});
    auto         leaves = std::make_unique<PresetState>();
    *leaves             = *state;
    const ModeState defaults{};
    leaves->mode    = defaults.mode;
    leaves->control = defaults.control;
    RequireSame(got, RenderStateReference(*leaves, in, kRate, {RefParam(0, ParamId::EffectVolumeDb, -3.0f)}),
                "engram's leaves on the default mode, -3 dB");
  }
}

TEST_CASE("restart on transport start restarts with the document's mode") {
  const auto   state   = CompiledState("engram.json");
  const Stereo source  = MakeInput(4 * 48000);
  const Stereo preroll = Slice(source, 0, 24000);
  const Stereo take    = Slice(source, 48000, 120000);
  const Stereo gap     = Slice(source, 130000, 140000);
  const Stereo ref     = RenderStateReference(*state, take, kRate, {}, {}, -2.0f);
  TestPlayHead head;
  auto         proc = MakeProcessor({}, {});
  REQUIRE(proc->LoadPresetState(*state));
  proc->EffectVolume().SetPlainNotifyingHost(-2.0f);
  WrapperSettings s = proc->GetSettings();
  s.restartOnStart  = true;
  proc->SetSettings(s);
  proc->setPlayHead(&head);
  const auto play = [&](const Stereo& in, std::vector<int> pattern) {
    head.playing     = true;
    const Stereo out = RenderProcessor(*proc, in, {}, {std::move(pattern)});
    head.playing     = false;
    return out;
  };
  RenderProcessor(*proc, preroll, {}, {{441}});
  REQUIRE(WaitForSpare(*proc));
  RequireSame(play(take, {441}), ref, "real time: the spare holds the mode and the volume");
  REQUIRE(proc->GetStatus().lastStart == BrainscapeProcessor::TransportStart::Restarted);
  RenderProcessor(*proc, gap, {}, {{441}});
  proc->setNonRealtime(true);
  RequireSame(play(take, {512}), ref, "offline: restarted in place with the mode");
  proc->setPlayHead(nullptr);
}

// ── The curation slice's document (mode-compiler.md §9.1) ─────────────────────────────────

namespace {

// A scratch directory of the test's own, removed afterwards.
struct ScratchDir {
  juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                       .getNonexistentChildFile("brainscape-curation", "", false);
  ScratchDir() { REQUIRE(dir.createDirectory()); }
  ~ScratchDir() { dir.deleteRecursively(); }
  juce::File Copy(const char* name) const {
    const juce::File to = dir.getChildFile(name);
    REQUIRE(juce::File(BRAINSCAPE_TEST_DATA).getChildFile(name).copyFileTo(to));
    return to;
  }
};

// Runs the processor (silence in) so the mirrors follow what it applied.
void Pump(BrainscapeProcessor& p, int frames = 4800) {
  juce::AudioBuffer<float> buffer(2, 480);
  juce::MidiBuffer         midi;
  for (int done = 0; done < frames; done += 480) {
    buffer.clear();
    p.processBlock(buffer, midi);
  }
}

std::string Text(const juce::File& f) { return f.loadFileAsString().toStdString(); }

std::unique_ptr<bsc::Document> ReadBack(const juce::File& f) {
  auto                      d = std::make_unique<bsc::Document>();
  std::vector<bsc::Finding> found;
  REQUIRE(bsc::ReadDocumentText(Text(f), {}, d.get(), &found));
  return d;
}

float StoredPositionOf(const bsc::Document& d, ParamId macro) {
  for (uint32_t k = 0; k < d.state->control.macroCount; ++k) {
    if (d.state->control.positions[k].macroId == static_cast<uint32_t>(macro)) return d.state->control.positions[k].position;
  }
  return -1.f;
}

float LeafOf(const bsc::Document& d, ParamId id) { return bsc::FloatOf(d.LeafBits(static_cast<uint32_t>(id))); }

float Eval1(const PresetState& s, ParamId macro, float position, ParamId leaf) {
  PresetLeaf   out[kMaxMacroTargets];
  const size_t n = EvalMacro(s.mode, macro, position, out, kMaxMacroTargets);
  for (size_t k = 0; k < n; ++k) {
    if (out[k].id == static_cast<uint32_t>(leaf)) return out[k].value;
  }
  return -1.f;
}

}  // namespace

TEST_CASE("the curation session opens a document, plays it and saves it back canonical") {
  ScratchDir         scratch;
  const juce::File   json = scratch.Copy("engram.json");
  const std::string  original = Text(json);
  auto               proc = MakeProcessor({}, {});
  CurationSession&   s    = proc->Curation();
  juce::String       error;
  REQUIRE(s.Open(json, &error));
  INFO(error);
  REQUIRE(s.HasDocument());
  REQUIRE(s.DocumentFile() == json);
  REQUIRE_FALSE(s.WritesPackage());
  REQUIRE(s.StampCurrent());
  REQUIRE_FALSE(s.Dirty());
  REQUIRE(s.PendingDerives().empty());
  REQUIRE(s.MacroName(ParamId::MacroActivity) == "Smear");  // the document's display name
  REQUIRE(s.MacroName(ParamId::MacroTime) == "Time");
  REQUIRE(s.MacroDefined(ParamId::MacroSpace));
  REQUIRE_FALSE(s.MacroDefined(ParamId::MacroAux1));
  REQUIRE(s.TargetsOf(ParamId::DelayTimeMs).size() == 1u);
  REQUIRE(s.TargetsOf(ParamId::DelayTimeMs)[0].macro == ParamId::MacroTime);
  const auto state = CompiledState("engram.json");
  REQUIRE(Bits(proc->Param(ParamId::DelayTimeMs).Plain()) == Bits(405.0f));

  SECTION("an unchanged document saves back byte for byte") {
    REQUIRE_FALSE(s.SaveChangesFile());
    const auto r = s.Save();
    INFO(r.message);
    REQUIRE(r.written);
    REQUIRE(r.compiled);
    REQUIRE(r.derived.empty());
    REQUIRE(Text(json) == original);
  }
  SECTION("a macro move saves its position, and the leaves derived from it") {
    proc->Macro(ParamId::MacroTime).setValue(0.8f);  // a host move: a MacroMove
    Pump(*proc);
    s.Refresh();
    REQUIRE(s.Dirty());
    REQUIRE(s.PendingDerives().empty());  // the mirrors already hold the fan-out
    const auto r = s.Save();
    REQUIRE(r.compiled);
    REQUIRE(r.derived.empty());
    const auto back = ReadBack(json);
    REQUIRE(Bits(StoredPositionOf(*back, ParamId::MacroTime)) == Bits(0.8f));
    REQUIRE(Bits(LeafOf(*back, ParamId::DelayTimeMs)) == Bits(Eval1(*state, ParamId::MacroTime, 0.8f, ParamId::DelayTimeMs)));
    REQUIRE(back->stamped);
    REQUIRE(back->soundRev == kSoundRevision);
    REQUIRE_FALSE(s.Dirty());
    REQUIRE(Bits(proc->Macro(ParamId::MacroTime).Plain()) == Bits(0.8f));
    // Canonical: formatting the written text changes nothing.
    std::string               formatted;
    std::vector<bsc::Finding> found;
    REQUIRE(bsc::FormatText(Text(json), &formatted, &found));
    REQUIRE(formatted == Text(json));
  }
  SECTION("a hand-edited targeted leaf is derived back on save, unless detached") {
    proc->Param(ParamId::DelayFb).SetPlainNotifyingHost(0.3f);  // Repeats at 0.5 gives 0.45
    Pump(*proc);
    s.Refresh();
    REQUIRE(s.Dirty());
    REQUIRE(s.SaveChangesFile());
    REQUIRE(s.PendingDerives().size() == 1u);
    REQUIRE(s.PendingDerives()[0].find("post.delay.fb") != std::string::npos);
    auto r = s.Save();
    REQUIRE(r.derived.size() == 1u);
    REQUIRE(Bits(proc->Param(ParamId::DelayFb).Plain()) == Bits(0.45f));  // what plays is what saved
    REQUIRE(Bits(LeafOf(*ReadBack(json), ParamId::DelayFb)) == Bits(0.45f));

    s.SetDetached(ParamId::DelayFb, true);
    proc->Param(ParamId::DelayFb).SetPlainNotifyingHost(0.3f);
    Pump(*proc);
    s.Refresh();
    REQUIRE(s.PendingDerives().empty());
    r = s.Save();
    REQUIRE(r.derived.empty());
    const auto back = ReadBack(json);
    REQUIRE(Bits(LeafOf(*back, ParamId::DelayFb)) == Bits(0.3f));
    REQUIRE(back->detached == std::vector<uint32_t>{static_cast<uint32_t>(ParamId::DelayFb)});
    REQUIRE(Text(json).find("\"detached\"") != std::string::npos);
    REQUIRE(s.IsDetached(ParamId::DelayFb));
  }
  SECTION("solve position moves a macro to the leaf it should reach") {
    proc->Param(ParamId::DelayFb).SetPlainNotifyingHost(0.72f);  // Repeats 0.8
    Pump(*proc);
    const auto log = s.SolvePositions(ParamId::MacroRepeats);
    REQUIRE_FALSE(log.empty());
    const float position = proc->Macro(ParamId::MacroRepeats).Plain();
    REQUIRE(std::fabs(position - 0.8f) < 1e-6f);
    // The leaf is now exactly the macro's value at the solved position: nothing left to derive.
    Pump(*proc);
    s.Refresh();
    REQUIRE(Bits(proc->Param(ParamId::DelayFb).Plain()) == Bits(Eval1(*state, ParamId::MacroRepeats, position, ParamId::DelayFb)));
    REQUIRE(s.PendingDerives().empty());
    // A derived document solves back to itself.
    REQUIRE(s.SolvePositions().empty());
  }
  SECTION("solve from a leaf that is not the macro's first target") {
    // The review's case: Smear's third target, scheduler.jitter, set by hand to Smear's value at
    // 0.5; "Solve Smear's position from this leaf" solves from jitter, not from the overlap.
    const float want = Eval1(*state, ParamId::MacroActivity, 0.5f, ParamId::Jitter);
    REQUIRE(want > 0.f);
    proc->Param(ParamId::Jitter).SetPlainNotifyingHost(want);
    Pump(*proc);
    s.Refresh();
    const auto log = s.SolvePositions(ParamId::MacroActivity, ParamId::Jitter);
    REQUIRE_FALSE(log.empty());
    Pump(*proc);
    s.Refresh();
    REQUIRE(Bits(proc->Macro(ParamId::MacroActivity).Plain()) == Bits(0.5f));
    REQUIRE(Bits(proc->Param(ParamId::Jitter).Plain()) == Bits(want));  // the hand-set value stays
    // Smear's other targets follow the solved position.
    REQUIRE(Bits(proc->Param(ParamId::Overlap).Plain()) == Bits(Eval1(*state, ParamId::MacroActivity, 0.5f, ParamId::Overlap)));
    REQUIRE(s.PendingDerives().empty());
  }
  SECTION("A/B switches between the stored version and the working state") {
    proc->Macro(ParamId::MacroTime).setValue(0.8f);
    proc->Param(ParamId::Mix).SetPlainNotifyingHost(0.6f);
    Pump(*proc);
    s.Refresh();
    const float time = proc->Param(ParamId::DelayTimeMs).Plain();
    REQUIRE(s.SetSide(CurationSession::Side::Stored));
    Pump(*proc);
    REQUIRE(s.GetSide() == CurationSession::Side::Stored);
    REQUIRE(Bits(proc->Macro(ParamId::MacroTime).Plain()) == Bits(0.5f));
    REQUIRE(Bits(proc->Param(ParamId::DelayTimeMs).Plain()) == Bits(405.0f));
    REQUIRE(Bits(proc->Param(ParamId::Mix).Plain()) == Bits(0.35f));
    REQUIRE(s.Dirty());  // the working document waits while A plays
    REQUIRE(s.SetSide(CurationSession::Side::Working));
    Pump(*proc);
    s.Refresh();
    REQUIRE(Bits(proc->Macro(ParamId::MacroTime).Plain()) == Bits(0.8f));
    REQUIRE(Bits(proc->Param(ParamId::DelayTimeMs).Plain()) == Bits(time));
    REQUIRE(Bits(proc->Param(ParamId::Mix).Plain()) == Bits(0.6f));
    // Save while A plays saves B.
    REQUIRE(s.SetSide(CurationSession::Side::Stored));
    REQUIRE(s.Save().compiled);
    REQUIRE(s.GetSide() == CurationSession::Side::Working);
    REQUIRE(Bits(LeafOf(*ReadBack(json), ParamId::Mix)) == Bits(0.6f));
  }
  SECTION("a session recall that loads another mode closes the document") {
    BrainscapeProcessor other;
    juce::MemoryBlock   blob;
    other.getStateInformation(blob);
    proc->setStateInformation(blob.getData(), static_cast<int>(blob.getSize()));
    REQUIRE(s.Refresh());
    REQUIRE_FALSE(s.HasDocument());
  }
}

TEST_CASE("the curation session refuses what does not read or compile, and keeps what it has") {
  ScratchDir       scratch;
  const juce::File good = scratch.Copy("engram.json");
  auto             proc = MakeProcessor({}, {});
  CurationSession& s    = proc->Curation();
  juce::String     error;
  REQUIRE(s.Open(good, &error));
  const std::vector<bsc::Finding> before = s.Findings();
  const juce::File bad = scratch.dir.getChildFile("bad.json");
  REQUIRE(bad.replaceWithText(juce::String(Text(good)).replace("\"size_ms\": 100", "\"size_ms\": 900")));
  REQUIRE_FALSE(s.Open(bad, &error));
  REQUIRE(error.contains("E4"));
  REQUIRE_FALSE(s.RefusedFindings().empty());
  REQUIRE(s.RefusedFindings().front().code == "E4");
  REQUIRE(s.RefusedFile() == "bad.json");
  REQUIRE(s.HasDocument());
  REQUIRE(s.SourceFile() == good);
  // The open document's own findings stay: the refused file's errors are not its.
  for (int i = 0; i < 5; ++i) s.Refresh();
  REQUIRE(s.Findings().size() == before.size());
  for (const bsc::Finding& f : s.Findings()) REQUIRE_FALSE(f.error);
  FindingsPanel panel(s);
  panel.Refresh();
  REQUIRE(panel.Rows() >= 1);
  const juce::File broken = scratch.dir.getChildFile("broken.json");
  REQUIRE(broken.replaceWithText("{\"schema_version\": 1,"));
  REQUIRE_FALSE(s.Open(broken, &error));
  REQUIRE(error.contains("E1"));
  REQUIRE_FALSE(s.Open(scratch.dir.getChildFile("missing.json"), &error));
  REQUIRE(s.SourceFile() == good);
}

// Every document of the compiler's examples and the golden corpus, opened and saved unedited:
// what Save writes is `bspc derive` then `bspc stamp`, and it is the file itself exactly when the
// session says Save changes nothing (no pending derive, a current stamp). Under recall Preset a
// document plays its stored tempo, Subdiv and time mode, so the performance Save captures is the
// stored one (docs/design/clock.md §10.3); under Keep the running tempo would replace a stored
// one ("saving a preset captures the live tempo, Subdiv and time mode").
TEST_CASE("saving an unedited document writes what derive and stamp write") {
  int opened = 0, unchanged = 0;
  for (const char* dir : {BRAINSCAPE_TEST_DATA, BRAINSCAPE_GOLDEN_PRESETS}) {
    for (const juce::File& src : juce::File(dir).findChildFiles(juce::File::findFiles, false, "*.json")) {
      ScratchDir       scratch;
      const juce::File json = scratch.dir.getChildFile(src.getFileName());
      REQUIRE(src.copyFileTo(json));
      const std::string original = Text(json);
      auto              proc     = MakeProcessor({}, {});
      WrapperSettings   recall   = proc->GetSettings();
      recall.tempoRecallPreset   = true;
      proc->SetSettings(recall);
      CurationSession&  s        = proc->Curation();
      juce::String      error;
      if (!s.Open(json, &error)) continue;  // the compiler's refused examples
      ++opened;
      INFO(src.getFileName());
      REQUIRE_FALSE(s.Dirty());
      REQUIRE(s.PendingPerformance().empty());
      const bool changes = s.SaveChangesFile();
      REQUIRE(changes == (!s.PendingDerives().empty() || !s.StampCurrent() || !s.Canonical()));
      bsc::Document             doc;
      std::vector<bsc::Finding> found;
      REQUIRE(bsc::ReadDocumentText(original, {}, &doc, &found));
      bsc::Derive(&doc, false, nullptr);
      const bsc::CompileResult want = bsc::CompileDocument(doc);
      REQUIRE(want.ok);
      const auto r = s.Save();
      REQUIRE(r.compiled);
      REQUIRE(Text(json) == want.json);
      REQUIRE((Text(json) == original) == !changes);
      unchanged += changes ? 0 : 1;
      REQUIRE_FALSE(s.SaveChangesFile());  // saved: nothing more to change
    }
  }
  REQUIRE(opened >= 20);
  REQUIRE(unchanged >= 1);
  // A current stamp and nothing to derive, but not in canonical form (indented by hand): Save
  // rewrites it, and the session says so.
  ScratchDir       scratch;
  const juce::File json = scratch.Copy("engram.json");
  REQUIRE(json.replaceWithText(juce::String(Text(json)).replace("\n  \"", "\n    \"")));
  auto             proc = MakeProcessor({}, {});
  CurationSession& s    = proc->Curation();
  juce::String     error;
  REQUIRE(s.Open(json, &error));
  REQUIRE(s.StampCurrent());
  REQUIRE(s.PendingDerives().empty());
  REQUIRE_FALSE(s.Canonical());
  REQUIRE(s.SaveChangesFile());
  REQUIRE(s.Save().compiled);
  REQUIRE(s.Canonical());
  REQUIRE_FALSE(s.SaveChangesFile());
}

// The package beside the document is written with it or not at all (the review's case: a
// directory where the package goes).
TEST_CASE("a save whose package cannot be written leaves the pair on disk as it was") {
  ScratchDir       scratch;
  auto             proc = MakeProcessor({}, {});
  CurationSession& s    = proc->Curation();
  juce::String     error;
  const juce::File source = scratch.Copy("engram.json");
  REQUIRE(s.Open(source, &error));
  const juce::File target = scratch.dir.getChildFile("x.json");
  REQUIRE(target.replaceWithText("old"));
  REQUIRE(scratch.dir.getChildFile("x.bsp").createDirectory());
  const auto r = s.SaveAs(target, true);
  INFO(r.message);
  REQUIRE_FALSE(r.written);
  REQUIRE(r.message.contains("x.bsp"));
  REQUIRE(Text(target) == "old");  // put back
  REQUIRE(s.SourceFile() == source);
  REQUIRE(scratch.dir.getChildFile("x.bsp").isDirectory());
  // A new pair whose package fails leaves no document behind either.
  const juce::File fresh = scratch.dir.getChildFile("y.json");
  REQUIRE(scratch.dir.getChildFile("y.bsp").createDirectory());
  REQUIRE_FALSE(s.SaveAs(fresh, true).written);
  REQUIRE_FALSE(fresh.existsAsFile());
  // And with a writable package path the pair is written.
  REQUIRE(scratch.dir.getChildFile("x.bsp").deleteRecursively());
  const auto ok = s.SaveAs(target, true);
  REQUIRE(ok.written);
  REQUIRE(ok.compiled);
  REQUIRE(scratch.dir.getChildFile("x.bsp").existsAsFile());
}

TEST_CASE("the curation session opens a package and saves the pair") {
  ScratchDir               scratch;
  const bsc::CompileResult r = bsc::Compile(Text(juce::File(BRAINSCAPE_TEST_DATA).getChildFile("engram.json")));
  REQUIRE(r.ok);
  const juce::File bsp = scratch.dir.getChildFile("engram.bsp");
  REQUIRE(bsp.replaceWithData(r.package.data(), r.package.size()));
  auto             proc = MakeProcessor({}, {});
  CurationSession& s    = proc->Curation();
  juce::String     error;
  REQUIRE(s.Open(bsp, &error));
  REQUIRE(s.WritesPackage());
  REQUIRE(s.DocumentFile() == scratch.dir.getChildFile("engram.json"));
  const auto saved = s.Save();
  REQUIRE(saved.compiled);
  juce::MemoryBlock written;
  REQUIRE(bsp.loadFileAsData(written));
  REQUIRE(written.getSize() == r.package.size());
  REQUIRE(std::memcmp(written.getData(), r.package.data(), r.package.size()) == 0);
  REQUIRE(Text(scratch.dir.getChildFile("engram.json")) == r.json);
}

TEST_CASE("level matching measures both versions and trims the louder on the monitor output") {
  ScratchDir       scratch;
  auto             proc = MakeProcessor({}, {});
  CurationSession& s    = proc->Curation();
  juce::String     error;
  REQUIRE(s.Open(scratch.Copy("engram.json"), &error));
  const auto wait = [&] {
    for (int i = 0; i < 4000; ++i) {
      s.Refresh();
      const auto m = s.GetLevelMatch();
      if (m.state == CurationSession::LevelMatch::State::Ready || m.state == CurationSession::LevelMatch::State::Failed) {
        return m;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return s.GetLevelMatch();
  };
  s.SetMatchLevel(true);
  auto m = wait();
  REQUIRE(m.state == CurationSession::LevelMatch::State::Ready);
  REQUIRE(m.storedLufs == m.workingLufs);  // one preset, one input: the same bits
  REQUIRE(m.trimStored == 0.f);
  REQUIRE(m.trimWorking == 0.f);
  proc->Param(ParamId::WetTrimDb).SetPlainNotifyingHost(6.0f);  // B louder
  Pump(*proc);
  s.Refresh();
  std::this_thread::sleep_for(std::chrono::milliseconds(450));
  for (int i = 0; i < 4000 && s.GetLevelMatch().workingLufs == m.workingLufs; ++i) {
    s.Refresh();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  m = wait();
  REQUIRE(m.state == CurationSession::LevelMatch::State::Ready);
  REQUIRE(m.workingLufs > m.storedLufs + 1.0);
  REQUIRE(m.trimWorking < -1.f);
  REQUIRE(m.trimStored == 0.f);
  s.Refresh();
  REQUIRE(Bits(proc->MonitorTrimDb()) == Bits(CanonicalGainDb(m.trimWorking)));
  REQUIRE(s.SetSide(CurationSession::Side::Stored));
  REQUIRE(proc->MonitorTrimDb() == 0.f);
  REQUIRE(s.SetSide(CurationSession::Side::Working));
  s.SetMatchLevel(false);
  REQUIRE(proc->MonitorTrimDb() == 0.f);
}

TEST_CASE("one click renders the document through the audition scripts with the pre-screen") {
  ScratchDir       scratch;
  auto             proc = MakeProcessor({}, {});
  CurationSession& s    = proc->Curation();
  juce::String     error;
  REQUIRE(s.Open(scratch.Copy("engram.json"), &error));
  CurationSession::RenderRequest rq;
  rq.outDir = scratch.dir.getChildFile("renders");
  REQUIRE(s.StartRender(rq, &error));
  REQUIRE_FALSE(s.StartRender(rq, &error));  // one at a time
  CurationSession::RenderStatus st;
  for (int i = 0; i < 6000; ++i) {
    s.Refresh();
    st = s.GetRender();
    if (st.state != CurationSession::RenderStatus::State::Running) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  INFO(st.message << "\n" << st.summary);
  REQUIRE(st.state == CurationSession::RenderStatus::State::Done);
  REQUIRE(st.renders > 0);
  REQUIRE(st.dir == rq.outDir.getChildFile("factory.engram"));
  REQUIRE(st.dir.getChildFile("audition.json").existsAsFile());
  REQUIRE(st.dir.findChildFiles(juce::File::findFiles, false, "*.wav").size() >= 1);
  REQUIRE(st.summary.contains("factory.engram"));
}

TEST_CASE("closing the plugin during a render does not wait for it") {
  ScratchDir   scratch;
  auto         proc = MakeProcessor({}, {});
  juce::String error;
  REQUIRE(proc->Curation().Open(scratch.Copy("engram.json"), &error));
  CurationSession::RenderRequest rq;
  rq.allScripts = true;  // half a minute of renders
  rq.outDir     = scratch.dir.getChildFile("renders");
  REQUIRE(proc->Curation().StartRender(rq, &error));
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  REQUIRE(proc->Curation().GetRender().state == CurationSession::RenderStatus::State::Running);
  const auto start = std::chrono::steady_clock::now();
  proc.reset();  // the host removes the plugin
  const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  REQUIRE(took < 1.0);
  REQUIRE_FALSE(rq.outDir.getChildFile("factory.engram").getChildFile("audition.json").existsAsFile());

  // Opening another document stops the render of the one it replaces.
  auto proc2 = MakeProcessor({}, {});
  REQUIRE(proc2->Curation().Open(scratch.Copy("engram.json"), &error));
  REQUIRE(proc2->Curation().StartRender(rq, &error));
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  const auto again = std::chrono::steady_clock::now();
  REQUIRE(proc2->Curation().Open(scratch.dir.getChildFile("engram.json"), &error));
  REQUIRE(std::chrono::duration<double>(std::chrono::steady_clock::now() - again).count() < 1.0);
  const auto st = proc2->Curation().GetRender();
  REQUIRE(st.state == CurationSession::RenderStatus::State::Failed);
  REQUIRE(st.message.startsWith("Stopped"));
}

// The knobs' names are the document's (META): a renamed knob shows its new name on Revert or
// Open, though the macro table is the same; a document opened over the same mode shows its
// target counts.
TEST_CASE("the pedal knobs take their names from the open document") {
  ScratchDir       scratch;
  auto             proc = MakeProcessor({}, {});
  MacroPanel       panel(*proc);
  panel.Refresh();
  REQUIRE(panel.Knob(0).Caption() == "default macro");
  juce::String     error;
  const juce::File json = scratch.Copy("engram.json");
  REQUIRE(proc->Curation().Open(json, &error));
  panel.Refresh();
  REQUIRE(panel.Knob(0).Title() == "Smear");
  REQUIRE(panel.Knob(0).Caption().contains("3 targets"));
  REQUIRE(json.replaceWithText(juce::String(Text(json)).replace("\"Smear\"", "\"Blur\"")));
  REQUIRE(proc->Curation().Revert(&error));
  panel.Refresh();
  REQUIRE(panel.Knob(0).Title() == "Blur");
  proc->Curation().Close();
  panel.Refresh();
  REQUIRE(panel.Knob(0).Title() == "Activity");
  REQUIRE(panel.Knob(0).Caption() == "default macro");
}

// The pedal view's knob (mode-compiler.md §3.5, Q8): after a load it waits until the hand reaches
// the stored position, then moves the macro; a caught knob follows what the host moves.
TEST_CASE("a pedal knob picks up after a load, then sends MacroMoves") {
  ScratchDir       scratch;
  auto             proc = MakeProcessor({}, {});
  BrainscapeParam& time = proc->Macro(ParamId::MacroTime);
  PickupKnob       knob;
  knob.Bind(&time, juce::Colours::white, "Time", "", "");
  REQUIRE(knob.Caught());  // opened at the value
  REQUIRE(knob.Pointer() == time.getValue());
  juce::String error;
  REQUIRE(proc->Curation().Open(scratch.Copy("engram.json"), &error));
  // Engram stores Time at 0.5, the knob's pointer too: still caught. Moved to 0.3 by the host:
  // a caught knob follows.
  time.setValueNotifyingHost(0.3f);
  knob.Refresh({});
  REQUIRE(knob.Pointer() == 0.3f);
  // A load that stores another position locks it: Spillover-load Engram with Time at 0.7.
  auto state = CompiledState("engram.json");
  for (uint32_t k = 0; k < state->control.macroCount; ++k) {
    if (state->control.positions[k].macroId == static_cast<uint32_t>(ParamId::MacroTime)) {
      state->control.positions[k].position = 0.7f;
    }
  }
  REQUIRE(proc->LoadPresetState(*state));
  knob.Lock();
  knob.Refresh({});
  REQUIRE_FALSE(knob.Caught());
  REQUIRE(knob.Pointer() == 0.3f);  // the hand stays where it was
  knob.MoveTo(0.5f);                // short of 0.7: nothing moves
  Pump(*proc);
  REQUIRE_FALSE(knob.Caught());
  REQUIRE(Bits(time.Plain()) == Bits(0.7f));
  REQUIRE(Bits(proc->Param(ParamId::DelayTimeMs).Plain()) == Bits(405.0f));  // the stored leaf
  knob.MoveTo(0.75f);  // across 0.7: caught, and the move goes out as a MacroMove
  Pump(*proc);
  REQUIRE(knob.Caught());
  REQUIRE(Bits(time.Plain()) == Bits(0.75f));
  PresetLeaf out[kMaxMacroTargets];
  REQUIRE(EvalMacro(state->mode, ParamId::MacroTime, 0.75f, out, kMaxMacroTargets) == 1u);
  REQUIRE(Bits(proc->Param(ParamId::DelayTimeMs).Plain()) == Bits(out[0].value));
  // Double-click catches a locked knob where the value is, sending nothing.
  REQUIRE(proc->LoadPresetState(*state));
  knob.Lock();
  REQUIRE_FALSE(knob.Caught());
  knob.Bind(nullptr, juce::Colours::white, "", "", "");
  REQUIRE(knob.Caught());  // a knob with no target has nothing to wait for
}

// ── The factory set and the Modes menu ───────────────────────────────────────────────────

namespace {

struct ManifestLine {
  std::string packageHash, soundHash, controlHash, path;
};

// firmware/factory/MANIFEST as committed: bspc roundtrip --write-manifest's lines.
std::vector<ManifestLine> ReadManifest() {
  juce::StringArray lines;
  juce::File(BRAINSCAPE_FACTORY_DIR).getChildFile("MANIFEST").readLines(lines);
  std::vector<ManifestLine> out;
  for (const juce::String& line : lines) {
    if (line.trim().isEmpty()) continue;
    juce::StringArray f;
    f.addTokens(line.trim(), " ", "");
    REQUIRE(f.size() == 4);
    out.push_back({f[0].toStdString(), f[1].toStdString(), f[2].toStdString(), f[3].toStdString()});
  }
  return out;
}

std::string HexOf(const Digest32& d) { return golden::Sha256::ToHex(d.bytes, sizeof d.bytes); }

std::string MetaOf(const FactoryPackage& f, const SectionSpan& s) {
  return std::string(reinterpret_cast<const char*>(f.bytes) + s.offset, s.length);
}

// The committed .bsp of a factory package and its document beside it, copied into `dir`: the
// .bsp file Open reads, as it sits in firmware/factory.
juce::File CopyFactoryBsp(const juce::File& dir, const FactoryPackage& f) {
  const juce::File json = juce::File(BRAINSCAPE_FACTORY_DIR).getChildFile(f.path);
  const juce::File from = json.withFileExtension(".bsp");
  const juce::File to   = dir.getChildFile(from.getFileName());
  REQUIRE(from.copyFileTo(to));
  REQUIRE(json.copyFileTo(to.withFileExtension(".json")));
  return to;
}

int Lull() {
  const int lull = FindFactory("factory.lull");
  REQUIRE(lull >= 0);
  return lull;
}

}  // namespace

// Every package MANIFEST lists is in the plugin, in its order, byte for byte as committed, with
// the document's identity; its bytes hash to MANIFEST's hashes (computed here, and checked by
// DecodePreset over the content), and it decodes and validates.
TEST_CASE("the factory set is embedded byte for byte as MANIFEST lists it, and every package validates") {
  const std::vector<ManifestLine> manifest = ReadManifest();
  REQUIRE(FactoryCount() == manifest.size());
  size_t reserves = 0;
  for (size_t i = 0; i < FactoryCount(); ++i) {
    const FactoryPackage& f = Factory(i);
    const ManifestLine&   m = manifest[i];
    INFO(m.path);
    REQUIRE(m.path == f.path);
    REQUIRE(m.packageHash == f.packageHash);
    REQUIRE(m.soundHash == f.soundHash);
    REQUIRE(m.controlHash == f.controlHash);
    REQUIRE(f.reserve == (m.path.rfind("reserve/", 0) == 0));
    reserves += f.reserve ? 1u : 0u;
    REQUIRE(FindFactory(f.id) == static_cast<int>(i));

    // Byte for byte the committed package.
    juce::MemoryBlock committed;
    const juce::String bsp = juce::String(f.path).upToLastOccurrenceOf(".json", false, false) + ".bsp";
    REQUIRE(juce::File(BRAINSCAPE_FACTORY_DIR).getChildFile(bsp).loadFileAsData(committed));
    REQUIRE(committed.getSize() == f.size);
    REQUIRE(std::memcmp(committed.getData(), f.bytes, f.size) == 0);

    // package_hash: SHA-256 of the package with its own field (bytes 96-127) zeroed.
    std::vector<uint8_t> zeroed(f.bytes, f.bytes + f.size);
    std::fill(zeroed.begin() + 96, zeroed.begin() + 128, uint8_t{0});
    golden::Sha256 sha;
    sha.Update(zeroed.data(), zeroed.size());
    REQUIRE(sha.Hex() == m.packageHash);

    // DecodePreset checks all three hashes against the content; ValidateMode the semantics.
    PresetDiagnostic d;
    PackageInfo      info;
    PresetMeta       meta;
    const auto       state = DecodeFactory(i, &d, &info, &meta);
    INFO(PresetErrorName(d.error));
    REQUIRE(state != nullptr);
    REQUIRE(ValidateMode(*state, &d));
    REQUIRE(HexOf(info.packageHash) == m.packageHash);
    REQUIRE(HexOf(info.soundHash) == m.soundHash);
    REQUIRE(HexOf(info.controlHash) == m.controlHash);
    REQUIRE((info.flags & kPackageFlagFactory) != 0u);
    REQUIRE(info.soundRev == kSoundRevision);
    LoadReport report;
    REQUIRE(CheckPreset(*state, &report));  // loads exact on this build
    // The table's identity is the package's META.
    REQUIRE(MetaOf(f, meta.id) == f.id);
    REQUIRE(MetaOf(f, meta.name) == f.name);
    REQUIRE(meta.family == f.family);
  }
  REQUIRE(reserves > 0u);
  REQUIRE(reserves < FactoryCount());
  // The menu's order holds every package once: the set by family, then the reserves.
  std::vector<size_t> order = FactoryMenuOrder();
  REQUIRE(order.size() == FactoryCount());
  for (size_t k = 0; k + 1 < order.size(); ++k) {
    REQUIRE((Factory(order[k]).reserve <= Factory(order[k + 1]).reserve));
  }
  std::sort(order.begin(), order.end());
  for (size_t k = 0; k < order.size(); ++k) REQUIRE(order[k] == k);
}

TEST_CASE("the Modes menu lists the set by family, the reserves in a submenu, and names what plays") {
  auto     proc = MakeProcessor({}, {});
  ModeMenu menu(*proc);
  REQUIRE(menu.Name() == "Default mode");
  REQUIRE(menu.Caption() == "MODE");
  REQUIRE_FALSE(menu.Named());

  const auto check = [&](int ticked) {
    const juce::PopupMenu    m = menu.BuildMenu();
    std::vector<juce::String> headers;
    std::vector<int>          set, reserve;
    bool                      open = false;
    for (juce::PopupMenu::MenuItemIterator it(m); it.next();) {
      const juce::PopupMenu::Item& item = it.getItem();
      if (item.isSectionHeader) {
        headers.push_back(item.text);
        continue;
      }
      if (item.isSeparator) continue;
      if (item.itemID == ModeMenu::kOpenItem) {
        open = true;
        continue;
      }
      if (item.subMenu != nullptr) {
        REQUIRE(item.text == "Reserves");
        REQUIRE(item.isTicked == (ticked >= 0 && Factory(static_cast<size_t>(ticked)).reserve));
        for (juce::PopupMenu::MenuItemIterator sub(*item.subMenu); sub.next();) {
          const int i = sub.getItem().itemID - ModeMenu::kFactoryItem;
          REQUIRE(Factory(static_cast<size_t>(i)).reserve);
          REQUIRE(sub.getItem().text == juce::String::fromUTF8(Factory(static_cast<size_t>(i)).name));
          REQUIRE(sub.getItem().shortcutKeyDescription == FamilyName(Factory(static_cast<size_t>(i)).family));
          REQUIRE(sub.getItem().isTicked == (i == ticked));
          reserve.push_back(i);
        }
        continue;
      }
      const int i = item.itemID - ModeMenu::kFactoryItem;
      REQUIRE(i >= 0);
      REQUIRE(static_cast<size_t>(i) < FactoryCount());
      const FactoryPackage& f = Factory(static_cast<size_t>(i));
      REQUIRE_FALSE(f.reserve);
      REQUIRE(item.text == juce::String::fromUTF8(f.name));
      REQUIRE(item.isTicked == (i == ticked));
      REQUIRE_FALSE(headers.empty());
      REQUIRE(headers.back() == juce::String(FamilyName(f.family)).toUpperCase());  // under its family
      set.push_back(i);
    }
    // The four families in the README's order, every mode of the set once, every reserve once.
    REQUIRE(headers == std::vector<juce::String>{"ECHOIC", "REVERIE", "RECALL", "MISFIRE"});
    size_t setCount = 0;
    for (size_t i = 0; i < FactoryCount(); ++i) setCount += Factory(i).reserve ? 0u : 1u;
    REQUIRE(set.size() == setCount);
    REQUIRE(reserve.size() == FactoryCount() - setCount);
    REQUIRE(open);
  };
  check(-1);

  // Choosing Lull plays it and names it; the menu ticks it.
  bool         heard = false;
  juce::String message;
  menu.onChosen = [&](bool opened, const juce::String& m) {
    heard   = opened;
    message = m;
  };
  REQUIRE(menu.Choose(ModeMenu::kFactoryItem + Lull()));
  REQUIRE(heard);
  REQUIRE(message.contains("lull.bsp"));
  REQUIRE(proc->CurrentSource().factory == Lull());
  REQUIRE(proc->Curation().FactoryIndex() == Lull());
  REQUIRE(menu.Name() == "Lull");
  REQUIRE(menu.Caption().endsWith("REVERIE"));
  REQUIRE(menu.Named());
  check(Lull());
  // A reserve is captioned as one.
  size_t reserve = 0;
  while (!Factory(reserve).reserve) ++reserve;
  REQUIRE(menu.Choose(ModeMenu::kFactoryItem + static_cast<int>(reserve)));
  REQUIRE(menu.Caption().contains("RESERVE"));
  check(static_cast<int>(reserve));
  // Ids outside the menu do nothing; "Open..." asks the editor.
  REQUIRE_FALSE(menu.Choose(0));
  REQUIRE_FALSE(menu.Choose(ModeMenu::kFactoryItem + static_cast<int>(FactoryCount())));
  bool asked      = false;
  menu.onOpenFile = [&] { asked = true; };
  REQUIRE(menu.Choose(ModeMenu::kOpenItem));
  REQUIRE(asked);
}

// Unsaved edits in the open document go only when the curator says so: the menu asks
// (onAskDiscard, an OK/Cancel box in the editor), and until then nothing loads, even for the mode
// already ticked; Discard opens the mode chosen. Without edits it opens at once.
TEST_CASE("the Modes menu asks before it drops unsaved edits") {
  auto             proc = MakeProcessor({}, {});
  CurationSession& s    = proc->Curation();
  ModeMenu         menu(*proc);
  int              asks = 0;
  juce::String     askedFor;
  std::function<void()> discard;
  menu.onAskDiscard = [&](const juce::String& mode, std::function<void()> d) {
    ++asks;
    askedFor = mode;
    discard  = std::move(d);
  };
  int chosen    = 0;
  menu.onChosen = [&](bool opened, const juce::String&) { chosen += opened ? 1 : 0; };

  REQUIRE(menu.Choose(ModeMenu::kFactoryItem + Lull()));  // nothing open: no question
  REQUIRE(asks == 0);
  REQUIRE(chosen == 1);
  Pump(*proc);
  s.Refresh();
  REQUIRE_FALSE(s.Dirty());
  size_t reserve = 0;
  while (!Factory(reserve).reserve) ++reserve;
  REQUIRE(menu.Choose(ModeMenu::kFactoryItem + static_cast<int>(reserve)));  // unedited: no question
  REQUIRE(asks == 0);
  REQUIRE(chosen == 2);
  REQUIRE(s.FactoryIndex() == static_cast<int>(reserve));

  // An edit: the menu asks, and nothing loads until Discard.
  proc->Param(ParamId::Mix).SetPlainNotifyingHost(0.5f);
  Pump(*proc);
  s.Refresh();
  REQUIRE(s.Dirty());
  const uint32_t serial = proc->LoadSerial();
  for (const int item : {Lull(), static_cast<int>(reserve)}) {  // another mode, and the one ticked
    REQUIRE(menu.Choose(ModeMenu::kFactoryItem + item));
    REQUIRE(asks > 0);
    REQUIRE(askedFor == juce::String::fromUTF8(Factory(static_cast<size_t>(item)).name));
    REQUIRE(discard != nullptr);
    REQUIRE(chosen == 2);
    REQUIRE(proc->LoadSerial() == serial);
    REQUIRE(s.FactoryIndex() == static_cast<int>(reserve));
    REQUIRE(s.Dirty());
    REQUIRE(proc->CurrentSource().factory == static_cast<int>(reserve));
  }
  REQUIRE(asks == 2);
  // Cancel is not calling it; Discard opens what was chosen last.
  discard();
  REQUIRE(chosen == 3);
  REQUIRE(proc->LoadSerial() != serial);
  REQUIRE(s.FactoryIndex() == static_cast<int>(reserve));
  REQUIRE(proc->CurrentSource().factory == static_cast<int>(reserve));
  Pump(*proc);
  s.Refresh();
  REQUIRE_FALSE(s.Dirty());
  REQUIRE(menu.Choose(ModeMenu::kFactoryItem + Lull()));
  REQUIRE(asks == 2);
  REQUIRE(s.FactoryIndex() == Lull());
}

// The menu's path (ModeMenu::Choose, CurationSession::OpenFactory) against Open on the committed
// .bsp, each on a processor that has been playing: the same state unit, a Spillover load with
// Trails at the next block's first frame, the same mirrors, the same document, the same output
// bit for bit; and processBlock allocates nothing (RenderProcessor's audit).
TEST_CASE("every factory mode loads through the menu exactly as its .bsp file loads") {
  ScratchDir   scratch;
  const Stereo in     = MakeInput(9600);
  const int    change = 4800;
  for (size_t i = 0; i < FactoryCount(); ++i) {
    const FactoryPackage& f = Factory(i);
    INFO(f.path);
    const juce::File bsp = CopyFactoryBsp(scratch.dir, f);
    auto             viaMenu = MakeProcessor(Busy(), {});
    auto             viaFile = MakeProcessor(Busy(), {});
    ModeMenu         menu(*viaMenu);
    juce::String     error;
    HostRender       a, b;
    a.pattern = b.pattern = {480};
    a.beforeBlock = [&](int pos) {
      if (pos == change) REQUIRE(menu.Choose(ModeMenu::kFactoryItem + static_cast<int>(i)));
    };
    b.beforeBlock = [&](int pos) {
      if (pos == change) REQUIRE(viaFile->Curation().Open(bsp, &error));
    };
    const Stereo gotMenu = RenderProcessor(*viaMenu, in, {}, a);
    const Stereo gotFile = RenderProcessor(*viaFile, in, {}, b);
    RequireSame(gotMenu, gotFile, "the menu against the file");
    const auto state = DecodeFactory(i);
    REQUIRE(state != nullptr);
    RequireSame(gotMenu, RenderReference(Busy(), in, kRate, {RefLoad(change, state.get())}),
                "a Spillover load with Trails of the package at the block's first frame");

    // The same wrapper state: leaves, mode, CTRL, performance, the macro mirrors, the report.
    const auto pm = viaMenu->CurrentPreset();
    const auto pf = viaFile->CurrentPreset();
    REQUIRE(pm->leafCount == pf->leafCount);
    for (uint32_t k = 0; k < pm->leafCount; ++k) REQUIRE(Bits(pm->leaves[k].value) == Bits(pf->leaves[k].value));
    REQUIRE(std::memcmp(&pm->mode, &state->mode, sizeof(ModeBlob)) == 0);
    REQUIRE(std::memcmp(&pm->mode, &pf->mode, sizeof(ModeBlob)) == 0);
    REQUIRE(std::memcmp(&pm->control, &pf->control, sizeof(ControlState)) == 0);
    REQUIRE(std::memcmp(&pm->performance, &pf->performance, sizeof(PerformanceState)) == 0);
    REQUIRE(pm->soundRev == pf->soundRev);
    const ModeState mm = viaMenu->CurrentMode(), mf = viaFile->CurrentMode();
    REQUIRE(std::memcmp(&mm, &mf, sizeof(ModeState)) == 0);
    for (uint32_t id = static_cast<uint32_t>(ParamId::MacroActivity); id <= static_cast<uint32_t>(ParamId::MacroAux2);
         ++id) {
      REQUIRE(Bits(viaMenu->Macro(static_cast<ParamId>(id)).Plain()) ==
              Bits(viaFile->Macro(static_cast<ParamId>(id)).Plain()));
    }
    REQUIRE(viaMenu->GetStatus().lastLoadInexact == viaFile->GetStatus().lastLoadInexact);
    REQUIRE_FALSE(viaMenu->GetStatus().lastLoadInexact);
    REQUIRE(viaMenu->LoadSerial() == viaFile->LoadSerial());

    // The same document in the curation session; the menu's has no file and names the package.
    CurationSession& sm = viaMenu->Curation();
    CurationSession& sf = viaFile->Curation();
    REQUIRE(sm.HasDocument());
    REQUIRE(sm.FactoryIndex() == static_cast<int>(i));
    REQUIRE(sf.FactoryIndex() == -1);
    REQUIRE(sm.Stored().id == sf.Stored().id);
    REQUIRE(sm.Stored().name == sf.Stored().name);
    REQUIRE(std::memcmp(&sm.Mode(), &sf.Mode(), sizeof(ModeBlob)) == 0);
    REQUIRE(sm.WritesPackage() == sf.WritesPackage());
    REQUIRE(sm.StampCurrent() == sf.StampCurrent());
    REQUIRE(sm.Canonical() == sf.Canonical());
    REQUIRE(sm.Dirty() == sf.Dirty());
    REQUIRE(sm.Findings().size() == sf.Findings().size());
    REQUIRE(sm.DocumentFile() == juce::File());
    REQUIRE(sm.SourceLabel().startsWith("firmware/factory/"));
    REQUIRE(viaMenu->CurrentSource().factory == static_cast<int>(i));
    REQUIRE(viaMenu->CurrentSource().name == juce::String::fromUTF8(f.name));
    REQUIRE(viaFile->CurrentSource().factory == -1);
    REQUIRE(viaFile->CurrentSource().name == juce::String::fromUTF8(f.name));
  }
}

// A factory mode has no file: Save writes nothing, Save as writes a copy that then plays as a
// document file, and Revert opens the built-in package again.
TEST_CASE("a factory mode saves only as a copy, and reverts to the built-in package") {
  ScratchDir       scratch;
  auto             proc = MakeProcessor({}, {});
  CurationSession& s    = proc->Curation();
  juce::String     error;
  REQUIRE(s.OpenFactory(static_cast<size_t>(Lull()), &error));
  proc->Param(ParamId::Mix).SetPlainNotifyingHost(0.5f);
  Pump(*proc);
  s.Refresh();
  REQUIRE(s.Dirty());
  const auto refused = s.Save();
  REQUIRE_FALSE(refused.written);
  REQUIRE(scratch.dir.getNumberOfChildFiles(juce::File::findFiles) == 0);
  REQUIRE(s.Revert(&error));
  Pump(*proc);
  s.Refresh();
  REQUIRE_FALSE(s.Dirty());
  REQUIRE(s.FactoryIndex() == Lull());
  proc->Param(ParamId::Mix).SetPlainNotifyingHost(0.5f);
  Pump(*proc);
  const juce::File copy  = scratch.dir.getChildFile("my-lull.json");
  const auto       saved = s.SaveAs(copy, s.WritesPackage());
  INFO(saved.message);
  REQUIRE(saved.compiled);
  REQUIRE(copy.existsAsFile());
  REQUIRE(copy.withFileExtension(".bsp").existsAsFile());
  REQUIRE(s.FactoryIndex() == -1);
  REQUIRE(s.DocumentFile() == copy);
  REQUIRE(proc->CurrentSource().factory == -1);
  REQUIRE(proc->CurrentSource().name == "Lull");
  REQUIRE(LeafOf(*ReadBack(copy), ParamId::Mix) == 0.5f);
}

// Host state (StateCodec.h's FMOD block): a session saved while a factory mode plays reloads it,
// with the leaves and the macro positions it had, and saves back byte for byte; readers before the
// block, and builds without the mode, play the default mode as every v1 session did.
TEST_CASE("a session saved with a factory mode reloads it") {
  const int  lull = Lull();
  auto       a    = MakeProcessor({}, {});
  ModeMenu   menu(*a);
  REQUIRE(menu.Choose(ModeMenu::kFactoryItem + lull));
  Pump(*a);
  // A knob turned (a MacroMove: the leaves follow its fan-out) and a leaf set by hand.
  a->Macro(ParamId::MacroActivity).SetPlainNotifyingHost(0.8f);
  a->Param(ParamId::Mix).SetPlainNotifyingHost(0.3f);
  Pump(*a);
  juce::MemoryBlock blob;
  a->getStateInformation(blob);
  const auto played = a->CurrentPreset();
  REQUIRE(Bits(a->Macro(ParamId::MacroActivity).Plain()) == Bits(0.8f));

  WrapperState st{};
  REQUIRE(DecodeState(blob.getData(), blob.getSize(), st));
  REQUIRE(st.hasFactory);
  REQUIRE_FALSE(st.unreadTail);
  REQUIRE(st.factory.id == "factory.lull");
  REQUIRE(golden::Sha256::ToHex(st.factory.packageHash, 32) == Factory(static_cast<size_t>(lull)).packageHash);
  REQUIRE(st.factory.macroCount == played->control.macroCount);

  const Stereo in = MakeInput(24000);
  SECTION("restored before anything plays: an Exact load of what played, saved back byte for byte") {
    auto b = MakeProcessor({}, {});
    b->setStateInformation(blob.getData(), static_cast<int>(blob.getSize()));
    REQUIRE(b->CurrentSource().factory == lull);
    REQUIRE(b->CurrentSource().name == "Lull");
    REQUIRE_FALSE(b->GetStatus().lastLoadInexact);
    const auto now = b->CurrentPreset();
    for (uint32_t k = 0; k < now->leafCount; ++k) REQUIRE(Bits(now->leaves[k].value) == Bits(played->leaves[k].value));
    REQUIRE(std::memcmp(&now->mode, &played->mode, sizeof(ModeBlob)) == 0);
    REQUIRE(std::memcmp(&now->control, &played->control, sizeof(ControlState)) == 0);  // positions too
    REQUIRE(Bits(b->Macro(ParamId::MacroActivity).Plain()) == Bits(0.8f));
    juce::MemoryBlock again;
    b->getStateInformation(again);
    REQUIRE(again == blob);
    const Stereo got = RenderProcessor(*b, in, {}, {{480}});
    RequireSame(got, RenderStateReference(*played, in), "the restored factory mode, Exact");
    // The curation views open its document for what plays (no load): edited, so dirty.
    const uint32_t serial = b->LoadSerial();
    REQUIRE(b->Curation().Refresh());
    REQUIRE(b->Curation().FactoryIndex() == lull);
    REQUIRE(b->Curation().Dirty());
    REQUIRE(b->LoadSerial() == serial);
    ModeMenu shown(*b);
    REQUIRE(shown.Name() == "Lull");
  }
  SECTION("a recall replaces an open document with the factory mode it played") {
    ScratchDir   scratch;
    auto         b = MakeProcessor({}, {});
    juce::String error;
    REQUIRE(b->Curation().Open(scratch.Copy("engram.json"), &error));
    Pump(*b);
    b->setStateInformation(blob.getData(), static_cast<int>(blob.getSize()));
    Pump(*b);
    REQUIRE(b->Curation().Refresh());
    REQUIRE(b->Curation().FactoryIndex() == lull);
    // Closed by hand, it stays closed for that load.
    b->Curation().Close();
    REQUIRE_FALSE(b->Curation().Refresh());
    REQUIRE_FALSE(b->Curation().HasDocument());
  }
  SECTION("a reader before the block, and a session without one, play the default mode") {
    WrapperState plainState = st;
    plainState.hasFactory   = false;
    std::vector<uint8_t> v1;
    EncodeState(plainState, v1);
    REQUIRE(v1.size() < blob.getSize());
    REQUIRE(std::memcmp(v1.data(), blob.getData(), v1.size()) == 0);  // the block is only appended
    auto b = MakeProcessor({}, {});
    b->setStateInformation(v1.data(), static_cast<int>(v1.size()));
    const ModeState def{};
    const ModeState now = b->CurrentMode();
    REQUIRE(std::memcmp(&now, &def, sizeof(ModeState)) == 0);
    REQUIRE(b->CurrentSource().factory == -1);
    REQUIRE(b->CurrentSource().name.isEmpty());
    for (size_t i = 0; i < kNumLeafParams; ++i) REQUIRE(Bits(b->Param(LeafId(i)).Plain()) == Bits(st.plain[i]));
  }
  SECTION("unknown blocks are skipped; a block that does not read whole is ignored, inexact") {
    std::vector<uint8_t> bytes(static_cast<const uint8_t*>(blob.getData()),
                               static_cast<const uint8_t*>(blob.getData()) + blob.getSize());
    WrapperState plainState = st;
    plainState.hasFactory   = false;
    std::vector<uint8_t> v1;
    EncodeState(plainState, v1);
    std::vector<uint8_t> other(v1);
    const uint8_t        unknown[] = {'Z', 'Z', 'Z', 'Z', 3, 0, 0, 0, 1, 2, 3, 0};  // a 3-byte payload, padded
    other.insert(other.end(), std::begin(unknown), std::end(unknown));
    other.insert(other.end(), bytes.begin() + static_cast<std::ptrdiff_t>(v1.size()), bytes.end());
    WrapperState got{};
    REQUIRE(DecodeState(other.data(), other.size(), got));
    REQUIRE(got.hasFactory);
    REQUIRE_FALSE(got.unreadTail);
    auto b = MakeProcessor({}, {});
    b->setStateInformation(other.data(), static_cast<int>(other.size()));
    REQUIRE(b->CurrentSource().factory == lull);

    bytes.resize(bytes.size() - 3);  // cut inside the block
    REQUIRE(DecodeState(bytes.data(), bytes.size(), got));
    REQUIRE_FALSE(got.hasFactory);
    REQUIRE(got.unreadTail);
    b->setStateInformation(bytes.data(), static_cast<int>(bytes.size()));
    REQUIRE(b->CurrentSource().factory == -1);
    REQUIRE(b->GetStatus().lastLoadInexact);
  }
  SECTION("a mode this build lacks plays the default mode; another package of it plays this one's") {
    WrapperState missing = st;
    missing.factory.id   = "factory.not-built-in";
    std::vector<uint8_t> bytes;
    EncodeState(missing, bytes);
    auto b = MakeProcessor({}, {});
    b->setStateInformation(bytes.data(), static_cast<int>(bytes.size()));
    REQUIRE(b->CurrentSource().factory == -1);
    REQUIRE(b->GetStatus().lastLoadInexact);
    for (size_t i = 0; i < kNumLeafParams; ++i) REQUIRE(Bits(b->Param(LeafId(i)).Plain()) == Bits(st.plain[i]));

    WrapperState other = st;
    other.factory.packageHash[0] ^= 1u;
    EncodeState(other, bytes);
    b->setStateInformation(bytes.data(), static_cast<int>(bytes.size()));
    REQUIRE(b->CurrentSource().factory == lull);
    REQUIRE(b->GetStatus().lastLoadInexact);
    const ModeState now = b->CurrentMode();
    REQUIRE(std::memcmp(&now.mode, &played->mode, sizeof(ModeBlob)) == 0);
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

// ── The tempo core in the plugin (docs/design/clock.md §4.4, §10, §8.4) ──────────────────────

namespace {

// A golden-corpus package, decoded as a load takes it: clock_hits is a CLOCK mode (grains on the
// tempo grid alone, no jitter, one grain a hit) stored at 140 BPM on TAP.
std::unique_ptr<PresetState> GoldenPackage(const char* name) {
  juce::MemoryBlock bytes;
  REQUIRE(juce::File(BRAINSCAPE_GOLDEN_PRESETS).getChildFile(name).loadFileAsData(bytes));
  auto state = std::make_unique<PresetState>();
  REQUIRE(DecodePreset(bytes.getData(), bytes.getSize(), state.get()));
  return state;
}

float FloatOfBits(uint32_t u) {
  float v = 0.f;
  std::memcpy(&v, &u, sizeof v);
  return v;
}

RefEvent RefTempoEvent(int64_t frame, Engine::EventType type, uint32_t id, uint32_t valueBits = 0u) {
  RefEvent e;
  e.frame = frame;
  e.type  = type;
  e.id    = id;
  e.value = FloatOfBits(valueBits);
  return e;
}
RefEvent RefTempo(int64_t frame, uint32_t ns) { return RefTempoEvent(frame, Engine::EventType::Tempo, ns); }
RefEvent RefSubdiv(int64_t frame, tempo::SubdivField field, uint8_t code) {
  return RefTempoEvent(frame, Engine::EventType::Subdivision, tempo::SubdivisionId(field, code));
}
RefEvent RefHostStart(int64_t frame, uint32_t position, uint32_t offset) {
  return RefTempoEvent(frame, Engine::EventType::Transport,
                       tempo::TransportId(tempo::TransportKind::Start, false, offset),
                       tempo::IntegerValueBits(position));
}
RefEvent RefRecall(int64_t frame, bool preset) { return RefParam(frame, ParamId::TempoRecall, preset ? 1.f : 0.f); }

// The engine alone from an Exact load of `state` with `events`, rendered to `frames` in 48-frame
// blocks: its tempo snapshot and counters at the end (the reference's grid, not its audio).
struct RefTempoState {
  TempoInfo  info;
  TempoStats counts;
};
RefTempoState RenderTempoReference(const PresetState& state, int frames, const std::vector<RefEvent>& events) {
  RefTempoState out;
  const Stereo  in = MakeInput(frames);
  EngineConfig  cfg;
  cfg.sampleRate    = kRate;
  cfg.maxBlockSize  = 512;
  cfg.historyFrames = 1u << 22;
  host::HeapArenas arenas(PlanMemory(cfg));
  auto             engine = std::make_unique<Engine>();
  REQUIRE(arenas.ok());
  REQUIRE(engine->Init(cfg, arenas.get()));
  engine->LoadPreset(state, LoadMode::Exact);
  std::vector<float>              l(48), r(48), ol(48), orr(48);
  std::vector<Engine::BlockEvent> block;
  size_t                          ei = 0;
  for (int pos = 0; pos < frames; pos += 48) {
    const int end = std::min(frames, pos + 48);
    block.clear();
    for (; ei < events.size() && events[ei].frame < end; ++ei) {
      Engine::BlockEvent b;
      b.offset = static_cast<uint32_t>(std::max<int64_t>(events[ei].frame - pos, 0));
      b.seq    = static_cast<uint32_t>(ei);
      b.type   = events[ei].type;
      b.id     = events[ei].id;
      b.value  = events[ei].value;
      b.preset = events[ei].preset;
      block.push_back(b);
    }
    std::copy(in.l.begin() + pos, in.l.begin() + end, l.begin());
    std::copy(in.r.begin() + pos, in.r.begin() + end, r.begin());
    const float*           ins[2]  = {l.data(), r.data()};
    float*                 outs[2] = {ol.data(), orr.data()};
    Engine::ProcessContext ctx;
    ctx.in        = ins;
    ctx.out       = outs;
    ctx.numFrames = static_cast<uint32_t>(end - pos);
    ctx.events    = block.data();
    ctx.numEvents = static_cast<uint32_t>(block.size());
    engine->Process(ctx);
  }
  out.info   = engine->Tempo();
  out.counts = engine->TempoCounts();
  return out;
}

// A processor playing `state` (a preset change before anything plays: an Exact load), its Tempo
// source and restart option as given, its playhead `head`.
std::unique_ptr<BrainscapeProcessor> MakeTempoProcessor(const PresetState& state, TestPlayHead& head,
                                                        TempoSource source, bool restartOnStart,
                                                        int maxBlock = 4096) {
  auto            proc = MakeProcessor({}, {}, maxBlock);
  WrapperSettings s    = proc->GetSettings();
  s.tempoSource        = source;
  s.restartOnStart     = restartOnStart;
  proc->SetSettings(s);
  REQUIRE(proc->LoadPresetState(state));
  proc->setPlayHead(&head);
  return proc;
}

// The host's transport for a render: from `ppq` at `bpm`, the position of each block's first frame.
HostRender Transport(TestPlayHead& head, double ppq, double bpm, std::vector<int> pattern) {
  HostRender r;
  r.pattern     = std::move(pattern);
  r.beforeBlock = [&head, ppq, bpm](int pos) { head.ppq = ppq + static_cast<double>(pos) * bpm / (60.0 * kRate); };
  return r;
}

// The frame at which the host's quarter `beat` lies, from a transport at `ppq` and `bpm` whose
// first block starts at frame `start`.
double HostBeatFrame(double beat, double ppq, double bpm, int64_t start = 0) {
  return static_cast<double>(start) + (beat - ppq) * 60.0 / bpm * kRate;
}

}  // namespace

TEST_CASE("rows 83 and 84 are Subdivision events; their typed names read back") {
  // §10.4: perf.subdiv is the Subdiv knob's position in the Microcosm's CC#5 order, which the
  // wrapper sends as §5.1's code; perf.time_mode the time mode (field 1). The text parser reads
  // the rates in ASCII or with the display's multiplication sign (§5.1).
  auto proc = MakeProcessor({}, {});
  const Stereo in = MakeInput(480);
  const uint8_t codes[] = {1, 2, 0, 3, 4, 5};
  for (int position = 0; position < 6; ++position) {
    INFO("position " << position);
    proc->SubdivParam().SetPlainNotifyingHost(static_cast<float>(position));
    RenderProcessor(*proc, in, {}, {{480}});
    REQUIRE(proc->EngineTempo().subdiv == codes[position]);
  }
  for (int mode = 2; mode >= 0; --mode) {
    proc->TimeModeParam().SetPlainNotifyingHost(static_cast<float>(mode));
    RenderProcessor(*proc, in, {}, {{480}});
    REQUIRE(proc->EngineTempo().timeMode == mode);
  }
  // A host's lossy set lands on a position, as every discrete row's does.
  proc->SubdivParam().setValue(0.61f);
  RenderProcessor(*proc, in, {}, {{480}});
  REQUIRE(proc->SubdivParam().Plain() == 3.0f);
  REQUIRE(proc->EngineTempo().subdiv == 3u);

  const auto parsed = [](ParamId id, const char* text) {
    float v = -1.f;
    return ParsePlainText(id, juce::String::fromUTF8(text), v) ? v : -1.f;
  };
  REQUIRE(parsed(ParamId::PerfSubdiv, "x1/4") == 0.f);
  REQUIRE(parsed(ParamId::PerfSubdiv, "\xC3\x97" "1/2") == 1.f);
  REQUIRE(parsed(ParamId::PerfSubdiv, "TAP") == 2.f);
  REQUIRE(parsed(ParamId::PerfSubdiv, "X2") == 3.f);
  REQUIRE(parsed(ParamId::PerfSubdiv, "\xC3\x97" "8") == 5.f);
  REQUIRE(parsed(ParamId::PerfSubdiv, "4") == 4.f);
  REQUIRE(parsed(ParamId::PerfSubdiv, "1/4") == -1.f);  // a note value, never a rate (§5.1)
  REQUIRE(parsed(ParamId::PerfTimeMode, "Subdiv") == 1.f);
  REQUIRE(parsed(ParamId::PerfTimeMode, "tempo") == 2.f);
  REQUIRE(parsed(ParamId::TempoRecall, "Preset") == 1.f);
  for (int position = 0; position < 6; ++position) {  // what the display shows reads back
    const juce::String shown = FormatPlainText(ParamId::PerfSubdiv, static_cast<float>(position));
    REQUIRE(shown == juce::String::fromUTF8(position == 2 ? "TAP" : position == 0 ? "\xC3\x97" "1/4" : position == 1 ? "\xC3\x97" "1/2"
                                                       : position == 3 ? "\xC3\x97" "2" : position == 4 ? "\xC3\x97" "4"
                                                                                                          : "\xC3\x97" "8"));
    REQUIRE(parsed(ParamId::PerfSubdiv, shown.toRawUTF8()) == static_cast<float>(position));
  }
}

TEST_CASE("host tempo: a bounce at a constant tempo is one output at every host block size") {
  // §8.4, §4.4: with Restart on transport start, a bounce at 137.5 BPM from ppq 3.37 depends on
  // the first block's ppq and tempo only. The restart's re-asserts (rows 83 and 84, row 85), the
  // host's Tempo and its anchored Start land at frame 0, so the bounce equals the engine alone
  // with those events; and the first CLOCK hit is the first quarter after the anchor, on the
  // host's beat, with none before it.
  const auto   clock  = GoldenPackage("clock_hits.bsp");
  const double bpm    = 137.5;
  const double ppq    = 3.37;
  const int    frames = 3 * 48000;
  const Stereo take   = MakeInput(frames);
  const uint32_t ns   = tempo::NsPerQuarterFromBpm(bpm);
  uint32_t position = 0, offset = 0;
  REQUIRE(tempo::HostAnchor(ppq, ns, 48000, &position, &offset));
  REQUIRE(position == 81u);
  REQUIRE(offset == 105u);
  const auto reference = [&](uint8_t subdivCode) {
    std::vector<RefEvent> ev = {RefSubdiv(0, tempo::SubdivField::Subdivision, subdivCode),
                                RefSubdiv(0, tempo::SubdivField::TimeMode, 0),
                                RefRecall(0, false),
                                RefTempo(0, ns),
                                RefHostStart(0, position, offset)};
    return ev;
  };
  const Stereo want = RenderStateReference(*clock, take, kRate, reference(0));
  // No hit before the anchor's first quarter (position 96, ppq 4), then that one at its frame.
  const double firstBeat = HostBeatFrame(4.0, ppq, bpm);
  RefTempoState before   = RenderTempoReference(*clock, static_cast<int>(firstBeat) - 1, reference(0));
  REQUIRE(before.counts.clockBirths == 0u);
  RefTempoState after = RenderTempoReference(*clock, static_cast<int>(firstBeat) + 48, reference(0));
  REQUIRE(after.counts.clockBirths == 1u);
  REQUIRE(std::fabs(static_cast<double>(after.info.lastClockBirth) - firstBeat) <= 1.0);
  REQUIRE(after.counts.transports == 1u);
  REQUIRE(after.counts.tempoEvents == 1u);
  // Without the host's events the grid would be the stored 140 BPM from frame 0: another output.
  REQUIRE_FALSE(SameBits(want.l, RenderStateReference(*clock, take).l));

  const std::vector<std::vector<int>> patterns = {{37}, {64}, {441}, {512}, {1024}, {4096}};
  for (const bool offline : {true, false}) {
    for (const auto& pattern : patterns) {
      INFO((offline ? "offline " : "real time ") << PatternName(pattern));
      TestPlayHead head;
      head.hasBpm = head.hasPpq = true;
      head.bpm                  = bpm;
      auto proc = MakeTempoProcessor(*clock, head, TempoSource::Host, true);
      proc->setNonRealtime(offline);
      // Stopped at another position first: the host's tempo goes out, nothing anchors.
      head.ppq = 1.0;
      RenderProcessor(*proc, MakeInput(4800), {}, {{480}});
      if (!offline) REQUIRE(WaitForSpare(*proc));
      head.playing     = true;
      const Stereo got = RenderProcessor(*proc, take, {}, Transport(head, ppq, bpm, pattern));
      head.playing     = false;
      REQUIRE(proc->GetStatus().lastStart == BrainscapeProcessor::TransportStart::Restarted);
      RequireSame(got, want, "a bounce at 137.5 BPM from ppq 3.37");
      REQUIRE(proc->GetTempoDisplay().followingHost);
      REQUIRE(proc->GetTempoDisplay().nsPerQuarter == ns);
      proc->setPlayHead(nullptr);
    }
  }

  SECTION("row 83 changed before play: the restart re-asserts it (E7)") {
    const Stereo x2 = RenderStateReference(*clock, take, kRate, reference(3));
    REQUIRE_FALSE(SameBits(x2.l, want.l));
    for (const auto& pattern : std::vector<std::vector<int>>{{441}, {4096}}) {
      TestPlayHead head;
      head.hasBpm = head.hasPpq = true;
      head.bpm                  = bpm;
      auto proc = MakeTempoProcessor(*clock, head, TempoSource::Host, true);
      proc->setNonRealtime(true);
      RenderProcessor(*proc, MakeInput(4800), {}, {{480}});
      proc->SubdivParam().SetPlainNotifyingHost(3.0f);  // x2
      RenderProcessor(*proc, MakeInput(4800), {}, {{480}});
      head.playing = true;
      RequireSame(RenderProcessor(*proc, take, {}, Transport(head, ppq, bpm, pattern)), x2, "x2 re-asserted");
      head.playing = false;
      proc->setPlayHead(nullptr);
    }
  }
  SECTION("a stop and a second play at another ppq anchors its first hit on the host's beat") {
    TestPlayHead head;
    head.hasBpm = head.hasPpq = true;
    head.bpm                  = bpm;
    auto proc = MakeTempoProcessor(*clock, head, TempoSource::Host, true);
    proc->setNonRealtime(true);
    head.playing = true;
    RenderProcessor(*proc, take, {}, Transport(head, ppq, bpm, {441}));
    head.playing = false;
    RenderProcessor(*proc, MakeInput(9600), {}, {{441}});
    const uint64_t births = proc->EngineTempoCounts().clockBirths;  // counts since Init
    const double ppq2  = 10.123;
    const double beat2 = HostBeatFrame(11.0, ppq2, bpm);
    head.playing       = true;
    RenderProcessor(*proc, MakeInput(static_cast<int>(beat2) + 600), {}, Transport(head, ppq2, bpm, {441}));
    head.playing = false;
    const TempoInfo info = proc->EngineTempo();
    REQUIRE(std::fabs(static_cast<double>(info.lastClockBirth) - beat2) <= 1.0);
    REQUIRE(proc->EngineTempoCounts().clockBirths == births + 1u);  // the second play's first quarter
    proc->setPlayHead(nullptr);
  }
}

TEST_CASE("host tempo: without the restart option the host's Start anchors the running grid") {
  // §10.1: "Restart on transport start" stays off by default, and the anchor aligns the grid to
  // the host's beats either way: the Start applies at the playing block's first frame, its offset
  // places the next tick, and the hits land on the host's quarters.
  REQUIRE_FALSE(WrapperSettings{}.restartOnStart);
  REQUIRE(WrapperSettings{}.tempoSource == TempoSource::Host);
  const auto clock = GoldenPackage("clock_hits.bsp");
  for (const auto& pattern : std::vector<std::vector<int>>{{256}, {37}, {1024}}) {
    INFO(PatternName(pattern));
    TestPlayHead head;
    head.hasBpm = head.hasPpq = true;
    head.bpm                  = 97.0;
    auto proc = MakeTempoProcessor(*clock, head, TempoSource::Host, false);
    // 0.5 s stopped (the grid runs at the host's tempo from frame 0), then play from ppq 21.7.
    head.ppq = 21.7;
    RenderProcessor(*proc, MakeInput(24000), {}, {{256}});
    const int64_t start = 24000;
    head.playing        = true;
    const double next   = HostBeatFrame(22.0, 21.7, 97.0, start);
    RenderProcessor(*proc, MakeInput(static_cast<int>(next) - start + 300), {},
                    Transport(head, 21.7, 97.0, pattern));
    const TempoInfo info = proc->EngineTempo();
    REQUIRE(proc->GetStatus().lastStart == BrainscapeProcessor::TransportStart::None);
    REQUIRE((info.flags & kTempoFlagRunning) != 0u);
    REQUIRE(std::fabs(static_cast<double>(info.lastGridFrame) - next) <= 1.0);
    REQUIRE(std::fabs(static_cast<double>(info.lastClockBirth) - next) <= 1.0);
    REQUIRE(info.nsPerQuarter == tempo::NsPerQuarterFromBpm(97.0));
    proc->setPlayHead(nullptr);
  }
}

TEST_CASE("host tempo: stop, loop, jumps and tempo changes") {
  const auto   clock = GoldenPackage("clock_hits.bsp");
  TestPlayHead head;
  head.hasBpm = head.hasPpq = true;
  head.bpm                  = 120.0;
  auto proc = MakeTempoProcessor(*clock, head, TempoSource::Host, false);
  // The host's position, continuous from render to render unless a test moves it.
  double     ppq  = 0.0;
  const auto play = [&](int frames, double bpm) {
    HostRender   r;
    const double base = ppq;
    r.pattern         = {512};
    r.beforeBlock     = [&head, base, bpm](int pos) {
      head.bpm = bpm;
      head.ppq = base + static_cast<double>(pos) * bpm / (60.0 * kRate);
    };
    RenderProcessor(*proc, MakeInput(frames), {}, r);
    ppq = base + static_cast<double>(frames) * bpm / (60.0 * kRate);
  };
  play(4800, 120.0);  // stopped: the host's tempo goes out, no transport
  ppq = 0.0;
  const TempoStats s0 = proc->EngineTempoCounts();
  REQUIRE(s0.tempoEvents >= 1u);
  REQUIRE(s0.transports == 0u);

  // Play 8 quarters at 120 BPM from ppq 0: one Start, no Locate (each block where predicted).
  head.playing  = true;
  int64_t frame = 4800;
  play(8 * 24000, 120.0);
  frame += 8 * 24000;
  REQUIRE(proc->EngineTempoCounts().transports == 1u);
  REQUIRE((proc->EngineTempo().flags & kTempoFlagRunning) != 0u);

  SECTION("a loop back to ppq 2 is a Locate, anchored: the next hit on the host's beat") {
    ppq = 2.0;
    play(24000 + 3000, 120.0);
    REQUIRE(proc->EngineTempoCounts().transports == 2u);
    // Beat 3 of the loop, 24,000 frames into it (1,000 frames a tick at 120 BPM and 48 kHz).
    REQUIRE(proc->EngineTempo().lastGridFrame == frame + 24000);
    REQUIRE((proc->EngineTempo().flags & kTempoFlagRunning) != 0u);  // a Locate keeps it running
  }
  SECTION("a jump within half a tick is no Locate; past half a tick it is") {
    ppq += 0.4 / 24.0;
    play(512, 120.0);
    REQUIRE(proc->EngineTempoCounts().transports == 1u);
    ppq += 0.6 / 24.0;
    play(512, 120.0);
    REQUIRE(proc->EngineTempoCounts().transports == 2u);
  }
  SECTION("Stop at the first block that does not play; the grid runs on") {
    head.playing = false;
    play(480, 120.0);
    REQUIRE(proc->EngineTempoCounts().transports == 2u);
    REQUIRE((proc->EngineTempo().flags & kTempoFlagRunning) == 0u);
    play(4800, 120.0);
    REQUIRE(proc->EngineTempoCounts().transports == 2u);  // one Stop only
  }
  SECTION("a tempo change of 1 µs per quarter or more is a Tempo event; rounding noise is not") {
    const uint64_t before = proc->EngineTempoCounts().tempoEvents;
    play(2048, 6.0e10 / 500000500.0);  // 0.5 µs per quarter longer
    REQUIRE(proc->EngineTempoCounts().tempoEvents == before);
    REQUIRE(proc->EngineTempo().nsPerQuarter == 500000000u);
    play(512, 6.0e10 / 499999000.0);  // 1 µs shorter
    REQUIRE(proc->EngineTempoCounts().tempoEvents == before + 1u);
    REQUIRE(proc->EngineTempo().nsPerQuarter == 499999000u);
    play(512, 128.0);
    REQUIRE(proc->EngineTempo().nsPerQuarter == 468750000u);
    REQUIRE(proc->EngineTempoCounts().transports == 1u);  // a ramp is no jump
  }
  proc->setPlayHead(nullptr);
}

TEST_CASE("host tempo: taps and typed tempos are dropped while the host's tempo is followed") {
  TestPlayHead head;
  head.hasBpm = true;
  head.bpm    = 128.0;
  auto proc   = MakeProcessor({}, {});
  proc->setPlayHead(&head);
  const Stereo block = MakeInput(9600);
  RenderProcessor(*proc, block, {}, {{480}});
  REQUIRE(proc->GetTempoDisplay().followingHost);
  REQUIRE(proc->GetTempoDisplay().nsPerQuarter == 468750000u);
  for (int k = 0; k < 3; ++k) {
    proc->TapFromUi();
    RenderProcessor(*proc, block, {}, {{480}});
  }
  REQUIRE(proc->SetTempoFromUi(90.0));
  RenderProcessor(*proc, block, {}, {{480}});
  REQUIRE(proc->EngineTempoCounts().taps == 0u);
  REQUIRE(proc->EngineTempo().nsPerQuarter == 468750000u);

  // Internal: the taps set the tempo (three taps 9,600 frames apart: 300 BPM), then a typed one.
  WrapperSettings s = proc->GetSettings();
  s.tempoSource     = TempoSource::Internal;
  proc->SetSettings(s);
  RenderProcessor(*proc, block, {}, {{480}});
  REQUIRE_FALSE(proc->GetTempoDisplay().followingHost);
  for (int k = 0; k < 3; ++k) {
    proc->TapFromUi();
    RenderProcessor(*proc, block, {}, {{480}});
  }
  REQUIRE(proc->EngineTempoCounts().taps == 3u);
  REQUIRE(proc->EngineTempo().nsPerQuarter == 200000000u);
  REQUIRE(proc->SetTempoFromUi(93.75));
  REQUIRE_FALSE(proc->SetTempoFromUi(0.0));
  REQUIRE_FALSE(proc->SetTempoFromUi(std::numeric_limits<double>::quiet_NaN()));
  RenderProcessor(*proc, MakeInput(480), {}, {{480}});
  REQUIRE(proc->EngineTempo().nsPerQuarter == 640000000u);
  REQUIRE(proc->GetTempoDisplay().nsPerQuarter == 640000000u);
  // A host without a tempo (the Standalone, some hosts) is Internal under Host too.
  s.tempoSource = TempoSource::Host;
  proc->SetSettings(s);
  head.hasBpm = false;
  REQUIRE(proc->SetTempoFromUi(110.0));
  RenderProcessor(*proc, MakeInput(480), {}, {{480}});
  REQUIRE_FALSE(proc->GetTempoDisplay().followingHost);
  REQUIRE(proc->EngineTempo().nsPerQuarter == tempo::NsPerQuarterFromBpm(110.0));
  proc->setPlayHead(nullptr);
}

TEST_CASE("host tempo: a preset recalled while following plays the host's tempo after its load") {
  // Under recall Preset a Spillover load plays the preset's stored tempo; the host's goes out
  // again after it, at the load's frame, so the host keeps the tempo (§3.6: the external source
  // wins, as a clock does).
  const auto   clock = GoldenPackage("clock_hits.bsp");  // stored at 140 BPM
  TestPlayHead head;
  head.hasBpm = true;
  head.bpm    = 128.0;
  auto proc   = MakeProcessor({}, {});
  WrapperSettings s   = proc->GetSettings();
  s.tempoRecallPreset = true;
  proc->SetSettings(s);
  proc->setPlayHead(&head);
  RenderProcessor(*proc, MakeInput(4800), {}, {{480}});
  REQUIRE(proc->LoadPresetState(*clock));
  RenderProcessor(*proc, MakeInput(4800), {}, {{480}});
  REQUIRE(proc->EngineTempo().nsPerQuarter == 468750000u);
  // Internal: the recall plays the stored tempo.
  s.tempoSource = TempoSource::Internal;
  proc->SetSettings(s);
  REQUIRE(proc->LoadPresetState(*clock));
  RenderProcessor(*proc, MakeInput(4800), {}, {{480}});
  REQUIRE(proc->EngineTempo().nsPerQuarter == 428571000u);
  REQUIRE(proc->GetTempoDisplay().nsPerQuarter == 428571000u);
  // Keep: a preset change keeps the running tempo.
  s.tempoRecallPreset = false;
  proc->SetSettings(s);
  REQUIRE(proc->SetTempoFromUi(100.0));
  RenderProcessor(*proc, MakeInput(480), {}, {{480}});
  REQUIRE(proc->LoadPresetState(*clock));
  RenderProcessor(*proc, MakeInput(4800), {}, {{480}});
  REQUIRE(proc->EngineTempo().nsPerQuarter == 600000000u);
  proc->setPlayHead(nullptr);
}

namespace {

// A host tempo ramp from b0 to b1 BPM over d seconds from ppq 0, then b1: the host's tempo and
// position at time t (seconds), exactly.
struct Ramp {
  double b0, b1, d;
  double Bpm(double t) const { return t <= d ? b0 + (b1 - b0) * t / d : b1; }
  double Ppq(double t) const {
    if (t <= d) return (b0 * t + 0.5 * (b1 - b0) * t * t / d) / 60.0;
    return (b0 * d + 0.5 * (b1 - b0) * d) / 60.0 + b1 * (t - d) / 60.0;
  }
};

// A host playing `ramp` from ppq 0 over `seconds` in blocks of `block`, followed with the restart
// option off: the last CLOCK hit's distance from the host's nearest quarter, in ms (its quarters
// divided by 2^octaves when the host's tempo is folded), and the transports the engine got.
struct RampResult {
  double   offMs      = 0.0;
  uint64_t transports = 0;
};
RampResult RunRamp(const PresetState& clock, const Ramp& ramp, int block, double seconds, double beatsPerQuarter = 1.0) {
  TestPlayHead head;
  head.hasBpm = head.hasPpq = true;
  head.bpm                  = ramp.b0;
  auto proc    = MakeTempoProcessor(clock, head, TempoSource::Host, false, 4096);
  head.playing = true;
  HostRender r;
  r.pattern     = {block};
  r.beforeBlock = [&head, &ramp](int pos) {
    const double t = static_cast<double>(pos) / kRate;
    head.bpm       = ramp.Bpm(t);
    head.ppq       = ramp.Ppq(t);
  };
  RenderProcessor(*proc, MakeInput(static_cast<int>(seconds * kRate)), {}, r);
  const TempoInfo info = proc->EngineTempo();
  REQUIRE(info.lastGridFrame > 0);
  const double t = static_cast<double>(info.lastGridFrame) / kRate;
  const double q = ramp.Ppq(t) / beatsPerQuarter;  // in the engine's quarters
  RampResult   out;
  out.offMs      = (q - std::round(q)) * beatsPerQuarter * 60.0 / ramp.Bpm(t) * 1000.0;
  out.transports = proc->EngineTempoCounts().transports;
  proc->setPlayHead(nullptr);
  return out;
}

}  // namespace

TEST_CASE("host tempo: a ramp keeps the CLOCK grid on the host's beats (§4.4 step 3, §11.16)") {
  // The engine plays each host block at the tempo of its first frame, so over a ramp its grid
  // falls behind or ahead of the host's beats; the follower measures the engine's grid against
  // the host's position and re-anchors it past half a tick. T3's prediction from the host's own
  // last position missed it: 60 to 180 BPM at 4,096-frame blocks ended 28.5 ms late, 180 to 60
  // 85 ms early, with no Locate.
  const auto clock = GoldenPackage("clock_hits.bsp");
  struct Case {
    Ramp ramp;
    int  block;
  };
  for (const Case c : {Case{{60.0, 180.0, 16.0}, 4096}, Case{{180.0, 60.0, 16.0}, 4096},
                       Case{{90.0, 150.0, 8.0}, 2048}, Case{{60.0, 180.0, 16.0}, 512}}) {
    INFO(c.ramp.b0 << " to " << c.ramp.b1 << " BPM at blocks of " << c.block);
    const RampResult r       = RunRamp(*clock, c.ramp, c.block, c.ramp.d + 4.0);
    const double     halfTick = 0.5 * 60.0 / (24.0 * c.ramp.b1) * 1000.0;
    CHECK(std::fabs(r.offMs) < halfTick);
    // At 4,096-frame blocks it drifts past half a tick: the Start, and the Locates that kept it.
    if (c.block == 4096) CHECK(r.transports >= 2u);
  }
  // At a constant tempo the grid never leaves the host's beats: the Start alone.
  const RampResult still = RunRamp(*clock, {120.0, 120.0, 1.0}, 4096, 20.0);
  CHECK(std::fabs(still.offMs) < 0.03);
  CHECK(still.transports == 1u);
}

TEST_CASE("host tempo: a host tempo outside 20-300 BPM folds by octaves, the grid on its beats "
          "(§4.4, §11.16)") {
  // Clamped, the engine ran at 300 BPM under a 400 BPM host, the grid drifting off its beats at
  // once with the lock dot lit. Folded, the engine runs at 200 BPM with a quarter on every second
  // host beat, and at 30 BPM under a 15 BPM host with a quarter on every half beat.
  const auto clock = GoldenPackage("clock_hits.bsp");
  struct Case {
    double   bpm;
    uint32_t ns;
    int8_t   octaves;
    double   beatsPerQuarter;
  };
  for (const Case c : {Case{400.0, 300000000u, 1, 2.0}, Case{15.0, 2000000000u, -1, 0.5},
                       Case{1000.0, 240000000u, 2, 4.0}}) {
    INFO(c.bpm << " BPM");
    const RampResult r = RunRamp(*clock, {c.bpm, c.bpm, 1.0}, 512, 12.0, c.beatsPerQuarter);
    CHECK(std::fabs(r.offMs) < 0.05);
    CHECK(r.transports == 1u);
    TestPlayHead head;
    head.hasBpm = head.hasPpq = true;
    head.bpm                  = c.bpm;
    head.playing              = true;
    auto proc = MakeTempoProcessor(*clock, head, TempoSource::Host, false);
    RenderProcessor(*proc, MakeInput(4800), {}, Transport(head, 1.0, c.bpm, {480}));
    CHECK(proc->EngineTempo().nsPerQuarter == c.ns);
    const BrainscapeProcessor::TempoDisplay d = proc->GetTempoDisplay();
    CHECK(d.followingHost);
    CHECK(d.hostOctaves == c.octaves);
    CHECK_FALSE(d.hostClamped);
    proc->setPlayHead(nullptr);
  }
}

namespace {

// MIDI messages at absolute frames, handed to processBlock in host blocks of `pattern`.
using TimedMidi = std::vector<std::pair<int, juce::MidiMessage>>;

Stereo RenderMidi(BrainscapeProcessor& p, const Stereo& in, const std::vector<int>& pattern,
                  const TimedMidi& messages) {
  const int frames = static_cast<int>(in.l.size());
  Stereo    io     = in;
  juce::MidiBuffer midi;
  midi.ensureSize(8192);
  size_t   k = 0, mi = 0;
  int      pos  = 0;
  uint64_t hits = gAuditHits.load();
  while (pos < frames) {
    const int n = std::min(pattern[k++ % pattern.size()], frames - pos);
    float*    chans[2] = {io.l.data() + pos, io.r.data() + pos};
    juce::AudioBuffer<float> buffer(chans, 2, n);
    midi.clear();
    for (; mi < messages.size() && messages[mi].first < pos + n; ++mi) {
      midi.addEvent(messages[mi].second, messages[mi].first - pos);
    }
    tAudit = true;
    p.processBlock(buffer, midi);
    tAudit = false;
    pos += n;
  }
  CHECK(gAuditHits.load() - hits == 0u);
  return io;
}

}  // namespace

TEST_CASE("MIDI clock in the Standalone: the device's bytes become §4.3's events at their frames") {
  // §10.2: the Standalone's MIDI path passes F8, FA, FB, FC and F2 to the translator at their
  // sample positions, at the MIDI rank, with every other message's bytes in order (a Program
  // Change before a Song Position, SysEx, Active Sensing, note-ons among the ticks), and Receive
  // MIDI clock gates it. A clock mode follows: the output equals the engine fed the translator's
  // events at the same frames.
  const auto clock = GoldenPackage("clock_hits.bsp");
  const int  frames = 4 * 48000;
  TimedMidi  messages;
  const uint8_t sysex[] = {0x7D, 0x01, 0x02};
  messages.push_back({200, juce::MidiMessage::programChange(1, 5)});
  messages.push_back({200, juce::MidiMessage::songPositionPointer(16)});  // 16 sixteenths: tick 96
  messages.push_back({300, juce::MidiMessage::createSysExMessage(sysex, 3)});
  messages.push_back({300, juce::MidiMessage(0xFE)});                     // Active Sensing
  messages.push_back({400, juce::MidiMessage::midiContinue()});
  const double tick = 48000.0 * 60.0 / 132.0 / 24.0;  // 909.09 frames: 132 BPM
  int          n    = 0;
  for (double f = 1000.0; f < frames - 1000; f += tick, ++n) {
    messages.push_back({static_cast<int>(f), juce::MidiMessage::midiClock()});
    if (n % 30 == 7) messages.push_back({static_cast<int>(f), juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(90))});
    if (n == 150) messages.push_back({static_cast<int>(f) + 5, juce::MidiMessage::midiStop()});
    if (n == 170) messages.push_back({static_cast<int>(f) + 5, juce::MidiMessage::midiStart()});
  }
  std::stable_sort(messages.begin(), messages.end(),
                   [](const auto& a, const auto& b) { return a.first < b.first; });

  // The reference: the processor's frame-0 events (the Exact load is a preset change under Keep:
  // the running 120 BPM, row 85), then each message's bytes through one translator, and note-ons
  // as MIDI-note triggers, in message order at their frames.
  std::vector<RefEvent> ev = {RefTempo(0, 500000000u), RefRecall(0, false)};
  MidiClockParser       parser;
  int                   ticks = 0;
  for (const auto& m : messages) {
    const juce::MidiMessage& msg = m.second;
    if (msg.isNoteOn()) ev.push_back(RefTrigger(m.first, Engine::TriggerSource::MidiNote, 90.0f / 127.0f));
    for (int i = 0; i < msg.getRawDataSize(); ++i) {
      MidiClockEvent c;
      if (parser.Feed(msg.getRawData()[i], &c) != MidiClockParser::Result::Event) continue;
      ticks += c.type == tempo::kEventClockTick ? 1 : 0;
      ev.push_back(RefTempoEvent(m.first, static_cast<Engine::EventType>(c.type), c.id, c.valueBits));
    }
  }
  REQUIRE(ticks == n);
  const Stereo in   = MakeInput(frames);
  const Stereo want = RenderStateReference(*clock, in, kRate, ev);
  REQUIRE_FALSE(SameBits(want.l, RenderStateReference(*clock, in, kRate, {ev[0], ev[1]}).l));

  juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_Standalone);
  std::unique_ptr<BrainscapeProcessor> app = MakeProcessor({}, {}, 4096);
  juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_Undefined);
  REQUIRE(app->wrapperType == juce::AudioProcessor::wrapperType_Standalone);
  REQUIRE(app->GetSettings().receiveMidiClock);
  for (const auto& pattern : std::vector<std::vector<int>>{{441}, {37}, {4096}, {512, 1, 77}}) {
    INFO(PatternName(pattern));
    auto proc = MakeProcessor({}, {}, 4096);
    REQUIRE(proc->LoadPresetState(*clock));
    RequireSame(RenderMidi(*proc, in, pattern, messages), want, "MIDI clock at its frames");
    const TempoStats counts = proc->EngineTempoCounts();
    REQUIRE(counts.ticks == static_cast<uint64_t>(n));
    REQUIRE(counts.commits >= 1u);
    REQUIRE(proc->GetTempoDisplay().source == static_cast<uint8_t>(tempo::ClockSource::ClockRunning));
    REQUIRE(proc->GetTempoDisplay().locked);
    REQUIRE(proc->GetTempoDisplay().nsPerQuarter == proc->EngineTempo().nsPerQuarter);
  }
  REQUIRE(app->LoadPresetState(*clock));
  RequireSame(RenderMidi(*app, in, {441}, messages), want, "the Standalone");

  SECTION("Receive MIDI clock off: the bytes reach no translator") {
    auto            proc = MakeProcessor({}, {}, 4096);
    WrapperSettings s    = proc->GetSettings();
    s.receiveMidiClock   = false;
    proc->SetSettings(s);
    REQUIRE(proc->LoadPresetState(*clock));
    std::vector<RefEvent> notes = {ev[0], ev[1]};
    for (const RefEvent& e : ev) {
      if (e.type == Engine::EventType::Trigger) notes.push_back(e);
    }
    RequireSame(RenderMidi(*proc, in, {441}, messages), RenderStateReference(*clock, in, kRate, notes),
                "clock off: note-ons only");
    REQUIRE(proc->EngineTempoCounts().ticks == 0u);
  }
  SECTION("an Exact load re-asserts the master's position and that it runs (§2.5 item 2)") {
    auto proc = MakeProcessor({}, {}, 4096);
    REQUIRE(proc->LoadPresetState(*clock));
    const int half = frames / 2;
    TimedMidi first(messages.begin(), std::find_if(messages.begin(), messages.end(),
                                                   [&](const auto& m) { return m.first >= half; }));
    RenderMidi(*proc, Slice(in, 0, half), {441}, first);
    REQUIRE(proc->GetTempoDisplay().source == static_cast<uint8_t>(tempo::ClockSource::ClockRunning));
    // A device change restarts the engine (prepareToPlay at another rate, then back).
    proc->prepareToPlay(96000.0, 4096);
    proc->prepareToPlay(48000.0, 4096);
    juce::AudioBuffer<float> buffer(2, 64);
    buffer.clear();
    juce::MidiBuffer none;
    proc->processBlock(buffer, none);
    const TempoStats counts = proc->EngineTempoCounts();
    // The restart cleared the counts' engine: the re-asserted Locate and Continue are armed,
    // waiting for the master's next tick (§3.4), and the tempo crossed the restart.
    REQUIRE(counts.transports == 2u);
    REQUIRE(proc->EngineTempo().nsPerQuarter == proc->GetTempoDisplay().nsPerQuarter);
    REQUIRE(proc->EngineTempo().source == static_cast<uint8_t>(tempo::ClockSource::Internal));
  }
}

TEST_CASE("MIDI clock in the Standalone: a master gone without its Stop does not hold the grid "
          "after an Exact load (§3.4, §11.16)") {
  // The translator kept Running() after the master vanished (a cable pulled, the app quit) or
  // after its Stop arrived while Receive MIDI clock was off; every Exact load then re-asserted an
  // armed Continue, which held the CLOCK grid for a tick that never came.
  const auto clock = GoldenPackage("clock_hits.bsp");
  const auto standalone = [] {
    juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_Standalone);
    auto p = MakeProcessor({}, {}, 4096);
    juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_Undefined);
    return p;
  };
  TimedMidi master;
  master.push_back({100, juce::MidiMessage::midiStart()});
  for (int f = 1000; f < 3 * 48000; f += 1000) master.push_back({f, juce::MidiMessage::midiClock()});

  SECTION("the master vanishes: no Stop") {
    auto proc = standalone();
    REQUIRE(proc->LoadPresetState(*clock));
    RenderMidi(*proc, MakeInput(3 * 48000), {441}, master);
    REQUIRE(proc->GetTempoDisplay().source == static_cast<uint8_t>(tempo::ClockSource::ClockRunning));
    RenderMidi(*proc, MakeInput(2 * 48000), {441}, {});  // past §3.5's second: Internal again
    REQUIRE(proc->GetTempoDisplay().source == static_cast<uint8_t>(tempo::ClockSource::Internal));
    proc->prepareToPlay(44100.0, 4096);
    proc->prepareToPlay(48000.0, 4096);
    RenderMidi(*proc, MakeInput(5 * 48000), {441}, {});
    CHECK(proc->EngineTempoCounts().clockBirths >= 8u);
    CHECK(proc->EngineTempo().lastGridFrame > 0);
  }
  SECTION("its Stop arrives while Receive MIDI clock is off") {
    auto proc = standalone();
    REQUIRE(proc->LoadPresetState(*clock));
    RenderMidi(*proc, MakeInput(3 * 48000), {441}, master);
    WrapperSettings s  = proc->GetSettings();
    s.receiveMidiClock = false;
    proc->SetSettings(s);
    RenderMidi(*proc, MakeInput(2 * 48000), {441}, {{500, juce::MidiMessage::midiStop()}});
    s.receiveMidiClock = true;
    proc->SetSettings(s);
    RenderMidi(*proc, MakeInput(48000), {441}, {});
    proc->prepareToPlay(44100.0, 4096);
    proc->prepareToPlay(48000.0, 4096);
    RenderMidi(*proc, MakeInput(5 * 48000), {441}, {});
    CHECK(proc->EngineTempoCounts().clockBirths >= 8u);
    CHECK(proc->EngineTempo().lastGridFrame > 0);
  }
}

TEST_CASE("the internal tempo, Subdiv and time mode persist in the session and across a restart") {
  // §10.1 (P12): the session saves the last committed tempo, so a tapped or typed tempo survives
  // a session save and restore, a relaunch and a change of rate; rows 83 and 84 with it.
  auto            a = MakeProcessor({}, {});
  WrapperSettings s = a->GetSettings();
  s.tempoSource     = TempoSource::Internal;
  s.tempoRecallPreset = true;
  s.receiveMidiClock  = false;
  a->SetSettings(s);
  REQUIRE(a->SetTempoFromUi(93.75));
  a->SubdivParam().SetPlainNotifyingHost(4.0f);    // x4
  a->TimeModeParam().SetPlainNotifyingHost(2.0f);  // Tempo
  RenderProcessor(*a, MakeInput(480), {}, {{480}});
  REQUIRE(a->EngineTempo().nsPerQuarter == 640000000u);
  juce::MemoryBlock blob;
  a->getStateInformation(blob);
  WrapperState st{};
  REQUIRE(DecodeState(blob.getData(), blob.getSize(), st));
  REQUIRE(st.tempoNs == 640000000u);
  REQUIRE(st.hasPerformance);
  REQUIRE(st.subdivPosition == 4.0f);
  REQUIRE(st.timeMode == 2.0f);
  REQUIRE(st.settings.tempoSource == TempoSource::Internal);
  REQUIRE(st.settings.tempoRecallPreset);
  REQUIRE_FALSE(st.settings.receiveMidiClock);
  std::vector<uint8_t> again;
  EncodeState(st, again);
  REQUIRE(again.size() == blob.getSize());
  REQUIRE(std::memcmp(again.data(), blob.getData(), again.size()) == 0);

  const auto check = [](BrainscapeProcessor& p, const char* what) {
    INFO(what);
    RenderProcessor(p, MakeInput(480), {}, {{480}});
    REQUIRE(p.EngineTempo().nsPerQuarter == 640000000u);
    REQUIRE(p.EngineTempo().subdiv == 4u);
    REQUIRE(p.EngineTempo().timeMode == 2u);
    REQUIRE(p.GetTempoDisplay().nsPerQuarter == 640000000u);
  };
  SECTION("restored before anything plays (a project reload, a relaunch): an Exact load") {
    auto b = MakeProcessor({}, {});
    b->setStateInformation(blob.getData(), static_cast<int>(blob.getSize()));
    REQUIRE(b->GetTempoDisplay().nsPerQuarter == 640000000u);  // before any audio
    REQUIRE(b->SubdivParam().Plain() == 4.0f);
    REQUIRE(b->GetSettings().tempoSource == TempoSource::Internal);
    check(*b, "restored");
    b->prepareToPlay(96000.0, 512);  // a change of device rate: Init, an Exact load
    check(*b, "at 96 kHz");
    b->prepareToPlay(44100.0, 512);
    check(*b, "at 44.1 kHz");
  }
  SECTION("restored into a processor not yet prepared (the Standalone's launch)") {
    auto b = std::make_unique<BrainscapeProcessor>();
    b->setStateInformation(blob.getData(), static_cast<int>(blob.getSize()));
    b->setRateAndBufferSizeDetails(kRate, 512);
    b->prepareToPlay(kRate, 512);
    check(*b, "prepared after the restore");
  }
  SECTION("restored while running: the session's tempo and rows follow its Spillover load") {
    auto b = MakeProcessor({}, {});
    RenderProcessor(*b, MakeInput(4800), {}, {{480}});
    b->setStateInformation(blob.getData(), static_cast<int>(blob.getSize()));
    check(*b, "restored live");
  }
  SECTION("a session without the tempo keys plays the preset's stored ones") {
    WrapperState old = st;
    old.tempoNs        = 0;
    old.hasPerformance = false;
    std::vector<uint8_t> bytes;
    EncodeState(old, bytes);
    auto b = MakeProcessor({}, {});
    b->setStateInformation(bytes.data(), static_cast<int>(bytes.size()));
    RenderProcessor(*b, MakeInput(480), {}, {{480}});
    REQUIRE(b->EngineTempo().nsPerQuarter == 500000000u);
    REQUIRE(b->EngineTempo().subdiv == 0u);
    REQUIRE(b->SubdivParam().Plain() == 2.0f);  // TAP
  }
  SECTION("a same-rate prepareToPlay keeps the running engine, and its tempo") {
    a->prepareToPlay(kRate, 512);
    check(*a, "re-prepared");
  }
  SECTION("a tapped tempo, not a whole number of ns, survives to the ns (§8.4)") {
    // Three taps 9,700 frames apart: 9,700 frames a quarter, 202,083,333.3 ns.
    for (int k = 0; k < 3; ++k) {
      a->TapFromUi();
      RenderProcessor(*a, MakeInput(9700), {}, {{485}});
    }
    const uint32_t tapped = a->EngineTempo().nsPerQuarter;
    REQUIRE(tapped == 202083333u);
    juce::MemoryBlock taps;
    a->getStateInformation(taps);
    auto b = MakeProcessor({}, {});
    b->setStateInformation(taps.getData(), static_cast<int>(taps.getSize()));
    RenderProcessor(*b, MakeInput(480), {}, {{480}});
    REQUIRE(b->EngineTempo().nsPerQuarter == tapped);
    b->prepareToPlay(96000.0, 512);
    RenderProcessor(*b, MakeInput(960), {}, {{480}});
    REQUIRE(b->EngineTempo().nsPerQuarter == tapped);
  }
}

TEST_CASE("saving a preset captures the live tempo, Subdiv and time mode") {
  // §10.3: Save writes the live committed tempo (whole µs), the live Subdiv and time mode into
  // the document's performance, so what a recall under Preset and every Exact load play is what
  // the player set. Until then the session lists what Save will store.
  ScratchDir       scratch;
  const juce::File json = scratch.dir.getChildFile("clock_hits.json");
  REQUIRE(juce::File(BRAINSCAPE_GOLDEN_PRESETS).getChildFile("clock_hits.json").copyFileTo(json));
  auto             proc = MakeProcessor({}, {});
  CurationSession& s    = proc->Curation();
  juce::String     error;
  REQUIRE(s.Open(json, &error));
  // Under Keep the running 120 BPM crosses the load: Save would store it over the stored 140.
  s.Refresh();
  REQUIRE(s.PendingPerformance().size() == 1u);
  REQUIRE(s.PendingPerformance()[0].find("428571 -> 500000") != std::string::npos);
  REQUIRE(s.SaveChangesFile());
  REQUIRE_FALSE(s.Dirty());

  REQUIRE(proc->SetTempoFromUi(128.0));
  proc->SubdivParam().SetPlainNotifyingHost(3.0f);    // x2
  proc->TimeModeParam().SetPlainNotifyingHost(1.0f);  // Subdiv
  RenderProcessor(*proc, MakeInput(960), {}, {{480}});
  s.Refresh();
  REQUIRE(s.PendingPerformance().size() == 3u);
  const PerformanceState live = proc->LivePerformance();
  REQUIRE(live.usPerQuarter == 468750u);
  REQUIRE(live.subdiv == Subdivision::Double);
  REQUIRE(live.timeMode == brainscape::TimeMode::Subdivision);
  const CurationSession::SaveResult r = s.Save();
  REQUIRE(r.written);
  REQUIRE(r.compiled);
  bsc::Document              saved;
  std::vector<bsc::Finding>  found;
  REQUIRE(bsc::ReadDocumentText(Text(json), {}, &saved, &found));
  REQUIRE(saved.state->performance.usPerQuarter == 468750u);
  REQUIRE(saved.state->performance.subdiv == Subdivision::Double);
  REQUIRE(saved.state->performance.timeMode == brainscape::TimeMode::Subdivision);
  REQUIRE(Text(json).find("\"subdiv\": \"x2\"") != std::string::npos);
  s.Refresh();
  REQUIRE(s.PendingPerformance().empty());
  REQUIRE(proc->CurrentMode().performance.usPerQuarter == 468750u);  // what plays is what was saved
  // The rendered audition is what saves: the current preset carries the live performance.
  REQUIRE(proc->CurrentPreset()->performance.usPerQuarter == 468750u);

  SECTION("under recall Preset a document plays its stored tempo, so Save stores it unchanged") {
    WrapperSettings w   = proc->GetSettings();
    w.tempoRecallPreset = true;
    proc->SetSettings(w);
    const juce::File other = scratch.dir.getChildFile("clock_recall.json");
    REQUIRE(juce::File(BRAINSCAPE_GOLDEN_PRESETS).getChildFile("clock_recall.json").copyFileTo(other));
    REQUIRE(s.Open(other, &error));
    s.Refresh();
    REQUIRE(s.PendingPerformance().empty());
    REQUIRE(proc->SubdivParam().Plain() == 1.0f);    // x1/2, stored
    REQUIRE(proc->TimeModeParam().Plain() == 1.0f);  // Subdiv, stored
    REQUIRE(proc->GetTempoDisplay().nsPerQuarter == 600000000u);
  }
}

TEST_CASE("the tempo strip shows the tempo, its source and the rows, and sends taps") {
  TestPlayHead head;
  head.hasBpm = true;
  head.bpm    = 128.0;
  auto proc   = MakeProcessor({}, {});
  std::unique_ptr<juce::AudioProcessorEditor> owned(proc->createEditor());
  auto* editor = dynamic_cast<BrainscapeEditor*>(owned.get());
  REQUIRE(editor != nullptr);
  TempoPanel& t = editor->Tempo();
  editor->RefreshNow();
  REQUIRE(t.Bpm().getText() == "120.0");
  REQUIRE_FALSE(t.UsesTempo());  // the default mode reads no tempo (§6.6)
  REQUIRE(t.SubdivSegment(2).getToggleState());  // TAP
  REQUIRE(t.TimeSegment(0).getToggleState());    // Free
  REQUIRE_FALSE(t.Store().isEnabled());          // no document to store into

  proc->setPlayHead(&head);
  RenderProcessor(*proc, MakeInput(480), {}, {{480}});
  editor->RefreshNow();
  REQUIRE(t.Bpm().getText() == "128.0");
  REQUIRE_FALSE(t.Tap().isEnabled());  // the host's tempo is followed: taps are dropped
  REQUIRE(t.Settings().getButtonText() == "Sync: Host");

  // The menu switches the source; TAP sends a Tap; the segments set rows 83 and 84.
  WrapperSettings s = proc->GetSettings();
  s.tempoSource     = TempoSource::Internal;
  proc->SetSettings(s);
  RenderProcessor(*proc, MakeInput(480), {}, {{480}});
  editor->RefreshNow();
  REQUIRE(t.Tap().isEnabled());
  REQUIRE(t.Settings().getButtonText() == "Sync: Internal");
  t.Tap().onClick();
  RenderProcessor(*proc, MakeInput(480), {}, {{480}});
  REQUIRE(proc->EngineTempoCounts().taps == 1u);
  t.SubdivSegment(5).onClick();
  t.TimeSegment(2).onClick();
  REQUIRE(proc->SubdivParam().Plain() == 5.0f);
  REQUIRE(proc->TimeModeParam().Plain() == 2.0f);
  RenderProcessor(*proc, MakeInput(480), {}, {{480}});
  REQUIRE(proc->EngineTempo().subdiv == 5u);
  REQUIRE(proc->EngineTempo().timeMode == 2u);
  // Typing a tempo sends it.
  t.Bpm().setText("97.5", juce::sendNotificationSync);
  RenderProcessor(*proc, MakeInput(480), {}, {{480}});
  editor->RefreshNow();
  REQUIRE(proc->EngineTempo().nsPerQuarter == tempo::NsPerQuarterFromBpm(97.5));
  REQUIRE(t.Bpm().getText() == "97.5");
  // The settings menu offers the source and the recall (and, in the Standalone, MIDI clock).
  int                   items = 0;
  const juce::PopupMenu menu  = t.SettingsMenu();
  for (juce::PopupMenu::MenuItemIterator it(menu); it.next();) items += it.getItem().isSectionHeader ? 0 : 1;
  REQUIRE(items == 4);
  // A clock mode uses tempo.
  REQUIRE(proc->LoadPresetState(*GoldenPackage("clock_hits.bsp")));
  editor->RefreshNow();
  REQUIRE(t.UsesTempo());
  proc->setPlayHead(nullptr);
  owned.reset();
}

int main(int argc, char* argv[]) {
  ReportCrtErrorsOnStderr();
#if defined(_MSC_VER) && defined(_DEBUG)
  _CrtSetAllocHook(CrtAllocHook);
#endif
  juce::ScopedJuceInitialiser_GUI juce;  // the attachment tests create sliders
  return Catch::Session().run(argc, argv);
}
