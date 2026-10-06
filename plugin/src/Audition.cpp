#include "Audition.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_cryptography/juce_cryptography.h>

#include "brainscape/Engine.h"
#include "brainscape/HostArenas.h"
#include "brainscape/InputCondition.h"
#include "brainscape/SoundRevision.h"
#include "brainscape/TestSignal.h"

namespace brainscape::plugin {

namespace {

constexpr double kRate = 48000.0;

size_t Frames(double seconds) { return static_cast<size_t>(seconds * kRate); }

uint32_t Bits(float v) {
  uint32_t u = 0;
  std::memcpy(&u, &v, sizeof u);
  return u;
}

// The interleaved little-endian float32 bytes of frames [from, to) of a stereo signal,
// streamed into the hash without a second copy of the signal.
class InterleavedStream final : public juce::InputStream {
 public:
  InterleavedStream(const std::vector<float>& l, const std::vector<float>& r, size_t from, size_t to)
      : l_(l), r_(r), from_(from), frames_(to - from) {}
  juce::int64 getTotalLength() override { return static_cast<juce::int64>(frames_) * 8; }
  bool        isExhausted() override { return pos_ >= getTotalLength(); }
  juce::int64 getPosition() override { return pos_; }
  bool        setPosition(juce::int64 p) override {
    pos_ = juce::jlimit<juce::int64>(0, getTotalLength(), p);
    return true;
  }
  int read(void* dest, int maxBytes) override {
    auto* out = static_cast<uint8_t*>(dest);
    int   n   = 0;
    for (; n < maxBytes && pos_ < getTotalLength(); ++n, ++pos_) {
      const auto     frame = from_ + static_cast<size_t>(pos_ / 8);
      const auto     byte  = static_cast<int>(pos_ % 8);
      const uint32_t u     = Bits(byte < 4 ? l_[frame] : r_[frame]);
      out[n]               = static_cast<uint8_t>(u >> (8 * (byte % 4)));
    }
    return n;
  }

