#include "TempoPanel.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "../Curation.h"
#include "BrainscapeLookAndFeel.h"
#include "Widgets.h"
#include "brainscape/ModeEval.h"

namespace brainscape::plugin {

namespace {

const juce::String kTimes = juce::String::fromUTF8("\xc3\x97");  // ×
const juce::Colour kAccent = palette::GroupAccent(ParamGroup::Performance);

juce::String BpmText(uint32_t nsPerQuarter) {
  // 0.1 BPM: 6·10^10 / ns, rounded to a tenth for display only.
  const double bpm = nsPerQuarter > 0u ? 6.0e10 / static_cast<double>(nsPerQuarter) : 0.0;
  return juce::String(std::round(bpm * 10.0) / 10.0, 1);
}

juce::String BpmTextUs(uint32_t usPerQuarter) { return BpmText(usPerQuarter * 1000u); }

void StyleSegment(juce::TextButton& b, int group, juce::Colour on) {
  b.setClickingTogglesState(false);
  b.setRadioGroupId(group);
  b.setColour(juce::TextButton::buttonOnColourId, on);
  b.setColour(juce::TextButton::textColourOnId, palette::kBackground);
}

}  // namespace

// TAP: the trigger button's look in the performance accent, a flash per tap.
class TempoPanel::TapButton final : public Scalable<juce::Button> {
 public:
  TapButton() : Scalable<juce::Button>("Tap") {}
  void Flash() { flash_ = 1.f; }
  void Tick() {
    if (flash_ <= 0.f) return;
    flash_ *= 0.7f;
    if (flash_ < 0.03f) flash_ = 0.f;
    repaint();
  }
  void paintButton(juce::Graphics& g, bool highlighted, bool down) override {
    const auto  b    = getLocalBounds().toFloat().reduced(1.0f);
    const bool  on   = isEnabled();
    const float lit  = on ? (down ? 1.f : flash_) : 0.f;
    const auto  ink  = on ? kAccent : palette::kTextFaint;
    g.setColour((highlighted && on ? palette::kControlHover : palette::kControl).interpolatedWith(kAccent, 0.85f * lit));
    g.fillRoundedRectangle(b, 8.0f);
    g.setColour(ink.withAlpha(on ? (highlighted ? 0.95f : 0.6f) : 0.35f));
    g.drawRoundedRectangle(b.reduced(0.5f), 8.0f, 1.5f);
    g.setColour(lit > 0.5f ? palette::kBackground : ink);
    g.setFont(UiFont(std::min(14.0f * scale_, b.getHeight() * 0.42f), true).withExtraKerningFactor(0.1f));
    g.drawText("TAP", b, juce::Justification::centred, false);
  }

