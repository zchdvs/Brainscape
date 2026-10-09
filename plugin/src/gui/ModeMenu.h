#pragma once
#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

#include "../PluginProcessor.h"
#include "Widgets.h"

namespace brainscape::plugin {

// The header's Modes menu: the mode that plays, by name and family, and on click the factory set
// built into the plugin (FactoryModes.h), the set by family in the README's order (echoic,
// reverie, recall, misfire), the reserves in a submenu, then "Open a document or package...".
// Choosing a mode opens its package in the curation session exactly as opening its .bsp does
// (CurationSession::OpenFactory): the processor plays it as a state restore does, a Spillover
// load with Trails while audio runs (Exact before anything has played), the knobs wait for
// pickup, and the curation views show its document. Unsaved edits in the open document are
// dropped only after the curator confirms (AskDiscard).
class ModeMenu final : public Scalable<juce::Button> {
 public:
  explicit ModeMenu(BrainscapeProcessor& processor);

  // One GUI tick: what plays, from the processor's source (BrainscapeProcessor::CurrentSource).
  void Refresh();
  // The menu the button shows: a factory mode's item id is kFactoryItem + its index.
  juce::PopupMenu BuildMenu() const;
  // An item chosen from the menu: a factory mode opens and plays (onChosen hears how it went),
  // kOpenItem asks onOpenFile. Returns whether the id was one of the menu's. While the open
  // document has unsaved edits (CurationSession::Dirty), the mode opens only once onAskDiscard
  // says so, even the mode already ticked.
  bool Choose(int itemId);

  // What the header shows: the mode's name and the caption above it ("MODE · REVERIE"); not
  // named, the default mode (or a state loaded without a name) plays.
  juce::String Name() const { return name_; }
  juce::String Caption() const { return caption_; }
  bool         Named() const { return named_; }
  // The width the longest caption or name of the factory set needs at the current scale,
  // between 200 and 320 pixels at scale 1: the header lays the menu out at it.
  int PreferredWidth() const;

  std::function<void(bool opened, const juce::String& message)> onChosen;
  std::function<void()>                                         onOpenFile;
  // Asked before a factory mode replaces a document with unsaved edits: `mode` names the mode
  // chosen, and `discard` opens it; not calling it keeps the edits and what plays. Unset, an
  // OK/Cancel box asks (AskDiscard, CurationViews.h).
  std::function<void(const juce::String& mode, std::function<void()> discard)> onAskDiscard;

  static constexpr int kFactoryItem = 1;
  static constexpr int kOpenItem    = 10000;

  void paintButton(juce::Graphics&, bool highlighted, bool down) override;
  void clicked() override;

 private:
  // Button's other clicked(const ModifierKeys&) stays visible: hidden, GCC's -Woverloaded-virtual
  // (JUCE's warning set) fails the -Werror build. JUCE's own buttons do the same.
  using juce::Button::clicked;

  // Opens and plays factory package `index` (CurationSession::OpenFactory) and tells onChosen.
  void Open(size_t index);

  BrainscapeProcessor& processor_;
  juce::ScopedMessageBox askBox_;  // the discard question, while it is up
  juce::String         name_, caption_, family_;
  juce::Colour         accent_;
  bool                 named_ = false;  // a mode with a name plays (not the default mode)
};

}  // namespace brainscape::plugin
