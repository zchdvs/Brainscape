#include "PluginEditor.h"

namespace brainscape::plugin {

namespace {

constexpr int kHeaderHeight = 76;
constexpr int kStatusHeight = 28;
constexpr int kGap          = 10;

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

}  // namespace

BrainscapeEditor::BrainscapeEditor(BrainscapeProcessor& owner)
    : juce::AudioProcessorEditor(owner), processor_(owner), testPanel_(owner) {
  setLookAndFeel(&laf_);
  for (size_t g = 0; g < kNumParamGroups; ++g) {
    const auto group = static_cast<ParamGroup>(g);
    sections_[g]     = std::make_unique<SectionPanel>(GroupTitle(group), palette::GroupAccent(group));
    addAndMakeVisible(*sections_[g]);
  }
  for (const ParamDescriptor& d : kParamTable) {
    const ParamDisplay* m = FindParamDisplay(d.id);
    knobs_.push_back(&sections_[static_cast<size_t>(m->group)]->AddKnob(processor_.Param(d.id)));
  }

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
  outMeter_.setTooltip("Output after the output level.");

  setResizable(true, true);
  setResizeLimits(kMinWidth, kMinHeight, 2600, 1700);
  setSize(kDefaultWidth, kDefaultHeight);
  RefreshNow();
  startTimerHz(30);
}

BrainscapeEditor::~BrainscapeEditor() {
  stopTimer();
  setLookAndFeel(nullptr);
}

void BrainscapeEditor::ToggleFreeze() {
  FreezeParam& f = processor_.Freeze();
  f.beginChangeGesture();
  f.setValueNotifyingHost(f.get() ? 0.0f : 1.0f);
  f.endChangeGesture();
  RefreshNow();
}

void BrainscapeEditor::RefreshNow() {
  for (ParamKnob* k : knobs_) k->Refresh();
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

  // Wordmark: a small grain cloud, the name and what this window is for.
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
  logo.removeFromLeft(Scaled(12, scale_));
  auto text = logo.withSizeKeepingCentre(logo.getWidth(), Scaled(44, scale_));
  g.setColour(palette::kText);
  g.setFont(UiFont(23.0f * scale_, true).withExtraKerningFactor(0.22f));
  g.drawText("BRAINSCAPE", text.removeFromTop(Scaled(26, scale_)), juce::Justification::bottomLeft, false);
  g.setColour(palette::kTextDim);
  g.setFont(UiFont(13.0f * scale_));
  g.drawText(juce::String::fromUTF8("granular delay \xc2\xb7 engine test bench"), text,
             juce::Justification::topLeft, false);
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

  auto r  = getLocalBounds();
  header_ = r.removeFromTop(px(kHeaderHeight));
  status_.setBounds(r.removeFromBottom(px(kStatusHeight)));

  auto h = header_.reduced(px(16), px(12));
  freeze_.setBounds(h.removeFromRight(px(148)));
  h.removeFromRight(px(10));
  trigger_.setBounds(h.removeFromRight(px(110)));
  h.removeFromRight(px(22));
  auto meters = h.removeFromRight(juce::jlimit(px(160), px(260), h.getWidth() / 3));
  inMeter_.setBounds(meters.removeFromTop(meters.getHeight() / 2).reduced(0, px(3)));
  outMeter_.setBounds(meters.reduced(0, px(3)));
  h.removeFromRight(px(18));
  led_.setBounds(h.removeFromRight(px(84)).withSizeKeepingCentre(px(84), px(34)));

  r              = r.reduced(12, kGap);
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