 private:
  float flash_ = 0.f;
};

TempoPanel::TempoPanel(BrainscapeProcessor& processor)
    : processor_(processor),
      standalone_(processor.wrapperType == juce::AudioProcessor::wrapperType_Standalone),
      tap_(std::make_unique<TapButton>()) {
  bpm_.setFont(UiFont(24.0f, true));
  bpm_.setColour(juce::Label::textColourId, palette::kText);
  bpm_.setJustificationType(juce::Justification::centredRight);
  bpm_.setEditable(false, true, false);
  bpm_.setTooltip("The committed tempo. Double-click to type one (20-300 BPM), sent as a Tempo event; "
                  "ignored while the host's tempo or a MIDI clock is followed.");
  bpm_.onTextChange = [this] {
    const juce::String text = bpm_.getText().trim().upToFirstOccurrenceOf(" ", false, false);
    if (text.containsOnly("0123456789.") && text.isNotEmpty()) processor_.SetTempoFromUi(text.getDoubleValue());
    bpm_.setText(BpmText(shown_.nsPerQuarter), juce::dontSendNotification);
  };
  addAndMakeVisible(bpm_);

  tap_->onClick = [this] {
    processor_.TapFromUi();
    tap_->Flash();
  };
  addAndMakeVisible(*tap_);

  // Row 83's positions in the Microcosm's CC#5 order, as rates (§5.1); row 84's modes.
  const char* const subdivs[] = {"1/4", "1/2", "TAP", "2", "4", "8"};
  for (int k = 0; k < 6; ++k) {
    juce::TextButton& b = subdiv_[static_cast<size_t>(k)];
    b.setButtonText(k == 2 ? juce::String("TAP") : kTimes + subdivs[k]);
    StyleSegment(b, 1, kAccent);
    b.setTitle("Subdiv " + b.getButtonText());
    b.onClick = [this, k] { SetSubdiv(k); };
    b.setTooltip("Subdiv (perf.subdiv): rate multipliers of the tapped quarter note, x1/4 (whole notes) to "
                 "x8 (thirty-seconds). Scales the CLOCK grid and every synced time.");
    addAndMakeVisible(b);
  }
  const char* const modes[] = {"Free", "Subdiv", "Tempo"};
  for (int k = 0; k < 3; ++k) {
    juce::TextButton& b = time_[static_cast<size_t>(k)];
    b.setButtonText(modes[k]);
    StyleSegment(b, 2, palette::GroupAccent(ParamGroup::Device));
    b.setTitle(juce::String("Time mode ") + modes[k]);
    b.onClick = [this, k] { SetTimeMode(k); };
    b.setTooltip("Time mode (perf.time_mode): what the pedal's Time knob sends. Free: macro.time; Subdiv: "
                 "the Subdiv zone; Tempo: the tempo, Subdiv forced to TAP.");
    addAndMakeVisible(b);
  }

  store_.onClick = [this] {
    if (onStore) onStore();
  };
  addAndMakeVisible(store_);

  settings_.onClick = [this] { ShowSettings(); };
  addAndMakeVisible(settings_);
  Refresh();
}

TempoPanel::~TempoPanel() = default;

juce::Button& TempoPanel::Tap() noexcept { return *tap_; }

void TempoPanel::SetSubdiv(int position) {
  processor_.SubdivParam().SetPlainNotifyingHost(static_cast<float>(position));
  Refresh();
}

void TempoPanel::SetTimeMode(int mode) {
  processor_.TimeModeParam().SetPlainNotifyingHost(static_cast<float>(mode));
  Refresh();
}

juce::PopupMenu TempoPanel::SettingsMenu() {
  const WrapperSettings s = processor_.GetSettings();
  juce::PopupMenu       menu;
  const auto            set = [this](std::function<void(WrapperSettings&)> change) {
    return [this, change] {
      WrapperSettings w = processor_.GetSettings();
      change(w);
      processor_.SetSettings(w);
      Refresh();
    };
  };
  menu.addSectionHeader("Tempo source");
  menu.addItem("Follow the host's tempo and transport", true, s.tempoSource == TempoSource::Host,
               set([](WrapperSettings& w) { w.tempoSource = TempoSource::Host; }));
  menu.addItem("Internal: taps, typed tempos and presets", true, s.tempoSource == TempoSource::Internal,
               set([](WrapperSettings& w) { w.tempoSource = TempoSource::Internal; }));
  menu.addSectionHeader("Preset changes (tempo recall)");
  menu.addItem("Keep the running tempo", true, !s.tempoRecallPreset,
               set([](WrapperSettings& w) { w.tempoRecallPreset = false; }));
  menu.addItem("Play the preset's stored tempo", true, s.tempoRecallPreset,
               set([](WrapperSettings& w) { w.tempoRecallPreset = true; }));
  // T2 (synced times) adds row 86, global.tempo_glide, here: "Tempo jumps glide" (Off by default,
  // clock.md §7.1, D22), a device setting in the session like row 85.
  if (standalone_) {
    menu.addSectionHeader("MIDI input");
    menu.addItem("Receive MIDI clock", true, s.receiveMidiClock,
                 set([](WrapperSettings& w) { w.receiveMidiClock = !w.receiveMidiClock; }));
  }
  return menu;
}

void TempoPanel::ShowSettings() {
  SettingsMenu().showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&settings_));
}

