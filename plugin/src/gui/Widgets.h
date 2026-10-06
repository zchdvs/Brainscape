#pragma once
#include <functional>
#include <memory>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "../BrainscapeParam.h"
#include "../PlainAttachment.h"
#include "../PluginProcessor.h"

namespace brainscape::plugin {

// One parameter: caption, rotary knob, and the plain value in units. Double-click the
// knob for the default, double-click the value to type one. A two-position parameter is
// a switch instead: one segment per position, click to select.
class ParamKnob final : public juce::Component, public juce::SettableTooltipClient {
 public:
  ParamKnob(BrainscapeParam& param, juce::Colour accent);
  ~ParamKnob() override;

  void                       Refresh();
  void                       SetScale(float scale);
  BrainscapePlainAttachment& Attachment() noexcept { return *attachment_; }
  juce::Slider&              Knob() noexcept { return slider_; }
  juce::Label&               ValueLabel() noexcept { return value_; }
  bool                       IsSwitch() const noexcept { return isSwitch_; }
  juce::TextButton&          Segment(int position) noexcept { return segments_[position != 0 ? 1 : 0]; }
  void                       resized() override;

 private:
  class KnobSlider final : public juce::Slider {
   public:
    std::function<void()> onDoubleClick;
    void mouseDoubleClick(const juce::MouseEvent&) override {
      if (onDoubleClick) onDoubleClick();
    }
  };

  juce::Label                                title_;
  KnobSlider                                 slider_;
  juce::Label                                value_;
  juce::TextButton                           segments_[2];  // switch positions 0 and 1
  std::unique_ptr<BrainscapePlainAttachment> attachment_;
  bool                                       isSwitch_ = false;
  float                                      scale_    = 1.f;
};

// A titled group of knobs (companion §2.5's raw-parameter view, one panel per group).
class SectionPanel final : public juce::Component {
 public:
  SectionPanel(const juce::String& title, juce::Colour accent);
  ParamKnob& AddKnob(BrainscapeParam& param);
  const std::vector<std::unique_ptr<ParamKnob>>& Knobs() const noexcept { return knobs_; }
  void       SetScale(float scale);
  void       paint(juce::Graphics&) override;
  void       resized() override;
  static constexpr int kTitleHeight = 24;  // at scale 1

 private:
  juce::String                            title_;
  juce::Colour                            accent_;
  std::vector<std::unique_ptr<ParamKnob>> knobs_;
  float                                   scale_ = 1.f;
};

// Header and status widgets draw at a scale the editor sets, like the panels.
template <typename Base>
class Scalable : public Base {
 public:
  using Base::Base;
  void SetScale(float scale) {
    if (scale == scale_) return;
    scale_ = scale;
    this->repaint();
  }

 protected:
  float scale_ = 1.f;
};

// Peak meter with hold, -60..+6 dBFS.
class LevelMeter final : public Scalable<juce::Component>, public juce::SettableTooltipClient {
 public:
  explicit LevelMeter(const juce::String& label) : label_(label) {}
  void Push(float peakLinear);  // one GUI tick
  void paint(juce::Graphics&) override;

 private:
  juce::String label_;
  float        levelDb_   = -100.f;
  float        holdDb_    = -100.f;
  int          holdTicks_ = 0;
};

// Lights for each detected onset, stretched to a visible flash (grain-engine.md §9).
class OnsetLed final : public Scalable<juce::Component>, public juce::SettableTooltipClient {
 public:
  OnsetLed();
  void Push(uint32_t onsets);  // one GUI tick
  void paint(juce::Graphics&) override;

 private:
  float    brightness_ = 0.f;
  uint32_t total_      = 0;
};

class FreezeButton final : public Scalable<juce::Button> {
 public:
  FreezeButton() : Scalable<juce::Button>("Freeze") {}
  void paintButton(juce::Graphics&, bool highlighted, bool down) override;
};

class TriggerButton final : public Scalable<juce::Button> {
 public:
  TriggerButton() : Scalable<juce::Button>("Trigger") {}
  void Flash() { flash_ = 1.f; }
  void Tick();
  void paintButton(juce::Graphics&, bool highlighted, bool down) override;

 private:
  float flash_ = 0.f;
};

// The status line: engine rate (the pedal's or not), host block size, the restart option,
// lost events.
class StatusBar final : public Scalable<juce::Component>, public juce::SettableTooltipClient {
 public:
  void Set(const BrainscapeProcessor::Status& status, const WrapperSettings& settings);
  void paint(juce::Graphics&) override;

 private:
  BrainscapeProcessor::Status status_{};
  WrapperSettings             settings_{};
};

// Developer test input: live input, a looped file or the pluck generator, plus the
// global input mode and levels (companion §4.8). Harmless in a DAW: Live is the default.
// Its last row holds the reproducible renders of §4.9: in the Standalone the offline
// audition of the test input, in a plugin the "Restart on transport start" option.
class TestInputPanel final : public juce::Component {
 public:
  explicit TestInputPanel(BrainscapeProcessor& processor);
  void Refresh();
  void SetScale(float scale);
  void paint(juce::Graphics&) override;
  void resized() override;

 private:
  void ChooseFile();
  void ChooseAuditionFile();
  void PushSettings();
  static void StyleSegment(juce::TextButton& b, int group, juce::Colour on);

  BrainscapeProcessor&               processor_;
  const bool                         standalone_;
  juce::TextButton                   live_{"Live in"}, file_{"File loop"}, pluck_{"Pluck"};
  juce::TextButton                   load_{"Load file..."};
  juce::Label                        fileName_;
  juce::TextButton                   mono_{"Mono"}, stereo_{"Stereo"};
  juce::Label                        modeCaption_;
  juce::TextButton                   audition_{"Render audition..."};
  juce::TextButton                   restartOnPlay_{"Restart on play"};
  juce::Label                        renderNote_;
  juce::Slider                       inLevel_, outLevel_;
  juce::Label                        inCaption_, outCaption_, inValue_, outValue_;
  std::unique_ptr<juce::FileChooser> chooser_;
  bool                               fileError_ = false;
  float                              scale_     = 1.f;
};

}  // namespace brainscape::plugin
