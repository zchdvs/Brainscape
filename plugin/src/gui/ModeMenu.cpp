#include "ModeMenu.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

#include "../Curation.h"
#include "../FactoryModes.h"
#include "BrainscapeLookAndFeel.h"
#include "CurationViews.h"

namespace brainscape::plugin {

namespace {

const juce::String kDot = juce::String::fromUTF8("\xc2\xb7");  // ·

juce::String Upper(const char* s) { return juce::String(s).toUpperCase(); }

// The caption's second part for a factory package: its family, after "RESERVE" for a reserve.
juce::String FamilyText(const FactoryPackage& f) {
  return (f.reserve ? "RESERVE  " + kDot + "  " : juce::String()) + Upper(FamilyName(f.family));
}

juce::Font CaptionFont(float scale) { return UiFont(10.5f * scale, true).withExtraKerningFactor(0.14f); }
juce::Font NameFont(float scale) { return UiFont(17.0f * scale, true); }
const juce::String kHead = "MODE";
const juce::String kSep  = "  " + kDot + "  ";

// The button's chrome around its text (paintButton): the side insets, the stripe and its gap,
// the chevron and its gap.
int Chrome(float scale) { return Scaled(2 * 12 + 4 + 10 + 12 + 8 + 2, scale); }

bool IsDefaultMode(const ModeBlob& mode) {
  static const ModeBlob kDefault{};
  return std::memcmp(&mode, &kDefault, sizeof(ModeBlob)) == 0;
}

}  // namespace

ModeMenu::ModeMenu(BrainscapeProcessor& processor)
    : Scalable<juce::Button>("Modes"), processor_(processor), accent_(palette::kTextFaint) {
  setTitle("Modes");
  setTooltip("The factory modes, by family, and the reserves: choosing one plays it as opening its .bsp does "
             "(a Spillover load with Trails while audio runs) and opens it in the curation view, asking first "
             "when the open document has unsaved edits.");
  Refresh();
}

void ModeMenu::Refresh() {
  const PresetSource source = processor_.CurrentSource();
  juce::String       name, caption = "MODE", family;
  juce::Colour       accent = palette::kTextFaint;
  bool               named  = true;
  if (source.factory >= 0 && static_cast<size_t>(source.factory) < FactoryCount()) {
    const FactoryPackage& f = Factory(static_cast<size_t>(source.factory));
    name                    = juce::String::fromUTF8(f.name);
    family                  = FamilyText(f);
    accent                  = palette::FamilyAccent(f.family);
  } else if (source.name.isNotEmpty()) {  // a document file
    name   = source.name;
    family = "FILE" + (source.family != PresetFamily::None ? kSep + Upper(FamilyName(source.family)) : juce::String());
    accent = palette::FamilyAccent(source.family);
  } else {
    named = false;
    name  = IsDefaultMode(processor_.CurrentMode().mode) ? "Default mode" : "Unnamed mode";
  }
  if (family.isNotEmpty()) caption << kSep << family;
  if (name == name_ && caption == caption_ && accent == accent_ && named == named_) return;
  name_    = name;
  caption_ = caption;
  family_  = family;
  accent_  = accent;
  named_   = named;
  repaint();
}

int ModeMenu::PreferredWidth() const {
  const juce::Font caption = CaptionFont(scale_), name = NameFont(scale_);
  float            widest  = juce::GlyphArrangement::getStringWidth(caption, kHead + kSep + "FILE" + kSep + "REVERIE");
  for (size_t i = 0; i < FactoryCount(); ++i) {
    const FactoryPackage& f = Factory(i);
    widest = std::max({widest, juce::GlyphArrangement::getStringWidth(caption, kHead + kSep + FamilyText(f)),
                       juce::GlyphArrangement::getStringWidth(name, juce::String::fromUTF8(f.name))});
  }
  const int w = static_cast<int>(std::ceil(widest)) + Chrome(scale_);
  return juce::jlimit(Scaled(200, scale_), Scaled(320, scale_), w);
}

juce::PopupMenu ModeMenu::BuildMenu() const {
  const int       current = processor_.CurrentSource().factory;
  juce::PopupMenu menu, reserves;
  bool            reserveTicked = false;
  bool            first         = true;
  PresetFamily    family        = PresetFamily::None;
  for (const size_t i : FactoryMenuOrder()) {
    const FactoryPackage&  f = Factory(i);
    juce::PopupMenu::Item item(juce::String::fromUTF8(f.name));
    item.itemID   = kFactoryItem + static_cast<int>(i);
    item.isTicked = current == static_cast<int>(i);
    if (f.reserve) {
      item.shortcutKeyDescription = FamilyName(f.family);
      reserveTicked = reserveTicked || item.isTicked;
      reserves.addItem(std::move(item));
      continue;
    }
    if (first || f.family != family) {
      menu.addSectionHeader(Upper(FamilyName(f.family)));
      family = f.family;
      first  = false;
    }
    menu.addItem(std::move(item));
  }
  if (reserves.getNumItems() > 0) {
    menu.addSeparator();
    juce::PopupMenu::Item sub("Reserves");
    sub.subMenu  = std::make_unique<juce::PopupMenu>(std::move(reserves));
    sub.isTicked = reserveTicked;
    menu.addItem(std::move(sub));
  }
  menu.addSeparator();
  menu.addItem(kOpenItem, "Open a document or package...", onOpenFile != nullptr);
  return menu;
}

bool ModeMenu::Choose(int itemId) {
  if (itemId == kOpenItem) {
    if (onOpenFile) onOpenFile();
    return true;
  }
  const int index = itemId - kFactoryItem;
  if (index < 0 || static_cast<size_t>(index) >= FactoryCount()) return false;
  const CurationSession& session = processor_.Curation();
  if (!session.HasDocument() || !session.Dirty()) {
    Open(static_cast<size_t>(index));
    return true;
  }
  // Unsaved edits: they go only when the curator says so.
  std::function<void()> discard = [safe = juce::Component::SafePointer<ModeMenu>(this), index] {
    if (safe != nullptr) safe->Open(static_cast<size_t>(index));
  };
  const juce::String mode = juce::String::fromUTF8(Factory(static_cast<size_t>(index)).name);
  if (onAskDiscard) {
    onAskDiscard(mode, std::move(discard));
  } else {
    askBox_ = AskDiscard(session, mode, this, std::move(discard));
  }
  return true;
}

void ModeMenu::Open(size_t index) {
  CurationSession& session = processor_.Curation();
  juce::String     error;
  const bool       opened = session.OpenFactory(index, &error);
  Refresh();
  if (onChosen) onChosen(opened, opened ? session.LastMessage() : error);
}

void ModeMenu::clicked() {
  BuildMenu().showMenuAsync(juce::PopupMenu::Options()
                                .withTargetComponent(this)
                                .withMinimumWidth(getWidth())
                                .withStandardItemHeight(Scaled(24, scale_)),
                            [safe = juce::Component::SafePointer<ModeMenu>(this)](int result) {
                              if (safe != nullptr && result != 0) safe->Choose(result);
                            });
}

void ModeMenu::paintButton(juce::Graphics& g, bool highlighted, bool down) {
  using namespace palette;
  const auto b = getLocalBounds().toFloat().reduced(1.0f);
  g.setColour(down ? kControlHover.brighter(0.06f) : highlighted ? kControlHover : kControl);
  g.fillRoundedRectangle(b, 9.0f);
  g.setColour(highlighted || down ? accent_.withAlpha(0.85f) : kPanelBorder.brighter(0.2f));
  g.drawRoundedRectangle(b.reduced(0.5f), 9.0f, 1.0f);

  auto r = b.reduced(12.0f * scale_, 0.0f);
  // The family's stripe at the left.
  const float stripeW = 4.0f * scale_;
  g.setColour(named_ ? accent_ : kTextFaint.withAlpha(0.6f));
  g.fillRoundedRectangle(r.removeFromLeft(stripeW).withSizeKeepingCentre(stripeW, b.getHeight() * 0.56f),
                         stripeW * 0.5f);
  r.removeFromLeft(10.0f * scale_);

  // A chevron at the right: the menu.
  const auto  chevron = r.removeFromRight(12.0f * scale_);
  const float cx = chevron.getCentreX(), cy = chevron.getCentreY(), w = 9.0f * scale_, h = 5.0f * scale_;
  juce::Path  p;
  p.startNewSubPath(cx - w * 0.5f, cy - h * 0.5f);
  p.lineTo(cx, cy + h * 0.5f);
  p.lineTo(cx + w * 0.5f, cy - h * 0.5f);
  g.setColour(highlighted || down ? kText : kTextDim);
  g.strokePath(p, juce::PathStrokeType(1.6f * scale_, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
  r.removeFromRight(8.0f * scale_);

  // The caption ("MODE · RESERVE · REVERIE") above the name.
  const float captionH = 14.0f * scale_;
  const float nameH    = std::min(22.0f * scale_, r.getHeight() - captionH);
  auto        lines    = r.withSizeKeepingCentre(r.getWidth(), captionH + nameH);
  auto        cap      = lines.removeFromTop(captionH);
  const auto  capFont  = CaptionFont(scale_);
  g.setFont(capFont);
  g.setColour(kTextFaint);
  g.drawText(kHead, cap, juce::Justification::centredLeft, false);
  if (family_.isNotEmpty()) {
    const float headW = juce::GlyphArrangement::getStringWidth(capFont, kHead + kSep);
    g.drawText(kSep, cap.withTrimmedLeft(juce::GlyphArrangement::getStringWidth(capFont, kHead)),
               juce::Justification::centredLeft, false);
    g.setColour(accent_);
    g.drawText(family_, cap.withTrimmedLeft(headW), juce::Justification::centredLeft, true);
  }
  g.setFont(NameFont(scale_));
  g.setColour(named_ ? kText : kTextDim);
  g.drawText(name_, lines, juce::Justification::centredLeft, true);
}

}  // namespace brainscape::plugin