 private:
  const std::vector<float>& l_;
  const std::vector<float>& r_;
  const size_t              from_, frames_;
  juce::int64               pos_ = 0;
};

juce::var PresetJson(const std::vector<float>& preset) {
  juce::Array<juce::var> leaves;
  for (size_t i = 0; i < kNumParams; ++i) {
    juce::DynamicObject::Ptr leaf = new juce::DynamicObject();
    leaf->setProperty("id", static_cast<int>(kParamTable[i].id));
    leaf->setProperty("name", juce::String(kParamTable[i].name));
    leaf->setProperty("value", static_cast<double>(preset[i]));
    const auto bits = static_cast<juce::int64>(Bits(preset[i]));
    leaf->setProperty("bits", juce::String::toHexString(bits).paddedLeft('0', 8));
    leaves.add(juce::var(leaf.get()));
  }
  return leaves;
}

}  // namespace

AuditionInput TestSignalInput() {
  using testsignal::Vector;
  const auto     active = static_cast<uint32_t>(Frames(kAuditionSignalSeconds));
  const uint32_t count  = testsignal::BuildVector(Vector::Plucks, active, nullptr, 0);
  std::vector<testsignal::Note> notes(count);
  testsignal::BuildVector(Vector::Plucks, active, notes.data(), count);

  AuditionInput in;
  const size_t  total = Frames(kAuditionSignalSeconds + kAuditionTailSeconds);
  in.l.resize(total);
  in.r.resize(total);
  auto gen = std::make_unique<testsignal::Generator>();
  gen->Start(notes.data(), count);
  for (size_t pos = 0; pos < total;) {
    const auto n = static_cast<uint32_t>(std::min<size_t>(512, total - pos));
    gen->Render(in.l.data() + pos, in.r.data() + pos, n);
    pos += n;
  }
  in.description = juce::String("test signal: ") + testsignal::VectorName(Vector::Plucks) +
                   ", generator v" + juce::String(testsignal::kVersion);
  return in;
}

AuditionInput FileInput(const juce::AudioBuffer<float>& audio, double sampleRate,
                        const juce::String& name) {
  AuditionInput in;
  in.description = "file: " + name;
  in.sourceRate  = sampleRate;
  in.converted   = sampleRate != kRate;
  const int    length = audio.getNumSamples();
  const float* a      = audio.getReadPointer(0);
  const float* b      = audio.getReadPointer(audio.getNumChannels() > 1 ? 1 : 0);
  if (!in.converted) {
    in.l.assign(a, a + length);
    in.r.assign(b, b + length);
  } else if (length > 0) {
    const double step   = sampleRate / kRate;
    const auto   frames = static_cast<size_t>(static_cast<double>(length - 1) / step) + 1u;
    in.l.resize(frames);
    in.r.resize(frames);
    for (size_t k = 0; k < frames; ++k) {
      const double phase = static_cast<double>(k) * step;
      const auto   i0    = static_cast<int>(phase);
      const int    i1    = std::min(i0 + 1, length - 1);
      const auto   frac  = static_cast<float>(phase - i0);
      in.l[k]            = a[i0] + (a[i1] - a[i0]) * frac;
      in.r[k]            = b[i0] + (b[i1] - b[i0]) * frac;
    }
  }
  in.l.resize(in.l.size() + Frames(kAuditionTailSeconds), 0.f);
  in.r.resize(in.r.size() + Frames(kAuditionTailSeconds), 0.f);
  return in;
}

bool RenderAudition(const float* preset, InputMode mode, AuditionInput& input,
                    AuditionOutput& out) {
  ConditionInput24(input.l.data(), input.l.data(), input.l.size());
  ConditionInput24(input.r.data(), input.r.data(), input.r.size());
  if (mode == InputMode::Mono) input.r = input.l;

  EngineConfig cfg;  // the canonical configuration (profile §2.3)
  cfg.sampleRate      = kRate;
  cfg.maxBlockSize    = 512;
  cfg.historyFrames   = 1u << 22;
  cfg.stereoInput     = true;
  cfg.ditherRingWrite = true;
  host::HeapArenas arenas(PlanMemory(cfg));
  auto             engine = std::make_unique<Engine>();
  if (!arenas.ok() || !engine->Init(cfg, arenas.get())) return false;
  auto state = std::make_unique<PresetState>();
  for (size_t i = 0; i < kNumParams; ++i) {
    state->leaves[i] = {static_cast<uint32_t>(kParamTable[i].id), preset[i]};
  }
  state->leafCount = static_cast<uint32_t>(kNumParams);
  engine->LoadPreset(*state, LoadMode::Exact);

  const size_t frames = input.l.size();
  out.l.assign(frames, 0.f);
  out.r.assign(frames, 0.f);
  for (size_t pos = 0; pos < frames;) {
    const auto             n       = static_cast<uint32_t>(std::min<size_t>(kAuditionBlock, frames - pos));
    const float*           ins[2]  = {input.l.data() + pos, input.r.data() + pos};
    float*                 outs[2] = {out.l.data() + pos, out.r.data() + pos};
    Engine::ProcessContext ctx;
    ctx.in        = ins;
    ctx.out       = outs;
    ctx.numFrames = n;
    engine->Process(ctx);
    pos += n;
  }
  return true;
}

juce::String InterleavedSha256(const std::vector<float>& l, const std::vector<float>& r) {
  InterleavedStream stream(l, r, 0, l.size());
  return juce::SHA256(stream).toHexString();
}

juce::StringArray SegmentSha256(const std::vector<float>& l, const std::vector<float>& r) {
  juce::StringArray hashes;
  for (size_t from = 0; from < l.size(); from += Frames(1.0)) {
    InterleavedStream stream(l, r, from, std::min(l.size(), from + Frames(1.0)));
    hashes.add(juce::SHA256(stream).toHexString());
  }
  return hashes;
}

// ── AuditionJob ───────────────────────────────────────────────────────────────────────

AuditionJob::~AuditionJob() {
  if (thread_.joinable()) thread_.join();
}

bool AuditionJob::Start(const juce::File& wav, const float* preset, InputMode mode, AuditionInput input) {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (result_.state == State::Running) return false;
    result_       = {};
    result_.state = State::Running;
    result_.wav   = wav;
    result_.message = "Rendering " + wav.getFileName() + "...";
  }
  if (thread_.joinable()) thread_.join();  // the previous render has finished
  thread_ = std::thread(&AuditionJob::Run, this, wav, std::vector<float>(preset, preset + kNumParams),
                        mode, std::move(input));
  return true;
}

