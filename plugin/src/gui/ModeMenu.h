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
// pickup, and the curation views show its document.
class ModeMenu final : public Scalable<juce::Button> {
 public:
  explicit ModeMenu(BrainscapeProcessor& processor);

  // One GUI tick: what plays, from the processor's source (BrainscapeProcessor::CurrentSource).
  void Refresh();
  // The menu the button shows: a factory mode's item id is kFactoryItem + its index.
  juce::PopupMenu BuildMenu() const;
  // An item chosen from the menu: a factory mode opens and plays (onChosen hears how it went),
  // kOpenItem asks onOpenFile. Returns whether the id was one of the menu's.
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

  static constexpr int kFactoryItem = 1;
  static constexpr int kOpenItem    = 10000;

  void paintButton(juce::Graphics&, bool highlighted, bool down) override;
  void clicked() override;

 private:
  BrainscapeProcessor& processor_;
  juce::String         name_, caption_, family_;
  juce::Colour         accent_;
  bool                 named_ = false;  // a mode with a name plays (not the default mode)
};

}  // namespace brainscape::plugin