void TempoPanel::Refresh() {
  const BrainscapeProcessor::TempoDisplay d = processor_.GetTempoDisplay();
  const WrapperSettings                   s = processor_.GetSettings();
  // The preset's stored tempo: the open document's, else the preset that plays; and whether it
  // reads tempo at all (UsesTempo, §6.6), once per load.
  CurationSession& session = processor_.Curation();
  const uint32_t   stored  = session.HasDocument() ? session.Stored().state->performance.usPerQuarter
                                                   : processor_.CurrentMode().performance.usPerQuarter;
  bool             uses    = usesTempo_;
  if (processor_.LoadSerial() != loadSerial_) {
    loadSerial_ = processor_.LoadSerial();
    uses        = brainscape::UsesTempo(*processor_.CurrentPreset());
  }
  bool repaintAll = d.nsPerQuarter != shown_.nsPerQuarter || d.source != shown_.source ||
                    d.followingHost != shown_.followingHost || d.locked != shown_.locked ||
                    d.running != shown_.running || stored != storedUs_ || uses != usesTempo_;
  storedUs_  = stored;
  usesTempo_ = uses;
  shown_ = d;
  if (!bpm_.isBeingEdited()) {
    const juce::String text = BpmText(d.nsPerQuarter);
    if (text != bpm_.getText()) bpm_.setText(text, juce::dontSendNotification);
  }
  // Under the host's tempo or a MIDI clock the engine owns the tempo: no typing, no tap (the
  // wrapper drops them under the host; a tap under a running MIDI master is ignored).
  const bool external = d.followingHost || d.source != static_cast<uint8_t>(tempo::ClockSource::Internal);
  bpm_.setEditable(false, !external, false);
  tap_->setEnabled(!d.followingHost);
  tap_->setTooltip(d.followingHost ? juce::String("Tap is off while the host's tempo is followed (Sync menu: Internal "
                                                  "to tap).")
                                   : juce::String("Tap the tempo: two taps set it, up to four are averaged; after a "
                                                  "pause the first tap is the bar's first beat."));
  tap_->Tick();

  // The beat LED: a flash at each new quarter of the grid's position, the bar's first brighter.
  if (d.position >= 0) {
    const int64_t beat = d.position / 24;
    if (beat != lastBeat_) {
      if (lastBeat_ != INT64_MIN) {
        beatGlow_ = 1.f;
        downbeat_ = beat % 4 == 0;
      }
      lastBeat_  = beat;
      repaintAll = true;
    }
  }
  if (beatGlow_ > 0.f) {
    beatGlow_ *= 0.75f;
    if (beatGlow_ < 0.03f) beatGlow_ = 0.f;
    repaint(beatArea_);
  }

  const int position = static_cast<int>(SubdivPositionOf(processor_.SubdivParam().Plain()));
  const int mode     = static_cast<int>(TimeModeOf(processor_.TimeModeParam().Plain()));
  for (int k = 0; k < 6; ++k) {
    juce::TextButton& b = subdiv_[static_cast<size_t>(k)];
    b.setToggleState(k == position, juce::dontSendNotification);
    b.setAlpha(usesTempo_ ? 1.0f : 0.45f);
  }
  for (int k = 0; k < 3; ++k) {
    juce::TextButton& b = time_[static_cast<size_t>(k)];
    b.setToggleState(k == mode, juce::dontSendNotification);
    b.setAlpha(usesTempo_ ? 1.0f : 0.45f);
  }
  store_.setEnabled(session.HasDocument());
  store_.setTooltip(session.HasDocument()
                        ? juce::String("Save the document with the live tempo, Subdiv and time mode as its stored "
                                       "performance (what Exact loads and a recall under Preset play).")
                        : juce::String("Open a document to store its tempo (Save captures the live tempo)."));
  settings_.setButtonText(s.tempoSource == TempoSource::Host ? "Sync: Host" : "Sync: Internal");
  settings_.setTooltip("The tempo source (follow the host's tempo and transport, or the internal tempo), what a "
                       "preset change does to the tempo (keep it, or play the preset's)" +
                       juce::String(standalone_ ? ", and whether MIDI clock is received." : "."));
  setTooltip(usesTempo_ ? juce::String()
                        : juce::String("This preset reads no tempo: no clock source, no synced time. Tap still "
                                       "sets the tempo, which the next preset that uses it plays."));
  if (repaintAll) repaint();
}

void TempoPanel::SetScale(float scale) {
  if (scale == scale_) return;
  scale_ = scale;
  bpm_.setFont(UiFont(24.0f * scale, true));
  tap_->SetScale(scale);
  resized();
  repaint();
}

