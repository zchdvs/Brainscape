#include "PluginEditor.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <string>

#include "Curation.h"

namespace brainscape::plugin {

namespace {

constexpr int kHeaderHeight = 76;
constexpr int kTabHeight    = 32;
constexpr int kStatusHeight = 28;
constexpr int kGap          = 10;

const juce::String kDot = juce::String::fromUTF8("\xc2\xb7");  // ·

struct Slot {
  juce::Component* component;
  int              units;  // knob columns
};

void LayoutRow(juce::Rectangle<int> row, std::initializer_list<Slot> slots) {
  int units = 0;
  for (const Slot& s : slots) units += s.units;
  const int avail = row.getWidth() - kGap * (static_cast<int>(slots.size()) - 1);
  size_t    i     = 0;
  for (const Slot& s : slots) {
    const bool last = ++i == slots.size();
    const int  w    = last ? row.getWidth() : avail * s.units / units;
    s.component->setBounds(row.removeFromLeft(w));
    row.removeFromLeft(kGap);
  }
}

void StyleTab(juce::TextButton& b) {
  b.setClickingTogglesState(false);
  b.setColour(juce::TextButton::buttonColourId, palette::kHeader);
  b.setColour(juce::TextButton::buttonOnColourId, palette::kControlHover);
  b.setColour(juce::TextButton::textColourOffId, palette::kTextDim);
  b.setColour(juce::TextButton::textColourOnId, palette::kText);
}

std::string Fmt(float v) {  // a macro target's range end, for tooltips
  return juce::String(v, 3).trimCharactersAtEnd("0").trimCharactersAtEnd(".").toStdString();
}

// The wordmark's two lines (paint), at a scale.
juce::Font WordmarkFont(float scale) { return UiFont(23.0f * scale, true).withExtraKerningFactor(0.22f); }
juce::Font TaglineFont(float scale) { return UiFont(13.0f * scale); }
const juce::String kWordmark = "BRAINSCAPE";
const juce::String kTagline  = "granular delay " + kDot + " curation bench";

int WordmarkTextWidth(float scale) {
  const float w = std::max(juce::GlyphArrangement::getStringWidth(WordmarkFont(scale), kWordmark),
                           juce::GlyphArrangement::getStringWidth(TaglineFont(scale), kTagline));
  return static_cast<int>(std::ceil(w)) + Scaled(4, scale);
}

}  // namespace

BrainscapeEditor::BrainscapeEditor(BrainscapeProcessor& owner)
    : juce::AudioProcessorEditor(owner),
      processor_(owner),
      modes_(owner),
      macros_(owner),
      document_(owner),
      findings_(owner.Curation()),
      tempo_(owner),
      testPanel_(owner) {
  setLookAndFeel(&laf_);
  for (size_t g = 0; g < kNumParamGroups; ++g) {
    const auto group = static_cast<ParamGroup>(g);
    sections_[g]     = std::make_unique<SectionPanel>(GroupTitle(group), palette::GroupAccent(group));
    addChildComponent(*sections_[g]);
  }
  for (size_t i = 0; i < kNumLeafParams; ++i) {  // the registered rows
    const ParamDisplay* m    = FindParamDisplay(LeafId(i));
    ParamKnob&          knob = sections_[static_cast<size_t>(m->group)]->AddKnob(processor_.Param(LeafId(i)));
    knob.onPopup             = [this, id = LeafId(i)] { ShowLeafMenu(id); };
    knobs_.push_back(&knob);
  }

  for (auto* t : {&pedalTab_, &leavesTab_}) {
    StyleTab(*t);
    addAndMakeVisible(*t);
  }
  pedalTab_.setTooltip("The pedal: its eight knobs with pickup, and the open preset document.");
  leavesTab_.setTooltip("Every engine leaf as a knob (Advanced), marked with the macros that move it.");
  pedalTab_.onClick  = [this] { SetView(View::Pedal); };
  leavesTab_.onClick = [this] { SetView(View::Leaves); };
  tabNote_.setFont(UiFont(13.0f));
  tabNote_.setJustificationType(juce::Justification::centredRight);
  tabNote_.setMinimumHorizontalScale(0.7f);
  tabNote_.setInterceptsMouseClicks(false, false);
  addAndMakeVisible(tabNote_);
  addChildComponent(macros_);
  addChildComponent(document_);
  addChildComponent(findings_);
  addChildComponent(tempo_);
  tempo_.onStore = [this] {
    document_.Save();
    RefreshNow();
  };
  addAndMakeVisible(modes_);
  modes_.onChosen = [this](bool opened, const juce::String& message) {
    document_.Note(message, opened ? palette::kText : palette::kBad);
    RefreshNow();
  };
  modes_.onOpenFile = [this] { document_.ChooseOpen(); };

  freeze_.onClick = [this] { ToggleFreeze(); };
  freeze_.setTooltip("Freeze pins the grain position: the grains keep replaying the captured "
                     "moment while the ring keeps recording (host-automatable).");
  trigger_.onClick = [this] {
    processor_.TriggerFromUi();
    trigger_.Flash();
  };
  trigger_.setTooltip("Fires one grain now, as a footswitch or MIDI note-on does.");
  for (juce::Component* c : std::initializer_list<juce::Component*>{
           &led_, &inMeter_, &outMeter_, &trigger_, &freeze_, &testPanel_, &status_}) {
    addAndMakeVisible(*c);
  }
  inMeter_.setTooltip("Engine input after the input level and the input functions.");
  outMeter_.setTooltip("Output after the output level (and the A/B level match, while it trims).");

  setWantsKeyboardFocus(true);
  setResizable(true, true);
  setResizeLimits(kMinWidth, kMinHeight, 2600, 1700);
  setSize(kDefaultWidth, kDefaultHeight);
  SetView(processor_.EditorView() == static_cast<int>(View::Leaves) ? View::Leaves : View::Pedal);
  RefreshNow();
  startTimerHz(30);
}

BrainscapeEditor::~BrainscapeEditor() {
  stopTimer();
  setLookAndFeel(nullptr);
}

void BrainscapeEditor::SetView(View view) {
  view_ = view;
  processor_.SetEditorView(static_cast<int>(view));
  const bool pedal = view == View::Pedal;
  pedalTab_.setToggleState(pedal, juce::dontSendNotification);
  leavesTab_.setToggleState(!pedal, juce::dontSendNotification);
  for (auto& s : sections_) {
    const bool shown = !pedal && !s->Knobs().empty();
    s->setVisible(shown);
  }
  macros_.setVisible(pedal);
  tempo_.setVisible(pedal);
  document_.setVisible(pedal);
  findings_.setVisible(pedal);
  resized();
  repaint();
}

bool BrainscapeEditor::keyPressed(const juce::KeyPress& key) {
  const auto c = juce::CharacterFunctions::toLowerCase(key.getTextCharacter());
  if (key.getModifiers().isCommandDown()) {
    if (key.getKeyCode() == 'S' || c == 's') {
      document_.Save();
      return true;
    }
    if (key.getKeyCode() == 'O' || c == 'o') {
      document_.ChooseOpen();
      return true;
    }
    return false;
  }
  if (c == 'b') {
    document_.ToggleSide();
    RefreshNow();
    return true;
  }
  return false;
}

void BrainscapeEditor::ToggleFreeze() {
  FreezeParam& f = processor_.Freeze();
  f.beginChangeGesture();
  f.setValueNotifyingHost(f.get() ? 0.0f : 1.0f);
  f.endChangeGesture();
  RefreshNow();
}

// The Leaves view's marks: which macros move each leaf, whether it is detached from them, and
// whether Save will derive it (it is off its macro's value at the macro's position).
void BrainscapeEditor::RefreshMarks() {
  CurationSession&      s = processor_.Curation();
  std::set<std::string> pending;
  for (const std::string& line : s.PendingDerives()) pending.insert(line.substr(0, line.find(':')));
  const BrainscapeProcessor::TempoDisplay tempoNow = processor_.GetTempoDisplay();
  const BrainscapeParam*                  sync     = processor_.FindHostParam(ParamId::DelaySync);
  const uint8_t echo = sync != nullptr ? DelaySyncCode(sync->Plain()) : 0u;
  const uint8_t base = processor_.CurrentMode().mode.layers[0].baseSync;
  for (ParamKnob* k : knobs_) {
    const ParamId id = k->Attachment().Param().Id();
    std::vector<juce::Colour> marks;
    juce::String              tip;
    if (s.HasDocument()) {
      for (const auto& t : s.TargetsOf(id)) {
        marks.push_back(MacroAccent(t.macro));
        tip << "Moved by " << s.MacroName(t.macro) << ": " << Fmt(t.lo) << " to " << Fmt(t.hi);
        if (t.curve != 1.0f) tip << ", curve " << Fmt(t.curve);
        if (t.inLo != 0.0f || t.inHi != 1.0f) tip << ", over " << Fmt(t.inLo) << "-" << Fmt(t.inHi);
        tip << "\n";
      }
    }
    // Synced times (clock.md §5.4): row 63 shows what it plays; post.delay.time_ms waits while row
    // 63 is set, and base_ms while layer 0's base_sync is, each dimmed.
    bool dim = false;
    if (id == ParamId::DelaySync) {
      const uint8_t code = DelaySyncCode(k->Attachment().Param().Plain());
      if (code != 0u) tip << "Plays " << SyncedTimeText(tempoNow, tempo::SyncTarget::PostDelay, code) << "\n";
    } else if (id == ParamId::DelayTimeMs && echo != 0u) {
      dim = true;
      tip << "Waits: post.delay.sync plays " << SyncedTimeText(tempoNow, tempo::SyncTarget::PostDelay, echo) << "\n";
    } else if (id == ParamId::DelayMs && base != 0u) {
      dim = true;
      tip << "Waits: base_sync plays " << SyncedTimeText(tempoNow, tempo::SyncTarget::BaseDelay, base) << "\n";
    }
    k->setAlpha(dim ? 0.45f : 1.0f);
    const bool detached = !marks.empty() && s.IsDetached(id);
    const bool due      = pending.count(FindParam(id)->name) != 0;
    if (!marks.empty()) {
      tip << (detached ? "Detached: Save keeps this value (editor.detached)."
                       : due ? "Off its macro's value: Save derives it from the knob, unless detached."
                             : "Save derives it from the knob's position.")
          << " Right-click to detach, attach or solve.";
    }
    k->SetMarks(marks, detached, due, tip);
  }
}

void BrainscapeEditor::ShowLeafMenu(ParamId leaf) {
  CurationSession& s = processor_.Curation();
  if (!s.HasDocument()) return;
  const auto targets = s.TargetsOf(leaf);
  if (targets.empty()) return;
  juce::PopupMenu menu;
  menu.addSectionHeader(FindParamDisplay(leaf)->title);
  const bool detached = s.IsDetached(leaf);
  const bool a        = s.GetSide() == CurationSession::Side::Stored;
  menu.addItem(1, detached ? "Attach to its macros (Save derives it)" : "Detach from its macros (Save keeps this value)",
               !a);
  int item = 10;
  for (const auto& t : targets) {  // a target whose range is one value says nothing of a position
    menu.addItem(item++, "Solve " + s.MacroName(t.macro) + "'s position from this leaf",
                 !a && !detached && t.lo != t.hi);
  }
  menu.showMenuAsync(juce::PopupMenu::Options(),
                     [safe = juce::Component::SafePointer<BrainscapeEditor>(this), leaf, targets, detached](int r) {
                       if (safe == nullptr || r == 0) return;
                       CurationSession& session = safe->processor_.Curation();
                       if (r == 1) {
                         session.SetDetached(leaf, !detached);
                       } else if (r >= 10 && static_cast<size_t>(r - 10) < targets.size()) {
                         session.SolvePositions(targets[static_cast<size_t>(r - 10)].macro, leaf);
                       }
                       safe->RefreshNow();
                     });
}

void BrainscapeEditor::RefreshNow() {
  CurationSession& s = processor_.Curation();
  s.Refresh();
  const bool a = s.HasDocument() && s.GetSide() == CurationSession::Side::Stored;
  for (ParamKnob* k : knobs_) k->Refresh();
  for (auto& section : sections_) section->setEnabled(!a);
  RefreshMarks();
  macros_.Refresh();
  document_.Refresh();
  findings_.Refresh();
  tempo_.Refresh();
  modes_.Refresh();
  juce::String note;
  if (s.HasDocument()) {
    note << juce::String::fromUTF8(s.Stored().name.c_str()) << "   " << kDot << "   " << s.SourceLabel()
         << (a ? "   " + kDot + "   A: stored version" : juce::String())
         << (s.Dirty() ? "   " + kDot + "   unsaved" : juce::String());
  } else {
    note = "No document " + kDot + " " + (modes_.Named() ? modes_.Name() + " plays" : "the " + modes_.Name().toLowerCase());
  }
  tabNote_.setText(note, juce::dontSendNotification);
  tabNote_.setColour(juce::Label::textColourId, a ? palette::kWarn : s.Dirty() ? palette::kWarn : palette::kTextDim);
  freeze_.setToggleState(processor_.Freeze().get(), juce::dontSendNotification);
  led_.Push(processor_.ConsumeOnsets());
  inMeter_.Push(processor_.ConsumeInputPeak());
  outMeter_.Push(processor_.ConsumeOutputPeak());
  trigger_.Tick();
  status_.Set(processor_.GetStatus(), processor_.GetSettings());
  testPanel_.Refresh();
}

void BrainscapeEditor::paint(juce::Graphics& g) {
  g.fillAll(palette::kBackground);
  g.setGradientFill(juce::ColourGradient(palette::kHeader.brighter(0.04f), 0.f, 0.f, palette::kHeader, 0.f,
                                         static_cast<float>(header_.getBottom()), false));
  g.fillRect(header_);
  g.setColour(palette::kPanelBorder);
  g.fillRect(header_.withTop(header_.getBottom() - 1));

  // Wordmark: a small grain cloud, the name and what this window is for (the name and the tagline
  // only when the header has room for them beside the Modes menu).
  auto       logo     = header_.reduced(Scaled(18, scale_), 0);
  const int  iconSide = Scaled(32, scale_);
  const auto icon     = logo.removeFromLeft(Scaled(36, scale_)).withSizeKeepingCentre(iconSide, iconSide).toFloat();
  static constexpr float kDots[][3] = {{0.18f, 0.30f, 5.0f}, {0.42f, 0.16f, 4.0f}, {0.70f, 0.28f, 6.0f},
                                       {0.30f, 0.62f, 6.5f}, {0.58f, 0.52f, 4.5f}, {0.84f, 0.62f, 3.5f},
                                       {0.50f, 0.86f, 4.0f}, {0.14f, 0.84f, 3.0f}};
  for (size_t k = 0; k < sizeof kDots / sizeof kDots[0]; ++k) {
    const auto group = static_cast<ParamGroup>(k % kNumParamGroups);
    g.setColour(palette::GroupAccent(group).withAlpha(0.85f));
    const float d = kDots[k][2] * scale_;
    g.fillEllipse(icon.getX() + icon.getWidth() * kDots[k][0] - d * 0.5f,
                  icon.getY() + icon.getHeight() * kDots[k][1] - d * 0.5f, d, d);
  }
  if (!wordmarkText_) return;
  logo.removeFromLeft(Scaled(12, scale_));
  auto text = logo.withSizeKeepingCentre(logo.getWidth(), Scaled(44, scale_));
  g.setColour(palette::kText);
  g.setFont(WordmarkFont(scale_));
  g.drawText(kWordmark, text.removeFromTop(Scaled(26, scale_)), juce::Justification::bottomLeft, false);
  g.setColour(palette::kTextDim);
  g.setFont(TaglineFont(scale_));
  g.drawText(kTagline, text, juce::Justification::topLeft, false);
}

void BrainscapeEditor::resized() {
  // Everything reflows with the window; above the default size, it all grows too.
  scale_ = juce::jlimit(1.0f, 1.6f,
                        std::min(static_cast<float>(getWidth()) / kDefaultWidth,
                                 static_cast<float>(getHeight()) / kDefaultHeight));
  const auto px = [this](int v) { return Scaled(v, scale_); };
  for (auto& s : sections_) s->SetScale(scale_);
  testPanel_.SetScale(scale_);
  led_.SetScale(scale_);
  inMeter_.SetScale(scale_);
  outMeter_.SetScale(scale_);
  trigger_.SetScale(scale_);
  freeze_.SetScale(scale_);
  status_.SetScale(scale_);
  macros_.SetScale(scale_);
  modes_.SetScale(scale_);
  document_.SetScale(scale_);
  findings_.SetScale(scale_);
  tempo_.SetScale(scale_);
  tabNote_.setFont(UiFont(13.0f * scale_));

  auto r  = getLocalBounds();
  header_ = r.removeFromTop(px(kHeaderHeight));
  status_.setBounds(r.removeFromBottom(px(kStatusHeight)));

  // The header, left to right: the wordmark, the Modes menu, then from the right freeze, trigger,
  // the meters and the onset LED. The menu takes the width its longest entry needs; the meters
  // what is left, up to their own; on a narrow window the wordmark keeps only its grain cloud.
  auto h = header_.reduced(px(16), px(12));
  freeze_.setBounds(h.removeFromRight(px(148)));
  h.removeFromRight(px(10));
  trigger_.setBounds(h.removeFromRight(px(110)));
  h.removeFromRight(px(22));
  const int gap     = px(18);
  const int ledW    = px(84);
  const int menuW   = modes_.PreferredWidth();
  const int cloudW  = px(2) + px(36);  // paint: the cloud from px(18), the header from px(16)
  const int textW   = px(12) + WordmarkTextWidth(scale_);
  wordmarkText_     = h.getWidth() - cloudW - textW - 3 * gap - ledW - menuW >= px(150);
  h.removeFromLeft(cloudW + (wordmarkText_ ? textW : 0) + gap);
  const int menu    = std::min(menuW, h.getWidth());
  modes_.setBounds(h.removeFromLeft(menu).withSizeKeepingCentre(menu, px(46)));
  h.removeFromLeft(gap);
  auto meters = h.removeFromRight(juce::jlimit(0, px(260), h.getWidth() - ledW - gap));
  inMeter_.setBounds(meters.removeFromTop(meters.getHeight() / 2).reduced(0, px(3)));
  outMeter_.setBounds(meters.reduced(0, px(3)));
  h.removeFromRight(gap);
  led_.setBounds(h.removeFromRight(ledW).withSizeKeepingCentre(ledW, px(34)));

  r        = r.reduced(12, kGap);
  auto tab = r.removeFromTop(px(kTabHeight) - 4);
  r.removeFromTop(kGap - 2);
  pedalTab_.setBounds(tab.removeFromLeft(px(96)));
  tab.removeFromLeft(4);
  leavesTab_.setBounds(tab.removeFromLeft(px(96)));
  tab.removeFromLeft(12);
  tabNote_.setBounds(tab);

  if (view_ == View::Pedal) {
    // The macro knobs, the tempo strip under them, then the document, the findings and the test
    // input.
    const int stripH = px(64);
    auto      row1   = r.removeFromTop(juce::jmax(px(196), (r.getHeight() - stripH - kGap) * 41 / 100));
    r.removeFromTop(kGap);
    macros_.setBounds(row1);
    tempo_.setBounds(r.removeFromTop(stripH));
    r.removeFromTop(kGap);
    LayoutRow(r, {{&document_, 9}, {&findings_, 7}, {&testPanel_, 10}});
    return;
  }
  const int rowH = (r.getHeight() - 2 * kGap) / 3;
  auto      row1 = r.removeFromTop(rowH);
  r.removeFromTop(kGap);
  auto row2 = r.removeFromTop(rowH);
  r.removeFromTop(kGap);
  auto row3 = r;

  // Row 2 is the post chain in signal order (mod -> delay -> reverb -> filter).
  auto section = [this](ParamGroup g) { return sections_[static_cast<size_t>(g)].get(); };
  LayoutRow(row1, {{section(ParamGroup::GrainDelay), 4}, {section(ParamGroup::Grains), 5},
                   {section(ParamGroup::Pitch), 3}});
  LayoutRow(row2, {{section(ParamGroup::Mod), 2}, {section(ParamGroup::PostDelay), 3},
                   {section(ParamGroup::Reverb), 2}, {section(ParamGroup::Filter), 3}});
  LayoutRow(row3, {{section(ParamGroup::Window), 3}, {section(ParamGroup::Triggers), 3},
                   {&testPanel_, 5}});
}

}  // namespace brainscape::plugin
