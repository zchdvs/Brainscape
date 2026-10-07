#pragma once
#include <atomic>

#include <juce_audio_processors/juce_audio_processors.h>

#include "EventQueue.h"
#include "brainscape/ParamDisplay.h"

namespace brainscape::plugin {

// One host parameter per Leaf row (companion §5.3, mode-compiler.md §4.1). The exact
// binary32 plain value is the source of truth; the normalised value JUCE and the host see
// is only a view computed by dsp/'s taper. The atomic is a mirror for host and GUI: the
// engine receives values only as events through the wrapper queue (companion §4.7).
class BrainscapeParam final : public juce::RangedAudioParameter {
 public:
  BrainscapeParam(ParamId id, EventSink& sink);

  ParamId Id() const noexcept { return id_; }
  float   Plain() const noexcept { return plain_.load(std::memory_order_relaxed); }
  float   DefaultPlain() const noexcept { return defaultPlain_; }

  // GUI (message thread): writes exact plain bits, inside its own gesture.
  void SetPlainNotifyingHost(float plain);
  // GUI inside a gesture the caller brackets (a knob drag).
  void SetPlainInGesture(float plain);
  // Wrapper only: the mirror after a state restore or a drained block. No event.
  void StoreMirror(float canonicalPlain) noexcept {
    plain_.store(canonicalPlain, std::memory_order_relaxed);
  }

  float        getValue() const override;
  void         setValue(float newValue) override;
  float        getDefaultValue() const override;
  juce::String getText(float normalisedValue, int maximumStringLength) const override;
  float        getValueForText(const juce::String& text) const override;
  juce::String getCurrentValueAsText() const override;
  int          getNumSteps() const override;
  bool         isDiscrete() const override;
  bool         isBoolean() const override;
  const juce::NormalisableRange<float>& getNormalisableRange() const override { return range_; }

 private:
  const ParamId                  id_;
  const ParamDisplay&            display_;
  const float                    defaultPlain_;
  EventSink&                     sink_;
  std::atomic<float>             plain_;
  juce::NormalisableRange<float> range_;
};

// The performance freeze toggle (companion §5.7): host-automatable, shared by GUI, MIDI
// and DAW, never stored engaged. Row 77 of the ID table, perf.freeze, a Performance row
// (mode-compiler.md §4.2): the Freeze event's host face, never a SetParam.
class FreezeParam final : public juce::AudioParameterBool {
 public:
  explicit FreezeParam(EventSink& sink);

 private:
  void       valueChanged(bool on) override;
  EventSink& sink_;
};

// Typed text to a canonical plain value, in the display's units ("1.2 s", "2.5k", "40%",
// "-25%" on a centred balance, "Off", "Mark"). Decimal text is read by a correctly rounded
// parser, and unit scaling is a shift of the decimal exponent (exact decimal arithmetic for
// a balance) before parsing, so "55.55%" is exactly the float nearest 0.5555 (companion
// §5.3, §6.4).
bool ParsePlainText(ParamId id, const juce::String& text, float& plainOut);

juce::String FormatPlainText(ParamId id, float plain);

}  // namespace brainscape::plugin
