#include "BrainscapeLookAndFeel.h"

namespace brainscape::plugin {

juce::Colour palette::GroupAccent(ParamGroup group) {
  switch (group) {
    case ParamGroup::GrainDelay: return juce::Colour(0xFF4FD1C5);
    case ParamGroup::Grains:     return juce::Colour(0xFF7F9CF5);
    case ParamGroup::Pitch:      return juce::Colour(0xFFB794F4);
    case ParamGroup::Window:     return juce::Colour(0xFF63B3ED);
    case ParamGroup::Mod:        return juce::Colour(0xFFF687B3);
    case ParamGroup::PostDelay:  return juce::Colour(0xFF68D391);
    case ParamGroup::Reverb:     return juce::Colour(0xFF76E4F7);
    case ParamGroup::Filter:     return juce::Colour(0xFFF6AD55);
    case ParamGroup::Triggers:   return juce::Colour(0xFFF6E05E);
    // The mode system's groups (mode-compiler.md §4.3), not on the test bench yet.
    case ParamGroup::Scheduler:   return juce::Colour(0xFF9F7AEA);
    case ParamGroup::Layer2:      return juce::Colour(0xFF4299E1);
    case ParamGroup::Modifiers:   return juce::Colour(0xFFED8936);
    case ParamGroup::Modulation:  return juce::Colour(0xFFED64A6);
    case ParamGroup::Macros:      return juce::Colour(0xFF38B2AC);
    case ParamGroup::Performance: return juce::Colour(0xFFECC94B);
    case ParamGroup::Device:      return juce::Colour(0xFFA0AEC0);
  }
  return kText;
}

juce::Colour palette::FamilyAccent(PresetFamily family) {
  switch (family) {
    case PresetFamily::Echoic:  return juce::Colour(0xFF68D391);  // the post delay's green
    case PresetFamily::Reverie: return juce::Colour(0xFF7F9CF5);  // the grains' blue
    case PresetFamily::Recall:  return juce::Colour(0xFFB794F4);  // the pitch's violet
    case PresetFamily::Misfire: return juce::Colour(0xFFF687B3);  // the modulation's pink
    case PresetFamily::None: break;
  }
  return kTextFaint;
}

juce::Font UiFont(float height, bool bold) {
#if JUCE_WINDOWS
  const juce::String name = bold ? "Segoe UI Semibold" : "Segoe UI";
  return juce::Font(juce::FontOptions(name, height, juce::Font::plain));
#else
  return juce::Font(juce::FontOptions(height, bold ? juce::Font::bold : juce::Font::plain));
#endif
}

BrainscapeLookAndFeel::BrainscapeLookAndFeel() {
  using namespace palette;
  setColourScheme({kBackground, kPanel, kPanel, kPanelBorder, kText, kControlHover, kBackground,
                   juce::Colour(0xFF4FD1C5), kText});
  setColour(juce::ResizableWindow::backgroundColourId, kBackground);
  setColour(juce::DocumentWindow::textColourId, kText);
  setColour(juce::Label::textColourId, kText);
  setColour(juce::Label::textWhenEditingColourId, kText);
  setColour(juce::Label::backgroundWhenEditingColourId, kControl);
  setColour(juce::Label::outlineWhenEditingColourId, juce::Colour(0xFF4FD1C5));
  setColour(juce::TextEditor::backgroundColourId, kControl);
  setColour(juce::TextEditor::textColourId, kText);
  setColour(juce::TextEditor::highlightColourId, juce::Colour(0x664FD1C5));
  setColour(juce::TextEditor::outlineColourId, kPanelBorder);
  setColour(juce::TextEditor::focusedOutlineColourId, juce::Colour(0xFF4FD1C5));
  setColour(juce::CaretComponent::caretColourId, kText);
  setColour(juce::TextButton::buttonColourId, kControl);
  setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xFF4FD1C5));
  setColour(juce::TextButton::textColourOffId, kText);
  setColour(juce::TextButton::textColourOnId, kBackground);
  setColour(juce::ComboBox::backgroundColourId, kControl);
  setColour(juce::ComboBox::outlineColourId, kPanelBorder);
  setColour(juce::ComboBox::textColourId, kText);
  setColour(juce::ComboBox::arrowColourId, kTextDim);
  setColour(juce::PopupMenu::backgroundColourId, kPanel);
  setColour(juce::PopupMenu::textColourId, kText);
  setColour(juce::PopupMenu::highlightedBackgroundColourId, kControlHover);
  setColour(juce::PopupMenu::highlightedTextColourId, kText);
  setColour(juce::Slider::rotarySliderFillColourId, juce::Colour(0xFF4FD1C5));
  setColour(juce::Slider::rotarySliderOutlineColourId, kTrack);
  setColour(juce::Slider::thumbColourId, kText);
  setColour(juce::Slider::textBoxTextColourId, kText);
  setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
  setColour(juce::TooltipWindow::backgroundColourId, kControl);
  setColour(juce::TooltipWindow::textColourId, kText);
  setColour(juce::TooltipWindow::outlineColourId, kPanelBorder);
  setColour(juce::ListBox::backgroundColourId, kPanel);
  setColour(juce::ListBox::textColourId, kText);
  setColour(juce::ToggleButton::textColourId, kText);
  setColour(juce::ToggleButton::tickColourId, juce::Colour(0xFF4FD1C5));
  setColour(juce::ToggleButton::tickDisabledColourId, kTextFaint);
  setColour(juce::ScrollBar::thumbColourId, kControlHover);
}

