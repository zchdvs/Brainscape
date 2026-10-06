#include "PlainAttachment.h"

#include <cstring>

namespace brainscape::plugin {

namespace {

uint32_t Bits(float v) {
  uint32_t u = 0;
  std::memcpy(&u, &v, sizeof u);
  return u;
}

}  // namespace

BrainscapePlainAttachment::BrainscapePlainAttachment(BrainscapeParam& param, juce::Slider& slider)
    : param_(param), slider_(slider) {
  const ParamDisplay* m = FindParamDisplay(param.Id());
  const double interval = (m != nullptr && m->steps >= 2) ? 1.0 / (m->steps - 1) : 0.0;
  slider_.setRange(0.0, 1.0, interval);
  slider_.setDoubleClickReturnValue(false, 0.0);
  ShowPlain(param_.Plain());
  slider_.addListener(this);
}

BrainscapePlainAttachment::~BrainscapePlainAttachment() {
  slider_.removeListener(this);
  if (dragging_) param_.endChangeGesture();
}

void BrainscapePlainAttachment::ShowPlain(float plain) {
  shownBits_ = Bits(plain);
  slider_.setValue(NormalizedFromPlain(param_.Id(), plain), juce::dontSendNotification);
}

bool BrainscapePlainAttachment::Refresh() {
  const float plain = param_.Plain();
  if (Bits(plain) == shownBits_ || dragging_) return false;
  ShowPlain(plain);
  return true;
}

bool BrainscapePlainAttachment::CommitText(const juce::String& text) {
  float plain = 0.f;
  if (!ParsePlainText(param_.Id(), text, plain)) return false;
  param_.SetPlainNotifyingHost(plain);
  ShowPlain(param_.Plain());
  return true;
}

void BrainscapePlainAttachment::CommitPlain(float plain) {
  param_.SetPlainNotifyingHost(plain);
  ShowPlain(param_.Plain());
}

void BrainscapePlainAttachment::ResetToDefault() { CommitPlain(param_.DefaultPlain()); }

void BrainscapePlainAttachment::sliderValueChanged(juce::Slider*) {
  // The pot path (companion §5.4): knob position -> dsp/ taper -> canonical plain bits.
  const float plain = PlainFromNormalized(param_.Id(), static_cast<float>(slider_.getValue()));
  if (dragging_) {
    param_.SetPlainInGesture(plain);
  } else {
    param_.SetPlainNotifyingHost(plain);  // wheel and keyboard steps
  }
  shownBits_ = Bits(param_.Plain());
}

void BrainscapePlainAttachment::sliderDragStarted(juce::Slider*) {
  dragging_ = true;
  param_.beginChangeGesture();
}

void BrainscapePlainAttachment::sliderDragEnded(juce::Slider*) {
  dragging_ = false;
  param_.endChangeGesture();
}

}  // namespace brainscape::plugin
