#include "TestInput.h"

#include <algorithm>
#include <cmath>

namespace brainscape::plugin {

namespace {

constexpr double kMaxLoopSeconds = 600.0;
constexpr size_t kLineFrames     = 8192;  // the lowest note at 384 kHz still fits

// E minor pentatonic across the guitar's low and middle registers.
constexpr float kPluckHz[] = {82.41f,  98.00f,  110.00f, 123.47f, 146.83f, 164.81f,
                              196.00f, 220.00f, 246.94f, 293.66f, 329.63f, 392.00f};
constexpr float kGapSeconds[] = {0.5f, 0.5f, 1.0f, 0.75f, 1.25f, 0.5f, 2.0f};

}  // namespace

TestInput::TestInput() : line_(kLineFrames, 0.f) {}

TestInput::~TestInput() { current_.store(nullptr); }

bool TestInput::LoadFile(const juce::File& file, juce::String& error) {
  juce::AudioFormatManager formats;
  formats.registerBasicFormats();
  std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
  if (reader == nullptr) {
    error = "Not a readable WAV, AIFF, FLAC or Ogg file";
    return false;
  }
  if (reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0 || reader->numChannels == 0) {
    error = "The file holds no audio";
    return false;
  }
  if (static_cast<double>(reader->lengthInSamples) > reader->sampleRate * kMaxLoopSeconds) {
    error = "Files longer than 10 minutes are not loaded";
    return false;
  }
  auto       loop   = std::make_unique<Loop>();
  const auto frames = static_cast<int>(reader->lengthInSamples);
  loop->audio.setSize(2, frames);
  if (!reader->read(&loop->audio, 0, frames, 0, true, true)) {
    error = "Could not decode the file";
    return false;
  }
  if (reader->numChannels == 1) loop->audio.copyFrom(1, 0, loop->audio, 0, 0, frames);
  loop->sampleRate = reader->sampleRate;
  loop->name       = file.getFileName();

  current_.store(loop.get());
  if (owned_ != nullptr) retired_.push_back(std::move(owned_));
  owned_ = std::move(loop);
  CollectGarbage();
  return true;
}

juce::String TestInput::LoadedName() const { return owned_ != nullptr ? owned_->name : juce::String(); }

const juce::AudioBuffer<float>* TestInput::LoadedAudio(double* sampleRate) const {
  if (owned_ == nullptr) return nullptr;
  *sampleRate = owned_->sampleRate;
  return &owned_->audio;
}

void TestInput::CollectGarbage() {
  const Loop* reading = inUse_.load();
  retired_.erase(std::remove_if(retired_.begin(), retired_.end(),
                                [reading](const std::unique_ptr<Loop>& l) { return l.get() != reading; }),
                 retired_.end());
}

void TestInput::Prepare(double sampleRate) noexcept {
  sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
  std::fill(line_.begin(), line_.end(), 0.f);
  period_       = 1;
  lineIndex_    = 0;
  untilNext_    = static_cast<int>(0.1 * sampleRate_);
  patternIndex_ = 0;
  rng_          = 0x9E3779B9u;
  lastLoop_     = nullptr;
  loopPhase_    = 0.0;
}

bool TestInput::Render(float* l, float* r, int n) noexcept {
  const Source source = GetSource();
  if (source == Source::Live) return false;
  if (source == Source::Pluck) {
    RenderPluck(l, r, n);
    return true;
  }
  Loop* loop = nullptr;
  for (;;) {
    loop = current_.load();
    inUse_.store(loop);
    if (current_.load() == loop) break;
  }
  if (loop != nullptr && loop->audio.getNumSamples() > 0) {
    RenderLoop(*loop, l, r, n);
  } else {
    std::fill(l, l + n, 0.f);
    std::fill(r, r + n, 0.f);
  }
  inUse_.store(nullptr);
  return true;
}

void TestInput::RenderLoop(const Loop& loop, float* l, float* r, int n) noexcept {
  const int    length = loop.audio.getNumSamples();
  const double step   = loop.sampleRate / sampleRate_;
  if (&loop != lastLoop_ || !(loopPhase_ >= 0.0 && loopPhase_ < length)) {
    lastLoop_  = &loop;
    loopPhase_ = 0.0;
  }
  const float* a = loop.audio.getReadPointer(0);
  const float* b = loop.audio.getReadPointer(1);
  for (int k = 0; k < n; ++k) {
    const auto  i0   = static_cast<int>(loopPhase_);
    const int   i1   = i0 + 1 < length ? i0 + 1 : 0;
    const auto  frac = static_cast<float>(loopPhase_ - i0);
    l[k]             = a[i0] + (a[i1] - a[i0]) * frac;
    r[k]             = b[i0] + (b[i1] - b[i0]) * frac;
    loopPhase_ += step;
    if (loopPhase_ >= length) loopPhase_ = std::fmod(loopPhase_, static_cast<double>(length));
  }
}

float TestInput::NextNoise() noexcept {
  rng_ ^= rng_ << 13;
  rng_ ^= rng_ >> 17;
  rng_ ^= rng_ << 5;
  return static_cast<float>(rng_ & 0xFFFFFFu) / 8388608.0f - 1.0f;
}

void TestInput::StartPluck() noexcept {
  const float hz = kPluckHz[(rng_ >> 8) % (sizeof kPluckHz / sizeof kPluckHz[0])];
  period_        = std::clamp(static_cast<int>(sampleRate_ / hz + 0.5), 2, static_cast<int>(line_.size()));
  float smooth   = 0.f;
  for (int i = 0; i < period_; ++i) {  // a softened noise burst: the pick
    smooth += 0.5f * (NextNoise() - smooth);
    line_[static_cast<size_t>(i)] = 0.7f * smooth;
  }
  lineIndex_ = 0;
  const float gap = kGapSeconds[patternIndex_++ % (sizeof kGapSeconds / sizeof kGapSeconds[0])];
  untilNext_      = static_cast<int>(gap * sampleRate_);
}

void TestInput::RenderPluck(float* l, float* r, int n) noexcept {
  for (int k = 0; k < n; ++k) {
    if (--untilNext_ <= 0) StartPluck();
    const int   next = lineIndex_ + 1 < period_ ? lineIndex_ + 1 : 0;
    const float y    = line_[static_cast<size_t>(lineIndex_)];
    line_[static_cast<size_t>(lineIndex_)] = 0.4985f * (y + line_[static_cast<size_t>(next)]);
    lineIndex_ = next;
    l[k]       = y;
    r[k]       = y;
  }
}

}  // namespace brainscape::plugin
