#include "Widgets.h"

#include <cmath>

#include "BrainscapeLookAndFeel.h"

namespace brainscape::plugin {

namespace {

const juce::String kDot = juce::String::fromUTF8("\xc2\xb7");   // ·
const juce::String kLeq = juce::String::fromUTF8("\xe2\x89\xa4");  // ≤

juce::String RateText(double hz) {
  juce::String s(hz / 1000.0, 1);
  if (s.endsWith(".0")) s = s.dropLastCharacters(2);
  return s + " kHz";
}

juce::String DbText(float db) {
  return (db > 0.f ? "+" : "") + juce::String(db, 1) + " dB";
}

int Scaled(int px, float scale) { return juce::roundToInt(static_cast<float>(px) * scale); }

void DrawPanel(juce::Graphics& g, juce::Rectangle<int> area, const juce::String& title,
               juce::Colour accent, float scale, const juce::String& note = {}) {
  const auto b = area.toFloat().reduced(0.5f);
  g.setColour(palette::kPanel);
  g.fillRoundedRectangle(b, 8.0f);
  g.setColour(palette::kPanelBorder);
  g.drawRoundedRectangle(b, 8.0f, 1.0f);
  auto t = area.removeFromTop(Scaled(SectionPanel::kTitleHeight, scale)).reduced(12, 0).withTrimmedTop(4);
  const float dot = 6.0f * scale;
  g.setColour(accent);
  g.fillEllipse(static_cast<float>(t.getX()), static_cast<float>(t.getCentreY()) - dot * 0.5f, dot, dot);
  g.setColour(palette::kTextDim);
  g.setFont(UiFont(12.0f * scale, true).withExtraKerningFactor(0.08f));
  g.drawText(title.toUpperCase(), t.withTrimmedLeft(Scaled(12, scale)), juce::Justification::centredLeft, true);
  if (note.isNotEmpty()) {
    g.setColour(palette::kTextFaint);
    g.setFont(UiFont(11.5f * scale));
    g.drawText(note, t, juce::Justification::centredRight, true);
  }
}

void StyleCaption(juce::Label& l, float height, juce::Colour colour, bool bold = false) {
  l.setFont(UiFont(height, bold));
  l.setColour(juce::Label::textColourId, colour);
  l.setJustificationType(juce::Justification::centred);
  l.setInterceptsMouseClicks(false, false);
}

}  // namespace

// ── ParamKnob ─────────────────────────────────────────────────────────────────────────

ParamKnob::ParamKnob(BrainscapeParam& param, juce::Colour accent) {
  const ParamDisplay*    m = FindParamDisplay(param.Id());
  const ParamDescriptor* d = FindParam(param.Id());
  title_.setText(m->shortTitle, juce::dontSendNotification);
  StyleCaption(title_, 13.0f, palette::kTextDim);
  addAndMakeVisible(title_);

  slider_.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
  slider_.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
  slider_.setMouseDragSensitivity(240);
  slider_.setColour(juce::Slider::rotarySliderFillColourId, accent);
  slider_.getProperties().set("bipolar", d->min < 0.f || param.Id() == ParamId::WindowSkew);
  slider_.setPopupMenuEnabled(false);
  addAndMakeVisible(slider_);

  value_.setFont(UiFont(13.5f));
  value_.setColour(juce::Label::textColourId, palette::kText);
  value_.setJustificationType(juce::Justification::centred);
  value_.setEditable(false, true, false);
  addAndMakeVisible(value_);

  attachment_ = std::make_unique<BrainscapePlainAttachment>(param, slider_);
  slider_.onDoubleClick = [this] {
    attachment_->ResetToDefault();
    Refresh();
  };
  value_.onTextChange = [this] {
    attachment_->CommitText(value_.getText());
    value_.setText(attachment_->DisplayText(), juce::dontSendNotification);
  };
  const juce::String tip = juce::String(m->title) + "\nDouble-click the knob for the default (" +
                           FormatPlainText(param.Id(), param.DefaultPlain()) +
                           "); double-click the value to type one.";
  setTooltip(tip);
  slider_.setTooltip(tip);
  value_.setTooltip(tip);
  value_.setText(attachment_->DisplayText(), juce::dontSendNotification);
}

ParamKnob::~ParamKnob() { attachment_.reset(); }

void ParamKnob::Refresh() {
  attachment_->Refresh();
  if (value_.isBeingEdited()) return;
  const juce::String text = attachment_->DisplayText();
  if (text != value_.getText()) value_.setText(text, juce::dontSendNotification);
}

void ParamKnob::SetScale(float scale) {
  if (scale == scale_) return;
  scale_ = scale;
  title_.setFont(UiFont(13.0f * scale));
  value_.setFont(UiFont(13.5f * scale));
  resized();
}

void ParamKnob::resized() {
  const int titleH = Scaled(16, scale_);
  const int valueH = Scaled(20, scale_);
  auto      r      = getLocalBounds();
  const int side   = std::min({r.getWidth() - 4, r.getHeight() - titleH - valueH, Scaled(104, scale_)});
  const int total  = titleH + side + valueH;
  r                = r.withSizeKeepingCentre(r.getWidth(), std::min(total, r.getHeight()));
  title_.setBounds(r.removeFromTop(titleH));
  value_.setBounds(r.removeFromBottom(valueH).reduced(2, 0));
  slider_.setBounds(r.withSizeKeepingCentre(side, side));
}

// ── SectionPanel ──────────────────────────────────────────────────────────────────────

SectionPanel::SectionPanel(const juce::String& title, juce::Colour accent)
    : title_(title), accent_(accent) {}

ParamKnob& SectionPanel::AddKnob(BrainscapeParam& param) {
  knobs_.push_back(std::make_unique<ParamKnob>(param, accent_));
  addAndMakeVisible(*knobs_.back());
  return *knobs_.back();
}

void SectionPanel::SetScale(float scale) {
  scale_ = scale;
  for (auto& k : knobs_) k->SetScale(scale);
}

void SectionPanel::paint(juce::Graphics& g) { DrawPanel(g, getLocalBounds(), title_, accent_, scale_); }

void SectionPanel::resized() {
  auto r = getLocalBounds().reduced(6, 6);
  r.removeFromTop(Scaled(kTitleHeight, scale_) - 4);
  if (knobs_.empty()) return;
  const int w = r.getWidth() / static_cast<int>(knobs_.size());
  for (size_t i = 0; i < knobs_.size(); ++i) {
    const bool last = i + 1 == knobs_.size();
    knobs_[i]->setBounds(r.removeFromLeft(last ? r.getWidth() : w));
  }
}

// ── LevelMeter ────────────────────────────────────────────────────────────────────────

void LevelMeter::Push(float peakLinear) {
  const float db  = peakLinear > 1e-5f ? 20.0f * std::log10(peakLinear) : -100.f;
  const float was = levelDb_;
  levelDb_        = std::max(db, levelDb_ - 1.5f);
  if (db >= holdDb_) {
    holdDb_    = db;
    holdTicks_ = 36;
  } else if (--holdTicks_ <= 0) {
    holdDb_ = std::max(-100.f, holdDb_ - 1.5f);
  }
  if (std::abs(was - levelDb_) > 0.05f || holdTicks_ <= 0) repaint();
}

void LevelMeter::paint(juce::Graphics& g) {
  auto r = getLocalBounds();
  g.setFont(UiFont(11.5f, true));
  g.setColour(palette::kTextDim);
  g.drawText(label_, r.removeFromLeft(30), juce::Justification::centredLeft, false);
  const juce::String value = holdDb_ <= -60.f ? juce::String("-inf") : juce::String(holdDb_, 1);
  g.setFont(UiFont(11.5f));
  g.setColour(holdDb_ > -0.1f ? palette::kBad : palette::kTextDim);
  g.drawText(value, r.removeFromRight(40), juce::Justification::centredRight, false);
  r.removeFromRight(6);

  const auto  bar  = r.withSizeKeepingCentre(r.getWidth(), 8).toFloat();
  const auto  frac = [](float db) { return juce::jlimit(0.f, 1.f, (db + 60.f) / 66.f); };
  g.setColour(palette::kControl);
  g.fillRoundedRectangle(bar, 3.0f);
  const float w = bar.getWidth() * frac(levelDb_);
  if (w > 0.5f) {
    juce::ColourGradient grad(palette::kGood, bar.getX(), 0.f, palette::kBad, bar.getRight(), 0.f, false);
    grad.addColour(static_cast<double>(frac(-12.f)), palette::kGood);
    grad.addColour(static_cast<double>(frac(-3.f)), palette::kWarn);
    g.setGradientFill(grad);
    g.fillRoundedRectangle(bar.withWidth(w), 3.0f);
  }
  if (holdDb_ > -60.f) {
    const float x = bar.getX() + bar.getWidth() * frac(holdDb_);
    g.setColour(palette::kText.withAlpha(0.8f));
    g.fillRect(juce::Rectangle<float>(x - 1.0f, bar.getY() - 2.0f, 2.0f, bar.getHeight() + 4.0f));
  }
  const float zero = bar.getX() + bar.getWidth() * frac(0.f);  // 0 dBFS tick
  g.setColour(palette::kTextFaint);
  g.fillRect(juce::Rectangle<float>(zero, bar.getBottom() + 2.0f, 1.0f, 3.0f));
}

// ── OnsetLed ──────────────────────────────────────────────────────────────────────────

OnsetLed::OnsetLed() {
  setTooltip("Onset detector: flashes for every onset the engine detects (Trigger sense sets "
             "its threshold).");
}

void OnsetLed::Push(uint32_t onsets) {
  const float was = brightness_;
  if (onsets > 0u) {
    brightness_ = 1.f;
    total_ += onsets;
  } else {
    brightness_ *= 0.72f;
    if (brightness_ < 0.02f) brightness_ = 0.f;
  }
  if (was != brightness_ || onsets > 0u) repaint();
}

void OnsetLed::paint(juce::Graphics& g) {
  auto        r      = getLocalBounds();
  const float d      = 14.0f;
  const auto  ledBox = r.removeFromLeft(26).toFloat();
  const auto  led    = juce::Rectangle<float>(d, d).withCentre(ledBox.getCentre());
  if (brightness_ > 0.f) {
    g.setColour(palette::kLed.withAlpha(0.35f * brightness_));
    g.fillEllipse(led.expanded(5.0f * brightness_));
  }
  g.setColour(palette::kLed.withAlpha(0.18f).interpolatedWith(palette::kLed, brightness_));
  g.fillEllipse(led);
  g.setColour(palette::kLed.withAlpha(0.5f));
  g.drawEllipse(led, 1.0f);
  r.removeFromLeft(4);
  g.setColour(palette::kTextDim);
  g.setFont(UiFont(11.5f, true).withExtraKerningFactor(0.08f));
  g.drawText("ONSET", r.removeFromTop(r.getHeight() / 2), juce::Justification::bottomLeft, false);
  g.setColour(palette::kTextFaint);
  g.setFont(UiFont(11.5f));
  g.drawText(juce::String(total_), r, juce::Justification::topLeft, false);
}

// ── Buttons ───────────────────────────────────────────────────────────────────────────

void FreezeButton::paintButton(juce::Graphics& g, bool highlighted, bool down) {
  auto       b  = getLocalBounds().toFloat().reduced(1.0f);
  const bool on = getToggleState();
  if (on) {
    g.setColour(palette::kIce.withAlpha(0.22f));
    g.fillRoundedRectangle(b, 9.0f);
    b = b.reduced(3.0f);
    g.setGradientFill(juce::ColourGradient(palette::kIce.brighter(0.25f), 0.f, b.getY(),
                                           palette::kIce.darker(0.25f), 0.f, b.getBottom(), false));
    g.fillRoundedRectangle(b, 7.0f);
  } else {
    g.setColour(highlighted || down ? palette::kControlHover : palette::kControl);
    g.fillRoundedRectangle(b, 9.0f);
    g.setColour(palette::kIce.withAlpha(highlighted ? 0.9f : 0.55f));
    g.drawRoundedRectangle(b.reduced(0.5f), 9.0f, 1.5f);
  }
  const auto ink = on ? palette::kBackground : palette::kIce;
  // A six-armed snowflake beside the label.
  const float s  = std::min(b.getHeight() * 0.36f, 16.0f);
  const auto  c  = juce::Point<float>(b.getX() + 14.0f + s, b.getCentreY());
  g.setColour(ink);
  for (int k = 0; k < 3; ++k) {
    const float a = juce::MathConstants<float>::pi * static_cast<float>(k) / 3.0f;
    g.drawLine({c.getPointOnCircumference(s, a), c.getPointOnCircumference(s, a + juce::MathConstants<float>::pi)}, 2.0f);
  }
  g.setFont(UiFont(std::min(20.0f, b.getHeight() * 0.42f), true).withExtraKerningFactor(0.12f));
  g.drawText(on ? "FROZEN" : "FREEZE", b.withTrimmedLeft(s * 2.0f + 18.0f),
             juce::Justification::centred, false);
}

void TriggerButton::Tick() {
  if (flash_ <= 0.f) return;
  flash_ *= 0.7f;
  if (flash_ < 0.03f) flash_ = 0.f;
  repaint();
}

void TriggerButton::paintButton(juce::Graphics& g, bool highlighted, bool down) {
  const auto  b   = getLocalBounds().toFloat().reduced(1.0f);
  const float lit = down ? 1.f : flash_;
  g.setColour((highlighted ? palette::kControlHover : palette::kControl)
                  .interpolatedWith(palette::kWarn, 0.85f * lit));
  g.fillRoundedRectangle(b, 9.0f);
  g.setColour(palette::kWarn.withAlpha(highlighted ? 0.95f : 0.6f));
  g.drawRoundedRectangle(b.reduced(0.5f), 9.0f, 1.5f);
  g.setColour(lit > 0.5f ? palette::kBackground : palette::kWarn);
  g.setFont(UiFont(std::min(15.0f, b.getHeight() * 0.36f), true).withExtraKerningFactor(0.1f));
  g.drawText("TRIGGER", b, juce::Justification::centred, false);
}

// ── StatusBar ─────────────────────────────────────────────────────────────────────────

void StatusBar::Set(const BrainscapeProcessor::Status& status, const WrapperSettings& settings) {
  const bool changed = status.hostRate != status_.hostRate || status.engineRate != status_.engineRate ||
                       status.engineReady != status_.engineReady ||
                       status.lastHostBlock != status_.lastHostBlock ||
                       status.droppedEvents != status_.droppedEvents ||
                       status.lastLoadInexact != status_.lastLoadInexact ||
                       settings.inputMode != settings_.inputMode;
  status_   = status;
  settings_ = settings;
  if (!changed) return;
  setTooltip(status_.pedalExact
                 ? "The engine runs at 48 kHz with the pedal's configuration. Live input and raw "
                   "host buffers are not covered by the parity contract until the block-split "
                   "fix lands (companion-app.md, section 2.3)."
                 : "The pedal runs only at 48 kHz. At this host rate the engine runs natively, so "
                   "delay times and pitch are close but not identical; the resampled 48 kHz mode "
                   "is still to come (companion-app.md, section 4.2).");
  repaint();
}

void StatusBar::paint(juce::Graphics& g) {
  auto r = getLocalBounds();
  g.setColour(palette::kHeader);
  g.fillRect(r);
  g.setColour(palette::kPanelBorder);
  g.fillRect(r.removeFromTop(1));
  r = r.reduced(14, 0);

  juce::Colour dot;
  juce::String main;
  if (!status_.engineReady && status_.hostRate <= 0.0) {
    dot  = palette::kTextFaint;
    main = "Waiting for audio";
  } else if (!status_.engineReady) {
    dot  = palette::kBad;
    main = "Engine failed to initialise";
  } else if (status_.pedalExact) {
    dot  = palette::kGood;
    main = "Pedal-exact @ 48 kHz";
  } else {
    dot  = palette::kWarn;
    main = "Not pedal-exact: host at " + RateText(status_.hostRate);
  }
  const auto dotBox = r.removeFromLeft(14).toFloat();
  g.setColour(dot);
  g.fillEllipse(juce::Rectangle<float>(8.0f, 8.0f).withCentre(dotBox.getCentre()));
  r.removeFromLeft(4);
  const juce::Font mainFont = UiFont(13.5f, true);
  g.setFont(mainFont);
  g.setColour(dot == palette::kTextFaint ? palette::kTextDim : dot);
  const int mainW = static_cast<int>(std::ceil(juce::GlyphArrangement::getStringWidth(mainFont, main)));
  g.drawText(main, r.removeFromLeft(mainW + 2), juce::Justification::centredLeft, false);

  juce::String detail;
  if (status_.hostRate > 0.0) {
    detail << "   " << kDot << "   Engine " << RateText(status_.engineRate) << "   " << kDot
           << "   Host block " << status_.lastHostBlock << " (engine chunks " << kLeq << " 512)";
  }
  detail << "   " << kDot << "   Input " << (settings_.inputMode == InputMode::Mono ? "mono (R = L)" : "stereo");
  g.setFont(UiFont(13.0f));
  g.setColour(palette::kTextDim);
  const juce::String right =
      status_.droppedEvents > 0u
          ? juce::String(status_.droppedEvents) + " control events dropped"
          : (status_.lastLoadInexact ? juce::String("Last state load was inexact") : juce::String());
  auto rightArea = r.removeFromRight(right.isEmpty() ? 0 : 240);
  g.drawText(detail, r, juce::Justification::centredLeft, true);
  if (right.isNotEmpty()) {
    g.setColour(palette::kWarn);
    g.drawText(right, rightArea, juce::Justification::centredRight, true);
  }
}

// ── TestInputPanel ────────────────────────────────────────────────────────────────────

void TestInputPanel::StyleSegment(juce::TextButton& b, int group, juce::Colour on) {
  b.setClickingTogglesState(true);
  b.setRadioGroupId(group);
  b.setColour(juce::TextButton::buttonOnColourId, on);
  b.setColour(juce::TextButton::textColourOnId, palette::kBackground);
}

TestInputPanel::TestInputPanel(BrainscapeProcessor& processor) : processor_(processor) {
  const auto accent = palette::kWarn;
  for (auto* b : {&live_, &file_, &pluck_}) {
    StyleSegment(*b, 1, accent);
    addAndMakeVisible(*b);
  }
  live_.onClick  = [this] { processor_.GetTestInput().SetSource(TestInput::Source::Live); };
  pluck_.onClick = [this] { processor_.GetTestInput().SetSource(TestInput::Source::Pluck); };
  file_.onClick  = [this] {
    if (processor_.GetTestInput().LoadedName().isEmpty()) {
      ChooseFile();
    } else {
      processor_.GetTestInput().SetSource(TestInput::Source::FileLoop);
    }
  };
  live_.setTooltip("Process the live input (standalone: unmute it in Options > Audio/MIDI Settings).");
  file_.setTooltip("Loop an audio file through the engine instead of the live input.");
  pluck_.setTooltip("A plucked-string generator: E minor pentatonic notes at a steady pace.");

  load_.onClick = [this] { ChooseFile(); };
  addAndMakeVisible(load_);
  fileName_.setFont(UiFont(13.0f));
  fileName_.setJustificationType(juce::Justification::centredLeft);
  fileName_.setMinimumHorizontalScale(0.7f);
  addAndMakeVisible(fileName_);

  for (auto* b : {&mono_, &stereo_}) {
    StyleSegment(*b, 2, palette::GroupAccent(ParamGroup::GrainDelay));
    b->onClick = [this] { PushSettings(); };
    addAndMakeVisible(*b);
  }
  mono_.setTooltip("Mono input: the right channel copies the left, for a guitar on input 1.");
  stereo_.setTooltip("Stereo input: both channels as they arrive.");
  modeCaption_.setText("Input", juce::dontSendNotification);
  StyleCaption(modeCaption_, 13.0f, palette::kTextDim);
  modeCaption_.setJustificationType(juce::Justification::centredLeft);
  addAndMakeVisible(modeCaption_);

  for (auto* s : {&inLevel_, &outLevel_}) {
    s->setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    s->setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    s->setRange(-kWrapperGainRangeDb, kWrapperGainRangeDb, 0.1);
    s->setDoubleClickReturnValue(true, 0.0);
    s->setMouseDragSensitivity(240);
    s->setColour(juce::Slider::rotarySliderFillColourId, accent);
    s->getProperties().set("bipolar", true);
    s->onValueChange = [this] { PushSettings(); };
    addAndMakeVisible(*s);
  }
  inLevel_.setTooltip("Input level before the engine (a global setting, never part of a preset).");
  outLevel_.setTooltip("Output level after the engine (a global setting, never part of a preset).");
  inCaption_.setText("In level", juce::dontSendNotification);
  outCaption_.setText("Out level", juce::dontSendNotification);
  for (auto* l : {&inCaption_, &outCaption_}) {
    StyleCaption(*l, 13.0f, palette::kTextDim);
    addAndMakeVisible(*l);
  }
  for (auto* l : {&inValue_, &outValue_}) {
    StyleCaption(*l, 13.5f, palette::kText);
    addAndMakeVisible(*l);
  }
  Refresh();
}

void TestInputPanel::PushSettings() {
  WrapperSettings s = processor_.GetSettings();
  s.inputMode       = stereo_.getToggleState() ? InputMode::Stereo : InputMode::Mono;
  s.inputGainDb     = static_cast<float>(inLevel_.getValue());
  s.outputGainDb    = static_cast<float>(outLevel_.getValue());
  processor_.SetSettings(s);
  Refresh();
}

void TestInputPanel::ChooseFile() {
  chooser_ = std::make_unique<juce::FileChooser>(
      "Loop an audio file through the engine",
      juce::File::getSpecialLocation(juce::File::userMusicDirectory), "*.wav;*.aif;*.aiff;*.flac;*.ogg");
  chooser_->launchAsync(
      juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
      [safe = juce::Component::SafePointer<TestInputPanel>(this)](const juce::FileChooser& fc) {
        if (safe == nullptr || fc.getResult() == juce::File()) return;
        juce::String error;
        TestInput&   input = safe->processor_.GetTestInput();
        safe->fileError_   = !input.LoadFile(fc.getResult(), error);
        if (safe->fileError_) {
          safe->fileName_.setText(error, juce::dontSendNotification);
        } else {
          input.SetSource(TestInput::Source::FileLoop);
        }
        safe->Refresh();
      });
}

void TestInputPanel::Refresh() {
  TestInput& input = processor_.GetTestInput();
  input.CollectGarbage();
  const auto source = input.GetSource();
  live_.setToggleState(source == TestInput::Source::Live, juce::dontSendNotification);
  file_.setToggleState(source == TestInput::Source::FileLoop, juce::dontSendNotification);
  pluck_.setToggleState(source == TestInput::Source::Pluck, juce::dontSendNotification);
  if (!fileError_) {
    const juce::String name = input.LoadedName();
    fileName_.setText(name.isEmpty() ? juce::String("WAV, AIFF, FLAC or Ogg") : name,
                      juce::dontSendNotification);
  }
  fileName_.setColour(juce::Label::textColourId,
                      fileError_ ? palette::kBad
                                 : (input.LoadedName().isEmpty() ? palette::kTextFaint : palette::kText));

  const WrapperSettings s = processor_.GetSettings();
  mono_.setToggleState(s.inputMode == InputMode::Mono, juce::dontSendNotification);
  stereo_.setToggleState(s.inputMode == InputMode::Stereo, juce::dontSendNotification);
  if (!inLevel_.isMouseButtonDown()) inLevel_.setValue(s.inputGainDb, juce::dontSendNotification);
  if (!outLevel_.isMouseButtonDown()) outLevel_.setValue(s.outputGainDb, juce::dontSendNotification);
  inValue_.setText(DbText(static_cast<float>(inLevel_.getValue())), juce::dontSendNotification);
  outValue_.setText(DbText(static_cast<float>(outLevel_.getValue())), juce::dontSendNotification);
}

void TestInputPanel::SetScale(float scale) {
  if (scale == scale_) return;
  scale_ = scale;
  fileName_.setFont(UiFont(13.0f * scale));
  for (auto* l : {&modeCaption_, &inCaption_, &outCaption_}) l->setFont(UiFont(13.0f * scale));
  for (auto* l : {&inValue_, &outValue_}) l->setFont(UiFont(13.5f * scale));
  resized();
  repaint();
}

void TestInputPanel::paint(juce::Graphics& g) {
  DrawPanel(g, getLocalBounds(), "Test input", palette::kWarn, scale_, "developer");
}

void TestInputPanel::resized() {
  auto r = getLocalBounds().reduced(12, 8);
  r.removeFromTop(Scaled(SectionPanel::kTitleHeight, scale_) - 6);

  const int knobW  = juce::jlimit(Scaled(64, scale_), Scaled(96, scale_), r.getWidth() / 5);
  auto      levels = r.removeFromRight(knobW * 2);
  r.removeFromRight(12);
  const int captionH = Scaled(16, scale_), valueH = Scaled(20, scale_);
  auto placeKnob = [&](juce::Rectangle<int> area, juce::Slider& s, juce::Label& caption, juce::Label& value) {
    const int side  = std::min({area.getWidth() - 8, area.getHeight() - captionH - valueH, Scaled(72, scale_)});
    const int total = captionH + side + valueH;
    area            = area.withSizeKeepingCentre(area.getWidth(), std::min(total, area.getHeight()));
    caption.setBounds(area.removeFromTop(captionH));
    value.setBounds(area.removeFromBottom(valueH));
    s.setBounds(area.withSizeKeepingCentre(side, side));
  };
  placeKnob(levels.removeFromLeft(knobW), inLevel_, inCaption_, inValue_);
  placeKnob(levels, outLevel_, outCaption_, outValue_);

  const int rowH = juce::jlimit(24, Scaled(30, scale_), (r.getHeight() - 16) / 3);
  const int gap  = std::max(4, (r.getHeight() - 3 * rowH) / 4);
  r.removeFromTop(gap);
  auto row1 = r.removeFromTop(rowH);
  r.removeFromTop(gap);
  auto row2 = r.removeFromTop(rowH);
  r.removeFromTop(gap);
  auto row3 = r.removeFromTop(rowH);

  const int segW = (row1.getWidth() - 8) / 3;
  live_.setBounds(row1.removeFromLeft(segW));
  row1.removeFromLeft(4);
  file_.setBounds(row1.removeFromLeft(segW));
  row1.removeFromLeft(4);
  pluck_.setBounds(row1);

  load_.setBounds(row2.removeFromLeft(std::min(Scaled(110, scale_), row2.getWidth() / 2)));
  row2.removeFromLeft(8);
  fileName_.setBounds(row2);

  modeCaption_.setBounds(row3.removeFromLeft(Scaled(48, scale_)));
  const int modeW = (row3.getWidth() - 4) / 2;
  mono_.setBounds(row3.removeFromLeft(modeW));
  row3.removeFromLeft(4);
  stereo_.setBounds(row3);
}

}  // namespace brainscape::plugin