void TempoPanel::paint(juce::Graphics& g) {
  const auto b = getLocalBounds().toFloat().reduced(0.5f);
  g.setColour(palette::kPanel);
  g.fillRoundedRectangle(b, 8.0f);
  g.setColour(palette::kPanelBorder);
  g.drawRoundedRectangle(b, 8.0f, 1.0f);

  // The title, and under it what the preset makes of tempo.
  auto        t   = titleArea_;
  const float dot = 6.0f * scale_;
  auto        top = t.removeFromTop(t.getHeight() / 2);
  g.setColour(kAccent);
  g.fillEllipse(static_cast<float>(top.getX()), static_cast<float>(top.getBottom()) - dot - 3.0f * scale_, dot, dot);
  g.setColour(palette::kTextDim);
  g.setFont(UiFont(12.0f * scale_, true).withExtraKerningFactor(0.08f));
  g.drawText("TEMPO", top.withTrimmedLeft(Scaled(12, scale_)), juce::Justification::bottomLeft, false);
  g.setFont(UiFont(11.0f * scale_));
  g.setColour(usesTempo_ ? palette::kTextFaint : palette::kWarn.withAlpha(0.85f));
  g.drawText(usesTempo_ ? "in use" : "unused here", t.withTrimmedLeft(Scaled(12, scale_)),
             juce::Justification::topLeft, false);

  // "BPM" under the number's baseline, beside it.
  g.setColour(palette::kTextFaint);
  g.setFont(UiFont(11.0f * scale_, true));
  g.drawText("BPM", bpmArea_.withLeft(bpm_.getRight() + Scaled(3, scale_)), juce::Justification::centredLeft, false);

  // The source badge and the lock dot: HOST (the wrapper follows the host), MIDI (the follower
  // is fitting a clock), INT. The dot: a MIDI clock locked (24 ticks), or the host's transport
  // running.
  const bool   midi   = shown_.source != static_cast<uint8_t>(tempo::ClockSource::Internal);
  const juce::String source = shown_.followingHost ? "HOST" : midi ? "MIDI" : "INT";
  const juce::Colour ink    = shown_.followingHost ? palette::kIce : midi ? palette::kGood : palette::kTextDim;
  auto               badge  = badgeArea_.withSizeKeepingCentre(badgeArea_.getWidth(), Scaled(20, scale_)).toFloat();
  const float        lockD  = 7.0f * scale_;
  auto               pill   = badge.removeFromLeft(badge.getWidth() - lockD - 6.0f * scale_);
  g.setColour(ink.withAlpha(0.14f));
  g.fillRoundedRectangle(pill, 4.0f * scale_);
  g.setColour(ink.withAlpha(0.7f));
  g.drawRoundedRectangle(pill.reduced(0.5f), 4.0f * scale_, 1.0f);
  g.setColour(ink);
  g.setFont(UiFont(11.5f * scale_, true).withExtraKerningFactor(0.08f));
  g.drawText(source, pill, juce::Justification::centred, false);
  const bool lockOn = (midi && shown_.locked) || (shown_.followingHost && shown_.running);
  const auto lock   = juce::Rectangle<float>(lockD, lockD).withCentre({badge.getRight() - lockD * 0.5f, badge.getCentreY()});
  g.setColour(lockOn ? ink : palette::kControlHover);
  g.fillEllipse(lock);

  // The beat LED.
  const float d   = 13.0f * scale_;
  const auto  led = juce::Rectangle<float>(d, d).withCentre(beatArea_.toFloat().getCentre());
  const auto  col = downbeat_ ? kAccent : palette::kLed;
  if (beatGlow_ > 0.f) {
    g.setColour(col.withAlpha(0.35f * beatGlow_));
    g.fillEllipse(led.expanded(4.0f * scale_ * beatGlow_));
  }
  g.setColour(col.withAlpha(0.16f).interpolatedWith(col, beatGlow_));
  g.fillEllipse(led);
  g.setColour(col.withAlpha(0.5f));
  g.drawEllipse(led, 1.0f);

  // Captions.
  g.setColour(palette::kTextDim);
  g.setFont(UiFont(11.5f * scale_, true).withExtraKerningFactor(0.08f));
  g.drawText("SUBDIV", subdivCaption_, juce::Justification::centredRight, false);
  g.drawText("TIME", timeCaption_, juce::Justification::centredRight, false);

  // The stored tempo beside the live one.
  if (showStored_ && !storedArea_.isEmpty()) {
    auto area = storedArea_;
    g.setFont(UiFont(11.5f * scale_, true).withExtraKerningFactor(0.08f));
    g.setColour(palette::kTextDim);
    g.drawText("STORED", area.removeFromTop(area.getHeight() / 2), juce::Justification::bottomLeft, false);
    const bool differs = storedUs_ * 1000u != shown_.nsPerQuarter &&
                         BpmTextUs(storedUs_) != BpmText(shown_.nsPerQuarter);
    g.setColour(differs ? palette::kWarn : palette::kText);
    g.setFont(UiFont(13.5f * scale_, true));
    g.drawText(BpmTextUs(storedUs_), area, juce::Justification::topLeft, false);
  }
}

