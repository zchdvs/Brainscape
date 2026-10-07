#pragma once
#include <array>
#include <functional>
#include <memory>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "../BrainscapeParam.h"
#include "../Curation.h"
#include "../PluginProcessor.h"

// The curation slice's views (docs/design/mode-compiler.md §9.1): the pedal's eight knobs with
// pickup and Shift, the document panel (open, save, solve, A/B, render) and the compiler's
// findings. The raw leaves stay the Leaves view (gui/Widgets.h), as the design's Advanced tab.
namespace brainscape::plugin {

// One pedal knob (§3.1, §3.5): a pot whose position, the pointer, belongs to the hand, over a
// target whose value is marked on the ring. After a load the knob is locked until the pointer
// reaches the target's value (pickup, Q8); then it moves the target: a macro as MacroMove
// events, Mix and the Shift secondaries as their leaves, the effect volume as its device
// setting. Moves the host or a load make move the marker; a caught knob's pointer follows them.
// Double-click catches the knob where the value is, sending nothing.
class PickupKnob final : public juce::Component, public juce::SettableTooltipClient {
 public:
  PickupKnob();
  ~PickupKnob() override;

  // Binds the knob to a parameter (null: a disabled knob, as Loop Level is until the looper).
  // A new target locks the knob unless the pointer already sits at its value.
  void Bind(BrainscapeParam* param, juce::Colour accent, const juce::String& title,
            const juce::String& caption, const juce::String& tooltip);
  // After a load: locked until the pointer reaches the new value.
  void Lock();
  void Refresh(const juce::String& detail);
  void SetScale(float scale);
  bool Caught() const noexcept { return caught_; }
  float Pointer() const;  // the hand's position, 0-1
  float Value() const;    // the target's position, 0-1
  BrainscapeParam* Target() const noexcept { return param_; }
  // Tests and snapshots: a drag of the pointer to `position`.
  void MoveTo(float position);
  void resized() override;

 private:
  class Pot;
  void Moved(float from, float to);
  void Catch();

  std::unique_ptr<Pot> pot_;
  juce::Label          title_, caption_, value_, detail_;
  BrainscapeParam*     param_  = nullptr;
  juce::Colour         accent_ = juce::Colours::white;
  bool                 caught_ = true;
  bool                 placed_ = false;  // the pointer has been set from a first target
  bool                 gesture_ = false;
  float                last_   = 0.f;  // the pointer before the current move
  float                scale_  = 1.f;
};

// The pedal's eight knobs (§3.1): Activity, Repeats, Shape, Time, Space, Filter, Mix and Loop
// Level, with the mode's display names, and Shift, which moves Repeats to the mod depth, Shape to
// the mod rate, Space to the reverb time, Filter to the resonance and Mix to the effect volume.
class MacroPanel final : public juce::Component {
 public:
  explicit MacroPanel(BrainscapeProcessor& processor);
  void Refresh();
  void SetScale(float scale);
  void paint(juce::Graphics&) override;
  void resized() override;
  PickupKnob&       Knob(size_t i) { return knobs_[i]; }
  juce::TextButton& Shift() { return shift_; }

 private:
  void Bind();

  BrainscapeProcessor&      processor_;
  std::array<PickupKnob, 8> knobs_;
  juce::TextButton          shift_{"SHIFT"};
  bool                      shifted_    = false;
  uint32_t                  loadSerial_ = 0;
  uint64_t                  modeKey_    = 0;  // the mode the names were taken from
  juce::String              title_      = "Macros";
  float                     scale_      = 1.f;
};

// The document: what is open, its state, and the slice's commands.
class DocumentPanel final : public juce::Component {
 public:
  explicit DocumentPanel(BrainscapeProcessor& processor);
  void Refresh();
  void SetScale(float scale);
  void paint(juce::Graphics&) override;
  void resized() override;
  // Commands the editor's keys also reach.
  void ChooseOpen();
  void Save();
  void ToggleSide();

 private:
  void ChooseSaveAs();
  void StartRender();
  void Note(const juce::String& text, juce::Colour colour);
  static void StyleToggle(juce::TextButton& b, juce::Colour on);

  BrainscapeProcessor& processor_;
  CurationSession&     session_;
  juce::Label          name_, file_, state_, note_, render_;
  juce::TextButton     open_{"Open..."}, save_{"Save"}, saveAs_{"Save as..."}, revert_{"Revert"};
  juce::TextButton     solve_{"Solve positions"}, a_{"A  stored"}, b_{"B  working"}, match_{"Match level"};
  juce::TextButton     renderButton_{"Render"}, all_{"S0-S11"}, attack_{"Attack"}, pad_{"Pad"};
  juce::TextButton     reveal_{"Show"};
  std::unique_ptr<juce::FileChooser> chooser_;
  juce::String         noteText_;
  juce::Colour         noteColour_;
  float                scale_ = 1.f;
};

// The compiler's errors and lint on what Save would write, and the leaves Save will derive.
class FindingsPanel final : public juce::Component, private juce::ListBoxModel {
 public:
  explicit FindingsPanel(CurationSession& session);
  void Refresh();
  void SetScale(float scale);
  void paint(juce::Graphics&) override;
  void resized() override;
  int  Rows() const { return static_cast<int>(rows_.size()); }

 private:
  struct Row {
    enum class Kind : uint8_t { Error, Lint, Derive, Note } kind;
    juce::String code, text;
  };
  int          getNumRows() override { return static_cast<int>(rows_.size()); }
  void         paintListBoxItem(int row, juce::Graphics&, int width, int height, bool selected) override;
  juce::String getTooltipForRow(int row) override;

  CurationSession& session_;
  juce::ListBox    list_;
  std::vector<Row> rows_;
  juce::String     summary_;
  float            scale_ = 1.f;
};

// The accent of a pedal knob, by its index (Activity ... Loop Level).
juce::Colour KnobAccent(size_t knob);
// The accent of a macro, for the Leaves view's badges.
juce::Colour MacroAccent(ParamId macro);

}  // namespace brainscape::plugin
