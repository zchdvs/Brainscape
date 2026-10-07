#include "ParityStream.h"

#include "Corpus.h"
#include "brainscape/SoundRevision.h"
#include "brainscape/TestSignal.h"

namespace brainscape::golden {

namespace {

constexpr const char* kStreamFormat = "brainscape-parity-stream/1";

const char* DeliveryName(Delivery d) {
  return d == Delivery::Engine ? "engine-events" : "split-at-event-frames";
}

// The harness's own compiler, as golden_main's report names it (the engine's is
// BuildToolchain()).
std::string HarnessToolchain() {
  std::string s;
#if defined(__clang__)
  s = std::string("clang ") + __clang_version__;
#elif defined(__GNUC__)
  s = std::string("gcc ") + __VERSION__;
#elif defined(_MSC_VER)
  s = "msvc " + JsonUInt(_MSC_FULL_VER);
#else
  s = "unknown";
#endif
#if defined(__x86_64__) || defined(_M_X64)
  s += " x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
  s += " aarch64";
#elif defined(__arm__)
  s += " arm";
#if defined(__ARM_ARCH_7EM__)
  s += " armv7e-m";
#endif
#endif
  return s;
}

bool Selected(const StreamOptions& o, const VectorCase& v, const PresetCase& p) {
  if (o.quick && v.longRender) return false;
  if (o.only.empty()) return true;
  for (const std::string& name : o.only) {
    if (name == v.name || name == std::string(v.name) + "/" + p.name) return true;
  }
  return false;
}

uint64_t Now(const StreamOptions& o) { return o.clock != nullptr ? o.clock() : 0; }

using Obj = JsonObj;

std::string Header(Renderer& renderer, const StreamOptions& o) {
  const RenderConfig& rc = renderer.Config();
  std::string         pattern = "[";
  for (size_t i = 0; i < rc.blockPattern.size(); ++i) {
    pattern += (i ? "," : "") + JsonUInt(rc.blockPattern[i]);
  }
  pattern += "]";
  const ToolchainId& id = BuildToolchain();
  const std::string  engine = Obj()
                                 .Str("compiler", id.compiler)
                                 .Str("version", id.version)
                                 .Str("target", id.target)
                                 .Str("fpFlags", id.fpFlags)
                                 .Str("fpFlagsHash", id.fpFlagsHash)
                                 .Done();
  const std::string clock = Obj().Str("name", o.clockName).UInt("hz", o.clockHz).Done();
  Obj h;
  h.Str("type", "parity-begin")
      .Str("format", kStreamFormat)
      .Int("soundRevision", kSoundRevision)
      .Int("generatorVersion", testsignal::kVersion)
      .Int("corpusVersion", kCorpusVersion)
      .Int("sampleRate", 48000)
      .Bool("stereoInput", true)
      .Bool("ditherRingWrite", true)
      .UInt("historyFrames", renderer.HistoryFrames())
      .UInt("maxBlockSize", rc.maxBlockSize)
      .Raw("blockPattern", pattern)
      .Str("delivery", DeliveryName(rc.delivery))
      .Str("start", rc.freshEngine ? "init" : "restart")
      .Str("fpEnv", rc.fpEnv == FpEnv::Clean ? "clean" : "hostile")
      .Bool("quick", o.quick)
      .Bool("subset", !o.only.empty())
      .Str("tag", o.tag)
      .Str("harnessToolchain", HarnessToolchain())
      .Raw("engineToolchain", engine)
      .Raw("clock", clock);
  if (!o.platformJson.empty()) h.Raw("platform", o.platformJson);
  return h.Done();
}

}  // namespace

StreamResult StreamCorpus(Renderer& renderer, const StreamOptions& o, LineSink& sink) {
  StreamResult result;
  sink.Line(Header(renderer, o));
  const uint64_t                start  = Now(o);
  const std::vector<VectorCase> corpus = BuildCorpus();
  for (const VectorCase& v : corpus) {
    bool any = false;
    for (const PresetCase& p : v.presets) any = any || Selected(o, v, p);
    if (!any) continue;

    const std::vector<testsignal::Note> notes = VectorNotes(v);
    sink.Line(Obj()
                  .Str("type", "vector")
                  .Str("name", v.name)
                  .Str("source", testsignal::VectorName(v.source))
                  .Int("generatorVersion", testsignal::kVersion)
                  .UInt("frames", v.frames)
                  .Str("inputHash", InputHash(v, notes))
                  .Raw("ringSizes", "[" + JsonUInt(uint64_t{1} << 22) + "]")
                  .UInt("notes", notes.size())
                  .Done());

    for (const PresetCase& p : v.presets) {
      if (!Selected(o, v, p)) continue;
      ++result.presets;
      RenderOutput   r;
      const uint64_t t0       = Now(o);
      const bool     rendered = renderer.Render(v, notes, p, &r);
      const uint64_t cycles   = Now(o) - t0;
      if (!rendered) ++result.renderFailures;
      std::string seconds = "[";
      for (size_t i = 0; i < r.secondHashes.size(); ++i) {
        seconds += (i ? "," : "") + JsonString(r.secondHashes[i]);
      }
      seconds += "]";
      Obj counters;
      for (size_t i = 0; i < static_cast<size_t>(Counter::kCount); ++i) {
        counters.Int(CounterName(static_cast<Counter>(i)), r.counters[i]);
      }
      sink.Line(Obj()
                    .Str("type", "preset")
                    .Str("vector", v.name)
                    .Str("name", p.name)
                    .Bool("rendered", rendered)
                    .Str("hash", r.hash)
                    .Raw("secondHashes", seconds)
                    .Raw("counters", counters.Done())
                    .Int("frames", r.counters[static_cast<size_t>(Counter::Frames)])
                    .UInt("cycles", cycles)
                    .Done());
    }
  }
  sink.Line(Obj()
                .Str("type", "parity-end")
                .Int("presets", result.presets)
                .Int("renderFailures", result.renderFailures)
                .UInt("cycles", Now(o) - start)
                .Done());
  return result;
}

}  // namespace brainscape::golden