void TempoPanel::resized() {
  const auto px   = [this](int v) { return Scaled(v, scale_); };
  auto       r    = getLocalBounds().reduced(px(12), px(6));
  const int  g    = px(8);
  const int  rowH = std::min(r.getHeight(), px(30));

  // Widths at this scale, then what the strip leaves: the stored tempo and Store go first when it
  // is narrow, then the fixed parts take their compact widths, then the segments shrink.
  int       titleW = px(80), bpmW = px(92), badgeW = px(60), settingsW = px(100);
  const int beatW = px(20), tapW = px(60), subdivCapW = px(50), timeCapW = px(36);
  const int storedW = px(64), storeW = px(54);
  int       segW = px(40), modeW = px(54);
  const auto fixed = [&] {
    return titleW + bpmW + badgeW + beatW + tapW + 6 * g  // the left, with its gaps
           + subdivCapW + timeCapW + 3 * g                // the captions, with theirs
           + settingsW + g;                               // the menu
  };
  const int storedBlock = storeW + storedW + g + g / 2;
  showStored_           = r.getWidth() >= fixed() + storedBlock + 6 * segW + 3 * modeW;
  if (!showStored_ && r.getWidth() < fixed() + 6 * segW + 3 * modeW) {
    titleW    = px(70);
    bpmW      = px(84);
    badgeW    = px(54);
    settingsW = px(88);
  }
  const int room = r.getWidth() - fixed() - (showStored_ ? storedBlock : 0);
  if (room < 6 * segW + 3 * modeW) {
    segW  = std::max(px(28), room * 40 / (6 * 40 + 3 * 54));
    modeW = std::max(px(40), (room - 6 * segW) / 3);
  }

  const auto centred = [&](juce::Rectangle<int> a) { return a.withSizeKeepingCentre(a.getWidth(), rowH); };
  titleArea_ = r.removeFromLeft(titleW);
  r.removeFromLeft(g);
  bpmArea_       = r.removeFromLeft(bpmW);
  const int bpmL = bpmArea_.getWidth() - px(30);  // the number; "BPM" is painted beside it
  bpm_.setBounds(bpmArea_.withWidth(bpmL).withSizeKeepingCentre(bpmL, px(34)));
  r.removeFromLeft(g);
  badgeArea_ = r.removeFromLeft(badgeW);
  r.removeFromLeft(g);
  beatArea_ = r.removeFromLeft(beatW);
  r.removeFromLeft(g);
  tap_->setBounds(centred(r.removeFromLeft(tapW)));
  r.removeFromLeft(2 * g);

  settings_.setBounds(centred(r.removeFromRight(settingsW)));
  r.removeFromRight(g);
  storedArea_ = {};
  store_.setVisible(showStored_);
  if (showStored_) {
    store_.setBounds(centred(r.removeFromRight(storeW)));
    r.removeFromRight(g / 2);
    storedArea_ = r.removeFromRight(storedW);
    r.removeFromRight(g);
  }

  subdivCaption_ = r.removeFromLeft(subdivCapW);
  r.removeFromLeft(g / 2);
  for (auto& b : subdiv_) b.setBounds(centred(r.removeFromLeft(segW)));
  r.removeFromLeft(2 * g);
  timeCaption_ = r.removeFromLeft(timeCapW);
  r.removeFromLeft(g / 2);
  for (auto& b : time_) b.setBounds(centred(r.removeFromLeft(modeW)));
  for (size_t k = 0; k < subdiv_.size(); ++k) {
    subdiv_[k].setConnectedEdges((k > 0 ? juce::Button::ConnectedOnLeft : 0) |
                                 (k + 1 < subdiv_.size() ? juce::Button::ConnectedOnRight : 0));
  }
  for (size_t k = 0; k < time_.size(); ++k) {
    time_[k].setConnectedEdges((k > 0 ? juce::Button::ConnectedOnLeft : 0) |
                               (k + 1 < time_.size() ? juce::Button::ConnectedOnRight : 0));
  }
}

}  // namespace brainscape::plugin
