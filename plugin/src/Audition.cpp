#include "Audition.h"

#include <algorithm>
#include <cstring>
#include <memory>

#include <juce_audio_formats/juce_audio_formats.h>

#include "Hash.h"
#include "Inputs.h"
#include "Recipe.h"

namespace brainscape::plugin {

namespace {

constexpr double kRate = bsa::kRate;

size_t Frames(double seconds) { return static_cast<size_t>(seconds * kRate); }

bsa::InputMode ToBsa(InputMode mode) {
  return mode == InputMode::Mono ? bsa::InputMode::Mono : bsa::InputMode::Stereo;
}

}  // namespace

AuditionInput TestSignalInput() {
  const bsa::Input v = bsa::VectorInput(bsa::Vector::Plucks, static_cast<uint32_t>(Frames(kAuditionSignalSeconds)),
                                        static_cast<uint32_t>(Frames(kAuditionTailSeconds)));
  AuditionInput in;
  in.l            = v.audio.l;
  in.r            = v.audio.r;
  in.description  = juce::String(v.description);
  in.vector       = juce::String(v.name);
  in.signalFrames = v.signalFrames;
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
  in.signalFrames = static_cast<uint32_t>(in.l.size());
  in.l.resize(in.l.size() + Frames(kAuditionTailSeconds), 0.f);
  in.r.resize(in.r.size() + Frames(kAuditionTailSeconds), 0.f);
  return in;
}

bool RenderAudition(const PresetState& preset, InputMode mode, AuditionInput& input,
                    AuditionOutput& out) {
  bsa::Stereo raw;
  raw.l = std::move(input.l);
  raw.r = std::move(input.r);
  bsa::RenderRequest rq;
  rq.preset = &preset;
  rq.input  = &raw;  // the render conditions its own copy
  rq.mode   = ToBsa(mode);
  bsa::Renderer     renderer;
  bsa::RenderResult result;
  const bool        ok = renderer.Render(rq, &result);
  bsa::Stereo       heard = bsa::Conditioned(raw, ToBsa(mode));  // the input as the engine heard it
  input.l               = std::move(heard.l);
  input.r               = std::move(heard.r);
  if (!ok) return false;
  out.l = std::move(result.out.l);
  out.r = std::move(result.out.r);
  return true;
}

bool RenderAudition(const float* preset, InputMode mode, AuditionInput& input,
                    AuditionOutput& out) {
  return RenderAudition(*bsa::LeafPreset(preset), mode, input, out);
}

juce::String InterleavedSha256(const std::vector<float>& l, const std::vector<float>& r) {
  return juce::String(bsa::HashRender(l, r).whole);
}

juce::StringArray SegmentSha256(const std::vector<float>& l, const std::vector<float>& r) {
  juce::StringArray hashes;
  for (const std::string& h : bsa::HashRender(l, r).seconds) hashes.add(juce::String(h));
  return hashes;
}

// ── AuditionJob ───────────────────────────────────────────────────────────────────────

AuditionJob::~AuditionJob() {
  if (thread_.joinable()) thread_.join();
}

bool AuditionJob::Start(const juce::File& wav, const float* preset, InputMode mode, AuditionInput input) {
  return Start(wav, *bsa::LeafPreset(preset), mode, std::move(input));
}

bool AuditionJob::Start(const juce::File& wav, const PresetState& preset, InputMode mode,
                        AuditionInput input) {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (result_.state == State::Running) return false;
    result_         = {};
    result_.state   = State::Running;
    result_.wav     = wav;
    result_.message = "Rendering " + wav.getFileName() + "...";
  }
  if (thread_.joinable()) thread_.join();  // the previous render has finished
  thread_ = std::thread(&AuditionJob::Run, this, wav, std::make_shared<const PresetState>(preset),
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

void AuditionJob::Run(juce::File wav, std::shared_ptr<const PresetState> preset, InputMode mode,
                      AuditionInput input) {
  Result r;
  r.wav   = wav;
  r.state = State::Failed;
  AuditionOutput out;
  if (!RenderAudition(*preset, mode, input, out)) {
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

  // The recipe (companion §4.9), tools/audition's format, shared with `bspc render`: what the
  // render was made from, so it can be made again.
  bsa::RenderResult result;
  result.out.l = std::move(out.l);
  result.out.r = std::move(out.r);
  bsa::RecipeInput ri;
  ri.preset                 = preset.get();
  ri.inputDescription       = input.description.toStdString();
  ri.inputVector            = input.vector.toStdString();
  ri.inputSignalFrames      = input.signalFrames;
  ri.inputSourceRate        = input.sourceRate;
  ri.inputConverted         = input.converted;
  ri.mode                   = ToBsa(mode);
  ri.conditionedInputSha256 = bsa::HashRender(input.l, input.r).whole;  // conditioned above
  ri.result                 = &result;
  ri.hashes                 = bsa::HashRender(result.out);
  r.outputSha256            = juce::String(ri.hashes.whole);
  const juce::File json = wav.getSiblingFile(wav.getFileNameWithoutExtension() + ".recipe.json");
  if (!json.replaceWithText(juce::String(bsa::RecipeJson(ri)))) {
    r.message = "Wrote " + wav.getFileName() + ", but not its recipe";
    Finish(r);
    return;
  }
  r.state   = State::Done;
  r.message = "Wrote " + wav.getFileName() + " (" +
              juce::String(static_cast<double>(result.out.l.size()) / kRate, 1) + " s), SHA-256 " +
              r.outputSha256.substring(0, 12);
  Finish(r);
}

}  // namespace brainscape::plugin
