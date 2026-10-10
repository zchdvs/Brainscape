#pragma once
#include <array>
#include <functional>
#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

#include "../PluginProcessor.h"

namespace brainscape::plugin {

// The BPM panel (docs/design/clock.md §10.3), a strip of the Pedal view under the macro knobs:
// the committed tempo to 0.1 BPM (double-click to type one: a Tempo event), where it comes from
// (HOST, MIDI, INT) with the MIDI follower's lock dot, a beat LED from the grid's position (the
// bar's first beat brighter), TAP (a Tap event; greyed while the host's tempo is followed, which
// drops taps), rows 83 and 84 (the Subdiv knob's six positions and the time mode, §10.4), the
// preset's stored tempo beside the live one with Store (Save, which captures the performance),
// and the tempo core's device settings in a menu: the Tempo source, global.tempo_recall (row 85)
// and, in the Standalone, Receive MIDI clock. A preset that does not use tempo (UsesTempo, §6.6)
// dims rows 83 and 84 and says so. Message thread.
//
// T2's synced times (row 86, global.tempo_glide, in the settings menu; and the effective synced
// time of row 63, "1/1 -> 1/2 · 1,000 ms", §5.3) join this panel: see the hooks in TempoPanel.cpp.
class TempoPanel final : public juce::Component, public juce::SettableTooltipClient {
 public:
  explicit TempoPanel(BrainscapeProcessor& processor);
  ~TempoPanel() override;

  void Refresh();
  void SetScale(float scale);
  void paint(juce::Graphics&) override;
  void resized() override;

  // Store: the editor saves the open document, whose save captures the live tempo, Subdiv and
  // time mode (CurationSession::Save). Enabled while a document is open.
  std::function<void()> onStore;

  // Tests and snapshots.
  juce::Button&     Tap() noexcept;
  juce::Label&      Bpm() noexcept { return bpm_; }
  juce::TextButton& SubdivSegment(int position) noexcept { return subdiv_[static_cast<size_t>(position)]; }
  juce::TextButton& TimeSegment(int mode) noexcept { return time_[static_cast<size_t>(mode)]; }
  juce::TextButton& Store() noexcept { return store_; }
  juce::TextButton& Settings() noexcept { return settings_; }
  bool              UsesTempo() const noexcept { return usesTempo_; }
  // What the settings menu offers, as it would show it (its items' ticks follow the settings).
  juce::PopupMenu SettingsMenu();

 private:
  class TapButton;
  void ShowSettings();
  void SetSubdiv(int position);
  void SetTimeMode(int mode);

  BrainscapeProcessor&            processor_;
  const bool                      standalone_;
  juce::Label                     bpm_;
  std::unique_ptr<TapButton>      tap_;
  std::array<juce::TextButton, 6> subdiv_;
  std::array<juce::TextButton, 3> time_;
  juce::TextButton                store_{"Store"}, settings_;
  BrainscapeProcessor::TempoDisplay shown_{};
  uint32_t                        storedUs_   = 500000;
  bool                            usesTempo_  = false;
  uint32_t                        loadSerial_ = ~0u;
  int64_t                         lastBeat_   = INT64_MIN;
  float                           beatGlow_   = 0.f;
  bool                            downbeat_   = false;
  bool                            showStored_ = true;
  // Painted areas, set by resized().
  juce::Rectangle<int> titleArea_, bpmArea_, badgeArea_, beatArea_, subdivCaption_, timeCaption_, storedArea_;
  float                scale_ = 1.f;
};

}  // namespace brainscape::plugin
