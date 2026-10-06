#pragma once
#include <cstdint>

#include <juce_gui_basics/juce_gui_basics.h>

#include "BrainscapeParam.h"

namespace brainscape::plugin {

// Binds a slider to a BrainscapeParam by plain value (companion §5.3). Unlike
// juce::SliderParameterAttachment it never routes an edit through JUCE's normalised
// float, never drops a change within approximatelyEqual, and displays the plain mirror,
// not a reconstruction. The slider's own value is the knob position in [0, 1], mapped
// by dsp/'s taper exactly as a pedal pot is. Message thread only.
class BrainscapePlainAttachment final : private juce::Slider::Listener {
 public:
  BrainscapePlainAttachment(BrainscapeParam& param, juce::Slider& slider);
  ~BrainscapePlainAttachment() override;
  BrainscapePlainAttachment(const BrainscapePlainAttachment&) = delete;
  BrainscapePlainAttachment& operator=(const BrainscapePlainAttachment&) = delete;

  // Pulls the mirror into the slider if it changed elsewhere (host automation, a state
  // restore). Returns whether it did. Leaves a slider the user is dragging alone.
  bool Refresh();
  // Typed text in the display's units, parsed to the exact binary32 that is sent.
  bool CommitText(const juce::String& text);
  void ResetToDefault();

  juce::String     DisplayText() const { return FormatPlainText(param_.Id(), param_.Plain()); }
  BrainscapeParam& Param() noexcept { return param_; }

 private:
  void sliderValueChanged(juce::Slider*) override;
  void sliderDragStarted(juce::Slider*) override;
  void sliderDragEnded(juce::Slider*) override;
  void ShowPlain(float plain);

  BrainscapeParam& param_;
  juce::Slider&    slider_;
  uint32_t         shownBits_ = 0;
  bool             dragging_  = false;
};

}  // namespace brainscape::plugin
