#pragma once
#include <array>
#include <memory>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginProcessor.h"
#include "gui/BrainscapeLookAndFeel.h"
#include "gui/Widgets.h"

namespace brainscape::plugin {

// The engine test bench: every engine parameter as a knob, grouped as in
// companion §2.5's raw-parameter Pedal view, plus freeze, trigger, the onset LED, levels,
// the test input panel and the status line.
class BrainscapeEditor final : public juce::AudioProcessorEditor, private juce::Timer {
 public:
  static constexpr int kDefaultWidth  = 1180;
  static constexpr int kDefaultHeight = 720;
  static constexpr int kMinWidth      = 940;
  static constexpr int kMinHeight     = 600;

  explicit BrainscapeEditor(BrainscapeProcessor& owner);
  ~BrainscapeEditor() override;

  void paint(juce::Graphics&) override;
  void resized() override;

  // One GUI tick: pulls mirrors, meters, the onset LED and the status. Public so the
  // headless snapshot tool can render a current frame without a message loop.
  void RefreshNow();

  const std::vector<ParamKnob*>& Knobs() const noexcept { return knobs_; }

 private:
  void timerCallback() override { RefreshNow(); }
  void ToggleFreeze();

  BrainscapeProcessor&  processor_;
  BrainscapeLookAndFeel laf_;

  std::array<std::unique_ptr<SectionPanel>, kNumParamGroups> sections_;
  std::vector<ParamKnob*>                                    knobs_;  // in ParamId order

  OnsetLed       led_;
  LevelMeter     inMeter_{"IN"}, outMeter_{"OUT"};
  TriggerButton  trigger_;
  FreezeButton   freeze_;
  TestInputPanel testPanel_;
  StatusBar      status_;

  juce::Rectangle<int> header_;
  float                scale_ = 1.f;
  juce::TooltipWindow  tooltips_{this, 700};

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BrainscapeEditor)
};

}  // namespace brainscape::plugin