AuditionJob::Result AuditionJob::Get() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return result_;
}

void AuditionJob::Finish(const Result& r) {
  const std::lock_guard<std::mutex> lock(mutex_);
  result_ = r;
}

void AuditionJob::Run(juce::File wav, std::vector<float> preset, InputMode mode, AuditionInput input) {
  Result r;
  r.wav   = wav;
  r.state = State::Failed;
  AuditionOutput out;
  if (!RenderAudition(preset.data(), mode, input, out)) {
    r.message = "The audition engine could not be set up";
    Finish(r);
    return;
  }

  wav.deleteFile();
  std::unique_ptr<juce::OutputStream> stream = wav.createOutputStream();
  juce::WavAudioFormat                format;
  const auto options = juce::AudioFormatWriterOptions{}
                           .withSampleRate(kRate)
                           .withNumChannels(2)
                           .withBitsPerSample(32)
                           .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
  std::unique_ptr<juce::AudioFormatWriter> writer =
      stream != nullptr ? format.createWriterFor(stream, options) : nullptr;
  const float* channels[2] = {out.l.data(), out.r.data()};
  const bool   written     = writer != nullptr &&
                       writer->writeFromFloatArrays(channels, 2, static_cast<int>(out.l.size()));
  writer.reset();
  if (!written) {
    r.message = "Could not write " + wav.getFullPathName();
    Finish(r);
    return;
  }

  // The recipe (companion §4.9): what the render was made from, so it can be made again.
  r.outputSha256                 = InterleavedSha256(out.l, out.r);
  const ToolchainId&       tc    = BuildToolchain();
  juce::DynamicObject::Ptr build = new juce::DynamicObject();
  build->setProperty("compiler", juce::String(tc.compiler));
  build->setProperty("version", juce::String(tc.version));
  build->setProperty("target", juce::String(tc.target));
  build->setProperty("fpFlagsHash", juce::String(tc.fpFlagsHash));
  juce::DynamicObject::Ptr in = new juce::DynamicObject();
  in->setProperty("source", input.description);
  in->setProperty("sourceRate", input.sourceRate);
  in->setProperty("conversion", input.converted ? "linear interpolation to 48 kHz" : "none");
  in->setProperty("conditioning", "ConditionInput24");
  in->setProperty("mode", mode == InputMode::Mono ? "mono (R = L)" : "stereo");
  in->setProperty("sha256", InterleavedSha256(input.l, input.r));
  juce::DynamicObject::Ptr recipe = new juce::DynamicObject();
  recipe->setProperty("format", "brainscape-audition/1");
  recipe->setProperty("soundRevision", static_cast<int>(kSoundRevision));
  recipe->setProperty("engineToolchain", juce::var(build.get()));
  recipe->setProperty("sampleRate", static_cast<int>(kRate));
  recipe->setProperty("frames", static_cast<juce::int64>(out.l.size()));
  recipe->setProperty("blockFrames", kAuditionBlock);
  recipe->setProperty("start", "Init, then LoadPreset(Exact)");
  recipe->setProperty("events", juce::Array<juce::var>());
  recipe->setProperty("input", juce::var(in.get()));
  recipe->setProperty("preset", PresetJson(preset));
  recipe->setProperty("outputSha256", r.outputSha256);
  juce::Array<juce::var> segments;
  for (const juce::String& h : SegmentSha256(out.l, out.r)) segments.add(h);
  recipe->setProperty("outputSegmentSha256", segments);
  recipe->setProperty("identical", input.converted
                                       ? "on this machine only: the input was converted by platform code"
                                       : "on every conforming build of this sound revision");
  const juce::File json = wav.getSiblingFile(wav.getFileNameWithoutExtension() + ".recipe.json");
  if (!json.replaceWithText(juce::JSON::toString(juce::var(recipe.get())))) {
    r.message = "Wrote " + wav.getFileName() + ", but not its recipe";
    Finish(r);
    return;
  }
  r.state   = State::Done;
  r.message = "Wrote " + wav.getFileName() + " (" +
              juce::String(static_cast<double>(out.l.size()) / kRate, 1) + " s), SHA-256 " +
              r.outputSha256.substring(0, 12);
  Finish(r);
}

}  // namespace brainscape::plugin
