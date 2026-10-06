#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

#include "brainscape/ParamDisplay.h"

namespace brainscape::plugin {

namespace palette {
inline const juce::Colour kBackground{0xFF0F1115};
inline const juce::Colour kHeader{0xFF13161B};
inline const juce::Colour kPanel{0xFF171A20};
inline const juce::Colour kPanelBorder{0xFF242830};
inline const juce::Colour kControl{0xFF22262E};
inline const juce::Colour kControlHover{0xFF2A2F38};
inline const juce::Colour kTrack{0xFF282D36};
inline const juce::Colour kText{0xFFE2E8F0};
inline const juce::Colour kTextDim{0xFF8A94A6};
inline const juce::Colour kTextFaint{0xFF5A6372};
inline const juce::Colour kGood{0xFF68D391};
inline const juce::Colour kWarn{0xFFF6AD55};
inline const juce::Colour kBad{0xFFFC8181};
inline const juce::Colour kIce{0xFF90CDF4};
inline const juce::Colour kLed{0xFFF6E05E};

juce::Colour GroupAccent(ParamGroup group);
}  // namespace palette

// Fonts: Segoe UI on Windows, the system sans elsewhere.
juce::Font UiFont(float height, bool bold = false);

// The dark, flat look shared by the editor, the standalone window and its dialogs.
class BrainscapeLookAndFeel final : public juce::LookAndFeel_V4 {
 public:
  BrainscapeLookAndFeel();

  // Knobs: a 270° track, the value arc in the slider's rotarySliderFillColourId (from
  // the centre when the slider's "bipolar" property is set), and a pointer.
  void drawRotarySlider(juce::Graphics&, int x, int y, int width, int height, float sliderPos,
                        float rotaryStartAngle, float rotaryEndAngle, juce::Slider&) override;
  void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                            bool highlighted, bool down) override;
  juce::Font getTextButtonFont(juce::TextButton&, int buttonHeight) override;
  juce::Font getLabelFont(juce::Label&) override;
  juce::Font getPopupMenuFont() override;
  juce::Font getComboBoxFont(juce::ComboBox&) override;
  void       drawCornerResizer(juce::Graphics&, int w, int h, bool isMouseOver, bool isMouseDragging) override;
};

}  // namespace brainscape::plugin
