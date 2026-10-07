#pragma once
#include <array>
#include <memory>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginProcessor.h"
#include "gui/BrainscapeLookAndFeel.h"
#include "gui/CurationViews.h"
#include "gui/Widgets.h"

namespace brainscape::plugin {

// The curation bench (mode-compiler.md §9.1): two views over one processor.
//   Pedal  : the pedal's eight knobs with pickup and Shift, the open preset document with its
//            commands (open, save, solve, A/B, render) and the compiler's findings;
//   Leaves : every engine leaf as a knob, grouped as in companion §2.5's raw-parameter view (the
//            design's Advanced tab), marked with the macros that move it.
// Around both: freeze, trigger, the onset LED, levels, the test input panel and the status line.
class BrainscapeEditor final : public juce::AudioProcessorEditor, private juce::Timer {
 public:
  static constexpr int kDefaultWidth  = 1180;
  static constexpr int kDefaultHeight = 720;
  static constexpr int kMinWidth      = 940;
  static constexpr int kMinHeight     = 600;

  enum class View : uint8_t { Pedal, Leaves };

  explicit BrainscapeEditor(BrainscapeProcessor& owner);
  ~BrainscapeEditor() override;

  void paint(juce::Graphics&) override;
  void resized() override;
  // B toggles A/B, Ctrl+S saves, Ctrl+O opens.
  bool keyPressed(const juce::KeyPress&) override;

  // One GUI tick: pulls mirrors, meters, the onset LED, the document and the status. Public so
  // the headless snapshot tool can render a current frame without a message loop.
  void RefreshNow();

  void SetView(View view);
  View GetView() const noexcept { return view_; }

  const std::vector<ParamKnob*>& Knobs() const noexcept { return knobs_; }
  MacroPanel&                    Macros() noexcept { return macros_; }
  DocumentPanel&                 DocumentView() noexcept { return document_; }
  FindingsPanel&                 Findings() noexcept { return findings_; }

 private:
  void timerCallback() override { RefreshNow(); }
  void ToggleFreeze();
  void RefreshMarks();
  void ShowLeafMenu(ParamId leaf);

  BrainscapeProcessor&  processor_;
  BrainscapeLookAndFeel laf_;

  std::array<std::unique_ptr<SectionPanel>, kNumParamGroups> sections_;
  std::vector<ParamKnob*>                                    knobs_;  // in ParamId order

  juce::TextButton pedalTab_{"PEDAL"}, leavesTab_{"LEAVES"};
  juce::Label      tabNote_;
  MacroPanel       macros_;
  DocumentPanel    document_;
  FindingsPanel    findings_;

  OnsetLed       led_;
  LevelMeter     inMeter_{"IN"}, outMeter_{"OUT"};
  TriggerButton  trigger_;
  FreezeButton   freeze_;
  TestInputPanel testPanel_;
  StatusBar      status_;

  View                 view_ = View::Pedal;
  juce::Rectangle<int> header_;
  float                scale_ = 1.f;
  juce::TooltipWindow  tooltips_{this, 700};

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BrainscapeEditor)
};

}  // namespace brainscape::plugin
