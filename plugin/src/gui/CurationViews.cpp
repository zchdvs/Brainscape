#include "CurationViews.h"

#include <cmath>

#include "../FactoryModes.h"
#include "BrainscapeLookAndFeel.h"
#include "Widgets.h"
#include "brainscape/ParamDisplay.h"
#include "brainscape/SoundRevision.h"

namespace brainscape::plugin {

namespace {

const juce::String kDot = juce::String::fromUTF8("\xc2\xb7");  // ·

constexpr float kStartAngle = juce::MathConstants<float>::pi * 1.25f;  // the LAF's rotary arc
constexpr float kEndAngle   = juce::MathConstants<float>::pi * 2.75f;

// The pedal's knobs, in panel order, and their Shift secondaries (§3.1).
struct KnobRow {
  ParamId primary;
  ParamId secondary;  // 0: none
  const char* name;
  const char* secondaryName;
};
constexpr KnobRow kKnobs[8] = {
    {ParamId::MacroActivity, static_cast<ParamId>(0), "Activity", nullptr},
    {ParamId::MacroRepeats, ParamId::ModDepth, "Repeats", "Mod depth"},
    {ParamId::MacroShape, ParamId::ModRateHz, "Shape", "Mod rate"},
    {ParamId::MacroTime, static_cast<ParamId>(0), "Time", nullptr},
    {ParamId::MacroSpace, ParamId::ReverbTime, "Space", "Reverb time"},
    {ParamId::MacroFilter, ParamId::FilterRes, "Filter", "Resonance"},
    {ParamId::Mix, ParamId::EffectVolumeDb, "Mix", "Volume"},
    {ParamId::PerfLoopLevel, static_cast<ParamId>(0), "Loop", nullptr},
};

float Normalized(const BrainscapeParam& p) { return NormalizedFromPlain(p.Id(), p.Plain()); }

uint64_t MacroKey(const ModeBlob& mode) {  // FNV-1a over the macro table: targets change with it
  uint64_t    h = 0xcbf29ce484222325ull;
  const auto* b = reinterpret_cast<const unsigned char*>(&mode.macros);
  for (size_t i = 0; i < sizeof mode.macros; ++i) {
    h ^= b[i];
    h *= 0x100000001b3ull;
  }
  return h;
}

// What the knobs' titles and captions are made of: the mode's macro table, whether a document is
// open, and its display names (META, not MODE: a renamed knob keeps the macro table).
juce::String BindKey(const ModeBlob& mode, const CurationSession& s) {
  juce::String key = juce::String::toHexString(static_cast<juce::int64>(MacroKey(mode)));
  key << (s.HasDocument() ? "|open" : "|none");
  for (uint32_t id = static_cast<uint32_t>(ParamId::MacroActivity); id <= static_cast<uint32_t>(ParamId::MacroAux2);
       ++id) {
    key << "|" << s.MacroName(static_cast<ParamId>(id));
  }
  return key;
}

bool Defined(const ModeBlob& mode, ParamId macro) {
  for (uint32_t k = 0; k < mode.macros.macroCount; ++k) {
    if (mode.macros.macros[k].id == static_cast<uint32_t>(macro)) return true;
  }
  return false;
}

// The leaf a macro's first target moves, or 0.
ParamId FirstTarget(const ModeBlob& mode, ParamId macro) {
  for (uint32_t k = 0; k < mode.macros.macroCount; ++k) {
    const MacroDef& md = mode.macros.macros[k];
    if (md.id == static_cast<uint32_t>(macro) && md.count > 0) {
      return static_cast<ParamId>(mode.macros.targets[md.first].param);
    }
  }
  return static_cast<ParamId>(0);
}

juce::String Percent(float position) { return juce::String(juce::roundToInt(position * 100.0f)) + "%"; }

}  // namespace

juce::Colour KnobAccent(size_t knob) {
  static const juce::Colour kAccents[8] = {
      juce::Colour(0xFFB794F4), juce::Colour(0xFF68D391), juce::Colour(0xFF63B3ED), juce::Colour(0xFFF6E05E),
      juce::Colour(0xFFF687B3), juce::Colour(0xFFF6AD55), palette::kText,           palette::kTextFaint};
  return knob < 8 ? kAccents[knob] : palette::kText;
}

juce::Colour MacroAccent(ParamId macro) {
  for (size_t k = 0; k < 6; ++k) {
    if (kKnobs[k].primary == macro) return KnobAccent(k);
  }
  return macro == ParamId::MacroAux1 ? juce::Colour(0xFF4FD1C5) : juce::Colour(0xFFED8936);
}

// ── PickupKnob ───────────────────────────────────────────────────────────────────────────

class PickupKnob::Pot final : public juce::Slider {
 public:
  explicit Pot(PickupKnob& owner) : owner_(owner) {
    setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    setRange(0.0, 1.0, 0.0);
    setRotaryParameters(kStartAngle, kEndAngle, true);
    setMouseDragSensitivity(240);
    setPopupMenuEnabled(false);
    setDoubleClickReturnValue(false, 0.0);
  }
  void mouseDoubleClick(const juce::MouseEvent&) override { owner_.Catch(); }
  void paint(juce::Graphics& g) override {
    using namespace palette;
    const auto  bounds = getLocalBounds().toFloat();
    const float side   = std::min(bounds.getWidth(), bounds.getHeight());
    const auto  centre = bounds.getCentre();
    const float radius = side * 0.5f - 2.0f;
    if (radius < 8.0f) return;
    const float stroke  = std::max(2.5f, radius * 0.12f);
    const float arcR    = radius - stroke * 0.5f - 3.0f;
    // Bound and live; bound while A plays (the views lock): dimmed, the stored values shown.
    const bool  enabled = owner_.param_ != nullptr;
    const bool  live    = enabled && isEnabled();
    const bool  locked  = live && !owner_.caught_;
    const auto  accent  = !enabled ? kTextFaint : live ? owner_.accent_ : owner_.accent_.withAlpha(0.4f);
    const float value   = owner_.Value();
    const float pointer = static_cast<float>(getValue());
    const auto  angleOf = [](float v) { return kStartAngle + v * (kEndAngle - kStartAngle); };

    juce::Path track;
    track.addCentredArc(centre.x, centre.y, arcR, arcR, 0.f, kStartAngle, kEndAngle, true);
    g.setColour(kTrack);
    g.strokePath(track, juce::PathStrokeType(stroke, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    if (enabled && value > 0.0005f) {  // the target's value: what plays
      juce::Path arc;
      arc.addCentredArc(centre.x, centre.y, arcR, arcR, 0.f, kStartAngle, angleOf(value), true);
      g.setColour(locked ? accent.withMultipliedAlpha(0.45f) : accent);
      g.strokePath(arc, juce::PathStrokeType(stroke, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    if (enabled) {  // the value's marker outside the ring: the pickup point
      const auto  m = centre.getPointOnCircumference(arcR + stroke * 0.5f + 4.0f, angleOf(value));
      const float d = std::max(4.0f, stroke * 0.7f);
      g.setColour(accent);
      g.fillEllipse(juce::Rectangle<float>(d, d).withCentre(m));
    }
    const float bodyR = arcR - stroke * 0.5f - std::max(2.0f, radius * 0.07f);
    const auto  body  = juce::Rectangle<float>(bodyR * 2.f, bodyR * 2.f).withCentre(centre);
    const bool  hover = isMouseOverOrDragging();
    g.setGradientFill(juce::ColourGradient(hover ? juce::Colour(0xFF353B46) : juce::Colour(0xFF2D323C), centre.x,
                                           body.getY(), juce::Colour(0xFF1C2027), centre.x, body.getBottom(), false));
    g.fillEllipse(body);
    g.setColour(locked ? kWarn.withAlpha(0.7f) : juce::Colour(0xFF3A404C));
    g.drawEllipse(body.reduced(0.5f), locked ? 1.5f : 1.0f);
    // The hand's pointer: dim while the knob waits for pickup.
    const auto tip  = centre.getPointOnCircumference(bodyR * 0.80f, angleOf(pointer));
    const auto tail = centre.getPointOnCircumference(bodyR * 0.28f, angleOf(pointer));
    g.setColour(!live ? kTextFaint : locked ? kTextDim : kText);
    g.drawLine({tail, tip}, std::max(2.0f, bodyR * 0.09f));
  }

 private:
  PickupKnob& owner_;
};

PickupKnob::PickupKnob() : pot_(std::make_unique<Pot>(*this)) {
  StyleCaption(title_, 14.0f, palette::kText, true);
  StyleCaption(caption_, 11.5f, palette::kTextFaint);
  StyleCaption(value_, 13.5f, palette::kText);
  StyleCaption(detail_, 12.0f, palette::kTextDim);
  detail_.setMinimumHorizontalScale(0.7f);
  for (juce::Component* c : std::initializer_list<juce::Component*>{pot_.get(), &title_, &caption_, &value_, &detail_}) {
    addAndMakeVisible(*c);
  }
  pot_->onValueChange = [this] {
    const float to = static_cast<float>(pot_->getValue());
    Moved(last_, to);
    last_ = to;
  };
  pot_->onDragStart = [this] {
    last_ = Pointer();
    if (param_ != nullptr && !gesture_) {
      param_->beginChangeGesture();
      gesture_ = true;
    }
  };
  pot_->onDragEnd = [this] {
    if (param_ != nullptr && gesture_) param_->endChangeGesture();
    gesture_ = false;
  };
}

PickupKnob::~PickupKnob() {
  if (param_ != nullptr && gesture_) param_->endChangeGesture();
}

float PickupKnob::Pointer() const { return static_cast<float>(pot_->getValue()); }
float PickupKnob::Value() const { return param_ != nullptr ? Normalized(*param_) : Pointer(); }

void PickupKnob::Bind(BrainscapeParam* param, juce::Colour accent, const juce::String& title,
                      const juce::String& caption, const juce::String& tooltip) {
  if (param != param_ && param_ != nullptr && gesture_) {
    param_->endChangeGesture();
    gesture_ = false;
  }
  const bool changed = param != param_;
  const bool first   = param_ == nullptr && !placed_ && param != nullptr;
  param_             = param;
  if (first) {  // the hand starts where the value is: a knob opened on screen is caught
    placed_ = true;
    pot_->setValue(Value(), juce::dontSendNotification);
    last_ = Pointer();
  }
  accent_            = accent;
  title_.setText(title, juce::dontSendNotification);
  caption_.setText(caption, juce::dontSendNotification);
  setTooltip(tooltip);
  pot_->setTooltip(tooltip);
  pot_->setEnabled(param != nullptr);
  if (changed) Lock();
  repaint();
}

void PickupKnob::Lock() {
  caught_ = param_ == nullptr || std::fabs(Pointer() - Value()) < 1e-6f;
  pot_->repaint();
}

void PickupKnob::Catch() {
  if (param_ == nullptr) return;
  pot_->setValue(Value(), juce::dontSendNotification);
  last_   = Pointer();
  caught_ = true;
  repaint();
}

// The hand moved the pointer. Locked, the knob catches when the pointer reaches or crosses the
// value; caught, every move sends.
void PickupKnob::Moved(float from, float to) {
  if (param_ == nullptr) return;
  if (!caught_) {
    const float v = Value();
    if ((from - v) * (to - v) > 0.f) {
      repaint();
      return;
    }
    caught_ = true;
  }
  const float plain = PlainFromNormalized(param_->Id(), to);
  if (gesture_) {
    param_->SetPlainInGesture(plain);
  } else {
    param_->SetPlainNotifyingHost(plain);
  }
  repaint();
}

void PickupKnob::MoveTo(float position) {
  pot_->onDragStart();
  pot_->setValue(position, juce::sendNotificationSync);
  pot_->onDragEnd();
}

void PickupKnob::Refresh(const juce::String& detail) {
  if (param_ == nullptr) {
    value_.setText(juce::String::fromUTF8("\xe2\x80\x94"), juce::dontSendNotification);  // —
    detail_.setText(detail, juce::dontSendNotification);
    detail_.setColour(juce::Label::textColourId, palette::kTextFaint);
    return;
  }
  const float v = Value();
  // A caught knob follows what the host moves; a locked one shows the value it waits for.
  if (caught_ && !pot_->isMouseButtonDown() && std::fabs(Pointer() - v) > 1e-6f) {
    pot_->setValue(v, juce::dontSendNotification);
    last_ = v;
  }
  value_.setText(param_->getCurrentValueAsText(), juce::dontSendNotification);
  if (!caught_ && isEnabled()) {
    detail_.setText("pick up at " + Percent(v) + " (now " + Percent(Pointer()) + ")", juce::dontSendNotification);
    detail_.setColour(juce::Label::textColourId, palette::kWarn);
  } else {
    detail_.setText(detail, juce::dontSendNotification);
    detail_.setColour(juce::Label::textColourId, palette::kTextDim);
  }
  pot_->repaint();
}

void PickupKnob::SetScale(float scale) {
  if (scale == scale_) return;
  scale_ = scale;
  title_.setFont(UiFont(14.0f * scale, true));
  caption_.setFont(UiFont(11.5f * scale));
  value_.setFont(UiFont(13.5f * scale));
  detail_.setFont(UiFont(12.0f * scale));
  resized();
}

void PickupKnob::resized() {
  auto      r        = getLocalBounds();
  const int titleH   = Scaled(18, scale_);
  const int captionH = Scaled(14, scale_);
  const int valueH   = Scaled(18, scale_);
  const int detailH  = Scaled(16, scale_);
  title_.setBounds(r.removeFromTop(titleH));
  caption_.setBounds(r.removeFromTop(captionH));
  detail_.setBounds(r.removeFromBottom(detailH));
  value_.setBounds(r.removeFromBottom(valueH));
  const int side = std::min({r.getWidth() - 4, r.getHeight(), Scaled(120, scale_)});
  pot_->setBounds(r.withSizeKeepingCentre(side, side));
}

// ── MacroPanel ───────────────────────────────────────────────────────────────────────────

MacroPanel::MacroPanel(BrainscapeProcessor& processor) : processor_(processor) {
  for (auto& k : knobs_) addAndMakeVisible(k);
  shift_.setClickingTogglesState(true);
  shift_.setColour(juce::TextButton::buttonOnColourId, palette::kLed);
  shift_.setColour(juce::TextButton::textColourOnId, palette::kBackground);
  shift_.setTooltip("Shift: Repeats moves the mod depth, Shape the mod rate, Space the reverb time, Filter the "
                    "resonance and Mix the effect volume, as Shift does on the pedal. Each knob picks up its new "
                    "target.");
  shift_.onClick = [this] {
    shifted_ = shift_.getToggleState();
    Bind();
  };
  addAndMakeVisible(shift_);
  Bind();
}

void MacroPanel::Bind() {
  const ModeBlob mode = processor_.CurrentMode().mode;
  CurationSession& s  = processor_.Curation();
  bindKey_            = BindKey(mode, s);
  for (size_t i = 0; i < knobs_.size(); ++i) {
    const KnobRow& row = kKnobs[i];
    const auto     id  = shifted_ ? row.secondary : row.primary;
    if (static_cast<uint32_t>(id) == 0u) {
      knobs_[i].Bind(nullptr, KnobAccent(i), row.name, "Shift: none", "No Shift secondary on this knob.");
      continue;
    }
    if (id == ParamId::PerfLoopLevel) {
      knobs_[i].Bind(nullptr, KnobAccent(i), "Loop", "Loop level",
                     "Loop Level: reserved until the looper exists (perf.loop_level, mode-compiler.md section 4.2).");
      continue;
    }
    BrainscapeParam* p = processor_.FindHostParam(id);
    const bool macro   = BrainscapeProcessor::IsMacroRow(id);
    if (macro && !Defined(mode, id)) {
      knobs_[i].Bind(nullptr, KnobAccent(i), row.name, "undefined", "The mode leaves this macro undefined.");
      continue;
    }
    juce::String title = shifted_ ? juce::String(row.secondaryName) : juce::String(row.name);
    juce::String caption;
    if (macro) {
      const juce::String shown = s.HasDocument() ? s.MacroName(id) : juce::String(row.name);
      if (shown != row.name) {
        caption = row.name;
        title   = shown;
      }
    }
    if (macro) {  // the targets it moves
      const int n = static_cast<int>(s.HasDocument() ? s.TargetsOfMacro(id).size() : 0u);
      const juce::String targets = s.HasDocument() ? juce::String(n) + (n == 1 ? " target" : " targets")
                                                   : juce::String("default macro");
      caption = caption.isEmpty() ? targets : caption + " " + kDot + " " + targets;
    } else if (shifted_) {
      caption = juce::String("Shift ") + row.name;
    } else {
      caption = id == ParamId::Mix ? juce::String("dry to the middle, wet from it") : juce::String(FindParam(id)->name);
    }
    const juce::String tip =
        macro ? title + " (" + FindParam(id)->name + "): moves its targets as one MacroMove per change. After a load "
                    "the knob waits until it reaches the stored position (pickup); double-click to catch it there."
              : juce::String(FindParamDisplay(id)->title) + " (" + FindParam(id)->name + "), with pickup after a load.";
    knobs_[i].Bind(p, KnobAccent(i), title, caption, tip);
  }
}

void MacroPanel::Refresh() {
  const ModeBlob mode = processor_.CurrentMode().mode;
  CurationSession& s  = processor_.Curation();
  const bool     a    = s.HasDocument() && s.GetSide() == CurationSession::Side::Stored;
  if (BindKey(mode, s) != bindKey_) Bind();
  title_ = s.HasDocument() ? "Macros " + kDot + " " + juce::String::fromUTF8(s.Stored().name.c_str())
                           : juce::String("Macros ") + kDot + " default mode";
  const uint32_t serial = processor_.LoadSerial();
  if (serial != loadSerial_) {
    loadSerial_ = serial;
    for (auto& k : knobs_) k.Lock();
  }
  for (size_t i = 0; i < knobs_.size(); ++i) {
    juce::String detail;
    const auto   id = shifted_ ? kKnobs[i].secondary : kKnobs[i].primary;
    if (BrainscapeProcessor::IsMacroRow(id)) {
      const ParamId leaf = FirstTarget(mode, id);
      if (IsLeaf(leaf)) {
        detail = juce::String(FindParamDisplay(leaf)->shortTitle) + " " +
                 processor_.Param(leaf).getCurrentValueAsText();
      }
    } else if (id == ParamId::PerfLoopLevel) {
      detail = "the looper";
    }
    knobs_[i].setEnabled(!a);
    knobs_[i].Refresh(detail);
  }
  repaint();
}

void MacroPanel::SetScale(float scale) {
  scale_ = scale;
  for (auto& k : knobs_) k.SetScale(scale);
  resized();
}

void MacroPanel::paint(juce::Graphics& g) {
  DrawPanel(g, getLocalBounds(), title_, palette::GroupAccent(ParamGroup::Macros), scale_,
            shifted_ ? juce::String("Shift: the secondaries") : juce::String("pickup after a load"));
}

void MacroPanel::resized() {
  auto r = getLocalBounds().reduced(8, 6);
  r.removeFromTop(Scaled(SectionPanel::kTitleHeight, scale_) - 2);
  auto shiftArea = r.removeFromLeft(Scaled(76, scale_));
  shift_.setBounds(shiftArea.withSizeKeepingCentre(shiftArea.getWidth() - 8, Scaled(34, scale_)));
  r.removeFromLeft(6);
  const int w = r.getWidth() / static_cast<int>(knobs_.size());
  for (size_t i = 0; i < knobs_.size(); ++i) {
    const bool last = i + 1 == knobs_.size();
    knobs_[i].setBounds(r.removeFromLeft(last ? r.getWidth() : w).reduced(2, 0));
  }
}

// ── DocumentPanel ────────────────────────────────────────────────────────────────────────

juce::ScopedMessageBox AskDiscard(const CurationSession& session, const juce::String& replacement,
                                  juce::Component* parent, std::function<void()> discard) {
  const juce::String name =
      session.HasDocument() ? juce::String::fromUTF8(session.Stored().name.c_str()) : juce::String("The document");
  const juce::String keep = session.FactoryIndex() >= 0 ? "Save as... keeps them in a copy." : "Save keeps them.";
  const auto options = juce::MessageBoxOptions::makeOptionsOkCancel(
      juce::MessageBoxIconType::WarningIcon, "Discard unsaved edits to " + name + "?",
      name + " has unsaved edits (knob positions, leaves, detached leaves). Opening " + replacement +
          " drops them. " + keep,
      "Discard", "Cancel", parent);
  return juce::AlertWindow::showScopedAsync(options, [then = std::move(discard)](int result) {
    if (result != 0 && then) then();  // 1: Discard, 0: Cancel or closed
  });
}

void DocumentPanel::StyleToggle(juce::TextButton& b, juce::Colour on) {
  b.setClickingTogglesState(false);
  b.setColour(juce::TextButton::buttonOnColourId, on);
  b.setColour(juce::TextButton::textColourOnId, palette::kBackground);
}

DocumentPanel::DocumentPanel(BrainscapeProcessor& processor)
    : session_(processor.Curation()), noteColour_(palette::kTextDim) {
  name_.setFont(UiFont(17.0f, true));
  name_.setColour(juce::Label::textColourId, palette::kText);
  for (auto* l : {&file_, &state_, &note_, &render_}) {
    l->setFont(UiFont(12.5f));
    l->setColour(juce::Label::textColourId, palette::kTextDim);
    l->setMinimumHorizontalScale(0.75f);
  }
  for (auto* l : {&name_, &file_, &state_, &note_, &render_}) {
    l->setJustificationType(juce::Justification::centredLeft);
    l->setInterceptsMouseClicks(false, false);
    addAndMakeVisible(*l);
  }
  for (auto* b : {&open_, &save_, &saveAs_, &revert_, &solve_, &a_, &b_, &match_, &renderButton_, &all_, &attack_,
                  &pad_, &reveal_}) {
    addAndMakeVisible(*b);
  }
  StyleToggle(a_, palette::kWarn);
  StyleToggle(b_, palette::GroupAccent(ParamGroup::Macros));
  StyleToggle(match_, palette::kIce);
  StyleToggle(all_, palette::kIce);
  StyleToggle(attack_, palette::kTextDim);
  StyleToggle(pad_, palette::kTextDim);

  open_.setTooltip("Open a preset document (.json) or package (.bsp) and play it: a Spillover load with Trails "
                   "while audio runs (Ctrl+O).");
  open_.onClick = [this] { ChooseOpen(); };
  save_.setTooltip("Save the working version as canonical JSON (and its package, when one sits beside it): every "
                   "macro-targeted leaf that is not detached is derived from its knob's position, the document is "
                   "compiled and stamped, and what was saved plays (Ctrl+S).");
  save_.onClick = [this] { Save(); };
  saveAs_.setTooltip("Save the working version to another document file.");
  saveAs_.onClick = [this] { ChooseSaveAs(); };
  revert_.setTooltip("Re-open the document from disk, dropping unsaved changes.");
  revert_.onClick = [this] {
    juce::String error;
    if (!session_.Revert(&error)) Note(error, palette::kBad);
    Refresh();
  };
  solve_.setTooltip("Solve position: move each macro's stored position to the one whose value lands nearest its "
                    "first target's leaf, for leaves set by hand. Right-click a leaf in the Leaves view to solve one "
                    "macro.");
  solve_.onClick = [this] {
    session_.SolvePositions();
    Note(session_.LastMessage(), palette::kText);
    Refresh();
  };
  a_.setTooltip("A: the stored version, as on disk (key B toggles A/B). Edits are for B: the knobs lock while A "
                "plays.");
  b_.setTooltip("B: the working version, what you have edited (key B toggles A/B).");
  a_.onClick = [this] {
    if (session_.GetSide() != CurationSession::Side::Stored) ToggleSide();
  };
  b_.onClick = [this] {
    if (session_.GetSide() != CurationSession::Side::Working) ToggleSide();
  };
  match_.setTooltip("Level-match A and B: both are rendered offline on the input class's test signal and measured "
                    "as K-weighted loudness; the louder one is trimmed on the monitor output only (never in a "
                    "preset or a render).");
  match_.onClick = [this] {
    session_.SetMatchLevel(!session_.MatchLevel());
    Refresh();
  };
  renderButton_.setTooltip("Render the document as Save would write it through tools/audition's scripts (S0, or "
                           "S0-S11) with the objective pre-screen, into Music/Brainscape audition/<id>/: 16-bit WAVs, "
                           "a recipe per render and audition.json.");
  renderButton_.onClick = [this] { StartRender(); };
  all_.setTooltip("Render every script, S0-S11 (about 45 renders), instead of S0's stored positions alone.");
  all_.onClick = [this] { all_.setToggleState(!all_.getToggleState(), juce::dontSendNotification); };
  attack_.setTooltip("Input class Attack: the scripts and the level match play Plucks (mode-compiler.md 11.3).");
  pad_.setTooltip("Input class Pad: the scripts and the level match play SoftNotes.");
  attack_.onClick = [this] {
    session_.SetInputClass(bsa::InputClass::Attack);
    Refresh();
  };
  pad_.onClick = [this] {
    session_.SetInputClass(bsa::InputClass::Pad);
    Refresh();
  };
  reveal_.setTooltip("Show the renders' folder.");
  reveal_.onClick = [this] {
    const juce::File dir = session_.GetRender().dir;
    if (dir.isDirectory()) dir.revealToUser();
  };
  Refresh();
}

void DocumentPanel::Note(const juce::String& text, juce::Colour colour) {
  noteText_   = text;
  noteColour_ = colour;
}

void DocumentPanel::ChooseOpen() {
  const juce::File start = session_.HasDocument() && session_.FactoryIndex() < 0
                               ? session_.SourceFile().getParentDirectory()
                               : juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
  chooser_ = std::make_unique<juce::FileChooser>("Open a preset document or package", start, "*.json;*.bsp");
  chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                        [safe = juce::Component::SafePointer<DocumentPanel>(this)](const juce::FileChooser& fc) {
                          if (safe == nullptr || fc.getResult() == juce::File()) return;
                          safe->OpenChosen(fc.getResult());
                        });
}

void DocumentPanel::OpenChosen(const juce::File& file) {
  std::function<void()> openFile = [safe = juce::Component::SafePointer<DocumentPanel>(this), file] {
    if (safe == nullptr) return;
    juce::String error;
    if (safe->session_.Open(file, &error)) {
      safe->Note(safe->session_.LastMessage(), palette::kText);
    } else {
      safe->Note(error, palette::kBad);
    }
    safe->Refresh();
  };
  if (session_.HasDocument() && session_.Dirty()) {
    askBox_ = AskDiscard(session_, file.getFileName(), this, std::move(openFile));
  } else {
    openFile();
  }
}

void DocumentPanel::Save() {
  if (!session_.HasDocument()) return;
  if (session_.FactoryIndex() >= 0) {  // built in: a copy, wherever the user puts it
    ChooseSaveAs();
    return;
  }
  const auto r = session_.Save();
  Note(r.message, !r.written ? palette::kBad : r.compiled ? palette::kGood : palette::kWarn);
  Refresh();
}

void DocumentPanel::ChooseSaveAs() {
  if (!session_.HasDocument()) return;
  // A factory mode's copy starts in Documents, under its file name ("lull.json").
  const int        factory = session_.FactoryIndex();
  const juce::File start =
      factory < 0 ? session_.DocumentFile()
                  : juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                        .getChildFile(juce::File::createLegalFileName(
                            juce::String(Factory(static_cast<size_t>(factory)).path).fromLastOccurrenceOf("/", false, false)));
  chooser_ = std::make_unique<juce::FileChooser>("Save the working version as a preset document", start, "*.json");
  chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles |
                            juce::FileBrowserComponent::warnAboutOverwriting,
                        [safe = juce::Component::SafePointer<DocumentPanel>(this)](const juce::FileChooser& fc) {
                          if (safe == nullptr || fc.getResult() == juce::File()) return;
                          const auto r = safe->session_.SaveAs(fc.getResult(), safe->session_.WritesPackage());
                          safe->Note(r.message, !r.written ? palette::kBad : r.compiled ? palette::kGood : palette::kWarn);
                          safe->Refresh();
                        });
}

void DocumentPanel::ToggleSide() {
  if (!session_.HasDocument()) return;
  const auto to = session_.GetSide() == CurationSession::Side::Working ? CurationSession::Side::Stored
                                                                       : CurationSession::Side::Working;
  session_.SetSide(to);
  Refresh();
}

void DocumentPanel::StartRender() {
  CurationSession::RenderRequest rq;
  rq.allScripts = all_.getToggleState();
  rq.outDir     = CurationSession::DefaultRenderDir();
  juce::String error;
  if (!session_.StartRender(rq, &error)) Note(error, palette::kBad);
  Refresh();
}

void DocumentPanel::Refresh() {
  const bool open    = session_.HasDocument();
  const bool a       = open && session_.GetSide() == CurationSession::Side::Stored;
  const bool factory = open && session_.FactoryIndex() >= 0;
  if (open) {
    const bsc::Document& d = session_.Stored();
    name_.setText(juce::String::fromUTF8(d.name.c_str()) + "   " + juce::String::fromUTF8(d.id.c_str()),
                  juce::dontSendNotification);
    if (factory) {
      file_.setText(session_.SourceLabel() + "   " + kDot + "   the factory set: Save as... writes a copy",
                    juce::dontSendNotification);
    } else {
      file_.setText(session_.DocumentFile().getFileName() +
                        (session_.WritesPackage() ? " + " + session_.PackageFile().getFileName() : juce::String()) +
                        "   " + kDot + "   " + session_.DocumentFile().getParentDirectory().getFullPathName(),
                    juce::dontSendNotification);
    }
    juce::String state;
    juce::Colour colour = palette::kTextDim;
    if (a) {
      state  = "A: playing the stored version " + kDot + " B waits";
      colour = palette::kWarn;
    } else if (factory) {
      // Nothing to save back to: edits go to a copy (Save as).
      state  = session_.Dirty() ? juce::String("Edited: Save as... keeps the edits in a copy")
                                : juce::String("As built in");
      colour = session_.Dirty() ? palette::kWarn : palette::kTextDim;
    } else if (session_.SaveChangesFile()) {
      // Unsaved edits, or a document on disk that Save would still rewrite: a targeted leaf off
      // its macro's value (derive), or a stale stamp.
      const int derives = static_cast<int>(session_.PendingDerives().size());
      state  = session_.Dirty()          ? juce::String("Unsaved changes")
               : derives > 0             ? "Save will derive " + juce::String(derives) + " leaf value(s)"
               : !session_.PendingPerformance().empty() ? juce::String("Save will store the live tempo and Subdiv")
               : !session_.StampCurrent() ? juce::String("Save will restamp")
                                          : juce::String("Save will rewrite it in canonical form");
      if (session_.Dirty() && derives > 0) state << " " << kDot << " Save derives " << derives << " leaf value(s)";
      colour = palette::kWarn;
    } else {
      state = "Saved";
    }
    state << "   " << kDot << "   "
          << (session_.StampCurrent() ? "stamp current (r" + juce::String(static_cast<int>(kSoundRevision)) + ")"
                                      : juce::String("stamp stale: Save restamps"));
    state_.setText(state, juce::dontSendNotification);
    state_.setColour(juce::Label::textColourId, colour);
  } else {
    name_.setText("No document", juce::dontSendNotification);
    file_.setText("Pick a mode from the Modes menu, or open a document (.json) or package (.bsp), to curate it.",
                  juce::dontSendNotification);
    state_.setText({}, juce::dontSendNotification);
  }
  for (auto* b : {&save_, &saveAs_, &revert_, &a_, &b_, &match_, &renderButton_, &all_, &attack_, &pad_}) {
    b->setEnabled(open);
  }
  save_.setEnabled(open && !factory);  // built in: Save as
  solve_.setEnabled(open && !a);
  a_.setToggleState(a, juce::dontSendNotification);
  b_.setToggleState(open && !a, juce::dontSendNotification);
  match_.setToggleState(session_.MatchLevel(), juce::dontSendNotification);
  attack_.setToggleState(session_.GetInputClass() == bsa::InputClass::Attack, juce::dontSendNotification);
  pad_.setToggleState(session_.GetInputClass() == bsa::InputClass::Pad, juce::dontSendNotification);

  // The note line: the last command's outcome, else the level match.
  juce::String note   = noteText_;
  juce::Colour colour = noteColour_;
  if (session_.MatchLevel()) {
    const auto m = session_.GetLevelMatch();
    juce::String level;
    if (m.state == CurationSession::LevelMatch::State::Measuring) {
      level = "Level match: measuring...";
    } else if (m.state == CurationSession::LevelMatch::State::Ready) {
      level = "Level match: B " + juce::String(m.workingLufs - m.storedLufs, 1) + " LU against A; trim A " +
              juce::String(m.trimStored, 1) + " dB, B " + juce::String(m.trimWorking, 1) + " dB" +
              (session_.MatchPending() ? " (B changed: re-measuring)" : "");
    } else if (m.state == CurationSession::LevelMatch::State::Failed) {
      level = "Level match: one version is silent on this input";
    }
    if (level.isNotEmpty()) {
      note   = note.isEmpty() ? level : note + "   " + kDot + "   " + level;
      colour = noteText_.isEmpty() ? palette::kIce : colour;
    }
  }
  note_.setText(note, juce::dontSendNotification);
  note_.setColour(juce::Label::textColourId, colour);
  const auto r = session_.GetRender();
  render_.setText(r.state == CurationSession::RenderStatus::State::Idle
                      ? "Renders go to " + CurationSession::DefaultRenderDir().getFullPathName()
                      : r.message,
                  juce::dontSendNotification);
  render_.setColour(juce::Label::textColourId, r.state == CurationSession::RenderStatus::State::Failed  ? palette::kBad
                                               : r.state == CurationSession::RenderStatus::State::Running ? palette::kWarn
                                               : !r.failures.isEmpty()                                    ? palette::kWarn
                                               : r.state == CurationSession::RenderStatus::State::Done    ? palette::kGood
                                                                                                          : palette::kTextFaint);
  render_.setTooltip(r.summary + (r.failures.isEmpty() ? juce::String() : "\n" + r.failures.joinIntoString("\n")));
  renderButton_.setEnabled(open && r.state != CurationSession::RenderStatus::State::Running);
  reveal_.setEnabled(r.dir.isDirectory());
}

void DocumentPanel::SetScale(float scale) {
  if (scale == scale_) return;
  scale_ = scale;
  name_.setFont(UiFont(17.0f * scale, true));
  for (auto* l : {&file_, &state_, &note_, &render_}) l->setFont(UiFont(12.5f * scale));
  resized();
}

void DocumentPanel::paint(juce::Graphics& g) {
  DrawPanel(g, getLocalBounds(), "Document", palette::GroupAccent(ParamGroup::Macros), scale_, "curation");
}

void DocumentPanel::resized() {
  auto r = getLocalBounds().reduced(12, 6);
  r.removeFromTop(Scaled(SectionPanel::kTitleHeight, scale_) - 4);
  const int lineH = Scaled(18, scale_);
  const int rowH  = juce::jlimit(22, Scaled(28, scale_), (r.getHeight() - 5 * lineH) / 4);
  const int gap   = std::max(2, (r.getHeight() - 5 * lineH - 4 * rowH) / 8);
  name_.setBounds(r.removeFromTop(lineH + Scaled(4, scale_)));
  file_.setBounds(r.removeFromTop(lineH));
  state_.setBounds(r.removeFromTop(lineH));
  r.removeFromTop(gap);
  const auto row = [&](std::initializer_list<std::pair<juce::Component*, int>> items) {
    auto line = r.removeFromTop(rowH);
    r.removeFromTop(gap);
    int units = 0;
    for (const auto& i : items) units += i.second;
    const int avail = line.getWidth() - 4 * (static_cast<int>(items.size()) - 1);
    size_t    n     = 0;
    for (const auto& i : items) {
      const bool last = ++n == items.size();
      i.first->setBounds(line.removeFromLeft(last ? line.getWidth() : avail * i.second / units));
      line.removeFromLeft(4);
    }
  };
  row({{&open_, 3}, {&save_, 3}, {&saveAs_, 3}, {&revert_, 3}});
  row({{&a_, 3}, {&b_, 3}, {&match_, 3}, {&solve_, 4}});
  row({{&renderButton_, 3}, {&all_, 3}, {&attack_, 2}, {&pad_, 2}, {&reveal_, 2}});
  note_.setBounds(r.removeFromTop(lineH));
  render_.setBounds(r.removeFromTop(lineH));
}

// ── FindingsPanel ────────────────────────────────────────────────────────────────────────

FindingsPanel::FindingsPanel(CurationSession& session) : session_(session) {
  list_.setModel(this);
  list_.setRowHeight(22);
  list_.setColour(juce::ListBox::backgroundColourId, palette::kPanel);
  list_.setOutlineThickness(0);
  addAndMakeVisible(list_);
}

void FindingsPanel::Refresh() {
  std::vector<Row> rows;
  int              errors = 0, lints = 0;
  if (session_.HasDocument()) {
    for (const bsc::Finding& f : session_.Findings()) {
      const auto kind = f.error ? Row::Kind::Error : Row::Kind::Lint;
      (f.error ? errors : lints) += 1;
      rows.push_back({kind, juce::String(f.code),
                      juce::String::fromUTF8((f.at.pointer.empty() ? f.message : f.at.pointer + "  " + f.message).c_str())});
    }
    for (const std::string& line : session_.PendingDerives()) {
      rows.push_back({Row::Kind::Derive, "derive", juce::String::fromUTF8(line.c_str())});
    }
    for (const std::string& line : session_.PendingPerformance()) {  // clock.md §10.3
      rows.push_back({Row::Kind::Derive, "store", juce::String::fromUTF8(line.c_str())});
    }
    if (rows.empty()) rows.push_back({Row::Kind::Note, "ok", "Compiles; no lint findings; nothing to derive."});
  } else {
    for (const bsc::Finding& f : session_.RefusedFindings()) {  // why the last open failed
      rows.push_back({f.error ? Row::Kind::Error : Row::Kind::Lint, juce::String(f.code),
                      juce::String::fromUTF8((f.at.pointer + "  " + f.message).c_str())});
    }
  }
  summary_ = juce::String(errors) + " error" + (errors == 1 ? "" : "s") + "   " + kDot + "   " + juce::String(lints) +
             " lint   " + kDot + "   " + juce::String(static_cast<int>(session_.PendingDerives().size())) +
             " to derive";
  bool same = rows.size() == rows_.size();
  for (size_t i = 0; same && i < rows.size(); ++i) same = rows[i].text == rows_[i].text && rows[i].code == rows_[i].code;
  if (!same) {
    rows_ = std::move(rows);
    list_.updateContent();
    list_.repaint();
  }
  repaint();
}

void FindingsPanel::paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool) {
  if (row < 0 || row >= static_cast<int>(rows_.size())) return;
  const Row&   r = rows_[static_cast<size_t>(row)];
  juce::Colour c = r.kind == Row::Kind::Error ? palette::kBad
                   : r.kind == Row::Kind::Lint ? palette::kWarn
                   : r.kind == Row::Kind::Derive ? palette::kIce
                                                 : palette::kGood;
  const float chipW = 52.0f * scale_;
  const auto  chip  = juce::Rectangle<float>(4.0f, 3.0f, chipW, static_cast<float>(height) - 6.0f);
  g.setColour(c.withAlpha(0.16f));
  g.fillRoundedRectangle(chip, 4.0f);
  g.setColour(c);
  g.setFont(UiFont(11.5f * scale_, true));
  g.drawText(r.code, chip, juce::Justification::centred, true);
  g.setColour(palette::kText);
  g.setFont(UiFont(12.5f * scale_));
  g.drawText(r.text, juce::Rectangle<int>(static_cast<int>(chipW) + 12, 0, width - static_cast<int>(chipW) - 14, height),
             juce::Justification::centredLeft, true);
}

juce::String FindingsPanel::getTooltipForRow(int row) {
  if (row < 0 || row >= static_cast<int>(rows_.size())) return {};
  return rows_[static_cast<size_t>(row)].code + ": " + rows_[static_cast<size_t>(row)].text;
}

void FindingsPanel::SetScale(float scale) {
  if (scale == scale_) return;
  scale_ = scale;
  list_.setRowHeight(Scaled(22, scale));
  resized();
}

void FindingsPanel::paint(juce::Graphics& g) {
  DrawPanel(g, getLocalBounds(), "Compile " + kDot + " lint", palette::kBad.withSaturation(0.5f), scale_, summary_);
}

void FindingsPanel::resized() {
  auto r = getLocalBounds().reduced(8, 6);
  r.removeFromTop(Scaled(SectionPanel::kTitleHeight, scale_) - 2);
  list_.setBounds(r);
}

}  // namespace brainscape::plugin