void BrainscapeLookAndFeel::drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                                             float sliderPos, float rotaryStartAngle,
                                             float rotaryEndAngle, juce::Slider& slider) {
  using namespace palette;
  const auto  bounds  = juce::Rectangle<int>(x, y, width, height).toFloat();
  const float side    = std::min(bounds.getWidth(), bounds.getHeight());
  const auto  centre  = bounds.getCentre();
  const float radius  = side * 0.5f - 2.0f;
  if (radius < 6.0f) return;
  const float stroke  = std::max(2.5f, radius * 0.13f);
  const float arcR    = radius - stroke * 0.5f;
  const float angle   = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);
  const auto  accent  = slider.findColour(juce::Slider::rotarySliderFillColourId);
  const bool  enabled = slider.isEnabled();
  const bool  hover   = slider.isMouseOverOrDragging();

  juce::Path track;
  track.addCentredArc(centre.x, centre.y, arcR, arcR, 0.f, rotaryStartAngle, rotaryEndAngle, true);
  g.setColour(kTrack);
  g.strokePath(track, juce::PathStrokeType(stroke, juce::PathStrokeType::curved,
                                           juce::PathStrokeType::rounded));

  const bool  bipolar = static_cast<bool>(slider.getProperties()["bipolar"]);
  const float from    = bipolar ? (rotaryStartAngle + rotaryEndAngle) * 0.5f : rotaryStartAngle;
  if (std::abs(angle - from) > 0.001f) {
    juce::Path value;
    value.addCentredArc(centre.x, centre.y, arcR, arcR, 0.f, std::min(from, angle),
                        std::max(from, angle), true);
    g.setColour(enabled ? accent : kTextFaint);
    g.strokePath(value, juce::PathStrokeType(stroke, juce::PathStrokeType::curved,
                                             juce::PathStrokeType::rounded));
  }

  const float bodyR = radius - stroke - std::max(2.0f, radius * 0.08f);
  const auto  body  = juce::Rectangle<float>(bodyR * 2.f, bodyR * 2.f).withCentre(centre);
  g.setGradientFill(juce::ColourGradient(hover ? juce::Colour(0xFF353B46) : juce::Colour(0xFF2D323C),
                                         centre.x, body.getY(), juce::Colour(0xFF1C2027), centre.x,
                                         body.getBottom(), false));
  g.fillEllipse(body);
  g.setColour(juce::Colour(0xFF3A404C));
  g.drawEllipse(body.reduced(0.5f), 1.0f);

  const juce::Point<float> tip = centre.getPointOnCircumference(bodyR * 0.78f, angle);
  const juce::Point<float> tail = centre.getPointOnCircumference(bodyR * 0.30f, angle);
  g.setColour(enabled ? kText : kTextFaint);
  g.drawLine({tail, tip}, std::max(2.0f, bodyR * 0.09f));
}

void BrainscapeLookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& button,
                                                 const juce::Colour& backgroundColour,
                                                 bool highlighted, bool down) {
  const auto bounds = button.getLocalBounds().toFloat().reduced(0.5f);
  auto       fill   = backgroundColour;
  if (down) {
    fill = fill.brighter(0.15f);
  } else if (highlighted) {
    fill = fill.brighter(0.08f);
  }
  if (!button.isEnabled()) fill = fill.withMultipliedAlpha(0.5f);
  g.setColour(fill);
  g.fillRoundedRectangle(bounds, 5.0f);
  g.setColour(button.getToggleState() ? fill.brighter(0.2f) : palette::kPanelBorder.brighter(0.15f));
  g.drawRoundedRectangle(bounds, 5.0f, 1.0f);
}

juce::Font BrainscapeLookAndFeel::getTextButtonFont(juce::TextButton&, int buttonHeight) {
  return UiFont(juce::jlimit(11.0f, 21.0f, static_cast<float>(buttonHeight) * 0.48f), true);
}

juce::Font BrainscapeLookAndFeel::getLabelFont(juce::Label& label) { return label.getFont(); }

juce::Font BrainscapeLookAndFeel::getPopupMenuFont() { return UiFont(15.0f); }

juce::Font BrainscapeLookAndFeel::getComboBoxFont(juce::ComboBox&) { return UiFont(14.0f); }

void BrainscapeLookAndFeel::drawCornerResizer(juce::Graphics& g, int w, int h, bool isMouseOver,
                                              bool isMouseDragging) {
  g.setColour((isMouseOver || isMouseDragging) ? palette::kTextDim : palette::kTextFaint);
  for (int i = 1; i <= 3; ++i) {
    const float o = static_cast<float>(i) * 4.0f;
    g.drawLine(static_cast<float>(w) - o, static_cast<float>(h) - 1.0f, static_cast<float>(w) - 1.0f,
               static_cast<float>(h) - o, 1.0f);
  }
}

}  // namespace brainscape::plugin
